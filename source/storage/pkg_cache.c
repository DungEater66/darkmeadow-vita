/*
 * Unpacked package cache.
 *
 * Some packages are loaded again at every battle start (the battle sound tables, about 5 MB each; the enemy and
 * weapon packages), and each load froze the game for 0.65-1.4 s. Roughly half of that is file work: reading the
 * deflated zip entry from the .obb and inflating it (about 45 ms per MB on the Vita).
 *
 * Three ways into the cache:
 *  - preloaded: the battle sound tables (PRELOAD) are read and inflated once in a background thread right after
 *    start, and stay for the whole session;
 *  - kept after first load: while the engine reads an enemy or weapon package (KEEP_PREFIXES, up to 8 MB), a copy of
 *    what it unpacks is kept, so the next load of it (the same enemy at the next battle) comes from RAM. Other
 *    packages are area packages, loaded once per visit, and the engine keeps ~30 of them open for as long as a floor is
 *    loaded: keeping those would fill the cache with memory that cannot be freed. When the limit is reached, kept
 *    packages that were never used again go first (oldest first), then the least recently used;
 *  - prefetched: packages read ahead of the engine, from what it opened in earlier sessions (pkg_prefetch.c).
 *
 * The engine reads a zip entry through ZipFS_openFileFromDirectoryEntry / ZipFS_readFile / ZipFS_closeFile; for an
 * entry that is in the cache, ZipFS_readFile is answered with a memcpy from the unpacked copy (the engine's own
 * handle is still opened and closed normally, it just never has to inflate). Entries are matched by file name and
 * uncompressed size, and are served as far as they are filled in.
 */
#include "storage/pkg_cache.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "game_info.h"
#include "hooks/hook.h"
#include "storage/load_stats.h"
#include "storage/obb_stream.h"
#include "storage/pkg_cache_internal.h"
#include "util/logger.h"

#define LIMIT_MB 96
#define PRELOAD "light_sound_table_SF.xxx heavy_sound_table_SF.xxx witch_sound_table_SF.xxx " \
                "trickster_sound_table_SF.xxx ct_boss_sound_table_SF.xxx"
#define KEEP_PREFIXES "HOR_ai_ HOR_Weapon"

pthread_mutex_t pkg_lock = PTHREAD_MUTEX_INITIALIZER;
pkg_entry_t pkg_entries[PKG_MAX_ENTRIES];
int pkg_entries_n;
pkg_open_t pkg_opened[PKG_MAX_OPEN];
uint32_t pkg_used, pkg_limit;
char pkg_obb_path[256];

static uint32_t clock_;
static volatile int hooks_ready, started;
static unsigned kept_n, hits_n, evicted_n, keep_fail_n;

/* ---- zip helpers ---- */
static const char *pkg_base_name(const char *p) {
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

static uint16_t pkg_rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t pkg_rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int pkg_zip_for_each(SceUID fd, int64_t *file_size, pkg_zip_fn fn, void *ctx) {
    int64_t size = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (file_size) *file_size = size;
    static uint8_t tail[66 * 1024];   /* the end-of-directory record is within the last 64 KB + 22 bytes */
    static pthread_mutex_t tail_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&tail_lock);
    int64_t tpos = size > (int64_t)sizeof(tail) ? size - (int64_t)sizeof(tail) : 0;
    int tl = sceIoPread(fd, tail, (SceSize)(size - tpos), tpos);
    uint32_t cd_off = 0, cd_size = 0, count = 0;
    for (int i = tl - 22; i >= 0; i--)
        if (pkg_rd32(tail + i) == 0x06054b50) {
            count = pkg_rd16(tail + i + 10);
            cd_size = pkg_rd32(tail + i + 12);
            cd_off = pkg_rd32(tail + i + 16);
            break;
        }
    pthread_mutex_unlock(&tail_lock);
    uint8_t *cd = cd_size ? malloc(cd_size) : NULL;
    if (!cd || sceIoPread(fd, cd, cd_size, cd_off) != (int)cd_size) {
        free(cd);
        return 0;
    }
    uint32_t p = 0;
    for (uint32_t e = 0; e < count && p + 46 <= cd_size; e++) {
        if (pkg_rd32(cd + p) != 0x02014b50) break;
        uint16_t nl = pkg_rd16(cd + p + 28), xl = pkg_rd16(cd + p + 30), cl = pkg_rd16(cd + p + 32);
        char name[256];
        int l = nl < 255 ? nl : 255;
        memcpy(name, cd + p + 46, (size_t)l);
        name[l] = 0;
        const char *bn = pkg_base_name(name);
        if (*bn)
            fn(ctx, bn, pkg_rd16(cd + p + 10), pkg_rd32(cd + p + 20), pkg_rd32(cd + p + 24), pkg_rd32(cd + p + 42));
        p += 46u + nl + xl + cl;
    }
    free(cd);
    return 1;
}

