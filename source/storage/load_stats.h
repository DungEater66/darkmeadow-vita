#ifndef DM_LOAD_STATS_H
#define DM_LOAD_STATS_H

#include <stdint.h>

/* where the time of one load goes. A snapshot of the loader's counters is taken when a LoadPackage or
 * FlushAsyncLoading starts, and the difference is printed with its [hitch] line (io_stats.c). The counters are
 * global (all threads), so they cover the engine's async loader thread while the game thread waits. */
typedef struct {
    uint64_t t_us;
    uint64_t card_us, card_bytes;        /* sceIoPread on the expansion files (packages, texture caches, everything) */
    uint64_t inflate_us, uncompress_us;  /* zip entries (inflate) and the engine's compressed chunks (uncompress) */
    uint64_t pkg_cache_bytes;            /* served by the unpacked package cache */
    uint64_t tex_us, tex_bytes, tex_cached_bytes;
    uint32_t tex_reads;
    uint32_t obb_opens, obb_opens_real;  /* engine open() of an .obb; real = went to the memory card */
    uint32_t dir_scans, dir_scans_real;  /* opendir; real = went to the memory card */
    uint64_t dir_us;
    uint32_t allocs;                     /* malloc/calloc/realloc/memalign/free calls */
    uint64_t alloc_us;                   /* estimated from every 64th call */
    uint32_t upload_us;                  /* render thread: texture uploads */
    uint32_t open_seq;                   /* zip entries opened (pkg_cache.c) */
} load_snap_t;

void load_snap(load_snap_t *s);
/* the breakdown since `a`, appended to a [hitch] line: " | card reads ... | other ~N ms" */
void load_breakdown(const load_snap_t *a, char *out, int n);

/* counters kept elsewhere */
extern volatile uint32_t dm_open_seq;
void pkg_open_log_since(uint32_t seq, char *out, int n);   /* pkg_cache.c */
extern volatile uint32_t dm_alloc_n;
extern volatile uint64_t dm_alloc_us;          /* mem_valve.c */
extern volatile uint32_t dm_dir_scans, dm_dir_scans_real;
extern volatile uint64_t dm_dir_us;            /* io.c */

#endif
