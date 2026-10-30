/*
 * Texture-data cache.
 *
 * The engine reads its textures from the texture caches (.tfc) in the patch .obb with its own open() / lseek() /
 * read(), opening the file again for every texture and reading each one in pieces. Two thirds of those bytes had
 * already been read earlier in the session: the same textures are read again at every battle start and area load,
 * while all the distinct texture data comes to about 23 MB.
 *
 * Each read is kept here, keyed by (.obb, offset, length) - the engine repeats exactly the same reads - up to
 * CACHE_MB. A repeated read is a memcpy. When the cache is full, entries are dropped with the CLOCK rule (an entry used
 * since the hand last passed it gets a second chance). Called from read_counted (io_stats.c) for the engine's reads
 * of an .obb; the texture prefetch (tex_prefetch.c) fills it ahead of time.
 *
 * Where the data lives: vitaGL's video-memory pool keeps 59+ MB free, while the malloc heap gets down to 37 MB free.
 * So right after vitaGL starts (gl_init, before the rendering thread exists) one block of the cache's size is taken
 * from that pool, and the cache allocates inside it with its own mspace. The CPU reads video memory more slowly than
 * main RAM, but still far faster than the memory card. Without that block the cache uses the heap, and gives memory
 * back to the engine when the heap runs out (tex_cache_release).
 */
#include "storage/tex_cache.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/clib.h>
#include <vitaGL.h>

#include "util/logger.h"

#define CACHE_MB 44
#define MAX_ENTRIES 16384
#define NBUCKETS 8192
#define MAX_ONE (2u << 20)   /* a single read bigger than this is not kept */

typedef struct {
    uint8_t *data;   /* NULL = free slot */
    int64_t off;
    uint32_t len;
    int32_t next;    /* bucket chain */
    uint8_t obb, ref;
} entry_t;

static entry_t e[MAX_ENTRIES];
static int32_t bucket[NBUCKETS];
static int hand;
static uint32_t limit, used, count;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static SceClibMspace vram_space;   /* NULL = entries live on the heap */
static int vram_tried;
volatile uint64_t tex_cache_served;

static unsigned hash(int obb, int64_t off, uint32_t len) {
    uint64_t h = (uint64_t)off * 0x9E3779B97F4A7C15ull ^ (uint64_t)len * 0xC2B2AE3D27D4EB4Full ^ (uint64_t)obb;
    return (unsigned)(h >> 40) % NBUCKETS;
}

static void *cache_alloc(size_t n) { return vram_space ? sceClibMspaceMalloc(vram_space, n) : malloc(n); }

static void cache_free(void *p) {
    if (vram_space) sceClibMspaceFree(vram_space, p);
    else free(p);
}

void tex_cache_init(void) {
    limit = (uint32_t)CACHE_MB << 20;
    for (int i = 0; i < NBUCKETS; i++) bucket[i] = -1;
    l_info("[texcache] texture-data cache: up to %d MB", CACHE_MB);
}

/* From gl_init, right after vitaGL has started and before the rendering thread exists: nothing else calls vitaGL's
 * allocator at the same time. */
void tex_cache_attach_vram(void) {
    if (!limit || vram_tried) return;
    vram_tried = 1;
    size_t want = limit + (limit >> 4);   /* + mspace bookkeeping */
    void *blk = vglAlloc((uint32_t)want, VGL_MEM_VRAM);
    if (blk) vram_space = sceClibMspaceCreate(blk, want);
    if (vram_space)
        l_info("[texcache] %u MB block in video memory (vitaGL VRAM free now %u MB)", (unsigned)(want >> 20),
               (unsigned)(vglMemFree(VGL_MEM_VRAM) >> 20));
    else
        l_warn("[texcache] no %u MB block in video memory: the cache uses the heap", (unsigned)(want >> 20));
}

static int cacheable(size_t n) { return limit && n && n <= MAX_ONE; }

static int32_t find(int obb, int64_t off, uint32_t len) {
    for (int32_t i = bucket[hash(obb, off, len)]; i >= 0; i = e[i].next)
        if (e[i].off == off && e[i].len == len && e[i].obb == obb) return i;
    return -1;
}

int tex_cache_get(int obb, int64_t off, void *buf, size_t n) {
    if (!cacheable(n)) return 0;
    pthread_mutex_lock(&lock);
    int32_t i = find(obb, off, (uint32_t)n);
    if (i >= 0) {
        memcpy(buf, e[i].data, n);
        e[i].ref = 1;
        tex_cache_served += n;
    }
    pthread_mutex_unlock(&lock);
    return i >= 0;
}

