#ifndef DM_PKG_CACHE_H
#define DM_PKG_CACHE_H

/* Packages kept unpacked in RAM and served to the engine's zip reader (see pkg_cache.c). */
void pkg_cache_install_hooks(void);   /* from so_patch */
void pkg_cache_start(const char *obb_path, const char *names, int limit_mb, int keep);   /* preload + keep-after-load */

#include <stddef.h>
size_t pkg_cache_release(size_t want);   /* give memory back to the engine (mem_valve.c) */
void pkg_cache_set_keep_only(const char *list);   /* r62: DM_PACKAGE_CACHE_KEEP_ONLY */

void pkg_prefetch_config(int on, int window_mb);   /* r64: DM_PACKAGE_PREFETCH, DM_PACKAGE_PREFETCH_MB */
void pkg_cache_movie(int starting);
void pkg_cache_attach_spill(void);                  /* r71: from gl_init, before the rendering thread exists */                 /* r64: movie.c - give the prefetch window back for a movie */

#endif