int64_t pkg_zip_data_start(SceUID fd, uint32_t lho) {
    uint8_t lh[30];
    if (sceIoPread(fd, lh, 30, lho) != 30 || pkg_rd32(lh) != 0x04034b50) return -1;
    return (int64_t)lho + 30 + pkg_rd16(lh + 26) + pkg_rd16(lh + 28);
}

/* Inflate (or copy, for a stored entry) one zip entry into a new buffer of `usize` bytes. */
static uint8_t *unpack(SceUID fd, uint32_t lho, uint32_t csize, uint32_t usize, int method) {
    int64_t start = pkg_zip_data_start(fd, lho);
    if (start < 0 || (method != 0 && method != 8)) return NULL;
    uint8_t *out = malloc(usize ? usize : 1);
    if (!out) return NULL;
    if (method == 0) {
        if (sceIoPread(fd, out, usize, start) == (int)usize) return out;
        free(out);
        return NULL;
    }
    enum { CHUNK = 256 * 1024 };
    uint8_t *in = malloc(CHUNK);
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (!in || inflateInit2(&z, -MAX_WBITS) != Z_OK) {
        free(in);
        free(out);
        return NULL;
    }
    z.next_out = out;
    z.avail_out = usize;
    uint32_t done = 0;
    int r = Z_OK;
    while (r == Z_OK && done < csize) {
        int want = csize - done < CHUNK ? (int)(csize - done) : CHUNK;
        if (sceIoPread(fd, in, (SceSize)want, start + done) != want) {
            r = Z_DATA_ERROR;
            break;
        }
        done += want;
        z.next_in = in;
        z.avail_in = want;
        r = inflate(&z, Z_NO_FLUSH);
        if (r == Z_BUF_ERROR && z.avail_in == 0) r = Z_OK;
    }
    int ok = (r == Z_STREAM_END || r == Z_OK) && z.total_out == usize;
    inflateEnd(&z);
    free(in);
    if (ok) return out;
    free(out);
    return NULL;
}

/* ---- entries (lock held) ---- */

/* Make room for `need` more heap bytes: drop unpinned entries no handle uses, those never used again first, then the
 * oldest. Returns 1 if there is room. */
int pkg_make_room(uint32_t need) {
    while (pkg_used + need > pkg_limit) {
        pkg_entry_t *v = NULL;
        for (int i = 0; i < pkg_entries_n; i++) {
            pkg_entry_t *e = &pkg_entries[i];
            if (!e->data || e->pinned || e->refs || e->win || e->spill) continue;
            if (!v || (e->hits == 0) > (v->hits == 0) || ((e->hits == 0) == (v->hits == 0) && e->stamp < v->stamp)) v = e;
        }
        if (!v) return 0;
        pkg_used -= v->size;
        free(v->data);
        v->data = NULL;
        if (evicted_n++ < 40)
            l_info("[pcache] dropped %s (%u KB, used %u time(s)) to make room", v->name, (unsigned)(v->size / 1024),
                   (unsigned)v->hits);
    }
    return 1;
}

