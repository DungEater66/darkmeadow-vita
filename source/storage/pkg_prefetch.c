/*
 * Package prefetch: packages read into the package cache before the engine asks for them.
 *
 * Packages loaded for the first time in a session (area art, hot spots, audio, enemies) cost the memory card and
 * inflating at every load wait. Like the texture prefetch, the packages the engine opens are remembered across
 * sessions, in ux0:data/darkmeadow/pkgprefetch.txt: for each package, the next few packages that were opened at least
 * 2 s after it (so not the ones in the same load). When the engine opens a package that has a record, a low-priority
 * thread unpacks its successors into RAM while the game is quiet (it waits while the engine loads, unless the engine
 * is already reading the very package being unpacked, and while a song starts), and the engine's reads are then
 * served from memory as for any cached package.
 *
 * Where they go: enemy and weapon packages (the keep list) into the heap cache, where they stay; everything else into
 * the "window", a block of physically contiguous RAM (WINDOW_MB) that vitaGL leaves to the system for the movie
 * decoder. Window entries the engine has read are dropped first when room is needed, then ones not used for a minute,
 * then ones the engine holds open but has not read for a minute (its handles go on reading the .obb themselves).
 *
 * Movies: the decoder needs the window's memory. Before a movie the window is emptied and the block given back;
 * entries the engine still has open move to the "spill block" (WINDOW_MB in vitaGL's RAM pool, which is otherwise
 * idle) or, if that is full, the heap. After the movie the window is taken again and they move back. The window is
 * not taken before the opening movie has played.
 *
 * Settings app: "Package prefetch" (DM_PACKAGE_PREFETCH).
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <vitaGL.h>

#include "audio/song.h"
#include "config.h"
#include "storage/pkg_cache.h"
#include "storage/pkg_cache_internal.h"
#include "storage/tex_prefetch.h"
#include "util/logger.h"
#include "video/movie.h"

#define WINDOW_MB 24
#define RECORD_FILE DATA_PATH "pkgprefetch.txt"
#define RECORD_TMP DATA_PATH "pkgprefetch.tmp"
#define NAME_LEN 56
#define MAX_SUCC 6
#define MAX_SEQ 768
#define MAX_RECORDS 1024
#define MIN_GAP_US 2000000ull     /* a successor is opened at least this long after (not in the same load) */
#define MAX_GAP_US 300000000ull   /* and within 5 minutes */
#define QUIET_US 500000ull        /* start a package only after loading has been quiet this long */
#define STALE_US 60000000ull      /* a window entry nobody used for this long may be dropped */

typedef struct {
    char name[NAME_LEN];
    uint32_t size;
    uint8_t n;
    char succ[MAX_SUCC][NAME_LEN];
    uint32_t ssize[MAX_SUCC];
} record_t;

typedef struct { char name[NAME_LEN]; uint32_t lho, csize, usize; uint16_t method; } zip_ent_t;

static int enabled;

/* records from earlier sessions, and this session's order */
static record_t *records;
static int records_n;
static struct { char name[NAME_LEN]; uint32_t size; uint64_t t; } seq[MAX_SEQ];
static int seq_n, seq_dirty;
static pthread_mutex_t seq_lock = PTHREAD_MUTEX_INITIALIZER;   /* seq, the trigger */
static char trig_name[NAME_LEN];
static uint32_t trig_size;
static volatile uint32_t trig_seq;
static volatile int retrigger;   /* run the last package's list again (after a movie emptied the window) */

static int64_t obb_size;
static zip_ent_t *zip;
static int zip_n;

/* the window (guarded by pkg_lock) */
static SceUID win_blk = -1;
static uint8_t *win_base;
static uint32_t win_size, win_used;
static volatile int win_suspended, win_filling;

/* the spill block */
static SceClibMspace spill_space;
static uint32_t spill_used;

static unsigned done_n, skip_n;

