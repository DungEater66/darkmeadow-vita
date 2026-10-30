/*
 * Unpacked package cache.
 *
 * Some packages are loaded again at every battle start (light_sound_table_SF.xxx, heavy_sound_table_SF.xxx, ...: about
 * 5 MB each) and each load froze the game for 0.65-1.4 s. Roughly half of that is file work: reading the deflated zip
 * entry from the .obb and inflating it (about 45 ms per MB on the Vita). r40-r47 kept the compressed bytes in RAM, which
 * removed the reading but not the inflating.
 *
 * Two ways into the cache:
 *  - preloaded: the listed packages (DM_PACKAGE_CACHE_DEFAULT in dm_config.h: the battle sound tables) are read and
 *    inflated once in a background thread right after start, and stay for the whole session;
 *  - kept after first load: while the engine reads any other package (up to 8 MB) from start to end, a copy of the
 *    bytes it already unpacked is kept, so the next load of it (the same enemy at the next battle) comes from RAM.
 *    No extra reading or inflating is done for this. When the limit is reached, kept packages that were never used
 *    again go first (oldest first), then the least recently used.
 * The engine reads a zip entry through ZipFS_openFileFromDirectoryEntry / ZipFS_readFile / ZipFS_closeFile; for an
 * entry that is in the cache, ZipFS_readFile is answered with a memcpy from the unpacked copy (the engine's own
 * handle is still opened and closed normally, it just never has to inflate). Entries are matched by file name and
 * uncompressed size, and only once their unpacking has finished.
 *
 * config.txt: DM_PACKAGE_CACHE 0 = off, DM_PACKAGE_CACHE_FILES = the preload list (replaces the default),
 * DM_PACKAGE_CACHE_ADD = more names to preload (a name ending in * matches a prefix), DM_PACKAGE_CACHE_MB = RAM limit
 * for everything (default 64), DM_PACKAGE_CACHE_KEEP 0 = preload only, no keeping after first load.
 */
#include "reimpl/pkg_cache.h"
#include "reimpl/ue3_trace.h"

#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/clib.h>
#include <vitaGL.h>
#include <psp2/io/stat.h>
#include "reimpl/tex_prefetch.h"

#include "utils/logger.h"

#define MAX_ENTRIES 160   /* r64: 96 -> 160 (prefetched packages too) */
#define MAX_NAMES 32
#define MAX_OPEN 256   /* r58: 48 -> 256 (see h_open) */
#define KEEP_MAX_BYTES (8u << 20)   /* bigger packages (whole areas) are loaded once per visit: not worth keeping */
#define KEEP_MIN_BYTES (32u << 10)

typedef struct {
    char name[64];
    uint32_t size;
    uint8_t *data;      /* NULL = free slot; always `size` bytes */
    volatile uint32_t valid;   /* bytes [0, valid) are filled in (= size once complete) */
    uint8_t pinned;     /* preloaded: never dropped */
    uint8_t announced;  /* "kept" logged */
    uint8_t win;        /* r64: prefetched into the window (physically contiguous block), not the heap */
    uint8_t spill;      /* r71: parked in the spill block (vitaGL RAM pool) while a movie has the window's memory */
    uint8_t prefetched; /* r64: read ahead by the package prefetch */
    uint64_t t_fill;    /* r64: when the prefetch filled it */
    uint64_t last_us;   /* r72: last read by the engine */
    volatile int readers;   /* r72: reads copying from it right now */
    uint32_t hits, stamp;
    int refs;           /* engine handles using it right now */
} entry_t;
static entry_t entries[MAX_ENTRIES];
static int entries_n;   /* slots in use so far (some may be free again) */

/* An open engine handle: served from an entry, or being captured into a new one. */
/* An open engine handle on a cached entry. pos = what the engine has been given; real = how far the engine's own
 * (inflating) handle has actually been read. Zip entries are always read from the start, in order. */
typedef struct { void *h; entry_t *e; uint32_t pos, real; uint8_t served, eof, detached; } open_t;
/* r72: detached = its entry was taken away while the engine still had it open (idle for a minute, and the window
 * needed the room): further reads go to the engine's own handle, brought up to `pos` first (catch_up) */
/* r58: eof = the engine has been given the whole entry; the handle no longer holds the entry (so it can be dropped to
 * make room), and further reads return 0. r57: after floor 1 no enemy package was kept any more and each battle
 * read and unpacked its package twice; handles the engine keeps open after reading everything held their entries
 * (and handle slots) forever. */
static unsigned keep_fail_n;
static open_t opened[MAX_OPEN];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;   /* entries[] and opened[] */
static uint32_t used, clock_;
static int keep_on = 1;
static volatile int hooks_ok, hooks_ready;   /* hooked / started */
static unsigned kept_n, hits_n, evicted_n;

extern volatile uint64_t obb_cache_bytes;   /* "package cache served" in the [io] line (io_stats.c) */

static char c_path[256];
static char c_names[MAX_NAMES][64];
static int c_nnames;
static uint32_t c_limit;

static const char *base_name(const char *p) { const char *b = strrchr(p, '/'); return b ? b + 1 : p; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int wanted(const char *bn) {
    for (int i = 0; i < c_nnames; i++) {
        size_t l = strlen(c_names[i]);
        if (l && c_names[i][l - 1] == '*') { if (!strncasecmp(bn, c_names[i], l - 1)) return 1; }
        else if (!strcasecmp(bn, c_names[i])) return 1;
    }
    return 0;
}

/* Inflate (or copy, for a stored entry) one zip entry into a new buffer of `usize` bytes. */
static uint8_t *unpack(SceUID fd, uint32_t lho, uint32_t csize, uint32_t usize, int method) {
    uint8_t lh[30];
    if (sceIoPread(fd, lh, 30, lho) != 30 || rd32(lh) != 0x04034b50) return NULL;
    int64_t start = (int64_t)lho + 30 + rd16(lh + 26) + rd16(lh + 28);
    uint8_t *out = malloc(usize ? usize : 1);
    if (!out) return NULL;
    if (method == 0) {
        if (sceIoPread(fd, out, usize, start) != (int)usize) { free(out); return NULL; }
        return out;
    }
    if (method != 8) { free(out); return NULL; }
    enum { CHUNK = 256 * 1024 };
    uint8_t *in = malloc(CHUNK);
    if (!in) { free(out); return NULL; }
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) { free(in); free(out); return NULL; }
    z.next_out = out;
    z.avail_out = usize;
    uint32_t done = 0;
    int r = Z_OK;
    while (r == Z_OK && done < csize) {
        int want = csize - done < CHUNK ? (int)(csize - done) : CHUNK;
        if (sceIoPread(fd, in, (SceSize)want, start + done) != want) { r = Z_DATA_ERROR; break; }
        done += want;
        z.next_in = in;
        z.avail_in = want;
        r = inflate(&z, Z_NO_FLUSH);
        if (r == Z_BUF_ERROR && z.avail_in == 0) r = Z_OK;
    }
    int ok = (r == Z_STREAM_END || r == Z_OK) && z.total_out == usize;
    inflateEnd(&z);
    free(in);
    if (!ok) { free(out); return NULL; }
    return out;
}

