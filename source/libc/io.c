/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "libc/io.h"
#include "storage/io_stats.h"
#include "storage/obb_stream.h"
#include "storage/save_behind.h"

#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdarg.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <pthread.h>
#include <strings.h>
#include "storage/load_stats.h"


#include "util/logger.h"
#include "util/utils.h"

// Includes the following inline utilities:
// int oflags_musl_to_newlib(int flags);
// dirent64_bionic * dirent_newlib_to_bionic(struct dirent* dirent_newlib);
// void stat_newlib_to_bionic(struct stat * src, stat64_bionic * dst);
#include "libc/bits/_struct_converters.c"

FILE * fopen_soloader(const char * filename, const char * mode) {
    if (strcmp(filename, "/proc/cpuinfo") == 0) {
        return fopen_soloader("app0:/cpuinfo", mode);
    } else if (strcmp(filename, "/proc/meminfo") == 0) {
        return fopen_soloader("app0:/meminfo", mode);
    } else if (strcmp(filename, "/sys/devices/system/cpu/present") == 0) {
        return fopen_soloader("app0:/cpu_present", mode);
    }

    save_behind_flush(filename);   /* a save still on its way to the card is written first */
    // Android has the game's directories already (e.g. files/TheDarkMeadow); here nothing does. Create the parents of
    // anything opened for writing, otherwise e.g. the save writer comes back NULL and the engine dereferences it.
    if (mode && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+')))
        file_mkpath(filename, 0777);

    /* the expansion files: shared-descriptor streams, see obb_stream.c */
    if (obb_is_path(filename, mode)) {
        FILE *o = obb_fopen(filename);
        if (o) return o;
        l_warn("fopen(%s, %s): shared stream failed, using a plain FILE", filename, mode);
    }

    FILE* ret = fopen(filename, mode);

    if (ret)
        l_debug("fopen(%s, %s): %p", filename, mode, ret);
    else
        l_warn("fopen(%s, %s): %p", filename, mode, ret);

    return ret;
}

#define WTRACK_MAX 256
static struct { char *path; uint64_t bytes; unsigned errors; } wtrack[WTRACK_MAX];

int open_soloader(const char * path, int oflag, ...) {
    if (strcmp(path, "/proc/cpuinfo") == 0) {
        return open_soloader("app0:/cpuinfo", oflag);
    } else if (strcmp(path, "/proc/meminfo") == 0) {
        return open_soloader("app0:/meminfo", oflag);
    } else if (strcmp(path, "/sys/devices/system/cpu/present") == 0) {
        return open_soloader("app0:/cpu_present", oflag);
    } else if (strcmp(path, "/dev/urandom") == 0) {
        return open_soloader("app0:/urandom", oflag);
    }

    mode_t mode = 0666;
    if (((oflag & BIONIC_O_CREAT) == BIONIC_O_CREAT) ||
        ((oflag & BIONIC_O_TMPFILE) == BIONIC_O_TMPFILE)) {
        va_list args;
        va_start(args, oflag);
        mode = (mode_t)(va_arg(args, int));
        va_end(args);
    }

    // see fopen_soloader: make sure the parent directories exist when creating a file
    if ((oflag & BIONIC_O_CREAT) == BIONIC_O_CREAT)
        file_mkpath(path, 0777);

    size_t pl = strlen(path);
    int is_obb = pl > 4 && strcasecmp(path + pl - 4, ".obb") == 0;
    if (is_obb && (oflag & 3) == 0 /* O_RDONLY */) {
        /* no trip to the memory card, see obb_vfd_open in io_stats.c */
        int v = obb_vfd_open(path);
        if (v >= 0) {
            static int logged;
            if (logged++ < 3) l_debug("open(%s): virtual descriptor %d (the file stays open in obb_stream.c)", path, v);
            return v;
        }
    } else if (is_obb) {
        io_count_real_obb_open();
    }

    oflag = oflags_bionic_to_newlib(oflag);
    {   /* saves are collected in memory and written by a background thread (save_behind.c) */
        int v = save_behind_open(path, oflag);
        if (v >= 0) return v;
        save_behind_flush(path);
    }
    int ret = open(path, oflag, mode);
    if (ret >= 0) io_stats_track_obb(ret, is_obb ? path : NULL);
    if (ret >= 0)
        l_debug("open(%s, %x): %i", path, oflag, ret);
    else
        l_warn("open(%s, %x): %i", path, oflag, ret);
    if (ret >= 0 && ret < WTRACK_MAX && (oflag & (O_WRONLY | O_RDWR))) {
        free(wtrack[ret].path);
        wtrack[ret].path = strdup(path);
        wtrack[ret].bytes = 0;
        wtrack[ret].errors = 0;
    }
    return ret;
}

/* Writes are not logged one by one; instead every file opened for writing (saves, configs) reports at close how many
 * bytes went into it and whether any write failed, so "the game does not save" can be checked in log.txt. */
ssize_t write_soloader(int fd, const void *buf, size_t n) {
    if (save_behind_is(fd)) return save_behind_write(fd, buf, n);
    ssize_t r = write(fd, buf, n);
    if (fd >= 0 && fd < WTRACK_MAX && wtrack[fd].path) {
        if (r > 0) wtrack[fd].bytes += (uint64_t)r;
        if (r < 0 || (size_t)r != n) {
            wtrack[fd].errors++;
            l_warn("write(%s, %u bytes): %d", wtrack[fd].path, (unsigned)n, (int)r);
        }
    }
    return r;
}

/* POSIX rename() replaces an existing destination; sceIoRename refuses to. Do what POSIX says. */
int rename_soloader(const char *from, const char *to) {
    save_behind_flush(from);
    save_behind_flush(to);
    int r = rename(from, to);
    if (r != 0) {
        struct stat st;
        if (stat(to, &st) == 0) {
            remove(to);
            r = rename(from, to);
        }
    }
    if (r == 0) l_debug("rename(%s -> %s): 0", from, to);
    else l_warn("rename(%s -> %s): %d", from, to, r);
    return r;
}

int fstat_soloader(int fd, stat64_bionic * buf) {
    struct stat st;
    int res = obb_vfd_is(fd) ? obb_vfd_fstat(fd, &st) : save_behind_is(fd) ? save_behind_fstat(fd, &st) : fstat(fd, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("fstat(%i): %i", fd, res);
    return res;
}

int stat_soloader(const char * path, stat64_bionic * buf) {
    if (strcmp(path, "/system/lib/libOpenSLES.so") == 0) {
        l_debug("stat(%s): returning 0 in case this is a check for OpenSLES support", path);
        return 0;
    }

    struct stat st;
    save_behind_flush(path);
    int res = stat(path, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("stat(%s): %i", path, res);
    return res;
}

int fclose_soloader(FILE * f) {
    int ret = fclose(f);

    l_debug("fclose(%p): %i", f, ret);
    return ret;
}

int close_soloader(int fd) {
    if (obb_vfd_is(fd)) return obb_vfd_close(fd);   /* virtual .obb descriptor, nothing to close */
    if (save_behind_is(fd)) return save_behind_close(fd);   /* queued, written in the background */
    int ret = close(fd);
    io_stats_track_obb(fd, NULL);
    if (fd >= 0 && fd < WTRACK_MAX && wtrack[fd].path) {
        l_info("close(%s): wrote %u bytes%s", wtrack[fd].path, (unsigned)wtrack[fd].bytes,
               wtrack[fd].errors ? ", WITH WRITE ERRORS" : "");
        free(wtrack[fd].path);
        wtrack[fd].path = NULL;
    } else {
        l_debug("close(%i): %i", fd, ret);
    }
    return ret;
}

/* dup() and fdopen() of a virtual .obb descriptor */
int dup_soloader(int fd) {
    if (obb_vfd_is(fd)) return obb_vfd_dup(fd);
    return dup(fd);
}

/* ---- folder scans without a trip to the memory card ----
 * Before every texture read the engine calls opendir("../..") and closes it straight away without reading it (475
 * times in a floor-2 run). Once a folder has been opened successfully, later opendir() calls on the same path get a
 * placeholder handle; the real folder is opened only if the engine actually reads entries from it. */
#define FAKE_DIRS 64
typedef struct { char path[256]; DIR *real; } fakedir_t;
static fakedir_t *fakes[FAKE_DIRS];
static char *known_dirs[64];
static int n_known;
static pthread_mutex_t dir_lock = PTHREAD_MUTEX_INITIALIZER;
volatile uint32_t dm_dir_scans, dm_dir_scans_real;
volatile uint64_t dm_dir_us;

static fakedir_t *as_fake(DIR *d) {
    if (!d) return NULL;
    fakedir_t *r = NULL;
    pthread_mutex_lock(&dir_lock);
    for (int i = 0; i < FAKE_DIRS; i++) if ((void *)fakes[i] == (void *)d) { r = fakes[i]; break; }
    pthread_mutex_unlock(&dir_lock);
    return r;
}

/* the real handle behind a placeholder, opened on first use; the handle itself otherwise */
static DIR *real_dir(DIR *d) {
    fakedir_t *f = as_fake(d);
    if (!f) return d;
    if (!f->real) f->real = opendir(f->path);
    return f->real;
}

DIR* opendir_soloader(char* _pathname) {
    __sync_fetch_and_add(&dm_dir_scans, 1);
    if (_pathname && strlen(_pathname) < 256) {
        int known = 0;
        pthread_mutex_lock(&dir_lock);
        for (int i = 0; i < n_known && !known; i++) known = !strcmp(known_dirs[i], _pathname);
        fakedir_t *f = NULL;
        if (known) {
            for (int i = 0; i < FAKE_DIRS; i++)
                if (!fakes[i]) {
                    f = calloc(1, sizeof(*f));
                    if (f) { snprintf(f->path, sizeof(f->path), "%s", _pathname); fakes[i] = f; }
                    break;
                }
        }
        pthread_mutex_unlock(&dir_lock);
        if (f) return (DIR *)f;
    }
    __sync_fetch_and_add(&dm_dir_scans_real, 1);
    uint64_t t0 = sceKernelGetProcessTimeWide();
    DIR* ret = opendir(_pathname);
    __sync_fetch_and_add(&dm_dir_us, sceKernelGetProcessTimeWide() - t0);
    l_debug("opendir(\"%s\"): %p", _pathname, ret);
    if (ret && _pathname && strlen(_pathname) < 256) {
        pthread_mutex_lock(&dir_lock);
        int known = 0;
        for (int i = 0; i < n_known && !known; i++) known = !strcmp(known_dirs[i], _pathname);
        if (!known && n_known < 64) known_dirs[n_known++] = strdup(_pathname);
        pthread_mutex_unlock(&dir_lock);
    }
    return ret;
}

struct dirent64_bionic * readdir_soloader(DIR * dir) {
    static struct dirent64_bionic dirent_tmp;

    dir = real_dir(dir);
    if (!dir) return NULL;
    struct dirent* ret = readdir(dir);
    // (per-entry logging removed: directory scans produced hundreds of lines)

    if (ret) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(ret);
        memcpy(&dirent_tmp, entry_tmp, sizeof(dirent64_bionic));
        free(entry_tmp);
        return &dirent_tmp;
    }

    return NULL;
}

int closedir_soloader(DIR * dir) {
    fakedir_t *f = as_fake(dir);
    if (f) {
        pthread_mutex_lock(&dir_lock);
        for (int i = 0; i < FAKE_DIRS; i++) if (fakes[i] == f) { fakes[i] = NULL; break; }
        pthread_mutex_unlock(&dir_lock);
        if (f->real) closedir(f->real);
        free(f);
        return 0;
    }
    int ret = closedir(dir);
    l_debug("closedir(%p): %i", dir, ret);
    return ret;
}

int fcntl_soloader(int fd, int cmd, ...) {
    l_warn("fcntl(%i, %i, ...): not implemented", fd, cmd);
    return 0;
}

int ioctl_soloader(int fd, int request, ...) {
    l_warn("ioctl(%i, %i, ...): not implemented", fd, request);
    return 0;
}

/* remove() / unlink() of a save still on its way to the card waits for it first */
int remove_soloader(const char *path) { save_behind_flush(path); return remove(path); }
int unlink_soloader(const char *path) { save_behind_flush(path); return unlink(path); }
