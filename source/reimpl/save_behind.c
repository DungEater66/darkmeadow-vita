/*
 * r73: save files written behind the game's back.
 *
 * r71's log: every save wrote PactSave<N>.bin (13 KB) 2-5 times in a row, about 70 ms each on the memory card, all
 * on the game thread - 13 frames of 366-780 ms, mostly in shop, upgrade and dialog screens. Here open() for writing
 * (create or truncate) of a file in the game's save folder hands back a virtual descriptor that collects the data in
 * memory; close() queues it and returns at once. A background thread writes the last version of each file to the
 * card 150 ms after the last close, so the repeated writes become one. Anything that looks at such a file before
 * then (open, fopen, stat, rename, remove) waits for its write first, so the game always reads what it wrote.
 * DM_SAVE_BEHIND 0 in config.txt writes saves directly again.
 */
#include "reimpl/save_behind.h"
#include "utils/logger.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#define SV_BASE 0x6000
#define SV_MAX 16
#define SV_PENDING 16
#define SV_DELAY_US 150000ull

typedef struct { int used; char path[256]; uint8_t *buf; size_t len, cap, pos; } svfd_t;
typedef struct { int used, busy; char path[256]; uint8_t *buf; size_t len; uint64_t t; unsigned merged; } pending_t;

static svfd_t fds[SV_MAX];
static pending_t pend[SV_PENDING];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t done = PTHREAD_COND_INITIALIZER;
static int enabled = 1, started;

void save_behind_set(int on) { enabled = on; }

static int wanted(const char *path) {
    /* the game's own files: saves and their metadata (files/TheDarkMeadow/Pact*.bin) */
    return strstr(path, "/files/") && strstr(path, "Pact") && strstr(path, ".bin");
}

static int slot(int fd) {
    int s = fd - SV_BASE;
    return s >= 0 && s < SV_MAX && fds[s].used ? s : -1;
}

int save_behind_is(int fd) { return slot(fd) >= 0; }

static int write_file(const char *path, const uint8_t *buf, size_t len) {
    int f = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (f < 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(f, buf + off, len - off);
        if (w <= 0) { close(f); return -1; }
        off += (size_t)w;
    }
    return close(f);
}

static int writer(SceSize args, void *argp) {
    (void)args; (void)argp;
    for (;;) {
        sceKernelDelayThread(30 * 1000);
        uint64_t now = sceKernelGetProcessTimeWide();
        for (int i = 0; i < SV_PENDING; i++) {
            pthread_mutex_lock(&lock);
            pending_t *p = &pend[i];
            if (!p->used || p->busy || now - p->t < SV_DELAY_US) { pthread_mutex_unlock(&lock); continue; }
            p->busy = 1;
            char path[256];
            snprintf(path, sizeof(path), "%s", p->path);
            uint8_t *buf = p->buf;
            size_t len = p->len;
            unsigned merged = p->merged;
            p->buf = NULL;
            pthread_mutex_unlock(&lock);

            uint64_t t0 = sceKernelGetProcessTimeWide();
            int r = write_file(path, buf, len);
            unsigned ms = (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000);
            free(buf);
            if (r < 0) l_warn("[save] %s: WRITE FAILED (%u bytes)", path, (unsigned)len);
            else {
                static int n;
                if (n++ < 200) l_info("[save] %s: %u bytes written in %u ms (%u save(s) merged)", path, (unsigned)len, ms, merged);
            }

            pthread_mutex_lock(&lock);
            if (p->buf) {   /* saved again while it was being written: keep the newer one for the next round */
                p->busy = 0;
            } else {
                p->used = 0;
                p->busy = 0;
            }
            pthread_cond_broadcast(&done);
            pthread_mutex_unlock(&lock);
        }
    }
    return 0;
}