/* ---- recording ---- */
void pkg_prefetch_note_open(const char *bn, uint32_t usize) {
    if (!enabled || !*bn || usize < PKG_KEEP_MIN_BYTES) return;
    size_t l = strlen(bn);
    if (l < 5 || l >= NAME_LEN || strcasecmp(bn + l - 4, ".xxx")) return;
    pthread_mutex_lock(&pkg_lock);
    pkg_entry_t *e = pkg_find(bn, usize);
    int preloaded = e && e->pinned;
    pthread_mutex_unlock(&pkg_lock);
    if (preloaded) return;
    pthread_mutex_lock(&seq_lock);
    int seen = 0;
    for (int i = 0; i < seq_n && !seen; i++) seen = seq[i].size == usize && !strcasecmp(seq[i].name, bn);
    if (!seen && seq_n < MAX_SEQ) {
        snprintf(seq[seq_n].name, NAME_LEN, "%s", bn);
        seq[seq_n].size = usize;
        seq[seq_n].t = sceKernelGetProcessTimeWide();
        seq_n++;
        seq_dirty = 1;
    }
    if (strcasecmp(trig_name, bn) || trig_size != usize) {
        snprintf(trig_name, NAME_LEN, "%s", bn);
        trig_size = usize;
        trig_seq++;
    }
    pthread_mutex_unlock(&seq_lock);
}

static record_t *record_find(const char *bn, uint32_t size) {
    for (int i = 0; i < records_n; i++)
        if (records[i].size == size && !strcasecmp(records[i].name, bn)) return &records[i];
    return NULL;
}

static void records_load(void) {
    FILE *f = fopen(RECORD_FILE, "r");
    if (!f) {
        l_info("[pkgpf] no recorded packages yet (%s): recording this session", RECORD_FILE);
        return;
    }
    char line[1024];
    long long sz = 0;
    if (!fgets(line, sizeof(line), f) || sscanf(line, "# pkgprefetch 1 obb %lld", &sz) != 1 || sz != obb_size) {
        fclose(f);
        l_info("[pkgpf] %s is for another game version: starting over", RECORD_FILE);
        return;
    }
    while (fgets(line, sizeof(line), f) && records_n < MAX_RECORDS) {
        record_t r;
        memset(&r, 0, sizeof(r));
        char *save = NULL, *t = strtok_r(line, " \t\r\n", &save);
        if (!t || strlen(t) >= NAME_LEN) continue;
        snprintf(r.name, NAME_LEN, "%s", t);
        if (!(t = strtok_r(NULL, " \t\r\n", &save))) continue;
        r.size = (uint32_t)strtoul(t, NULL, 10);
        while (r.n < MAX_SUCC) {
            char *a = strtok_r(NULL, " \t\r\n", &save), *b = a ? strtok_r(NULL, " \t\r\n", &save) : NULL;
            if (!a || !b || strlen(a) >= NAME_LEN) break;
            snprintf(r.succ[r.n], NAME_LEN, "%s", a);
            r.ssize[r.n] = (uint32_t)strtoul(b, NULL, 10);
            r.n++;
        }
        if (r.n) records[records_n++] = r;
    }
    fclose(f);
    l_info("[pkgpf] %d packages with recorded successors loaded from %s", records_n, RECORD_FILE);
}