/* `data` becomes the cache's; counted as heap bytes */
pkg_entry_t *pkg_insert(const char *name, uint32_t size, uint8_t *data, int pinned) {
    pkg_entry_t *c = NULL;
    for (int i = 0; i < pkg_entries_n && !c; i++)
        if (!pkg_entries[i].data) c = &pkg_entries[i];
    if (!c && pkg_entries_n < PKG_MAX_ENTRIES) c = &pkg_entries[pkg_entries_n++];
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "%s", name);
    c->size = size;
    c->data = data;
    c->valid = pinned ? size : 0;
    c->pinned = (uint8_t)pinned;
    c->stamp = ++clock_;
    pkg_used += size;
    return c;
}

pkg_entry_t *pkg_find(const char *bn, uint32_t size) {
    for (int i = 0; i < pkg_entries_n; i++)
        if (pkg_entries[i].data && pkg_entries[i].size == size && !strcasecmp(pkg_entries[i].name, bn))
            return &pkg_entries[i];
    return NULL;
}

int pkg_keep_name(const char *bn) {
    char list[] = KEEP_PREFIXES, *save = NULL;
    for (char *t = strtok_r(list, " ", &save); t; t = strtok_r(NULL, " ", &save))
        if (!strncasecmp(bn, t, strlen(t))) return 1;
    return 0;
}

/* ---- preload ---- */
static int is_preloaded(const char *bn) {
    char list[] = PRELOAD, *save = NULL;
    for (char *t = strtok_r(list, " ", &save); t; t = strtok_r(NULL, " ", &save))
        if (!strcasecmp(bn, t)) return 1;
    return 0;
}

typedef struct { SceUID fd; uint32_t bytes; int count; } preload_t;

static void preload_one(void *ctx, const char *bn, int method, uint32_t csize, uint32_t usize, uint32_t lho) {
    preload_t *pl = ctx;
    if (!is_preloaded(bn)) return;
    pthread_mutex_lock(&pkg_lock);
    int have = pkg_find(bn, usize) != NULL, room = pkg_used + usize <= pkg_limit;
    pthread_mutex_unlock(&pkg_lock);
    if (have) return;
    if (!room) {
        l_warn("[pcache] %s skipped: the %u MB limit would be exceeded", bn, (unsigned)(pkg_limit >> 20));
        return;
    }
    uint64_t t0 = sceKernelGetProcessTimeWide();
    uint8_t *data = unpack(pl->fd, lho, csize, usize, method);
    if (!data) {
        l_warn("[pcache] %s: could not unpack (%u -> %u bytes)", bn, (unsigned)csize, (unsigned)usize);
        return;
    }
    pthread_mutex_lock(&pkg_lock);
    pkg_entry_t *c = pkg_find(bn, usize) ? NULL : pkg_insert(bn, usize, data, 1);
    pthread_mutex_unlock(&pkg_lock);
    if (!c) {
        free(data);
        return;
    }
    pl->bytes += usize;
    pl->count++;
    l_info("[pcache] %s: %u KB unpacked in RAM (%u ms)", bn, (unsigned)(usize / 1024),
           (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000));
}

static int preload_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    preload_t pl = { sceIoOpen(pkg_obb_path, SCE_O_RDONLY, 0), 0, 0 };
    if (pl.fd < 0) {
        l_warn("[pcache] cannot open %s", pkg_obb_path);
        return sceKernelExitDeleteThread(0);
    }
    uint64_t t0 = sceKernelGetProcessTimeWide();
    if (!pkg_zip_for_each(pl.fd, NULL, preload_one, &pl)) l_warn("[pcache] %s: cannot read the zip directory", pkg_obb_path);
    sceIoClose(pl.fd);
    l_info("[pcache] preload ready: %d package(s), %u KB, in %u ms", pl.count, (unsigned)(pl.bytes / 1024),
           (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000));
    return sceKernelExitDeleteThread(0);
}

/* ---- the zip entries opened, for the [hitch] breakdown (load_stats.h): which files a battle start reads ---- */
#define OPEN_LOG 32
static struct { uint32_t seq, kb; uint8_t cached; char name[40]; } open_log[OPEN_LOG];
volatile uint32_t dm_open_seq;

