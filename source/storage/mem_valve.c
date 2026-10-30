/*
 * Memory safety valve.
 *
 * The engine's malloc/realloc/calloc go through these. The loader's caches (unpacked packages, and texture data when
 * it could not get its video-memory block) never give memory back by themselves; if the heap runs out, the engine
 * would get NULL and crash. Here a failed allocation first makes the caches let go of what they hold, then tries again:
 * running short of memory costs cache hits, not a crash. Each time it happens it is logged as [mem].
 */
#include "storage/mem_valve.h"

#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>

#include "storage/load_stats.h"
#include "storage/pkg_cache.h"
#include "storage/tex_cache.h"
#include "util/logger.h"

/* The engine's allocations are counted for the [hitch] breakdown (load_stats.h). Timing every call would cost
 * more than the calls, so every 64th is timed and counted 64 times. */
volatile uint32_t dm_alloc_n;
volatile uint64_t dm_alloc_us;
#define TIMED(expr) do { \
        uint32_t c_ = __sync_fetch_and_add(&dm_alloc_n, 1); \
        if (c_ & 63) { expr; } \
        else { uint64_t t_ = sceKernelGetProcessTimeWide(); expr; \
               __sync_fetch_and_add(&dm_alloc_us, (sceKernelGetProcessTimeWide() - t_) * 64); } \
    } while (0)

static size_t release(size_t n) {
    size_t want = n * 2 + (4u << 20);   /* some slack: the heap is fragmented, and more requests usually follow */
    size_t f = tex_cache_release(want);
    if (f < want) f += pkg_cache_release(want - f);
    return f;
}

static void report(const char *what, size_t n, size_t freed, int ok) {
    static int count;
    if (count++ < 50)
        l_warn("[mem] engine %s of %u bytes failed: caches gave back %u KB, %s", what, (unsigned)n, (unsigned)(freed / 1024),
               ok ? "retry succeeded" : "STILL FAILED");
}

void free_valve(void *p) { TIMED(free(p)); }

void *malloc_valve(size_t n) {
    void *p;
    TIMED(p = malloc(n));
    if (p || !n) return p;
    size_t f = release(n);
    p = malloc(n);
    report("malloc", n, f, p != NULL);
    return p;
}

void *realloc_valve(void *old, size_t n) {
    void *p;
    TIMED(p = realloc(old, n));
    if (p || !n) return p;
    size_t f = release(n);
    p = realloc(old, n);   /* a failed realloc leaves `old` untouched */
    report("realloc", n, f, p != NULL);
    return p;
}

