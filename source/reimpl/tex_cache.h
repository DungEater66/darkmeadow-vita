#ifndef DM_TEX_CACHE_H
#define DM_TEX_CACHE_H

#include <stddef.h>
#include <stdint.h>

/* Texture-data cache for the engine's own reads of the .obb texture caches (see tex_cache.c). */
void tex_cache_init(int mb);                                          /* 0 = off */
int tex_cache_enabled(void);
int tex_cache_get(int obb, int64_t off, void *buf, size_t n);         /* 1 = filled from RAM */
void tex_cache_put(int obb, int64_t off, const void *buf, size_t n);
void tex_cache_stats(uint32_t *entries, uint32_t *bytes, uint32_t *limit);
void tex_cache_attach_vram(void);                                      /* from gl_init */
int tex_cache_in_vram(void);
size_t tex_cache_release(size_t want);                                 /* heap mode only */
int tex_cache_has(int obb, int64_t off, size_t n);                    /* r57: 1 = held (or not cacheable) */
int tex_cache_fill(int obb, int64_t off, const void *buf, size_t n);  /* r57: 1 added, 0 held, -1 no room */
extern volatile uint64_t tex_cache_served;

#endif
