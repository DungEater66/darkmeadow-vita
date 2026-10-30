/* Texture-data cache: the engine's own reads of the .obb texture files, kept in memory (see tex_cache.c). */
#ifndef DM_TEX_CACHE_H
#define DM_TEX_CACHE_H

#include <stddef.h>
#include <stdint.h>

void tex_cache_init(void);                                            /* engine_start */
void tex_cache_attach_vram(void);                                     /* gl_init: move into a video-memory block */

int tex_cache_get(int obb, int64_t off, void *buf, size_t n);         /* 1 = filled from the cache */
void tex_cache_put(int obb, int64_t off, const void *buf, size_t n);  /* keep a read (may push older ones out) */
int tex_cache_has(int obb, int64_t off, size_t n);                    /* 1 = held (or not cacheable) */
int tex_cache_fill(int obb, int64_t off, const void *buf, size_t n);  /* prefetch: 1 added, 0 held, -1 no room */

size_t tex_cache_release(size_t want);                                /* heap only: give memory back to the engine */
void tex_cache_stats(uint32_t *entries, uint32_t *bytes, uint32_t *limit);
extern volatile uint64_t tex_cache_served;                            /* bytes served from the cache */

#endif
