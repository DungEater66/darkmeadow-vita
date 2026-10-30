/* Packages kept unpacked in RAM and served to the engine's zip reader (pkg_cache.c), and read ahead of the engine
 * (pkg_prefetch.c). */
#ifndef DM_PKG_CACHE_H
#define DM_PKG_CACHE_H

#include <stddef.h>

void pkg_cache_install_hooks(void);   /* so_patch */
void pkg_cache_start(void);           /* engine_start: preload, keep-after-load and the prefetch */
void pkg_cache_attach_spill(void);    /* gl_init, before the rendering thread exists */
void pkg_cache_movie(int starting);   /* movie.c: give the prefetch window's memory to the decoder (1) / take it back (0) */
size_t pkg_cache_release(size_t want);   /* the heap ran out: give memory back to the engine (mem_valve.c) */

#endif
