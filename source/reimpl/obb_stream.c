/*
 * Read-only FILE streams for the expansion files (.obb).
 *
 * The engine opens the .obb zip with fopen() once per open package (minizip keeps a FILE per archive handle) and,
 * while a level loads, has well over a hundred of them open at once. Each one used a real file descriptor; after a
 * death (level reload) fopen() finally returned NULL and the engine crashed inside unzReadFile -> fseek(NULL).
 *
 * Here every such stream is a newlib funopen() stream that reads with sceIoPread() from ONE shared descriptor per
 * .obb, keeping its own position, so the number of streams is limited only by memory. If the engine asks for the
 * stream's file descriptor (_fileno, used to fstat/read the archive directly), it gets a private one opened then.
 */
#include "reimpl/obb_stream.h"
#include "reimpl/io_stats.h"

#include <fcntl.h>
#include <stdio.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "utils/logger.h"

#ifndef DATA_PATH
#define DATA_PATH "ux0:data/darkmeadow/"
#endif

/* Read-ahead (r39): the engine reads packages through these streams 8 KB at a time, and every read is a separate trip
 * to the memory card (r37/r38: 1.5-2.5 MB/s while loading, against ~12 MB/s for large reads). Each .obb gets a small
 * shared cache of RA_BLOCKS blocks of RA_BLOCK bytes: a read inside a cached block is a memcpy, a miss loads the whole
 * aligned block. Reads of a block or more go straight to the card. DM_OBB_READAHEAD 0 in config.txt turns it off. */
#define RA_BLOCK  (128 * 1024)
#define RA_BLOCKS 8            /* 1 MB per .obb */

typedef struct {
    int64_t off;        /* block start, -1 = empty */
    int len;            /* bytes valid (short at the end of the file) */
    unsigned stamp;     /* last use, for replacement */
    uint8_t *data;
} ra_block_t;

typedef struct {
    char path[256];
    SceUID fd;          /* shared, never closed */
    int64_t size;
    pthread_mutex_t ra_lock;
    ra_block_t ra[RA_BLOCKS];
    unsigned ra_clock;
} shared_t;

typedef struct {
    shared_t *sh;
    int64_t pos;
    int real_fd;        /* private descriptor handed out by obb_fileno, or -1 */
    FILE *f;
} cookie_t;

#define MAX_SHARED 4
#define MAX_STREAMS 1024
static shared_t shared[MAX_SHARED];
static int shared_n;
static cookie_t *streams[MAX_STREAMS];
static int open_now, open_peak;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

int obb_is_path(const char *path, const char *mode) {
    size_t n = path ? strlen(path) : 0;
    return n > 4 && strcasecmp(path + n - 4, ".obb") == 0 && mode && mode[0] == 'r' && !strchr(mode, '+');
}

static shared_t *get_shared(const char *path) {
    for (int i = 0; i < shared_n; i++)
        if (strcmp(shared[i].path, path) == 0) return &shared[i];
    if (shared_n >= MAX_SHARED) return NULL;
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return NULL;
    shared_t *s = &shared[shared_n++];
    snprintf(s->path, sizeof(s->path), "%s", path);
    s->fd = fd;
    s->size = sceIoLseek(fd, 0, SCE_SEEK_END);
    pthread_mutex_init(&s->ra_lock, NULL);
    for (int i = 0; i < RA_BLOCKS; i++) s->ra[i].off = -1;
    return s;
}

/* I/O timing for the [io] line (io_stats.c) */
volatile uint64_t obb_read_us, obb_read_bytes;
volatile uint32_t obb_read_max_us;

/* bytes served by the unpacked package cache (reimpl/pkg_cache.c), for the [io] line */
volatile uint64_t obb_cache_bytes;

volatile uint64_t obb_ra_hits, obb_ra_misses;
static int readahead = -1;