/* Fold this session's order into the records and write them out. */
static void records_save(void) {
    pthread_mutex_lock(&seq_lock);
    int n = seq_n;
    seq_dirty = 0;
    pthread_mutex_unlock(&seq_lock);
    int changed = 0;
    for (int i = 0; i < n; i++) {
        record_t r;
        memset(&r, 0, sizeof(r));
        memcpy(r.name, seq[i].name, NAME_LEN);
        r.size = seq[i].size;
        for (int j = i + 1; j < n && r.n < MAX_SUCC; j++) {
            uint64_t d = seq[j].t - seq[i].t;
            if (d < MIN_GAP_US) continue;
            if (d > MAX_GAP_US) break;
            snprintf(r.succ[r.n], NAME_LEN, "%s", seq[j].name);
            r.ssize[r.n] = seq[j].size;
            r.n++;
        }
        if (!r.n) continue;
        record_t *old = record_find(r.name, r.size);
        if (old) {
            if (old->n > r.n) continue;   /* the session ended sooner after it than the recorded one: keep that */
            if (!memcmp(old, &r, sizeof(r))) continue;
            *old = r;
        } else if (records_n < MAX_RECORDS) {
            records[records_n++] = r;
        } else {
            continue;
        }
        changed++;
    }
    if (!changed) return;
    FILE *f = fopen(RECORD_TMP, "w");
    if (!f) return;
    fprintf(f, "# pkgprefetch 1 obb %lld\n", (long long)obb_size);
    for (int i = 0; i < records_n; i++) {
        fprintf(f, "%s %u", records[i].name, (unsigned)records[i].size);
        for (int k = 0; k < records[i].n; k++) fprintf(f, " %s %u", records[i].succ[k], (unsigned)records[i].ssize[k]);
        fputc('\n', f);
    }
    fclose(f);
    sceIoRemove(RECORD_FILE);
    sceIoRename(RECORD_TMP, RECORD_FILE);
    l_info("[pkgpf] %d package records saved (%d new or changed)", records_n, changed);
}

/* ---- the .obb's packages ---- */
static void zip_add(void *ctx, const char *bn, int method, uint32_t csize, uint32_t usize, uint32_t lho) {
    (void)ctx;
    size_t l = strlen(bn);
    if (l <= 4 || l >= NAME_LEN || strcasecmp(bn + l - 4, ".xxx")) return;
    zip_ent_t *z = realloc(zip, (size_t)(zip_n + 1) * sizeof(*z));
    if (!z) return;
    zip = z;
    snprintf(zip[zip_n].name, NAME_LEN, "%s", bn);
    zip[zip_n].method = (uint16_t)method;
    zip[zip_n].csize = csize;
    zip[zip_n].usize = usize;
    zip[zip_n].lho = lho;
    zip_n++;
}

static const zip_ent_t *zip_find(const char *bn, uint32_t usize) {
    for (int i = 0; i < zip_n; i++)
        if (zip[i].usize == usize && !strcasecmp(zip[i].name, bn)) return &zip[i];
    return NULL;
}

/* ---- the window (pkg_lock held) ---- */

/* first fit */
static uint8_t *win_alloc(uint32_t n) {
    if (!win_base || win_suspended) return NULL;
    n = (n + 63) & ~63u;
    uint32_t at = 0;
    for (;;) {
        uint32_t next_end = 0;
        int clash = 0;
        for (int i = 0; i < pkg_entries_n; i++) {
            pkg_entry_t *e = &pkg_entries[i];
            if (!e->data || !e->win) continue;
            uint32_t s0 = (uint32_t)(e->data - win_base), s1 = s0 + ((e->size + 63) & ~63u);
            if (at < s1 && at + n > s0) {
                clash = 1;
                if (s1 > next_end) next_end = s1;
            }
        }
        if (!clash) return at + n <= win_size ? win_base + at : NULL;
        at = next_end;
        if (at + n > win_size) return NULL;
    }
}

/* refs == 0 */
static void entry_drop(pkg_entry_t *e) {
    if (e->spill) {
        spill_used -= e->size;
        sceClibMspaceFree(spill_space, e->data);
        e->spill = 0;
    } else if (e->win) {
        win_used -= e->size;
    } else {
        pkg_used -= e->size;
        free(e->data);
    }
    e->data = NULL;
}

/* An entry the engine holds open but has not read for STALE_US: its handles go on reading the .obb themselves.
 * Otherwise area packages the engine keeps open for the rest of the session fill the window. */
