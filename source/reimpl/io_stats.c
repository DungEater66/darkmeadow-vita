#include "reimpl/io_stats.h"
#include "reimpl/obb_stream.h"
#include "reimpl/tex_cache.h"
#include "reimpl/tex_prefetch.h"
#include "reimpl/load_stats.h"
#include "reimpl/save_behind.h"
#include <psp2/kernel/processmgr.h>
#include <pthread.h>
#include <sys/stat.h>

#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "utils/logger.h"
#include "reimpl/pthr.h"

static volatile unsigned io_calls;
static volatile uint64_t io_bytes;
static volatile uintptr_t io_last_caller;

void io_stats_note(size_t bytes, void *caller) {
    __sync_fetch_and_add(&io_calls, 1);
    io_bytes += bytes;   /* approximate under contention; only used for a rate in the log */
    io_last_caller = (uintptr_t)caller;
}

void io_stats_collect(unsigned *calls, uint64_t *bytes, uintptr_t *last_caller) {
    *calls = __sync_lock_test_and_set(&io_calls, 0);
    *bytes = io_bytes;
    io_bytes = 0;
    *last_caller = io_last_caller;
}

/* ---- reads from the expansion files through open()/read() (the engine reads its texture caches, the .tfc files in
 * the patch .obb, this way: open the .obb, lseek to the data, read). Logged so that a texture whose data comes back
 * short or all zeros (zero PVRTC data decodes as solid black) shows up in log.txt. */
#define OBB_FD_MAX 256
static uint8_t obb_fd[OBB_FD_MAX];          /* 0 = not an .obb, else 1 + index into obb_paths */
static char obb_paths[4][256];
static volatile uint64_t tex_read_us, tex_read_bytes;   /* reads through the engine's own open() of an .obb: the texture caches */
static volatile uint64_t tex_cached_bytes;             /* r57: of those, served by the texture-data cache */
static volatile uint32_t tex_reads, obb_opens, obb_opens_real;

/* r53: how much texture-cache data is read again (would a RAM cache for it help?). One bit per 128-byte chunk of the
 * first two .obb files (256 MB each): a read touching a chunk seen before counts as a re-read. Chunks this small keep a
 * texture read in several consecutive pieces from counting as re-reading its own neighbours. */
#define CHUNK_SHIFT 7
#define SEEN_CHUNKS (1 << 21)
static uint8_t seen_chunks[2][SEEN_CHUNKS / 8];
static volatile uint64_t tex_reread_bytes, tex_unique_chunks;
static void note_tex_range(int obb, int64_t at, ssize_t n) {
    if (obb < 0 || obb > 1 || at < 0 || n <= 0) return;
    int64_t end = at + n;
    for (int64_t c = at >> CHUNK_SHIFT; c <= (end - 1) >> CHUNK_SHIFT; c++) {
        if (c >= SEEN_CHUNKS) return;
        int64_t cs = c << CHUNK_SHIFT, ce = cs + (1 << CHUNK_SHIFT);
        int64_t ov = (end < ce ? end : ce) - (at > cs ? at : cs);
        uint8_t *b = &seen_chunks[obb][c >> 3], m = (uint8_t)(1u << (c & 7));
        if (*b & m) tex_reread_bytes += (uint64_t)ov;
        else { *b |= m; tex_unique_chunks++; }
    }
}

