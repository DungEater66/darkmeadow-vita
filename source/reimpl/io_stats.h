#ifndef DM_IO_STATS_H
#define DM_IO_STATS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Count the engine's file reads (read() and its stdio fread()), so the watchdog can tell "loading" from "stuck". */
void io_stats_note(size_t bytes, void *caller);

/* Reads and bytes since the last call (then reset), and the engine call site of the most recent read. */
void io_stats_collect(unsigned *calls, uint64_t *bytes, uintptr_t *last_caller);

ssize_t read_counted(int fd, void *buf, size_t n);
int32_t lseek_bionic(int fd, int32_t off, int whence);
int ftruncate_bionic(int fd, int32_t len);
/* Mark a descriptor as an expansion file (.obb) opened with open(), so its reads are checked and logged. */
void io_stats_track_obb(int fd, const char *obb_path);   /* NULL = not an .obb */
int io_obb_index(const char *path);   /* r57: an .obb's index (texture-data cache key), -1 if none */
const char *io_obb_path(int i);

/* r57: virtual descriptors for the engine's open() of an .obb (no trip to the memory card) */
struct stat;
void io_set_keep_obb_open(int on);
int obb_vfd_open(const char *path);   /* -1 = open it for real */
void io_count_real_obb_open(void);
int obb_vfd_is(int fd);
int obb_vfd_close(int fd);
int obb_vfd_dup(int fd);
int64_t obb_vfd_lseek(int fd, int64_t off, int whence);
int obb_vfd_fstat(int fd, struct stat *st);
const char *obb_vfd_path(int fd, int64_t *pos);
#include <zlib.h>
int uncompress_checked(Bytef *dest, uLongf *destLen, const Bytef *source, uLong sourceLen);
int inflate_timed(z_streamp strm, int flush);
/* [io] line: inflate and expansion-file read time over the last period (called by the watchdog). */
void io_time_report(unsigned period_s);

#endif