static pkg_entry_t *detach_idle(uint64_t now) {
    pkg_entry_t *v = NULL;
    for (int i = 0; i < pkg_entries_n; i++) {
        pkg_entry_t *e = &pkg_entries[i];
        if (!e->data || !e->win || !e->refs || e->readers || now - e->last_us < STALE_US) continue;
        if (!v || e->last_us < v->last_us) v = e;
    }
    if (!v) return NULL;
    int handles = 0;
    for (int i = 0; i < PKG_MAX_OPEN; i++)
        if (pkg_opened[i].h && pkg_opened[i].e == v) {
            pkg_opened[i].e = NULL;
            pkg_opened[i].detached = 1;
            handles++;
        }
    static int logged;
    if (logged++ < 60)
        l_info("[pkgpf] %s (%u KB) let go: open in the engine (%d handle(s)) but not read for %u s", v->name,
               (unsigned)(v->size / 1024), handles, (unsigned)((now - v->last_us) / 1000000));
    v->refs = 0;
    return v;
}

/* Room in the window: read entries first (oldest), then stale unread ones, then idle open ones. */
static uint8_t *win_get(uint32_t n) {
    uint8_t *p;
    while (!(p = win_alloc(n))) {
        pkg_entry_t *v = NULL;
        uint64_t now = sceKernelGetProcessTimeWide();
        for (int i = 0; i < pkg_entries_n; i++) {
            pkg_entry_t *e = &pkg_entries[i];
            if (!e->data || !e->win || e->refs) continue;
            if (!e->hits && now - e->t_fill < STALE_US) continue;
            if (!v || (e->hits > 0) > (v->hits > 0) || ((e->hits > 0) == (v->hits > 0) && e->stamp < v->stamp)) v = e;
        }
        if (!v && !(v = detach_idle(now))) return NULL;
        entry_drop(v);
    }
    return p;
}

static void win_take(void) {
    if (win_blk >= 0 || win_suspended || movie_opening_pending()) return;
    uint32_t sz = (uint32_t)WINDOW_MB << 20;
    SceUID b = sceKernelAllocMemBlock("dm_pkgwin", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, sz, NULL);
    if (b < 0) {
        static int logged;
        if (!logged++)
            l_warn("[pkgpf] no %d MB physically contiguous block (0x%08x): prefetching enemy packages only", WINDOW_MB,
                   (unsigned)b);
        return;
    }
    void *base = NULL;
    sceKernelGetMemBlockBase(b, &base);
    pthread_mutex_lock(&pkg_lock);
    win_blk = b;
    win_base = base;
    win_size = sz;
    win_used = 0;
    pthread_mutex_unlock(&pkg_lock);
    static int logged;
    if (logged++ < 10) l_info("[pkgpf] %d MB window for prefetched packages (physically contiguous RAM)", WINDOW_MB);
}

/* Window entries the engine still has open move to the spill block (or the heap) so the window can be given back. */
static void move_open_entries_out(void) {
    pthread_mutex_lock(&pkg_lock);
    int moved = 0, to_heap = 0;
    uint32_t kb = 0;
    for (int i = 0; i < pkg_entries_n; i++) {
        pkg_entry_t *e = &pkg_entries[i];
        if (!e->data || !e->win || !e->refs) continue;
        uint8_t *copy = spill_space ? sceClibMspaceMalloc(spill_space, e->size) : NULL;
        int spilled = copy != NULL;
        if (!copy) copy = malloc(e->size);
        if (!copy) continue;
        memcpy(copy, e->data, e->valid);
        __sync_synchronize();
        e->data = copy;
        e->win = 0;
        win_used -= e->size;
        if (spilled) {
            e->spill = 1;
            spill_used += e->size;
        } else {
            pkg_used += e->size;
            to_heap++;
        }
        moved++;
        kb += e->size / 1024;
        l_info("[pkgpf]   still open for the movie: %s (%u KB) -> %s", e->name, (unsigned)(e->size / 1024),
               spilled ? "spill block" : "heap");
    }
    pthread_mutex_unlock(&pkg_lock);
    if (moved) {
        l_info("[pkgpf] %d package(s), %u KB, still open moved out of the window for the movie (%d to the heap)", moved,
               (unsigned)kb, to_heap);
        sceKernelDelayThread(50 * 1000);   /* a read still copying from the old place finishes first */
    }
}