static void open_log_add(const char *bn, uint32_t usize) {
    uint32_t k = __sync_add_and_fetch(&dm_open_seq, 1);
    int i = k % OPEN_LOG;
    open_log[i].seq = 0;
    snprintf(open_log[i].name, sizeof(open_log[i].name), "%s", bn);
    open_log[i].kb = usize / 1024;
    pthread_mutex_lock(&pkg_lock);
    pkg_entry_t *ce = pkg_find(bn, usize);
    open_log[i].cached = ce && ce->valid == ce->size;
    pthread_mutex_unlock(&pkg_lock);
    open_log[i].seq = k;
}

void pkg_open_log_since(uint32_t seq, char *out, int n) {
    int o = 0;
    out[0] = 0;
    uint32_t now = dm_open_seq;
    if (now - seq > OPEN_LOG) seq = now - OPEN_LOG;
    for (uint32_t k = seq + 1; k <= now && o < n - 48; k++) {
        int i = k % OPEN_LOG;
        if (open_log[i].seq != k) continue;
        o += snprintf(out + o, n - o, "%s%s %u KB%s", o ? ", " : "", open_log[i].name, (unsigned)open_log[i].kb,
                      open_log[i].cached ? " (cache)" : "");
    }
}

/* ---- engine hooks ---- */
static int (*o_open)(void **, void *, const char *);
static int (*o_read)(void *, char *, int, int);
static void (*o_close)(void *);

static void release(pkg_open_t *o) {   /* lock held */
    pkg_entry_t *e = o->e;
    if (e) {
        e->refs--;
        if (e->valid == e->size && !e->announced && !e->pinned) {
            e->announced = 1;
            if (kept_n++ < 60)
                l_info("[pcache] kept %s (%u KB) after its first load; cache %u KB of %u", e->name,
                       (unsigned)(e->size / 1024), (unsigned)(pkg_used / 1024), (unsigned)(pkg_limit / 1024));
        }
    }
    memset(o, 0, sizeof(*o));
}

static void log_cache_full(const char *bn, uint32_t usize) {   /* lock held */
    uint32_t pinned = 0, busy = 0;
    int nb = 0;
    for (int i = 0; i < pkg_entries_n; i++) {
        if (!pkg_entries[i].data) continue;
        if (pkg_entries[i].pinned) pinned += pkg_entries[i].size;
        else if (pkg_entries[i].refs) {
            busy += pkg_entries[i].size;
            nb++;
        }
    }
    l_warn("[pcache] %s (%u KB) not kept: cache full (%u KB preloaded, %u KB in %d packages the engine still has open, "
           "of %u KB)", bn, (unsigned)(usize / 1024), (unsigned)(pinned / 1024), (unsigned)(busy / 1024), nb,
           (unsigned)(pkg_limit / 1024));
}