/* Make room for `need` more bytes: drop unpinned, unused entries, those never used again first, oldest first.
 * Called with lock held. Returns 1 if there is room. */
static int make_room(uint32_t need) {
    while (used + need > c_limit) {
        entry_t *v = NULL;
        for (int i = 0; i < entries_n; i++) {
            entry_t *e = &entries[i];
            if (!e->data || e->pinned || e->refs || e->win || e->spill) continue;
            if (!v || (e->hits == 0) > (v->hits == 0) || ((e->hits == 0) == (v->hits == 0) && e->stamp < v->stamp)) v = e;   /* never reused first, then oldest */
        }
        if (!v) return 0;
        used -= v->size;
        free(v->data);
        v->data = NULL;
        if (evicted_n++ < 40) l_info("[pcache] dropped %s (%u KB, used %u time(s)) to make room", v->name,
                                     (unsigned)(v->size / 1024), v->hits);
    }
    return 1;
}

/* Called with lock held; `data` becomes the cache's. */
static entry_t *insert(const char *name, uint32_t size, uint8_t *data, int pinned) {
    entry_t *c = NULL;
    for (int i = 0; i < entries_n; i++) if (!entries[i].data) { c = &entries[i]; break; }
    if (!c && entries_n < MAX_ENTRIES) c = &entries[entries_n++];
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "%s", name);
    c->size = size;
    c->data = data;
    c->valid = pinned ? size : 0;
    c->pinned = (uint8_t)pinned;
    c->stamp = ++clock_;
    used += size;
    return c;
}

static entry_t *find(const char *bn, uint32_t size) {
    for (int i = 0; i < entries_n; i++)
        if (entries[i].data && entries[i].size == size && !strcasecmp(entries[i].name, bn)) return &entries[i];
    return NULL;
}

static int cache_thread(SceSize args, void *argp) {
    SceUID fd = sceIoOpen(c_path, SCE_O_RDONLY, 0);
    if (fd < 0) { l_warn("[pcache] cannot open %s", c_path); return sceKernelExitDeleteThread(0); }
    int64_t size = sceIoLseek(fd, 0, SCE_SEEK_END);
    static uint8_t tail[66 * 1024];
    int64_t tpos = size > (int64_t)sizeof(tail) ? size - (int64_t)sizeof(tail) : 0;
    int tl = sceIoPread(fd, tail, (SceSize)(size - tpos), tpos);
    uint32_t cd_off = 0, cd_size = 0, count = 0;
    for (int i = tl - 22; i >= 0; i--)
        if (rd32(tail + i) == 0x06054b50) { count = rd16(tail + i + 10); cd_size = rd32(tail + i + 12); cd_off = rd32(tail + i + 16); break; }
    uint8_t *cd = cd_size ? malloc(cd_size) : NULL;
    if (!cd || sceIoPread(fd, cd, cd_size, cd_off) != (int)cd_size) {
        l_warn("[pcache] %s: cannot read the zip directory", c_path);
        free(cd);
        sceIoClose(fd);
        return sceKernelExitDeleteThread(0);
    }
    uint64_t t_all = sceKernelGetProcessTimeWide();
    uint32_t loaded = 0, p = 0;
    int nloaded = 0;
    for (uint32_t e = 0; e < count && p + 46 <= cd_size; e++) {
        if (rd32(cd + p) != 0x02014b50) break;
        int method = rd16(cd + p + 10);
        uint32_t csize = rd32(cd + p + 20), usize = rd32(cd + p + 24), lho = rd32(cd + p + 42);
        uint16_t nl = rd16(cd + p + 28), xl = rd16(cd + p + 30), cl = rd16(cd + p + 32);
        char nm[256];
        int l = nl < 255 ? nl : 255;
        memcpy(nm, cd + p + 46, (size_t)l);
        nm[l] = 0;
        p += 46u + nl + xl + cl;
        const char *bn = base_name(nm);
        if (!*bn || !wanted(bn)) continue;
        pthread_mutex_lock(&lock);
        int have = find(bn, usize) != NULL, room = have || used + usize <= c_limit;
        pthread_mutex_unlock(&lock);
        if (have) continue;
        if (!room) {
            l_warn("[pcache] %s skipped: the %u MB limit would be exceeded (DM_PACKAGE_CACHE_MB)", bn, c_limit >> 20);
            continue;
        }
        uint64_t t0 = sceKernelGetProcessTimeWide();
        uint8_t *data = unpack(fd, lho, csize, usize, method);
        if (!data) { l_warn("[pcache] %s: could not unpack (%u -> %u bytes)", bn, (unsigned)csize, (unsigned)usize); continue; }
        pthread_mutex_lock(&lock);
        entry_t *c = find(bn, usize) ? NULL : insert(bn, usize, data, 1);
        pthread_mutex_unlock(&lock);
        if (!c) { free(data); continue; }
        loaded += usize;
        nloaded++;
        l_info("[pcache] %s: %u KB unpacked in RAM (%u ms)", bn, (unsigned)(usize / 1024),
               (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000));
    }
    free(cd);
    sceIoClose(fd);
    l_info("[pcache] preload ready: %d package(s), %u KB, in %u ms; others are kept after their first load%s",
           nloaded, (unsigned)(loaded / 1024), (unsigned)((sceKernelGetProcessTimeWide() - t_all) / 1000),
           keep_on ? "" : " (off: DM_PACKAGE_CACHE_KEEP 0)");
    return sceKernelExitDeleteThread(0);
}

static void pf_note_open(const char *bn, uint32_t usize);   /* r64: package prefetch, below */

/* ---- engine hooks ---- */
static int (*o_open)(void **, void *, const char *);
static int (*o_read)(void *, char *, int, int);
static void (*o_close)(void *);

static void release(open_t *o) {   /* lock held */
    entry_t *e = o->e;
    if (e) {
        e->refs--;
        if (e->valid == e->size && !e->announced && !e->pinned) {
            e->announced = 1;
            if (kept_n++ < 60) l_info("[pcache] kept %s (%u KB) after its first load; cache %u KB of %u", e->name,
                                      (unsigned)(e->size / 1024), (unsigned)(used / 1024), (unsigned)(c_limit / 1024));
        }
    }
    memset(o, 0, sizeof(*o));
}

/* r60: the zip entries opened, for the [hitch] breakdown (load_stats.h): which files a battle start reads */
#define OPEN_LOG 32
static struct { uint32_t seq, kb; uint8_t cached; char name[40]; } open_log[OPEN_LOG];
volatile uint32_t dm_open_seq;