static pthread_mutex_t paths_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *base_name(const char *p) {
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

/* The index of an .obb in obb_paths (registered on first sight), -1 if all 4 slots are taken. Matched by file name,
 * so "ux0:data/darkmeadow/x.obb" and "ux0:data/darkmeadow//x.obb" are the same file. The index is the texture-data
 * cache's key, so it must stay the same for the whole session. */
int io_obb_index(const char *path) {
    if (!path) return -1;
    int v = -1;
    pthread_mutex_lock(&paths_lock);
    for (int i = 0; i < 4 && v < 0; i++) {
        if (!obb_paths[i][0]) snprintf(obb_paths[i], sizeof(obb_paths[i]), "%s", path);
        if (!strcmp(base_name(obb_paths[i]), base_name(path))) v = i;
    }
    pthread_mutex_unlock(&paths_lock);
    return v;
}

const char *io_obb_path(int i) { return i >= 0 && i < 4 && obb_paths[i][0] ? obb_paths[i] : NULL; }

void io_stats_track_obb(int fd, const char *path) {
    if (fd < 0 || fd >= OBB_FD_MAX) return;
    uint8_t v = 0;
    if (path) {
        int i = io_obb_index(path);
        v = i >= 0 ? (uint8_t)(i + 1) : 255;   /* 255: tracked, but no read-ahead */
    }
    obb_fd[fd] = v;
}

static int all_zero(const uint8_t *p, ssize_t n) {
    for (ssize_t i = 0; i < n; i++) if (p[i]) return 0;
    return 1;
}

static ssize_t vfd_read(int slot, void *buf, size_t n, void *caller);
static int vfd_slot(int fd);

ssize_t read_counted(int fd, void *buf, size_t n) {
    int vs = vfd_slot(fd);
    if (vs >= 0) return vfd_read(vs, buf, n, __builtin_return_address(0));
    if (save_behind_is(fd)) return save_behind_read(fd, buf, n);   /* r73 */
    off_t at = -1;
    int tracked = fd >= 0 && fd < OBB_FD_MAX && obb_fd[fd];
    if (tracked) at = lseek(fd, 0, SEEK_CUR);
    uint64_t t0 = tracked ? sceKernelGetProcessTimeWide() : 0;
    ssize_t r = -1;
    int via_ra = 0, via_cache = 0;
    int obb_i = tracked && obb_fd[fd] != 255 ? obb_fd[fd] - 1 : -1;
    /* r54: a read the engine has made before comes from the texture-data cache (tex_cache.c) */
    if (obb_i >= 0 && at >= 0 && tex_cache_get(obb_i, (int64_t)at, buf, n)) {
        r = (ssize_t)n;
        via_cache = via_ra = 1;
        lseek(fd, at + (off_t)n, SEEK_SET);
    }
    /* r51: small reads of the texture caches go through the .obb's shared read-ahead blocks (obb_stream.c) - the
     * engine opens the patch .obb for every texture and reads it in pieces, at 2-4 MB/s */
    if (!via_ra && tracked && obb_fd[fd] != 255 && at >= 0) {
        int rr = obb_ra_pread(obb_paths[obb_fd[fd] - 1], buf, (int)n, (int64_t)at);
        if (rr >= 0) {
            r = rr;
            via_ra = 1;
            lseek(fd, at + rr, SEEK_SET);   /* keep the descriptor where a real read would have left it */
        }
    }
    if (!via_ra) r = read(fd, buf, n);
    if (!via_cache && obb_i >= 0 && at >= 0 && r == (ssize_t)n) tex_cache_put(obb_i, (int64_t)at, buf, n);
    if (obb_i >= 0 && at >= 0 && r == (ssize_t)n) texpf_note_read(obb_i, (int64_t)at, (uint32_t)n);
    if (tracked) {
        __sync_fetch_and_add(&tex_read_us, sceKernelGetProcessTimeWide() - t0);
        __sync_fetch_and_add(&tex_reads, 1);
        if (via_cache) __sync_fetch_and_add(&tex_cached_bytes, (uint64_t)n);
        if (r > 0) {
            __sync_fetch_and_add(&tex_read_bytes, (uint64_t)r);
            if (obb_fd[fd] != 255) note_tex_range(obb_fd[fd] - 1, (int64_t)at, r);
        }
    }
    io_stats_note(r > 0 ? (size_t)r : 0, __builtin_return_address(0));
    if (tracked) {
        static int logged, bad;
        int zero = r > 0 && all_zero((const uint8_t *)buf, r);
        if (r != (ssize_t)n || zero) {
            if (bad++ < 60)
                l_warn("[obb read] fd %d at %ld: asked %u, got %d%s (from libUnrealEngine3.so+0x%x)", fd, (long)at,
                       (unsigned)n, (int)r, zero ? ", ALL ZEROS" : "",
                       (unsigned)pthr_engine_offset((uintptr_t)__builtin_return_address(0)));
        } else if (logged++ < 120)
            l_debug("[obb read] fd %d at %ld: %u bytes", fd, (long)at, (unsigned)n);
    }
    return r;
}


/* ---- r57: the engine's open() of an .obb without a trip to the memory card ----
 * The engine opens the patch .obb again for every texture it reads (326 times in a floor-2 run, each preceded by an
 * opendir), reads a few pieces with lseek()/read() and closes it. Here open() of an .obb hands back a virtual
 * descriptor (VFD_BASE + slot) that only keeps a position; its reads go to the texture-data cache, the shared
 * read-ahead blocks or one positional read on the descriptor obb_stream.c keeps open for the whole session, and
 * close() is free. lseek, read, fstat, dup and close know these descriptors (and fdopen gets a real stream).
 * DM_OBB_KEEP_OPEN 0 in config.txt goes back to real descriptors. */
#define VFD_BASE 0x4000
#define VFD_MAX 512
typedef struct { uint8_t used, obb; int64_t pos; } vfd_t;
static vfd_t vfds[VFD_MAX];
static pthread_mutex_t vfd_lock = PTHREAD_MUTEX_INITIALIZER;
static int keep_open = 1;

void io_set_keep_obb_open(int on) { keep_open = on; }

static int vfd_slot(int fd) {
    int s = fd - VFD_BASE;
    return s >= 0 && s < VFD_MAX && vfds[s].used ? s : -1;
}

int obb_vfd_is(int fd) { return vfd_slot(fd) >= 0; }

static int vfd_alloc(int obb, int64_t pos) {
    int r = -1;
    pthread_mutex_lock(&vfd_lock);
    for (int i = 0; i < VFD_MAX; i++)
        if (!vfds[i].used) { vfds[i].used = 1; vfds[i].obb = (uint8_t)obb; vfds[i].pos = pos; r = VFD_BASE + i; break; }
    pthread_mutex_unlock(&vfd_lock);
    return r;
}

/* -1 = not handled here (open it for real) */
int obb_vfd_open(const char *path) {
    __sync_fetch_and_add(&obb_opens, 1);
    if (!keep_open) { __sync_fetch_and_add(&obb_opens_real, 1); return -1; }
    int i = io_obb_index(path);
    if (i < 0 || obb_shared_size(obb_paths[i]) < 0) { __sync_fetch_and_add(&obb_opens_real, 1); return -1; }
    int fd = vfd_alloc(i, 0);
    if (fd < 0) __sync_fetch_and_add(&obb_opens_real, 1);
    return fd;
}

void io_count_real_obb_open(void) {
    __sync_fetch_and_add(&obb_opens, 1);
    __sync_fetch_and_add(&obb_opens_real, 1);
}

int obb_vfd_close(int fd) {
    int s = vfd_slot(fd);
    if (s < 0) return -1;
    vfds[s].used = 0;
    return 0;
}

int obb_vfd_dup(int fd) {
    int s = vfd_slot(fd);
    return s < 0 ? -1 : vfd_alloc(vfds[s].obb, vfds[s].pos);
}

int64_t obb_vfd_lseek(int fd, int64_t off, int whence) {
    int s = vfd_slot(fd);
    if (s < 0) return -1;
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? vfds[s].pos : obb_shared_size(obb_paths[vfds[s].obb]);
    if (base < 0 || base + off < 0) { errno = EINVAL; return -1; }
    vfds[s].pos = base + off;
    return vfds[s].pos;
}

int obb_vfd_fstat(int fd, struct stat *st) {
    int s = vfd_slot(fd);
    if (s < 0) return -1;
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0444;
    st->st_size = (off_t)obb_shared_size(obb_paths[vfds[s].obb]);
    st->st_blksize = 4096;
    return 0;
}

/* the path and position, for fdopen() of a virtual descriptor */
const char *obb_vfd_path(int fd, int64_t *pos) {
    int s = vfd_slot(fd);
    if (s < 0) return NULL;
    if (pos) *pos = vfds[s].pos;
    return obb_paths[vfds[s].obb];
}

static ssize_t vfd_read(int slot, void *buf, size_t n, void *caller) {
    vfd_t *v = &vfds[slot];
    int obb_i = v->obb;
    int64_t at = v->pos;
    const char *path = obb_paths[obb_i];
    uint64_t t0 = sceKernelGetProcessTimeWide();
    ssize_t r;
    int via_cache = 0;
    if (tex_cache_get(obb_i, at, buf, n)) {
        r = (ssize_t)n;
        via_cache = 1;
    } else {
        r = obb_ra_pread(path, buf, (int)n, at);
        if (r < 0) r = obb_shared_pread(path, buf, (int)n, at);
        if (r == (ssize_t)n) tex_cache_put(obb_i, at, buf, n);
    }
    if (r == (ssize_t)n) texpf_note_read(obb_i, at, (uint32_t)n);   /* cache hits too: the floor it was used on counts */
    if (r > 0) v->pos = at + r;
    __sync_fetch_and_add(&tex_read_us, sceKernelGetProcessTimeWide() - t0);
    __sync_fetch_and_add(&tex_reads, 1);
    if (via_cache) __sync_fetch_and_add(&tex_cached_bytes, (uint64_t)n);
    if (r > 0) {
        __sync_fetch_and_add(&tex_read_bytes, (uint64_t)r);
        note_tex_range(obb_i, at, r);
    }
    io_stats_note(r > 0 ? (size_t)r : 0, caller);
    static int logged, bad;
    int zero = r > 0 && all_zero((const uint8_t *)buf, r);
    if (r != (ssize_t)n || zero) {
        if (bad++ < 60)
            l_warn("[obb read] virtual fd %d at %ld: asked %u, got %d%s (from libUnrealEngine3.so+0x%x)", VFD_BASE + slot,
                   (long)at, (unsigned)n, (int)r, zero ? ", ALL ZEROS" : "",
                   (unsigned)pthr_engine_offset((uintptr_t)caller));
    } else if (logged++ < 40)
        l_debug("[obb read] virtual fd %d at %ld: %u bytes%s", VFD_BASE + slot, (long)at, (unsigned)n,
                via_cache ? " (texture-data cache)" : "");
    return r;
}

/* ---- zlib: the engine decompresses streamed texture data (and packages) with uncompress() on its worker threads
 * (appUncompressMemoryZLIB). Failures, and output that is all zeros (zero PVRTC data is solid black), are logged. */
#include <zlib.h>

static volatile uint64_t uncompress_us;

int uncompress_checked(Bytef *dest, uLongf *destLen, const Bytef *source, uLong sourceLen) {
    uLongf want = destLen ? *destLen : 0;
    uint64_t t0 = sceKernelGetProcessTimeWide();
    int r = uncompress(dest, destLen, source, sourceLen);
    __sync_fetch_and_add(&uncompress_us, sceKernelGetProcessTimeWide() - t0);
    static volatile int calls, bad, zero;
    int c = __sync_fetch_and_add(&calls, 1);
    if (r != Z_OK) {
        if (__sync_fetch_and_add(&bad, 1) < 40)
            l_warn("[zlib] uncompress failed: %d (%u -> %u bytes wanted, from libUnrealEngine3.so+0x%x)", r,
                   (unsigned)sourceLen, (unsigned)want, (unsigned)pthr_engine_offset((uintptr_t)__builtin_return_address(0)));
    } else if (*destLen != want) {
        if (__sync_fetch_and_add(&bad, 1) < 40)
            l_warn("[zlib] uncompress gave %u bytes, %u expected", (unsigned)*destLen, (unsigned)want);
    } else if (all_zero(dest, (ssize_t)*destLen)) {
        if (__sync_fetch_and_add(&zero, 1) < 40)
            l_warn("[zlib] uncompress output is ALL ZEROS (%u -> %u bytes)", (unsigned)sourceLen, (unsigned)*destLen);
    } else if (c < 40) {
        l_debug("[zlib] uncompress %u -> %u bytes", (unsigned)sourceLen, (unsigned)*destLen);
    }
    return r;
}

/* ---- [io] line: where loading time goes ----
 * inflate: the engine unpacks every package with zlib's inflate (minizip, from the .obb's deflated entries);
 * obb reads: time inside sceIoPread for the expansion-file streams. */
extern volatile uint64_t obb_read_us, obb_read_bytes, obb_ra_hits, obb_ra_misses, obb_cache_bytes;
extern volatile uint32_t obb_read_max_us;
static volatile uint64_t inflate_us, inflate_out;
static volatile uint32_t inflate_max_us;

int inflate_timed(z_streamp strm, int flush) {
    uLong before = strm->total_out;
    uint64_t t0 = sceKernelGetProcessTimeWide();
    int r = inflate(strm, flush);
    uint32_t dt = (uint32_t)(sceKernelGetProcessTimeWide() - t0);
    __sync_fetch_and_add(&inflate_us, (uint64_t)dt);
    __sync_fetch_and_add(&inflate_out, (uint64_t)(strm->total_out - before));
    if (dt > inflate_max_us) inflate_max_us = dt;
    return r;
}

void io_time_report(unsigned period_s) {
    static uint64_t p_inf_us, p_inf_out, p_rd_us, p_rd_b, p_h, p_m;
    uint64_t a = inflate_us, b = inflate_out, c = obb_read_us, d = obb_read_bytes, hh = obb_ra_hits, mm = obb_ra_misses;
    uint64_t dh = hh - p_h, dm = mm - p_m;
    p_h = hh; p_m = mm;
    static uint64_t p_c;
    uint64_t cc = obb_cache_bytes, dcc = cc - p_c;
    p_c = cc;
    uint32_t im = inflate_max_us, rm = obb_read_max_us;
    inflate_max_us = 0;
    obb_read_max_us = 0;
    uint64_t da = a - p_inf_us, db = b - p_inf_out, dc = c - p_rd_us, dd = d - p_rd_b;
    p_inf_us = a; p_inf_out = b; p_rd_us = c; p_rd_b = d;
    static uint64_t p_tu, p_tb, p_rr, p_tcs;
    uint64_t tcs = tex_cache_served, dtcs = tcs - p_tcs;
    p_tcs = tcs;
    uint32_t tce, tcb, tcl;
    tex_cache_stats(&tce, &tcb, &tcl);
    uint64_t tu = tex_read_us, tb = tex_read_bytes, dtu = tu - p_tu, dtb = tb - p_tb, rr = tex_reread_bytes, drr = rr - p_rr;
    p_tu = tu; p_tb = tb; p_rr = rr;
    if (!da && !dc && !dcc && !dtu) return;   /* nothing loaded in this period */
    l_info("[io] last %u s: inflate %u ms for %u KB out (longest call %u ms) | obb reads %u ms for %u KB (longest %u ms, "
           "%u.%u MB/s) | read-ahead %u hits, %u block loads | package cache served %u KB | texture-cache reads %u ms for %u KB"
           " (%u KB of it read before; %u KB different data so far; texture-data cache served %u KB, holds %u KB of %u)",
           period_s, (unsigned)(da / 1000), (unsigned)(db / 1024),
           (unsigned)(im / 1000), (unsigned)(dc / 1000), (unsigned)(dd / 1024), (unsigned)(rm / 1000),
           dc ? (unsigned)(dd * 1000000ull / dc / 1048576ull) : 0, dc ? (unsigned)(dd * 10000000ull / dc / 1048576ull % 10) : 0,
           (unsigned)dh, (unsigned)dm, (unsigned)(dcc / 1024), (unsigned)(dtu / 1000), (unsigned)(dtb / 1024), (unsigned)(drr / 1024),
           (unsigned)(tex_unique_chunks >> (10 - CHUNK_SHIFT)), (unsigned)(dtcs / 1024), tcb / 1024, tcl / 1024);
}
/* bionic's lseek/ftruncate take a 32-bit off_t (offset in r1, whence in r2). If VitaSDK's newlib off_t is 64-bit,
 * mapping the engine straight to newlib's lseek puts the offset in r2:r3 and the whence on the stack: every seek
 * goes wrong. This wrapper takes the bionic arguments and is correct whichever size newlib uses. Failed seeks are
 * logged (the engine only writes "SetFilePointer Failed" to its own log, which goes nowhere). */
int32_t lseek_bionic(int fd, int32_t off, int whence) {
    if (vfd_slot(fd) >= 0) return (int32_t)obb_vfd_lseek(fd, off, whence);
    if (save_behind_is(fd)) return (int32_t)save_behind_lseek(fd, off, whence);   /* r73 */
    off_t r = lseek(fd, (off_t)off, whence);
    if (r == (off_t)-1) {
        static int n;
        if (n++ < 20)
            l_warn("[io] lseek(fd %d, %ld, %d) failed, errno %d (from libUnrealEngine3.so+0x%x)", fd, (long)off, whence, errno,
                   (unsigned)pthr_engine_offset((uintptr_t)__builtin_return_address(0)));
        return -1;
    }
    return (int32_t)r;
}

int ftruncate_bionic(int fd, int32_t len) {
    if (save_behind_is(fd)) return save_behind_ftruncate(fd, len);   /* r73 */
    return ftruncate(fd, (off_t)len);
}

/* ---- r57: per-load breakdown for the [hitch] lines (load_stats.h) ---- */
extern volatile uint32_t dm_tex_us;   /* utils/gldiag.c */

void load_snap(load_snap_t *s) {
    s->t_us = sceKernelGetProcessTimeWide();
    s->card_us = obb_read_us; s->card_bytes = obb_read_bytes;
    s->inflate_us = inflate_us; s->uncompress_us = uncompress_us;
    s->pkg_cache_bytes = obb_cache_bytes;
    s->tex_us = tex_read_us; s->tex_bytes = tex_read_bytes; s->tex_cached_bytes = tex_cached_bytes; s->tex_reads = tex_reads;
    s->obb_opens = obb_opens; s->obb_opens_real = obb_opens_real;
    s->dir_scans = dm_dir_scans; s->dir_scans_real = dm_dir_scans_real; s->dir_us = dm_dir_us;
    s->allocs = dm_alloc_n; s->alloc_us = dm_alloc_us;
    s->upload_us = dm_tex_us;
    s->open_seq = dm_open_seq;
}

void load_breakdown(const load_snap_t *a, char *out, int n) {
    load_snap_t b;
    load_snap(&b);
    unsigned total = (unsigned)((b.t_us - a->t_us) / 1000);
    unsigned card = (unsigned)((b.card_us - a->card_us) / 1000), inf = (unsigned)((b.inflate_us - a->inflate_us) / 1000);
    unsigned unc = (unsigned)((b.uncompress_us - a->uncompress_us) / 1000), tex = (unsigned)((b.tex_us - a->tex_us) / 1000);
    unsigned dir = (unsigned)((b.dir_us - a->dir_us) / 1000), al = (unsigned)((b.alloc_us - a->alloc_us) / 1000);
    unsigned up = (b.upload_us - a->upload_us) / 1000;
    /* the texture reads' card time is inside "card reads" (both count sceIoPread); the rest of their time is the
     * texture-data cache's memcpy, small */
    unsigned known = card + inf + unc + dir + al;
    snprintf(out, (size_t)n,
             " | card reads %u ms (%u KB) | unpacking %u ms (zip) + %u ms (engine chunks) | package cache %u KB"
             " | texture reads %u ms (%u reads, %u KB, %u KB from the texture-data cache) | .obb opens %u (%u from the card)"
             " | folder scans %u (%u from the card, %u ms) | allocations %u (~%u ms) | render thread uploads %u ms"
             " | other (engine work) ~%u ms",
             card, (unsigned)((b.card_bytes - a->card_bytes) / 1024), inf, unc,
             (unsigned)((b.pkg_cache_bytes - a->pkg_cache_bytes) / 1024), tex, b.tex_reads - a->tex_reads,
             (unsigned)((b.tex_bytes - a->tex_bytes) / 1024), (unsigned)((b.tex_cached_bytes - a->tex_cached_bytes) / 1024),
             b.obb_opens - a->obb_opens, b.obb_opens_real - a->obb_opens_real, b.dir_scans - a->dir_scans,
             b.dir_scans_real - a->dir_scans_real, dir, b.allocs - a->allocs, al, up, total > known ? total - known : 0);
    /* r60: and which zip entries it opened */
    size_t l = strlen(out);
    if ((int)l < n - 64) {
        char files[400];
        pkg_open_log_since(a->open_seq, files, sizeof(files));
        if (files[0]) snprintf(out + l, (size_t)n - l, " | files: %s", files);
    }
}