static int h_open(void **out, void *de, const char *pw) {
    int r = o_open(out, de, pw);
    if (!r || !out || !*out || !de || !started) return r;
    void *h = *out;
    const char *nm = *(const char **)de;   /* directory_entry: +0 name, +0x20 uncompressed size */
    uint32_t usize = *(const uint32_t *)((const uint8_t *)de + 0x20);
    const char *bn = nm ? pkg_base_name(nm) : "";
    open_log_add(bn, usize);
    pkg_prefetch_note_open(bn, usize);

    pthread_mutex_lock(&pkg_lock);
    int slot = -1;
    for (int i = 0; i < PKG_MAX_OPEN; i++) {
        if (pkg_opened[i].h == h) release(&pkg_opened[i]);   /* a reused handle */
        if (slot < 0 && !pkg_opened[i].h) slot = i;
    }
    if (slot < 0)   /* reuse the slot of a handle that has read everything */
        for (int i = 0; i < PKG_MAX_OPEN && slot < 0; i++)
            if (pkg_opened[i].h && pkg_opened[i].eof) {
                release(&pkg_opened[i]);
                slot = i;
            }
    if (slot < 0 && *bn && keep_fail_n++ < 10) l_warn("[pcache] %s not cached: all %d handle slots are in use", bn, PKG_MAX_OPEN);

    pkg_open_t *fill = NULL;
    if (slot >= 0 && *bn) {
        pkg_entry_t *e = pkg_find(bn, usize);
        int fresh = 0;
        if (!e && usize >= PKG_KEEP_MIN_BYTES && usize <= PKG_KEEP_MAX_BYTES && pkg_keep_name(bn)) {
            if (pkg_make_room(usize)) {
                uint8_t *data = malloc(usize);
                if (data && !(e = pkg_insert(bn, usize, data, 0))) free(data);
                fresh = e != NULL;
            } else if (keep_fail_n++ < 10) {
                log_cache_full(bn, usize);
            }
        }
        if (e) {
            pkg_open_t *o = &pkg_opened[slot];
            memset(o, 0, sizeof(*o));
            o->h = h;
            o->e = e;
            e->refs++;
            e->stamp = ++clock_;
            e->last_us = sceKernelGetProcessTimeWide();
            if (fresh) fill = o;
        }
    }
    pthread_mutex_unlock(&pkg_lock);

    /* Unpack a newly kept package completely right away, through this handle. The engine opens a package two or three
     * times at once; a second handle that got ahead of what the first had filled in would unpack it all again. Filled
     * now, every handle is served from memory and the package is read and unpacked once. */
    if (fill) {
        pkg_entry_t *e = fill->e;
        uint32_t got = 0;
        while (got < e->size) {
            uint32_t want = e->size - got < 65536 ? e->size - got : 65536;
            int n = o_read(h, (char *)e->data, (int)got, (int)want);
            if (n <= 0) break;
            got += (uint32_t)n;
            __sync_synchronize();
            pthread_mutex_lock(&pkg_lock);
            if (got > e->valid) e->valid = got;
            pthread_mutex_unlock(&pkg_lock);
        }
        fill->real = got;   /* the engine's own handle is this far now; reads below `valid` come from memory */
    }
    return r;
}

/* Bring the engine's own handle up to `to` by reading (and dropping) what it would have produced. */
static int catch_up(pkg_open_t *o, uint32_t to) {
    char tmp[8192];
    while (o->real < to) {
        int want = to - o->real < sizeof(tmp) ? (int)(to - o->real) : (int)sizeof(tmp);
        int r = o_read(o->h, tmp, 0, want);
        if (r <= 0) return 0;
        o->real += (uint32_t)r;
    }
    return 1;
}

/* The engine has the whole entry: let go of it (the "kept" message and the reference), keep the slot. */
static void done_with(pkg_open_t *o) {
    pthread_mutex_lock(&pkg_lock);
    if (o->e && !o->eof) {
        pkg_open_t keep = *o;
        release(o);
        o->h = keep.h;
        o->pos = keep.pos;
        o->served = keep.served;
        o->eof = 1;
    }
    pthread_mutex_unlock(&pkg_lock);
}

/* A read from a handle on a cached entry. */
static int read_entry(pkg_open_t *o, pkg_entry_t *e, void *h, char *buf, int off, int len) {
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
            pthread_mutex_lock(&pkg_lock);
            e->hits++;
            pthread_mutex_unlock(&pkg_lock);
            if (hits_n++ < 60)
                l_info("[pcache] %s served from RAM (%u KB%s, use %u)", e->name, (unsigned)(e->size / 1024),
                       valid == e->size ? "" : " partly", (unsigned)e->hits);
        }
        return (int)want;
    }
    /* past what is filled in: the engine's handle has to do it, from where it really is */
    if (o->real < o->pos && !catch_up(o, o->pos)) return -1;
    int r = o_read(h, buf, off, len);
    if (r > 0) {
        uint32_t n = (uint32_t)r;
        if (o->pos + n > e->size) n = e->size - o->pos;
        if (o->pos <= e->valid && o->pos + n > e->valid) {   /* extends the filled-in part */
            memcpy(e->data + o->pos, buf + off, n);
            __sync_synchronize();
            pthread_mutex_lock(&pkg_lock);
            if (o->pos + n > e->valid) e->valid = o->pos + n;
            pthread_mutex_unlock(&pkg_lock);
        }
        o->pos += (uint32_t)r;
        o->real = o->pos;
        if (o->pos >= e->size) done_with(o);
    }
    return r;
}

