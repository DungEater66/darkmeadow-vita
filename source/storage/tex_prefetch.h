/* Texture prefetch: remembers the texture reads of each floor across sessions and reads them into the texture-data
 * cache in the background when that floor is entered again (see tex_prefetch.c). It also tracks whether the engine is
 * loading, for the other background readers. */
#ifndef DM_TEX_PREFETCH_H
#define DM_TEX_PREFETCH_H

#include <stdint.h>

void texpf_init(void);                                     /* engine_start, after tex_cache_init */
void texpf_note_read(int obb, int64_t off, uint32_t len);  /* a read that went to the card */
void texpf_package_loaded(const char *name);               /* every LoadPackage, with the package name */
void texpf_loading(int delta);                             /* +1 entering / -1 leaving a load or async-load wait */

int texpf_busy(void);                                      /* the engine is in a load right now */
uint64_t texpf_quiet_us(void);                             /* how long since the last load ended (0 while loading) */

#endif