static void start(void) {
    if (started) return;
    started = 1;
    SceUID t = sceKernelCreateThread("dm_save", writer, 120, 0x4000, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
}

int save_behind_open(const char *path, int newlib_oflag) {
    if (!enabled || !path || strlen(path) >= 256 || !wanted(path)) return -1;
    int acc = newlib_oflag & O_ACCMODE;
    if (acc == O_RDONLY || (newlib_oflag & O_APPEND)) return -1;
    if (!(newlib_oflag & (O_TRUNC | O_CREAT))) return -1;
    struct stat st;
    if (!(newlib_oflag & O_TRUNC) && stat(path, &st) == 0 && st.st_size > 0) return -1;   /* would keep old contents */
    start();
    pthread_mutex_lock(&lock);
    int s = -1;
    for (int i = 0; i < SV_MAX; i++) if (!fds[i].used) { s = i; break; }
    if (s >= 0) {
        memset(&fds[s], 0, sizeof(fds[s]));
        fds[s].used = 1;
        snprintf(fds[s].path, sizeof(fds[s].path), "%s", path);
    }
    pthread_mutex_unlock(&lock);
    return s < 0 ? -1 : SV_BASE + s;
}

ssize_t save_behind_write(int fd, const void *buf, size_t n) {
    int s = slot(fd);
    if (s < 0) { errno = EBADF; return -1; }
    svfd_t *f = &fds[s];
    if (f->pos + n > f->cap) {
        size_t c = f->cap ? f->cap : 16384;
        while (c < f->pos + n) c *= 2;
        uint8_t *nb = realloc(f->buf, c);
        if (!nb) { errno = ENOMEM; return -1; }
        memset(nb + f->cap, 0, c - f->cap);
        f->buf = nb;
        f->cap = c;
    }
    memcpy(f->buf + f->pos, buf, n);
    f->pos += n;
    if (f->pos > f->len) f->len = f->pos;
    return (ssize_t)n;
}

ssize_t save_behind_read(int fd, void *buf, size_t n) {
    int s = slot(fd);
    if (s < 0) { errno = EBADF; return -1; }
    svfd_t *f = &fds[s];
    size_t k = f->pos < f->len ? f->len - f->pos : 0;
    if (k > n) k = n;
    memcpy(buf, f->buf + f->pos, k);
    f->pos += k;
    return (ssize_t)k;
}

int64_t save_behind_lseek(int fd, int64_t off, int whence) {
    int s = slot(fd);
    if (s < 0) { errno = EBADF; return -1; }
    svfd_t *f = &fds[s];
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)f->pos : (int64_t)f->len;
    if (base + off < 0) { errno = EINVAL; return -1; }
    f->pos = (size_t)(base + off);
    return (int64_t)f->pos;
}

int save_behind_ftruncate(int fd, int64_t len) {
    int s = slot(fd);
    if (s < 0) { errno = EBADF; return -1; }
    svfd_t *f = &fds[s];
    if ((size_t)len > f->len) {
        size_t old = f->pos;
        f->pos = f->len;
        uint8_t z = 0;
        while (f->len < (size_t)len) save_behind_write(fd, &z, 1);
        f->pos = old;
    } else f->len = (size_t)len;
    return 0;
}

int save_behind_fstat(int fd, struct stat *st) {
    int s = slot(fd);
    if (s < 0) { errno = EBADF; return -1; }
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0666;
    st->st_size = (off_t)fds[s].len;
    st->st_blksize = 4096;
    return 0;
}

int save_behind_close(int fd) {
    int s = slot(fd);
    if (s < 0) { errno = EBADF; return -1; }
    svfd_t *f = &fds[s];
    pthread_mutex_lock(&lock);
    pending_t *p = NULL;
    for (int i = 0; i < SV_PENDING && !p; i++) if (pend[i].used && !strcmp(pend[i].path, f->path)) p = &pend[i];
    if (p) {   /* the same file saved again before it reached the card: keep only the newest */
        free(p->buf);
        p->merged++;
    } else {
        for (int i = 0; i < SV_PENDING && !p; i++) if (!pend[i].used) p = &pend[i];
        if (p) { memset(p, 0, sizeof(*p)); p->used = 1; p->merged = 1; snprintf(p->path, sizeof(p->path), "%s", f->path); }
    }
    int direct = !p;
    if (p) {
        p->buf = f->buf;
        p->len = f->len;
        p->t = sceKernelGetProcessTimeWide();
        f->buf = NULL;
    }
    pthread_mutex_unlock(&lock);
    if (direct) {   /* queue full (never seen): write it now */
        if (write_file(f->path, f->buf, f->len) < 0) l_warn("[save] %s: WRITE FAILED", f->path);
        free(f->buf);
    }
    memset(f, 0, sizeof(*f));
    return 0;
}

/* wait until `path` (NULL = every file) has reached the card */
void save_behind_flush(const char *path) {
    if (!started) return;
    pthread_mutex_lock(&lock);
    for (;;) {
        int waiting = 0;
        for (int i = 0; i < SV_PENDING; i++)
            if (pend[i].used && (!path || !strcmp(pend[i].path, path))) { pend[i].t = 0; waiting = 1; }
        if (!waiting) break;
        pthread_cond_wait(&done, &lock);
    }
    pthread_mutex_unlock(&lock);
}
