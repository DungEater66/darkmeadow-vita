#ifndef DM_OBB_STREAM_H
#define DM_OBB_STREAM_H

#include <stdio.h>

/* Read-only FILE streams on the expansion files that share one file descriptor per .obb (see obb_stream.c). */
FILE *obb_fopen(const char *path);
int obb_is_path(const char *path, const char *mode);
int obb_fileno(FILE *f);
void obb_report(void);
#include <stdint.h>
int obb_ra_pread(const char *path, void *buf, int n, int64_t off);   /* -1 = not applicable */
int obb_shared_pread(const char *path, void *buf, int n, int64_t off);   /* -1 = cannot open */
int64_t obb_shared_size(const char *path);                                /* -1 = cannot open */

#endif