void pkg_cache_movie(int starting) {
    if (!starting) {
        win_suspended = 0;
        retrigger = 1;
        return;
    }
    win_suspended = 1;
    for (int i = 0; i < 150 && win_filling; i++) sceKernelDelayThread(10 * 1000);
    for (int tries = 0; tries < 100; tries++) {
        if (tries == 50) move_open_entries_out();   /* still open in the engine after 0.5 s */
        int busy = 0;
        pthread_mutex_lock(&pkg_lock);
        for (int i = 0; i < pkg_entries_n; i++) {
            pkg_entry_t *e = &pkg_entries[i];
            if (!e->data || !e->win) continue;
            if (e->refs) busy++;
            else entry_drop(e);
        }
        SceUID b = -1;
        if (!busy && !win_filling && win_blk >= 0) {
            b = win_blk;
            win_blk = -1;
            win_base = NULL;
            win_size = win_used = 0;
        }
        pthread_mutex_unlock(&pkg_lock);
        if (b >= 0) {
            sceKernelFreeMemBlock(b);
            l_info("[pkgpf] window given back for the movie");
            return;
        }
        if (win_blk < 0) return;
        sceKernelDelayThread(10 * 1000);
    }
    l_warn("[pkgpf] the window is still in use: the movie has less memory");
}

/* From gl_init, before the rendering thread exists (nothing else uses vitaGL's allocator then). */
void pkg_cache_attach_spill(void) {
    if (!enabled || spill_space) return;
    uint32_t sz = (uint32_t)WINDOW_MB << 20;
    void *blk = vglAlloc(sz + (sz >> 4), VGL_MEM_RAM);
    if (blk) spill_space = sceClibMspaceCreate(blk, sz + (sz >> 4));
    if (spill_space)
        l_info("[pkgpf] %u MB spill block in vitaGL's RAM pool (vitaGL RAM free now %u MB)", (unsigned)(sz >> 20),
               (unsigned)(vglMemFree(VGL_MEM_RAM) >> 20));
    else
        l_warn("[pkgpf] no spill block in vitaGL's RAM pool: packages open during a movie go to the heap");
}