static int h_read(void *h, char *buf, int off, int len) {
    pkg_open_t *o = NULL;
    pkg_entry_t *e = NULL;
    pthread_mutex_lock(&pkg_lock);
    for (int i = 0; i < PKG_MAX_OPEN && !o; i++)
        if (pkg_opened[i].h == h) o = &pkg_opened[i];
    if (o && o->e && !o->detached) {
        e = o->e;
        e->readers++;
        e->last_us = sceKernelGetProcessTimeWide();
    }
    pthread_mutex_unlock(&pkg_lock);
    if (!o) return o_read(h, buf, off, len);
    if (o->detached) {   /* its entry is gone: the engine's own handle, caught up to where the engine is */
        if (o->eof) return 0;
        if (o->real < o->pos && !catch_up(o, o->pos)) return -1;
        int r = o_read(h, buf, off, len);
        if (r > 0) {
            o->pos += (uint32_t)r;
            o->real = o->pos;
        }
        return r;
    }
    if (!e) return o_read(h, buf, off, len);
    int r = read_entry(o, e, h, buf, off, len);
    __sync_fetch_and_sub(&e->readers, 1);
    return r;
}

static void h_close(void *h) {
    pthread_mutex_lock(&pkg_lock);
    for (int i = 0; i < PKG_MAX_OPEN; i++)
        if (pkg_opened[i].h == h) release(&pkg_opened[i]);
    pthread_mutex_unlock(&pkg_lock);
    o_close(h);
}

/* ---- API ---- */

/* The engine could not get memory (mem_valve.c): drop heap entries no handle is using - kept ones first, then the
 * preloaded ones - until `want` bytes are freed. Returns the bytes freed. */
size_t pkg_cache_release(size_t want) {
    size_t freed = 0;
    pthread_mutex_lock(&pkg_lock);
    for (int pass = 0; pass < 2 && freed < want; pass++)
        for (int i = 0; i < pkg_entries_n && freed < want; i++) {
            pkg_entry_t *x = &pkg_entries[i];
            if (!x->data || x->refs || x->win || x->spill || (pass == 0 && x->pinned)) continue;
            freed += x->size;
            pkg_used -= x->size;
            free(x->data);
            x->data = NULL;
        }
    pthread_mutex_unlock(&pkg_lock);
    return freed;
}

void pkg_cache_install_hooks(void) {
    int n = 0;
    n += ue3_hook("_Z32ZipFS_openFileFromDirectoryEntryPPvP15directory_entryPKc", 0xE3510000, 0x13500000, h_open,
                  (void **)&o_open);
    n += ue3_hook("_Z14ZipFS_readFilePvPKcii", 0xE0811002, 0xE1A02003, h_read, (void **)&o_read);
    n += ue3_hook("_Z15ZipFS_closeFilePv", 0xE59F300C, 0xE92D4010, h_close, (void **)&o_close);
    if (n != 3) l_warn("[pcache] only %d of 3 zip functions hooked: the package cache stays off", n);
    hooks_ready = n == 3;
}

void pkg_cache_start(void) {
    if (!hooks_ready) return;   /* a hook that did go in only passes through: harmless */
    snprintf(pkg_obb_path, sizeof(pkg_obb_path), "%s", DM_OBB_MAIN);
    pkg_limit = (uint32_t)LIMIT_MB << 20;
    __sync_synchronize();
    started = 1;   /* the hooks start working now */
    l_info("[pcache] up to %d MB; preloading %s; enemy and weapon packages kept after their first load", LIMIT_MB, PRELOAD);
    pkg_prefetch_start();
    /* priority 160: just below the engine's threads (159), so unpacking never competes with the game */
    SceUID th = sceKernelCreateThread("dm_pcache", preload_thread, 160, 0x10000, 0, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
}