static int timed_pread(SceUID fd, void *buf, int n, int64_t off) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    int r = sceIoPread(fd, buf, (SceSize)n, off);
    uint32_t dt = (uint32_t)(sceKernelGetProcessTimeWide() - t0);
    __sync_fetch_and_add(&obb_read_us, (uint64_t)dt);
    if (r > 0) __sync_fetch_and_add(&obb_read_bytes, (uint64_t)r);
    if (dt > obb_read_max_us) obb_read_max_us = dt;
    return r;
}

static int config_readahead(void) {
    int on = 1;
    FILE *f = fopen(DATA_PATH "config.txt", "r");
    if (f) {
        char line[128], k[64];
        int v;
        while (fgets(line, sizeof(line), f))
            if (sscanf(line, "%63s %d", k, &v) == 2 && !strcmp(k, "DM_OBB_READAHEAD")) on = v != 0;
        fclose(f);
    }
    l_info("[obb] read-ahead %s (%d x %d KB per expansion file, DM_OBB_READAHEAD)", on ? "on" : "off", RA_BLOCKS,
           RA_BLOCK / 1024);
    return on;
}

/* Copy [pos, pos + n) from the cache, loading blocks as needed. Called with ra_lock held. Returns bytes copied. */
static int ra_read(shared_t *sh, char *buf, int n, int64_t pos) {
    int done = 0;
    while (done < n) {
        int64_t p = pos + done, boff = p - p % RA_BLOCK;
        ra_block_t *b = NULL, *victim = &sh->ra[0];
        for (int i = 0; i < RA_BLOCKS; i++) {
            if (sh->ra[i].off == boff) { b = &sh->ra[i]; break; }
            if (sh->ra[i].off < 0 || sh->ra[i].stamp < victim->stamp) victim = &sh->ra[i];
        }
        if (b) {
            obb_ra_hits++;
        } else {
            obb_ra_misses++;
            if (!victim->data && !(victim->data = malloc(RA_BLOCK))) return done;
            int want = (int)(sh->size - boff < RA_BLOCK ? sh->size - boff : RA_BLOCK);
            int r = timed_pread(sh->fd, victim->data, want, boff);
            if (r <= 0) { victim->off = -1; return done ? done : r; }
            victim->off = boff;
            victim->len = r;
            b = victim;
        }
        b->stamp = ++sh->ra_clock;
        int in = (int)(p - boff), avail = b->len - in;
        if (avail <= 0) break;
        int take = n - done < avail ? n - done : avail;
        memcpy(buf + done, b->data + in, (size_t)take);
        done += take;
        if (b->len < RA_BLOCK) break;   /* end of file */
    }
    return done;
}

static int s_read(void *c, char *buf, int n) {
    cookie_t *k = c;
    if (n <= 0 || k->pos >= k->sh->size) return 0;
    if (k->pos + n > k->sh->size) n = (int)(k->sh->size - k->pos);
    int r;
    if (readahead && n < RA_BLOCK) {
        pthread_mutex_lock(&k->sh->ra_lock);
        r = ra_read(k->sh, buf, n, k->pos);
        pthread_mutex_unlock(&k->sh->ra_lock);
    } else {
        r = timed_pread(k->sh->fd, buf, n, k->pos);
    }
    if (r < 0) return -1;
    k->pos += r;
    return r;
}

/* Positional read through the shared read-ahead blocks, for the engine's own open()/read() of an .obb (the texture
 * caches; see read_counted in io_stats.c). Returns -1 when it does not apply (read-ahead off, large read). */
int obb_ra_pread(const char *path, void *buf, int n, int64_t off) {
    if (readahead < 0) readahead = config_readahead();
    if (!readahead || n <= 0 || n >= RA_BLOCK || off < 0) return -1;
    pthread_mutex_lock(&lock);
    shared_t *sh = get_shared(path);
    pthread_mutex_unlock(&lock);
    if (!sh) return -1;
    if (off >= sh->size) return 0;
    if (off + n > sh->size) n = (int)(sh->size - off);
    pthread_mutex_lock(&sh->ra_lock);
    int r = ra_read(sh, buf, n, off);
    pthread_mutex_unlock(&sh->ra_lock);
    return r;
}

