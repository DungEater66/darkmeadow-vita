/* Read-only FILE streams on the expansion files that share one file descriptor per .obb, with a small read-ahead
 * cache (see obb_stream.c). */
#ifndef DM_OBB_STREAM_H
#define DM_OBB_STREAM_H

#include <stdint.h>
#include <stdio.h>

FILE *obb_fopen(const char *path);
int obb_is_path(const char *path, const char *mode);
int obb_fileno(FILE *f);
void obb_report(void);   /* watchdog: open streams during a freeze */

int obb_ra_pread(const char *path, void *buf, int n, int64_t off);       /* through the read-ahead; -1 = not applicable */
int obb_shared_pread(const char *path, void *buf, int n, int64_t off);   /* -1 = cannot open */
int64_t obb_shared_size(const char *path);                                /* -1 = cannot open */

/* Counters for the [io] line (io_stats.c) */
extern volatile uint64_t obb_read_us, obb_read_bytes, obb_ra_hits, obb_ra_misses;
extern volatile uint32_t obb_read_max_us;
extern volatile uint64_t obb_cache_bytes;   /* served by the package cache (pkg_cache.c) */

#endif
