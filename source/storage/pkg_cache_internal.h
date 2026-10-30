/*
 * Shared between the package cache (pkg_cache.c) and the package prefetch (pkg_prefetch.c).
 */
#ifndef DM_PKG_CACHE_INTERNAL_H
#define DM_PKG_CACHE_INTERNAL_H

#include <pthread.h>
#include <stdint.h>
#include <psp2/types.h>

#define PKG_MAX_ENTRIES 160
#define PKG_MAX_OPEN 256
#define PKG_KEEP_MAX_BYTES (8u << 20)   /* bigger packages (whole areas) are loaded once per visit: not worth keeping */
#define PKG_KEEP_MIN_BYTES (32u << 10)

/* One unpacked package (a zip entry of the .obb). Where its bytes live: the heap (counted in pkg_used), the prefetch
 * window (win) or the spill block (spill). */
typedef struct {
    char name[64];
    uint32_t size;
    uint8_t *data;             /* NULL = free slot; always `size` bytes */
    volatile uint32_t valid;   /* bytes [0, valid) are filled in (= size once complete) */
    uint8_t pinned;            /* preloaded: never dropped */
    uint8_t announced;         /* "kept" logged */
    uint8_t win;               /* in the prefetch window */
    uint8_t spill;             /* parked in the spill block while a movie has the window's memory */
    uint8_t prefetched;
    uint64_t t_fill;           /* when the prefetch filled it */
    uint64_t last_us;          /* last read by the engine */
    volatile int readers;      /* reads copying from it right now */
    uint32_t hits, stamp;
    int refs;                  /* engine handles using it right now (+1 while the prefetch fills it) */
} pkg_entry_t;

/* An engine handle on a cached package. pos = what the engine has been given; real = how far the engine's own
 * (inflating) handle has actually been read. Zip entries are always read from the start, in order.
 *   eof: the engine has been given the whole entry; the handle no longer holds it (so it can be dropped to make
 *        room), and further reads return 0.
 *   detached: its entry was taken away while the engine still had it open; further reads go to the engine's own
 *        handle, brought up to `pos` first. */
typedef struct {
    void *h;
    pkg_entry_t *e;
    uint32_t pos, real;
    uint8_t served, eof, detached;
} pkg_open_t;

/* guarded by pkg_lock */
extern pthread_mutex_t pkg_lock;
extern pkg_entry_t pkg_entries[PKG_MAX_ENTRIES];
extern int pkg_entries_n;            /* slots in use so far (some may be free again) */
extern pkg_open_t pkg_opened[PKG_MAX_OPEN];
extern uint32_t pkg_used, pkg_limit; /* heap bytes held, and the limit */

extern char pkg_obb_path[256];

/* lock held */
pkg_entry_t *pkg_find(const char *basename, uint32_t size);
pkg_entry_t *pkg_insert(const char *basename, uint32_t size, uint8_t *data, int pinned);
int pkg_make_room(uint32_t need);
int pkg_keep_name(const char *basename);   /* enemy and weapon packages: kept after their first load */

/* ---- reading the .obb zip ---- */
/* Call fn for every entry of the zip's central directory. Returns 0 if it cannot be read; *file_size gets the size. */
typedef void (*pkg_zip_fn)(void *ctx, const char *basename, int method, uint32_t csize, uint32_t usize, uint32_t lho);
int pkg_zip_for_each(SceUID fd, int64_t *file_size, pkg_zip_fn fn, void *ctx);
/* Where an entry's data starts (after its local header), or -1. */
int64_t pkg_zip_data_start(SceUID fd, uint32_t lho);

/* ---- the prefetch side (pkg_prefetch.c) ---- */
void pkg_prefetch_start(void);
void pkg_prefetch_note_open(const char *basename, uint32_t size);

#endif