/* r57: a plain positional read on the shared descriptor (any size, no read-ahead), and the file's size. For the
 * engine's open() of an .obb, which no longer opens the file (virtual descriptors, io_stats.c), and for the texture
 * prefetch (tex_prefetch.c). Both return -1 if the file cannot be opened. */
int obb_shared_pread(const char *path, void *buf, int n, int64_t off) {
    pthread_mutex_lock(&lock);
    shared_t *sh = get_shared(path);
    pthread_mutex_unlock(&lock);
    if (!sh || off < 0) return -1;
    if (n <= 0 || off >= sh->size) return 0;
    if (off + n > sh->size) n = (int)(sh->size - off);
    return timed_pread(sh->fd, buf, n, off);
}

int64_t obb_shared_size(const char *path) {
    pthread_mutex_lock(&lock);
    shared_t *sh = get_shared(path);
    pthread_mutex_unlock(&lock);
    return sh ? sh->size : -1;
}

#ifdef __NEWLIB__
typedef _fpos_t seek_off_t;   /* what funopen's seek callback takes and returns in newlib */
#else
typedef long seek_off_t;
#endif

static seek_off_t s_seek(void *c, seek_off_t off, int whence) {
    cookie_t *k = c;
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? k->pos : k->sh->size;
    int64_t np = base + (int64_t)off;
    if (np < 0) return (seek_off_t)-1;
    k->pos = np;
    return (seek_off_t)np;
}

static int s_close(void *c) {
    cookie_t *k = c;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_STREAMS; i++) if (streams[i] == k) { streams[i] = NULL; break; }
    open_now--;
    pthread_mutex_unlock(&lock);
    if (k->real_fd >= 0) { io_stats_track_obb(k->real_fd, NULL); close(k->real_fd); }
    free(k);
    return 0;
}

FILE *obb_fopen(const char *path) {
    if (readahead < 0) readahead = config_readahead();   /* here, not inside a read callback */
    pthread_mutex_lock(&lock);
    shared_t *sh = get_shared(path);
    int slot = -1;
    for (int i = 0; i < MAX_STREAMS; i++) if (!streams[i]) { slot = i; break; }
    pthread_mutex_unlock(&lock);
    if (!sh || slot < 0) return NULL;

    cookie_t *k = calloc(1, sizeof(*k));
    if (!k) return NULL;
    k->sh = sh;
    k->real_fd = -1;
    FILE *f = funopen(k, s_read, NULL, s_seek, s_close);
    if (!f) { free(k); return NULL; }
    k->f = f;
    setvbuf(f, NULL, _IOFBF, 8192);

    pthread_mutex_lock(&lock);
    if (streams[slot]) {   /* taken meanwhile: find another */
        slot = -1;
        for (int i = 0; i < MAX_STREAMS; i++) if (!streams[i]) { slot = i; break; }
    }
    if (slot >= 0) streams[slot] = k;
    if (++open_now > open_peak) open_peak = open_now;
    pthread_mutex_unlock(&lock);
    return f;
}

int obb_fileno(FILE *f) {
    cookie_t *k = NULL;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_STREAMS; i++) if (streams[i] && streams[i]->f == f) { k = streams[i]; break; }
    pthread_mutex_unlock(&lock);
    if (!k) return fileno(f);
    if (k->real_fd < 0) {
        k->real_fd = open(k->sh->path, O_RDONLY);
        io_stats_track_obb(k->real_fd, k->sh->path);   /* its reads are checked like the engine's own open() of an .obb */
        if (k->real_fd >= 0) lseek(k->real_fd, k->pos, SEEK_SET);
    }
    return k->real_fd;
}

void obb_report(void) {
    l_info("[obb] %d expansion-file streams open (peak %d), %d shared descriptors", open_now, open_peak, shared_n);
}