void pkg_open_log_since(uint32_t seq, char *out, int n) {
    int o = 0;
    out[0] = 0;
    uint32_t now = dm_open_seq;
    if (now - seq > OPEN_LOG) seq = now - OPEN_LOG;
    for (uint32_t k = seq + 1; k <= now && o < n - 48; k++) {
        int i = k % OPEN_LOG;
        if (open_log[i].seq != k) continue;
        o += snprintf(out + o, n - o, "%s%s %u KB%s", o ? ", " : "", open_log[i].name, open_log[i].kb,
                      open_log[i].cached ? " (cache)" : "");
    }
}

/* r62: which packages are kept after their first load. r61 kept everything up to 8 MB, and the engine keeps ~30 area
 * packages (art, pickups, hot spots, audio, cutscenes) open for as long as a floor is loaded, so ~60 MB of the cache
 * could not be freed and from the climb on no enemy package fit any more ("not kept: cache full"). Area packages are
 * loaded once per visit anyway; enemy and weapon packages come back at every fight. DM_PACKAGE_CACHE_KEEP_ONLY in
 * config.txt replaces the list of name prefixes ("*" = everything). */
static char keep_prefixes[256] = "HOR_ai_ HOR_Weapon";
static void pf_start(void);
void pkg_cache_set_keep_only(const char *list) { snprintf(keep_prefixes, sizeof(keep_prefixes), "%s", list); }
static int keep_name(const char *bn) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", keep_prefixes);
    char *save = NULL;
    for (char *t = strtok_r(tmp, " ,", &save); t; t = strtok_r(NULL, " ,", &save)) {
        if (!strcmp(t, "*") || !strncasecmp(bn, t, strlen(t))) return 1;
    }
    return 0;
}

static int h_open(void **out, void *de, const char *pw) {
    int r = o_open(out, de, pw);
    if (!r || !out || !*out || !de || !hooks_ok) return r;
    void *h = *out;
    const char *nm = *(const char **)de;                       /* directory_entry: +0 name, +0x20 uncompressed size */
    uint32_t usize = *(const uint32_t *)((const uint8_t *)de + 0x20);
    const char *bn = nm ? base_name(nm) : "";
    {
        uint32_t k = __sync_add_and_fetch(&dm_open_seq, 1);
        int i = k % OPEN_LOG;
        open_log[i].seq = 0;
        snprintf(open_log[i].name, sizeof(open_log[i].name), "%s", bn);
        open_log[i].kb = usize / 1024;
        pthread_mutex_lock(&lock);
        entry_t *ce = find(bn, usize);
        open_log[i].cached = ce && ce->valid == ce->size;
        pthread_mutex_unlock(&lock);
        open_log[i].seq = k;
    }
    pf_note_open(bn, usize);
    pthread_mutex_lock(&lock);
    int slot = -1;
    for (int i = 0; i < MAX_OPEN; i++) {
        if (opened[i].h == h) release(&opened[i]);               /* a reused handle */
        if (slot < 0 && !opened[i].h) slot = i;
    }
    int fresh = 0;
    open_t *fill = NULL;
    if (slot < 0) {   /* r58: reuse the slot of a handle that has read everything (oldest first) */
        for (int i = 0; i < MAX_OPEN; i++) if (opened[i].h && opened[i].eof) { release(&opened[i]); slot = i; break; }
    }
    if (slot < 0 && *bn && keep_fail_n++ < 10)
        l_warn("[pcache] %s not cached: all %d handle slots are in use", bn, MAX_OPEN);
    if (slot >= 0 && *bn) {
        entry_t *e = find(bn, usize);
        if (!e && keep_on && usize >= KEEP_MIN_BYTES && usize <= KEEP_MAX_BYTES && keep_name(bn)) {
            if (make_room(usize)) {
                uint8_t *data = malloc(usize);
                if (data && !(e = insert(bn, usize, data, 0))) free(data);
                if (e) fresh = 1;
            } else if (keep_fail_n++ < 10) {
                uint32_t pinned = 0, busy = 0;
                int nb = 0;
                for (int i = 0; i < entries_n; i++) {
                    if (!entries[i].data) continue;
                    if (entries[i].pinned) pinned += entries[i].size;
                    else if (entries[i].refs) { busy += entries[i].size; nb++; }
                }
                l_warn("[pcache] %s (%u KB) not kept: cache full (%u KB preloaded, %u KB in %d packages the engine still "
                       "has open, of %u KB)", bn, usize / 1024, pinned / 1024, busy / 1024, nb, c_limit / 1024);
            }
        }
        if (e) {
            open_t *o = &opened[slot];
            memset(o, 0, sizeof(*o));
            o->h = h;
            o->e = e;
            e->refs++;
            e->stamp = ++clock_;
            e->last_us = sceKernelGetProcessTimeWide();   /* r72 */
            if (fresh) fill = o;
        }
    }
    pthread_mutex_unlock(&lock);
    /* r61: unpack a newly kept entry completely right away, through this handle. The engine opens a package two or
     * three times at once (r60's file lists: HOR_ai_light00_CT01.xxx x3); a second handle that got ahead of what the
     * first had filled in unpacked it all again from the start (catch_up), so a battle start read and unpacked its
     * package twice (2.7 MB from the card, 150 ms of inflate for a 1.5 MB package). Filled now, every handle is served
     * from memory and the package is read and unpacked once. */
    if (fill) {
        entry_t *e = fill->e;
        uint32_t got = 0;
        while (got < e->size) {
            uint32_t want = e->size - got < 65536 ? e->size - got : 65536;
            int n = o_read(h, (char *)e->data, (int)got, (int)want);
            if (n <= 0) break;
            got += (uint32_t)n;
            __sync_synchronize();
            pthread_mutex_lock(&lock);
            if (got > e->valid) e->valid = got;
            pthread_mutex_unlock(&lock);
        }
        fill->real = got;   /* the engine's own handle is now this far; reads below `valid` come from memory */
    }
    return r;
}

/* Bring the engine's own handle up to `to` by reading (and dropping) what it would have produced. */
static int catch_up(open_t *o, uint32_t to) {
    char tmp[8192];
    while (o->real < to) {
        int want = to - o->real < sizeof(tmp) ? (int)(to - o->real) : (int)sizeof(tmp);
        int r = o_read(o->h, tmp, 0, want);
        if (r <= 0) return 0;
        o->real += (uint32_t)r;
    }
    return 1;
}

/* r58: the engine has the whole entry: let go of it (the "kept" message and the reference), keep the slot */
static void done_with(open_t *o) {
    pthread_mutex_lock(&lock);
    if (o->e && !o->eof) {
        void *h = o->h;
        uint32_t pos = o->pos;
        uint8_t served = o->served;
        release(o);   /* clears o */
        o->h = h;
        o->pos = pos;
        o->served = served;
        o->eof = 1;
    }
    pthread_mutex_unlock(&lock);
}