/* After a movie: spilled entries back into the window (or dropped, if the engine has let go of them). */
static void spill_return(void) {
    if (!spill_used || !win_base) return;
    int back = 0, dropped = 0;
    uint8_t *old[PKG_MAX_ENTRIES];
    int nold = 0;
    pthread_mutex_lock(&pkg_lock);
    for (int i = 0; i < pkg_entries_n; i++) {
        pkg_entry_t *e = &pkg_entries[i];
        if (!e->data || !e->spill) continue;
        if (!e->refs) {
            entry_drop(e);
            dropped++;
            continue;
        }
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
    pthread_mutex_unlock(&pkg_lock);
    if (nold) {
        sceKernelDelayThread(50 * 1000);   /* a read still copying from the old place finishes first */
        for (int i = 0; i < nold; i++) sceClibMspaceFree(spill_space, old[i]);
    }
    if (back || dropped)
        l_info("[pkgpf] after the movie: %d package(s) back in the window, %d dropped (no longer open)", back, dropped);
}

/* The heap cache over its limit (entries moved there while open): drop the ones the engine has let go of since. */
static void heap_trim(void) {
    pthread_mutex_lock(&pkg_lock);
    uint32_t before = pkg_used;
    if (pkg_used > pkg_limit) pkg_make_room(0);
    uint32_t after = pkg_used;
    pthread_mutex_unlock(&pkg_lock);
    if (after < before) {
        static int logged;
        if (logged++ < 40)
            l_info("[pcache] over the limit: dropped %u KB no longer open; cache %u KB of %u", (unsigned)((before - after) / 1024),
                   (unsigned)(after / 1024), (unsigned)(pkg_limit / 1024));
    }
}

/* ---- fetching ---- */

/* Unpack straight into an entry, publishing progress so the engine can be served while it fills. */
static int unpack_into(SceUID fd, const zip_ent_t *c, pkg_entry_t *e) {
    int64_t start = pkg_zip_data_start(fd, c->lho);
    if (start < 0) return 0;
    enum { CHUNK = 128 * 1024 };
    if (c->method == 0) {
        uint32_t done = 0;
        while (done < c->usize) {
            while (song_io_busy && !win_suspended) sceKernelDelayThread(20 * 1000);
            uint32_t want = c->usize - done < CHUNK ? c->usize - done : CHUNK;
            if (sceIoPread(fd, e->data + done, want, start + done) != (int)want) return 0;
            done += want;
            __sync_synchronize();
            pthread_mutex_lock(&pkg_lock);
            if (done > e->valid) e->valid = done;
            pthread_mutex_unlock(&pkg_lock);
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
        /* the engine started loading and is not waiting for this one, or a song starts: let it have the card */
        while (((texpf_busy() && e->refs == 1) || song_io_busy) && !win_suspended) sceKernelDelayThread(20 * 1000);
        if (win_suspended && e->win) {
            r = Z_DATA_ERROR;
            break;
        }
        int want = c->csize - done < CHUNK ? (int)(c->csize - done) : CHUNK;
        if (sceIoPread(fd, in, (SceSize)want, start + done) != want) {
            r = Z_DATA_ERROR;
            break;
        }
        done += want;
        z.next_in = in;
        z.avail_in = want;
        r = inflate(&z, Z_NO_FLUSH);
        if (r == Z_BUF_ERROR && z.avail_in == 0) r = Z_OK;
        __sync_synchronize();
        pthread_mutex_lock(&pkg_lock);
        if (z.total_out > e->valid) e->valid = (uint32_t)z.total_out;
        pthread_mutex_unlock(&pkg_lock);
    }
    int ok = (r == Z_STREAM_END || r == Z_OK) && z.total_out == c->usize;
    inflateEnd(&z);
    return ok;
}

static void log_no_room(const char *bn, uint32_t usize) {
    if (skip_n++ >= 20) return;
    char held[400];
    int o = 0;
    held[0] = 0;
    pthread_mutex_lock(&pkg_lock);   /* what fills the window: packages the engine still has open */
    for (int i = 0; i < pkg_entries_n && o < (int)sizeof(held) - 64; i++)
        if (pkg_entries[i].data && pkg_entries[i].win && pkg_entries[i].refs)
            o += snprintf(held + o, sizeof(held) - o, "%s%s %u KB", o ? ", " : "", pkg_entries[i].name,
                          (unsigned)(pkg_entries[i].size / 1024));
    pthread_mutex_unlock(&pkg_lock);
    l_info("[pkgpf] %s (%u KB) not prefetched: no room; open in the engine: %s", bn, (unsigned)(usize / 1024),
           o ? held : "none");
}

static void fetch(SceUID fd, const char *bn, uint32_t usize, const char *after) {
    const zip_ent_t *c = zip_find(bn, usize);
    if (!c) return;
    pthread_mutex_lock(&pkg_lock);
    if (pkg_find(bn, usize)) {
        pthread_mutex_unlock(&pkg_lock);
        return;
    }
    int heap = pkg_keep_name(bn) && usize <= PKG_KEEP_MAX_BYTES;
    pkg_entry_t *e = NULL;
    uint8_t *data = NULL;
    if (heap) {
        if (pkg_make_room(usize) && (data = malloc(usize)) && !(e = pkg_insert(bn, usize, data, 0))) free(data);
    } else if (usize <= win_size / 2 && (data = win_get(usize))) {
        if ((e = pkg_insert(bn, usize, data, 0))) {
            pkg_used -= usize;   /* in the window, not the heap */
            win_used += usize;
            e->win = 1;
        }
    }
    if (e) {
        e->prefetched = 1;
        e->announced = 1;
        e->refs = 1;   /* ours while it fills */
        if (e->win) win_filling = 1;
    }
    pthread_mutex_unlock(&pkg_lock);
    if (!e) {
        log_no_room(bn, usize);
        return;
    }
    uint64_t t0 = sceKernelGetProcessTimeWide();
    int ok = unpack_into(fd, c, e);
    pthread_mutex_lock(&pkg_lock);
    e->refs--;
    e->t_fill = sceKernelGetProcessTimeWide();
    if (e->win) win_filling = 0;
    if (!ok) {
        e->valid = 0;
        if (!e->refs) entry_drop(e);   /* else: the engine's handle reads past `valid` itself, and the entry goes later */
    }
    uint32_t wu = win_used, hu = pkg_used;
    pthread_mutex_unlock(&pkg_lock);
    if (done_n++ < 200)
        l_info("[pkgpf] %s %s (%u KB, after %s) in %u ms%s; window %u/%u KB, heap cache %u KB",
               ok ? "prefetched" : "gave up on", bn, (unsigned)(usize / 1024), after,
               (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000), heap ? " into the heap cache" : "",
               (unsigned)(wu / 1024), (unsigned)(win_size / 1024), (unsigned)(hu / 1024));
}

static int prefetch_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUID fd = sceIoOpen(pkg_obb_path, SCE_O_RDONLY, 0);
    records = calloc(MAX_RECORDS, sizeof(record_t));
    if (fd < 0 || !records || !pkg_zip_for_each(fd, &obb_size, zip_add, NULL)) {
        l_warn("[pkgpf] cannot read %s: package prefetch off", pkg_obb_path);
        return sceKernelExitDeleteThread(0);
    }
    records_load();
    uint32_t seen = 0;
    uint64_t last_save = sceKernelGetProcessTimeWide(), last_trim = 0;
    for (;;) {
        sceKernelDelayThread(100 * 1000);
        uint64_t now = sceKernelGetProcessTimeWide();
        if (seq_dirty && now - last_save > 60000000ull && !texpf_busy()) {
            records_save();
            last_save = now;
        }
        if (now - last_trim > 2000000ull) {
            last_trim = now;
            heap_trim();
            if (!win_suspended && spill_used) {
                win_take();
                spill_return();
            }
        }
        if (retrigger) {
            retrigger = 0;
            seen = trig_seq - 1;
        }
        if (trig_seq == seen) continue;

        pthread_mutex_lock(&seq_lock);
        seen = trig_seq;
        char trig[NAME_LEN];
        snprintf(trig, sizeof(trig), "%s", trig_name);
        uint32_t ts = trig_size;
        pthread_mutex_unlock(&seq_lock);
        record_t *r = record_find(trig, ts);
        if (!r) continue;
        record_t rc = *r;
        /* wait for the load that opened it to finish */
        while (texpf_busy() || texpf_quiet_us() < QUIET_US || song_io_busy) sceKernelDelayThread(50 * 1000);
        if (!win_suspended) win_take();
        for (int k = 0; k < rc.n && trig_seq == seen; k++)   /* the engine moved on: follow the newer package */
            fetch(fd, rc.succ[k], rc.ssize[k], trig);
    }
    return 0;
}

void pkg_prefetch_start(void) {
    enabled = config_bool("DM_PACKAGE_PREFETCH", 1);
    if (!enabled) {
        l_info("[pkgpf] package prefetch off");
        return;
    }
    SceUID th = sceKernelCreateThread("dm_pkgpf", prefetch_thread, 160, 0x10000, 0, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
}
