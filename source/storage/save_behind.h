#ifndef DM_SAVE_BEHIND_H
#define DM_SAVE_BEHIND_H

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

/* The game's save files, collected in memory and written by a background thread (see save_behind.c) */
void save_behind_init(void);                                  /* engine_start */
int save_behind_open(const char *path, int newlib_oflag);     /* a virtual descriptor, or -1 = open it for real */
int save_behind_is(int fd);
ssize_t save_behind_write(int fd, const void *buf, size_t n);
ssize_t save_behind_read(int fd, void *buf, size_t n);
int64_t save_behind_lseek(int fd, int64_t off, int whence);
int save_behind_ftruncate(int fd, int64_t len);
int save_behind_fstat(int fd, struct stat *st);
int save_behind_close(int fd);
void save_behind_flush(const char *path);                     /* NULL = all */

#endif