static void unlink_entry(int32_t i) {
    unsigned b = hash(e[i].obb, e[i].off, e[i].len);
    for (int32_t *p = &bucket[b]; *p >= 0; p = &e[*p].next)
        if (*p == i) {
            *p = e[i].next;
            break;
        }
    used -= e[i].len;
    count--;
    cache_free(e[i].data);
    e[i].data = NULL;
}

/* Free one entry with the CLOCK rule. Returns 0 if nothing could be freed. */
static int evict_one(void) {
    for (int steps = 0; steps < 2 * MAX_ENTRIES; steps++) {
        int i = hand;
        hand = (hand + 1) % MAX_ENTRIES;
        if (!e[i].data) continue;
        if (e[i].ref) {
            e[i].ref = 0;
            continue;
        }
        unlink_entry(i);
        return 1;
    }
    return 0;
}

static int32_t free_slot(void) {
    for (int k = 0; k < MAX_ENTRIES; k++) {
        int i = (hand + k) % MAX_ENTRIES;
        if (!e[i].data) return i;
    }
    return -1;
}

/* Store `copy` (n bytes, already allocated) in a free slot. Lock held. */
static int insert(int obb, int64_t off, uint8_t *copy, const void *buf, size_t n) {
    int32_t slot = free_slot();
    if (slot < 0) {
        cache_free(copy);
        return 0;
    }
    memcpy(copy, buf, n);
    entry_t *t = &e[slot];
    t->data = copy;
    t->off = off;
    t->len = (uint32_t)n;
    t->obb = (uint8_t)obb;
    t->ref = 0;
    unsigned b = hash(obb, off, (uint32_t)n);
    t->next = bucket[b];
    bucket[b] = slot;
    used += (uint32_t)n;
    count++;
    return 1;
}

void tex_cache_put(int obb, int64_t off, const void *buf, size_t n) {
    if (!cacheable(n) || n > limit) return;
    pthread_mutex_lock(&lock);
    if (find(obb, off, (uint32_t)n) < 0) {
        while ((used + n > limit || count >= MAX_ENTRIES) && evict_one()) {}
        uint8_t *copy = NULL;
        if (used + n <= limit && count < MAX_ENTRIES)
            while (!(copy = cache_alloc(n)) && evict_one()) {}   /* the mspace may be fragmented: make room until it fits */
        if (copy) insert(obb, off, copy, buf, n);
    }
    pthread_mutex_unlock(&lock);
}

int tex_cache_has(int obb, int64_t off, size_t n) {
    if (!cacheable(n)) return 1;   /* "held": nothing to fetch for it */
    pthread_mutex_lock(&lock);
    int r = find(obb, off, (uint32_t)n) >= 0;
    pthread_mutex_unlock(&lock);
    return r;
}

/* For the prefetch: add a read without pushing anything out, so it never costs the cache a read the engine is using
 * now. Prefetched entries start unreferenced, so the CLOCK hand takes them first if the engine never asks for them. */
int tex_cache_fill(int obb, int64_t off, const void *buf, size_t n) {
    if (!cacheable(n)) return 0;
    pthread_mutex_lock(&lock);
    int r = 0;
    if (find(obb, off, (uint32_t)n) < 0) {
        r = -1;
        uint8_t *copy = used + n <= limit && count < MAX_ENTRIES ? cache_alloc(n) : NULL;
        if (copy && insert(obb, off, copy, buf, n)) r = 1;
    }
    pthread_mutex_unlock(&lock);
    return r;
}

/* The engine could not get memory: give back what the cache holds on the heap (nothing, when it lives in video
 * memory). Returns the bytes freed. */
size_t tex_cache_release(size_t want) {
    if (vram_space || !limit) return 0;
    size_t freed = 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_ENTRIES && freed < want; i++)
        if (e[i].data) {
            freed += e[i].len;
            unlink_entry(i);
        }
    pthread_mutex_unlock(&lock);
    return freed;
}

void tex_cache_stats(uint32_t *entries, uint32_t *bytes, uint32_t *lim) {
    pthread_mutex_lock(&lock);
    *entries = count;
    *bytes = used;
    *lim = limit;
    pthread_mutex_unlock(&lock);
}