static int h_read_entry(open_t *o, entry_t *e, void *h, char *buf, int off, int len);
static int h_read(void *h, char *buf, int off, int len) {
    open_t *o = NULL;
    entry_t *e = NULL;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_OPEN; i++) if (opened[i].h == h) { o = &opened[i]; break; }
    if (o && o->e && !o->detached) { e = o->e; e->readers++; e->last_us = sceKernelGetProcessTimeWide(); }
    pthread_mutex_unlock(&lock);
    if (!o) return o_read(h, buf, off, len);
    if (o->detached) {   /* r72: its entry is gone: the engine's own handle, caught up to where the engine is */
        if (o->eof) return 0;
        if (o->real < o->pos && !catch_up(o, o->pos)) return -1;
        int r = o_read(h, buf, off, len);
        if (r > 0) { o->pos += (uint32_t)r; o->real = o->pos; }
        return r;
    }
    if (!e) return o_read(h, buf, off, len);
    int r = h_read_entry(o, e, h, buf, off, len);
    __sync_fetch_and_sub(&e->readers, 1);
    return r;
}

static int h_read_entry(open_t *o, entry_t *e, void *h, char *buf, int off, int len) {
    if (o->eof) return 0;
    if (len <= 0) return o_read(h, buf, off, len);
    uint32_t want = (uint32_t)len, left = e->size - o->pos;
    if (want > left) want = left;
    if (!want) return 0;
    uint32_t valid = e->valid;
    __sync_synchronize();
    if (o->pos + want <= valid) {   /* all of it is in RAM */
        memcpy(buf + off, e->data + o->pos, want);
        o->pos += want;
        __sync_fetch_and_add(&obb_cache_bytes, (uint64_t)want);
        if (o->pos >= e->size) done_with(o);
        if (!o->served) {
            o->served = 1;
            pthread_mutex_lock(&lock);
            e->hits++;
            pthread_mutex_unlock(&lock);
            if (hits_n++ < 60) l_info("[pcache] %s served from RAM (%u KB%s, use %u)", e->name, (unsigned)(e->size / 1024),
                                      valid == e->size ? "" : " partly", e->hits);
        }
        return (int)want;
    }
    /* past what is cached: the engine's handle has to do it, from where it really is */
    if (o->real < o->pos && !catch_up(o, o->pos)) return -1;
    int r = o_read(h, buf, off, len);
    if (r > 0) {
        uint32_t n = (uint32_t)r;
        if (o->pos + n > e->size) n = e->size - o->pos;
        if (o->pos <= e->valid && o->pos + n > e->valid) {   /* extends the filled-in part */
            memcpy(e->data + o->pos, buf + off, n);
            __sync_synchronize();
            pthread_mutex_lock(&lock);
            if (o->pos + n > e->valid) e->valid = o->pos + n;
            pthread_mutex_unlock(&lock);
        }
        o->pos += (uint32_t)r;
        o->real = o->pos;
        if (o->pos >= e->size) done_with(o);
    }
    return r;
}

static void h_close(void *h) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_OPEN; i++) if (opened[i].h == h) release(&opened[i]);
    pthread_mutex_unlock(&lock);
    o_close(h);
}

/* The engine could not get memory (mem_valve.c): drop entries no handle is using - kept ones first, then the
 * preloaded ones - until `want` bytes are freed or nothing is left. Returns the bytes freed. */
size_t pkg_cache_release(size_t want) {
    size_t freed = 0;
    pthread_mutex_lock(&lock);
    for (int pass = 0; pass < 2 && freed < want; pass++)
        for (int i = 0; i < entries_n && freed < want; i++) {
            entry_t *x = &entries[i];
            if (!x->data || x->refs || x->win || x->spill || (pass == 0 && x->pinned)) continue;
            freed += x->size;
            used -= x->size;
            free(x->data);
            x->data = NULL;
        }
    pthread_mutex_unlock(&lock);
    return freed;
}

/* From so_patch, before the engine's code caches are flushed. Until the cache is started these only pass through. */
void pkg_cache_install_hooks(void) {
    int n = 0;
    n += ue3_hook("_Z32ZipFS_openFileFromDirectoryEntryPPvP15directory_entryPKc", 0xE3510000, 0x13500000,
                  h_open, (void **)&o_open);
    n += ue3_hook("_Z14ZipFS_readFilePvPKcii", 0xE0811002, 0xE1A02003, h_read, (void **)&o_read);
    n += ue3_hook("_Z15ZipFS_closeFilePv", 0xE59F300C, 0xE92D4010, h_close, (void **)&o_close);
    if (n != 3) l_warn("[pcache] only %d of 3 zip functions hooked: the package cache stays off", n);
    hooks_ready = n == 3;
}

void pkg_cache_start(const char *obb_path, const char *names, int limit_mb, int keep) {
    if (!hooks_ready) return;   /* a hook that did go in only passes through: harmless */
    snprintf(c_path, sizeof(c_path), "%s", obb_path);
    c_limit = (uint32_t)limit_mb << 20;
    keep_on = keep;
    c_nnames = 0;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", names);
    for (char *t = strtok(tmp, " ,\t"); t && c_nnames < MAX_NAMES; t = strtok(NULL, " ,\t"))
        snprintf(c_names[c_nnames++], 64, "%s", t);
    __sync_synchronize();
    hooks_ok = 1;   /* the hooks start working now */
    pf_start();     /* r64 */
    if (!c_nnames) return;
    /* priority 160: just below the engine's threads (159), so unpacking never competes with the game */
    SceUID th = sceKernelCreateThread("dm_pcache", cache_thread, 160, 0x10000, 0, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
}

/* ---- r64: package prefetch ----------------------------------------------------------------------------------------
 *
 * In r63 about 19 s of the 51 s of load waits were the memory card and inflating for packages loaded for the first
 * time in the session (area art, hot spots, audio, enemies). Like the texture prefetch, the packages the engine opens
 * are remembered across sessions, in ux0:data/darkmeadow/pkgprefetch.txt: for each package, the next few packages
 * that were opened at least 2 s after it (so not the ones in the same load). When the engine opens a package that
 * has a record, a low-priority thread unpacks its successors into RAM while the game is quiet (it waits while the
 * engine loads, unless the engine is already reading the very package it is unpacking), and the engine's reads are
 * then served from memory as for any cached package.
 *
 * Where they go: enemy and weapon packages (the keep list) into the heap cache, where they stay as before; everything
 * else into the "window", a block of physically contiguous RAM (DM_PACKAGE_PREFETCH_MB, default 24) that vitaGL leaves
 * to the system for the movie decoder and that nothing used after the opening movie. Before any movie the window is
 * emptied and the block given back; it is taken again when the movie is over. Window entries the engine has read
 * are dropped first when room is needed, then ones not used for a minute.
 *
 * config.txt: DM_PACKAGE_PREFETCH 0 = off; DM_PACKAGE_PREFETCH_MB = window size (0 = enemy packages only).
 */
#ifndef DATA_PATH
#define DATA_PATH "ux0:data/darkmeadow/"
#endif
#define PF_FILE DATA_PATH "pkgprefetch.txt"
#define PF_TMP DATA_PATH "pkgprefetch.tmp"
#define PF_NAME 56
#define PF_SUCC 6
#define PF_SEQ 768
#define PF_MAP 1024
#define PF_MIN_GAP_US 2000000ull     /* a successor is opened at least this long after (not in the same load) */
#define PF_MAX_GAP_US 300000000ull   /* and within 5 minutes */
#define PF_QUIET_US 500000ull        /* start a package only after loading has been quiet this long */
#define WIN_STALE_US 60000000ull     /* a window entry nobody used for this long may be dropped */

typedef struct { char name[PF_NAME]; uint32_t size; uint8_t n; char succ[PF_SUCC][PF_NAME]; uint32_t ssize[PF_SUCC]; } pfrec_t;
typedef struct { char name[PF_NAME]; uint32_t lho, csize, usize; uint16_t method; } cdent_t;

static int pf_on = 1, pf_win_mb = 24;
static pfrec_t *pf_map;
static int pf_n;
static struct { char name[PF_NAME]; uint32_t size; uint64_t t; } pf_seq[PF_SEQ];
static int pf_seq_n, pf_dirty;
static pthread_mutex_t pf_lock = PTHREAD_MUTEX_INITIALIZER;   /* pf_seq, the trigger */
static char pf_trig[PF_NAME];
static uint32_t pf_trig_size;
static volatile uint32_t pf_trig_seq;
static int64_t pf_obb_size;
static cdent_t *pf_cd;
static int pf_cd_n;
/* the window */
static SceUID win_blk = -1;
static uint8_t *win_base;
static uint32_t win_size, win_used;
static volatile int win_suspended, win_filling;
static unsigned pf_done_n, pf_skip_n;
static volatile int pf_retrigger;   /* r67: run the last package's list again (after a movie emptied the window) */
extern volatile int song_io_busy;   /* r67: song.c - a song is starting: keep off the card */
int movie_opening_pending(void);    /* r67: movie.c - the opening movie has not played yet */

void pkg_prefetch_config(int on, int win_mb) { pf_on = on; pf_win_mb = win_mb < 0 ? 0 : win_mb > 48 ? 48 : win_mb; }

static int is_pinned_name(const char *bn, uint32_t size) {   /* lock held */
    entry_t *e = find(bn, size);
    return e && e->pinned;
}

static void pf_note_open(const char *bn, uint32_t usize) {
    if (!pf_on || !*bn || usize < KEEP_MIN_BYTES) return;
    size_t l = strlen(bn);
    if (l < 5 || l >= PF_NAME || strcasecmp(bn + l - 4, ".xxx")) return;
    pthread_mutex_lock(&lock);
    int pin = is_pinned_name(bn, usize);
    pthread_mutex_unlock(&lock);
    if (pin) return;   /* the preloaded sound tables are always there */
    pthread_mutex_lock(&pf_lock);
    int seen = 0;
    for (int i = 0; i < pf_seq_n; i++) if (pf_seq[i].size == usize && !strcasecmp(pf_seq[i].name, bn)) { seen = 1; break; }
    if (!seen && pf_seq_n < PF_SEQ) {
        snprintf(pf_seq[pf_seq_n].name, PF_NAME, "%s", bn);
        pf_seq[pf_seq_n].size = usize;
        pf_seq[pf_seq_n].t = sceKernelGetProcessTimeWide();
        pf_seq_n++;
        pf_dirty = 1;
    }
    if (strcasecmp(pf_trig, bn) || pf_trig_size != usize) {
        snprintf(pf_trig, PF_NAME, "%s", bn);
        pf_trig_size = usize;
        pf_trig_seq++;
    }
    pthread_mutex_unlock(&pf_lock);
}

static pfrec_t *pf_find(const char *bn, uint32_t size) {
    for (int i = 0; i < pf_n; i++) if (pf_map[i].size == size && !strcasecmp(pf_map[i].name, bn)) return &pf_map[i];
    return NULL;
}

static void pf_load(void) {
    FILE *f = fopen(PF_FILE, "r");
    if (!f) { l_info("[pkgpf] no recorded packages yet (%s): recording this session", PF_FILE); return; }
    char line[1024];
    int ok = 0;
    if (fgets(line, sizeof(line), f)) {
        long long sz = 0;
        if (sscanf(line, "# pkgprefetch 1 obb %lld", &sz) == 1 && sz == pf_obb_size) ok = 1;
    }
    if (!ok) { fclose(f); l_info("[pkgpf] %s is for another game version: starting over", PF_FILE); return; }
    while (fgets(line, sizeof(line), f) && pf_n < PF_MAP) {
        pfrec_t r;
        memset(&r, 0, sizeof(r));
        char *save = NULL, *t = strtok_r(line, " \t\r\n", &save);
        if (!t || strlen(t) >= PF_NAME) continue;
        snprintf(r.name, PF_NAME, "%s", t);
        if (!(t = strtok_r(NULL, " \t\r\n", &save))) continue;
        r.size = (uint32_t)strtoul(t, NULL, 10);
        while (r.n < PF_SUCC) {
            char *a = strtok_r(NULL, " \t\r\n", &save), *b = a ? strtok_r(NULL, " \t\r\n", &save) : NULL;
            if (!a || !b || strlen(a) >= PF_NAME) break;
            snprintf(r.succ[r.n], PF_NAME, "%s", a);
            r.ssize[r.n] = (uint32_t)strtoul(b, NULL, 10);
            r.n++;
        }
        if (r.n) pf_map[pf_n++] = r;
    }
    fclose(f);
    l_info("[pkgpf] %d packages with recorded successors loaded from %s", pf_n, PF_FILE);
}

/* fold this session's order into the map and write it out */
static void pf_save(void) {
    pthread_mutex_lock(&pf_lock);
    int n = pf_seq_n;
    pf_dirty = 0;
    pthread_mutex_unlock(&pf_lock);
    int changed = 0;
    for (int i = 0; i < n; i++) {
        pfrec_t r;
        memset(&r, 0, sizeof(r));
        snprintf(r.name, PF_NAME, "%s", pf_seq[i].name);
        r.size = pf_seq[i].size;
        for (int j = i + 1; j < n && r.n < PF_SUCC; j++) {
            uint64_t d = pf_seq[j].t - pf_seq[i].t;
            if (d < PF_MIN_GAP_US) continue;
            if (d > PF_MAX_GAP_US) break;
            snprintf(r.succ[r.n], PF_NAME, "%s", pf_seq[j].name);
            r.ssize[r.n] = pf_seq[j].size;
            r.n++;
        }
        if (!r.n) continue;
        pfrec_t *old = pf_find(r.name, r.size);
        if (old) {
            if (old->n > r.n) continue;   /* the session ended sooner after it than the recorded one: keep that */
            if (!memcmp(old, &r, sizeof(r))) continue;
            *old = r;
        } else if (pf_n < PF_MAP) pf_map[pf_n++] = r;
        else continue;
        changed++;
    }
    if (!changed) return;
    FILE *f = fopen(PF_TMP, "w");
    if (!f) return;
    fprintf(f, "# pkgprefetch 1 obb %lld\n", (long long)pf_obb_size);
    for (int i = 0; i < pf_n; i++) {
        fprintf(f, "%s %u", pf_map[i].name, (unsigned)pf_map[i].size);
        for (int k = 0; k < pf_map[i].n; k++) fprintf(f, " %s %u", pf_map[i].succ[k], (unsigned)pf_map[i].ssize[k]);
        fputc('\n', f);
    }
    fclose(f);
    sceIoRemove(PF_FILE);
    sceIoRename(PF_TMP, PF_FILE);
    l_info("[pkgpf] %d package records saved (%d new or changed)", pf_n, changed);
}

static const cdent_t *pf_cd_find(const char *bn, uint32_t usize) {
    for (int i = 0; i < pf_cd_n; i++) if (pf_cd[i].usize == usize && !strcasecmp(pf_cd[i].name, bn)) return &pf_cd[i];
    return NULL;
}

static int pf_read_cd(SceUID fd) {
    int64_t size = sceIoLseek(fd, 0, SCE_SEEK_END);
    pf_obb_size = size;
    static uint8_t tail[66 * 1024];
    int64_t tpos = size > (int64_t)sizeof(tail) ? size - (int64_t)sizeof(tail) : 0;
    int tl = sceIoPread(fd, tail, (SceSize)(size - tpos), tpos);
    uint32_t cd_off = 0, cd_size = 0, count = 0;
    for (int i = tl - 22; i >= 0; i--)
        if (rd32(tail + i) == 0x06054b50) { count = rd16(tail + i + 10); cd_size = rd32(tail + i + 12); cd_off = rd32(tail + i + 16); break; }
    uint8_t *cd = cd_size ? malloc(cd_size) : NULL;
    if (!cd || sceIoPread(fd, cd, cd_size, cd_off) != (int)cd_size) { free(cd); return 0; }
    pf_cd = calloc(count ? count : 1, sizeof(cdent_t));
    if (!pf_cd) { free(cd); return 0; }
    uint32_t p = 0;
    for (uint32_t e = 0; e < count && p + 46 <= cd_size; e++) {
        if (rd32(cd + p) != 0x02014b50) break;
        uint16_t nl = rd16(cd + p + 28), xl = rd16(cd + p + 30), cl = rd16(cd + p + 32);
        char nm[256];
        int l = nl < 255 ? nl : 255;
        memcpy(nm, cd + p + 46, (size_t)l);
        nm[l] = 0;
        const char *bn = base_name(nm);
        size_t bl = strlen(bn);
        if (bl > 4 && bl < PF_NAME && !strcasecmp(bn + bl - 4, ".xxx")) {
            cdent_t *c = &pf_cd[pf_cd_n++];
            snprintf(c->name, PF_NAME, "%s", bn);
            c->method = rd16(cd + p + 10);
            c->csize = rd32(cd + p + 20);
            c->usize = rd32(cd + p + 24);
            c->lho = rd32(cd + p + 42);
        }
        p += 46u + nl + xl + cl;
    }
    free(cd);
    return 1;
}

/* ---- the window: first fit in the block, lock held ---- */
static uint8_t *win_alloc(uint32_t n) {
    if (!win_base || win_suspended) return NULL;
    n = (n + 63) & ~63u;
    uint32_t at = 0;
    for (;;) {
        uint32_t next_end = 0;
        int clash = 0;
        for (int i = 0; i < entries_n; i++) {
            entry_t *e = &entries[i];
            if (!e->data || !e->win) continue;
            uint32_t s0 = (uint32_t)(e->data - win_base), s1 = s0 + ((e->size + 63) & ~63u);
            if (at < s1 && at + n > s0) { clash = 1; if (s1 > next_end) next_end = s1; }
        }
        if (!clash) return at + n <= win_size ? win_base + at : NULL;
        at = next_end;
        if (at + n > win_size) return NULL;
    }
}

static SceClibMspace spill_space;   /* r71 */
static uint32_t spill_used;
static void entry_drop(entry_t *e) {   /* lock held, refs == 0 */
    if (e->spill) { spill_used -= e->size; sceClibMspaceFree(spill_space, e->data); e->spill = 0; }
    else if (e->win) win_used -= e->size;
    else { used -= e->size; free(e->data); }
    e->data = NULL;
}

/* make room in the window: read ones first (oldest), then stale unread ones */
static uint8_t *win_get(uint32_t n) {
    uint8_t *p;
    while (!(p = win_alloc(n))) {
        entry_t *v = NULL;
        uint64_t now = sceKernelGetProcessTimeWide();
        for (int i = 0; i < entries_n; i++) {
            entry_t *e = &entries[i];
            if (!e->data || !e->win || e->refs) continue;
            if (!e->hits && now - e->t_fill < WIN_STALE_US) continue;
            if (!v || (e->hits > 0) > (v->hits > 0) || ((e->hits > 0) == (v->hits > 0) && e->stamp < v->stamp)) v = e;
        }
        if (!v) {   /* r72: an entry the engine holds open but has not read for a minute: let its handles go on alone.
                     * r71's log: 13-15 area packages (fl1/fl2 art, hot spots, pick-ups, 21-24 MB) stayed open in the
                     * engine for the rest of the session and filled the window; the next floors' audio and hot spots
                     * (10-11 MB each) never fit. */
            for (int i = 0; i < entries_n; i++) {
                entry_t *e = &entries[i];
                if (!e->data || !e->win || !e->refs || e->readers || now - e->last_us < WIN_STALE_US) continue;
                if (!v || e->last_us < v->last_us) v = e;
            }
            if (!v) return NULL;
            int nh = 0;
            for (int i = 0; i < MAX_OPEN; i++)
                if (opened[i].h && opened[i].e == v) { opened[i].e = NULL; opened[i].detached = 1; nh++; }
            static int nd;
            if (nd++ < 60) l_info("[pkgpf] %s (%u KB) let go: open in the engine (%d handle(s)) but not read for %u s",
                                  v->name, v->size / 1024, nh, (unsigned)((now - v->last_us) / 1000000));
            v->refs = 0;
        }
        entry_drop(v);
    }
    return p;
}

static void win_take(void) {
    if (win_blk >= 0 || win_suspended || pf_win_mb <= 0) return;
    /* r67: not before the opening movie: it needs this memory, and r66 threw away the 21 MB prefetched at the title */
    if (movie_opening_pending()) return;
    uint32_t sz = (uint32_t)pf_win_mb << 20;
    SceUID b = sceKernelAllocMemBlock("dm_pkgwin", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, sz, NULL);
    if (b < 0) {
        static int said;
        if (!said++) l_warn("[pkgpf] no %d MB physically contiguous block (0x%08x): prefetching enemy packages only", pf_win_mb, (unsigned)b);
        return;
    }
    void *base = NULL;
    sceKernelGetMemBlockBase(b, &base);
    pthread_mutex_lock(&lock);
    win_blk = b;
    win_base = base;
    win_size = sz;
    win_used = 0;
    pthread_mutex_unlock(&lock);
    static int n;
    if (n++ < 10) l_info("[pkgpf] %d MB window for prefetched packages (physically contiguous RAM)", pf_win_mb);
}

/* movie.c: a movie starts (1) / is over (0). The decoder needs the physically contiguous RAM. */
void pkg_cache_movie(int starting) {
    if (!starting) { win_suspended = 0; pf_retrigger = 1; return; }
    win_suspended = 1;
    for (int i = 0; i < 150 && win_filling; i++) sceKernelDelayThread(10 * 1000);
    for (int tries = 0; tries < 100; tries++) {
        if (tries == 50) {   /* r70: still open in the engine after 0.5 s: move those entries out of the window. r69's
                              * ending movie got no physically contiguous memory (an entry stayed open) and did not play.
                              * r71: into the spill block (vitaGL's RAM pool, otherwise idle), the heap only if it is full:
                              * r70 put them on the heap, 20 MB over the cache's limit for the rest of the session. */
            pthread_mutex_lock(&lock);
            int moved = 0, to_heap = 0;
            uint32_t kb = 0;
            for (int i = 0; i < entries_n; i++) {
                entry_t *e = &entries[i];
                if (!e->data || !e->win || !e->refs) continue;
                uint8_t *copy = spill_space ? sceClibMspaceMalloc(spill_space, e->size) : NULL;
                int sp = copy != NULL;
                if (!copy) copy = malloc(e->size);
                if (!copy) continue;
                memcpy(copy, e->data, e->valid);
                __sync_synchronize();
                e->data = copy;
                e->win = 0;
                win_used -= e->size;
                if (sp) { e->spill = 1; spill_used += e->size; }
                else { used += e->size; to_heap++; }
                moved++;
                kb += e->size / 1024;
                l_info("[pkgpf]   still open for the movie: %s (%u KB) -> %s", e->name, e->size / 1024, sp ? "spill block" : "heap");
            }
            pthread_mutex_unlock(&lock);
            if (moved) {
                l_info("[pkgpf] %d package(s), %u KB, still open moved out of the window for the movie (%d to the heap)", moved, kb, to_heap);
                sceKernelDelayThread(50 * 1000);
            }
        }
        int busy = 0;
        pthread_mutex_lock(&lock);
        for (int i = 0; i < entries_n; i++) {
            entry_t *e = &entries[i];
            if (!e->data || !e->win) continue;
            if (e->refs) busy++;
            else entry_drop(e);
        }
        SceUID b = -1;
        if (!busy && !win_filling && win_blk >= 0) { b = win_blk; win_blk = -1; win_base = NULL; win_size = win_used = 0; }
        pthread_mutex_unlock(&lock);
        if (b >= 0) { sceKernelFreeMemBlock(b); l_info("[pkgpf] window given back for the movie"); return; }
        if (win_blk < 0) return;
        sceKernelDelayThread(10 * 1000);
    }
    l_warn("[pkgpf] the window is still in use: the movie has less memory");
}

/* unpack straight into an entry, publishing progress so the engine can be served while it fills */
static int unpack_into(SceUID fd, const cdent_t *c, entry_t *e) {
    uint8_t lh[30];
    if (sceIoPread(fd, lh, 30, c->lho) != 30 || rd32(lh) != 0x04034b50) return 0;
    int64_t start = (int64_t)c->lho + 30 + rd16(lh + 26) + rd16(lh + 28);
    enum { CHUNK = 128 * 1024 };
    if (c->method == 0) {
        uint32_t done = 0;
        while (done < c->usize) {
            while (song_io_busy && !win_suspended) sceKernelDelayThread(20 * 1000);
            uint32_t want = c->usize - done < CHUNK ? c->usize - done : CHUNK;
            if (sceIoPread(fd, e->data + done, want, start + done) != (int)want) return 0;
            done += want;
            __sync_synchronize();
            pthread_mutex_lock(&lock);
            if (done > e->valid) e->valid = done;
            pthread_mutex_unlock(&lock);
        }
        return 1;
    }
    if (c->method != 8) return 0;
    static uint8_t in[CHUNK];
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return 0;
    z.next_out = e->data;
    z.avail_out = c->usize;
    uint32_t done = 0;
    int r = Z_OK;
    while (r == Z_OK && done < c->csize) {
        /* the engine started loading and is not waiting for this one: let it have the card */
        while (((texpf_busy() && e->refs == 1) || song_io_busy) && !win_suspended) sceKernelDelayThread(20 * 1000);
        if (win_suspended && e->win) { r = Z_DATA_ERROR; break; }
        int want = c->csize - done < CHUNK ? (int)(c->csize - done) : CHUNK;
        if (sceIoPread(fd, in, (SceSize)want, start + done) != want) { r = Z_DATA_ERROR; break; }
        done += want;
        z.next_in = in;
        z.avail_in = want;
        r = inflate(&z, Z_NO_FLUSH);
        if (r == Z_BUF_ERROR && z.avail_in == 0) r = Z_OK;
        __sync_synchronize();
        pthread_mutex_lock(&lock);
        if (z.total_out > e->valid) e->valid = (uint32_t)z.total_out;
        pthread_mutex_unlock(&lock);
    }
    int ok = (r == Z_STREAM_END || r == Z_OK) && z.total_out == c->usize;
    inflateEnd(&z);
    return ok;
}

static void pf_fetch(SceUID fd, const char *bn, uint32_t usize, const char *why) {
    const cdent_t *c = pf_cd_find(bn, usize);
    if (!c) return;
    pthread_mutex_lock(&lock);
    if (find(bn, usize)) { pthread_mutex_unlock(&lock); return; }
    int heap = keep_on && keep_name(bn) && usize <= KEEP_MAX_BYTES;
    entry_t *e = NULL;
    uint8_t *data = NULL;
    if (heap) {
        if (make_room(usize) && (data = malloc(usize)) && !(e = insert(bn, usize, data, 0))) { free(data); data = NULL; }
    } else if (usize <= win_size / 2 && (data = win_get(usize))) {
        if ((e = insert(bn, usize, data, 0))) { used -= usize; win_used += usize; e->win = 1; }
    }
    if (e) {
        e->prefetched = 1;
        e->announced = 1;
        e->refs = 1;   /* ours while it fills */
        if (e->win) win_filling = 1;
    }
    pthread_mutex_unlock(&lock);
    if (!e) {
        if (pf_skip_n++ < 20) {
            char held[400];
            int o = 0;
            held[0] = 0;
            pthread_mutex_lock(&lock);   /* r71: what fills the window: packages the engine still has open */
            for (int i = 0; i < entries_n && o < (int)sizeof(held) - 64; i++)
                if (entries[i].data && entries[i].win && entries[i].refs)
                    o += snprintf(held + o, sizeof(held) - o, "%s%s %u KB", o ? ", " : "", entries[i].name, entries[i].size / 1024);
            pthread_mutex_unlock(&lock);
            l_info("[pkgpf] %s (%u KB) not prefetched: no room; open in the engine: %s", bn, usize / 1024, o ? held : "none");
        }
        return;
    }
    uint64_t t0 = sceKernelGetProcessTimeWide();
    int ok = unpack_into(fd, c, e);
    pthread_mutex_lock(&lock);
    e->refs--;
    e->t_fill = sceKernelGetProcessTimeWide();
    if (e->win) win_filling = 0;
    if (!ok) {
        e->valid = 0;
        if (!e->refs) entry_drop(e);   /* else: the engine's handle reads past `valid` itself, and the entry goes later */
    }
    uint32_t wu = win_used, hu = used;
    pthread_mutex_unlock(&lock);
    if (pf_done_n++ < 200)
        l_info("[pkgpf] %s %s (%u KB, after %s) in %u ms%s; window %u/%u KB, heap cache %u KB", ok ? "prefetched" : "gave up on",
               bn, usize / 1024, why, (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000), heap ? " into the heap cache" : "",
               wu / 1024, win_size / 1024, hu / 1024);
}

/* r71: from gl_init, before the rendering thread exists (vitaGL's allocator is not shared with it): a block of
 * vitaGL's RAM pool (about 39 MB of it sat unused in every log) for window entries the engine still has open when a
 * movie needs the window's memory. */
void pkg_cache_attach_spill(void) {
    if (!pf_on || pf_win_mb <= 0 || spill_space) return;
    uint32_t sz = (uint32_t)pf_win_mb << 20;
    void *blk = vglAlloc(sz + (sz >> 4), VGL_MEM_RAM);
    if (blk) spill_space = sceClibMspaceCreate(blk, sz + (sz >> 4));
    if (spill_space) l_info("[pkgpf] %u MB spill block in vitaGL's RAM pool (vitaGL RAM free now %u MB)", (unsigned)(sz >> 20),
                            (unsigned)(vglMemFree(VGL_MEM_RAM) >> 20));
    else l_warn("[pkgpf] no spill block in vitaGL's RAM pool: packages open during a movie go to the heap");
}

/* r71: after a movie, back into the window (or dropped, if the engine has let go of them) */
static void spill_return(void) {
    if (!spill_used || !win_base) return;
    int back = 0, dropped = 0;
    uint8_t *old[MAX_ENTRIES];
    int nold = 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < entries_n; i++) {
        entry_t *e = &entries[i];
        if (!e->data || !e->spill) continue;
        if (!e->refs) { entry_drop(e); dropped++; continue; }
        uint8_t *p = win_get(e->size);
        if (!p) continue;
        memcpy(p, e->data, e->valid);
        __sync_synchronize();
        old[nold++] = e->data;
        e->data = p;
        e->spill = 0;
        e->win = 1;
        spill_used -= e->size;
        win_used += e->size;
        back++;
    }
    pthread_mutex_unlock(&lock);
    if (nold) {
        sceKernelDelayThread(50 * 1000);   /* a read still copying from the old place finishes first */
        for (int i = 0; i < nold; i++) sceClibMspaceFree(spill_space, old[i]);
    }
    if (back || dropped) l_info("[pkgpf] after the movie: %d package(s) back in the window, %d dropped (no longer open)", back, dropped);
}

/* r71: the heap cache over its limit (entries that were open when they would have been dropped): drop the ones the
 * engine has let go of since */
static void heap_trim(void) {
    pthread_mutex_lock(&lock);
    uint32_t before = used;
    if (used > c_limit) make_room(0);
    uint32_t after = used;
    pthread_mutex_unlock(&lock);
    if (after < before) {
        static int n;
        if (n++ < 40) l_info("[pcache] over the limit: dropped %u KB no longer open; cache %u KB of %u", (before - after) / 1024,
                             after / 1024, c_limit / 1024);
    }
}

static int pf_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    SceUID fd = sceIoOpen(c_path, SCE_O_RDONLY, 0);
    if (fd < 0 || !pf_read_cd(fd)) { l_warn("[pkgpf] cannot read %s: package prefetch off", c_path); return sceKernelExitDeleteThread(0); }
    pf_map = calloc(PF_MAP, sizeof(pfrec_t));
    if (!pf_map) return sceKernelExitDeleteThread(0);
    pf_load();
    uint32_t seen = 0;
    uint64_t last_save = sceKernelGetProcessTimeWide();
    for (;;) {
        sceKernelDelayThread(100 * 1000);
        uint64_t now = sceKernelGetProcessTimeWide();
        if (pf_dirty && now - last_save > 60000000ull && !texpf_busy()) { pf_save(); last_save = now; }
        {   /* r71: every 2 s */
            static uint64_t last_trim;
            if (now - last_trim > 2000000ull) {
                last_trim = now;
                heap_trim();
                if (!win_suspended && spill_used) { win_take(); spill_return(); }
            }
        }
        if (pf_retrigger) { pf_retrigger = 0; seen = pf_trig_seq - 1; }   /* r67 */
        if (pf_trig_seq == seen) continue;
        pthread_mutex_lock(&pf_lock);
        seen = pf_trig_seq;
        char t[PF_NAME];
        snprintf(t, sizeof(t), "%s", pf_trig);
        uint32_t ts = pf_trig_size;
        pthread_mutex_unlock(&pf_lock);
        pfrec_t *r = pf_find(t, ts);
        if (!r) continue;
        pfrec_t rc = *r;
        /* wait for the load that opened it to finish */
        while (texpf_busy() || texpf_quiet_us() < PF_QUIET_US || song_io_busy) sceKernelDelayThread(50 * 1000);
        if (!win_suspended) win_take();
        for (int k = 0; k < rc.n; k++) {
            if (pf_trig_seq != seen) break;   /* the engine moved on: follow the newer package */
            pf_fetch(fd, rc.succ[k], rc.ssize[k], t);
        }
    }
    return 0;
}

static void pf_start(void) {
    if (!pf_on) { l_info("[pkgpf] off (DM_PACKAGE_PREFETCH 0)"); return; }
    SceUID th = sceKernelCreateThread("dm_pkgpf", pf_thread, 160, 0x10000, 0, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
}
