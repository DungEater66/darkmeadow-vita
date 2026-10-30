/*
 * Texture-data cache (r54).
 *
 * The engine reads its textures from the texture caches (.tfc) in the patch .obb with its own open()/lseek()/read(),
 * opening the file again for every texture and reading each one in pieces. r53 measured that 66% of those bytes had
 * already been read earlier in the session (44.5 of 67.1 MB over a 26-minute run), while all the distinct texture data
 * read came to 22.8 MB: the same textures are read again at every battle start and area load.
 *
 * Each read is kept here, keyed by (.obb, offset, length) - the engine repeats exactly the same reads - up to a byte
 * limit (DM_TEXTURE_CACHE_MB in config.txt, default 32 since r57, was 24; 0 = off). A repeated read is a memcpy. When the limit is
 * reached, entries are dropped with the CLOCK rule (an entry used since the hand last passed it gets a second chance).
 * Called from read_counted (io_stats.c) for descriptors the engine opened on an .obb.
 *
 * Where the data lives: vitaGL's video-memory (CDRAM) pool had 59+ MB free at its lowest in every run, while the
 * malloc heap got down to 37 MB free. So right after vitaGL starts (gl_init, before the rendering thread exists) one
 * block of the cache's size is taken from that pool, and the cache allocates inside it with its own mspace. The CPU
 * reads video memory more slowly than main RAM, but still hundreds of times faster than the memory card. If the block
 * cannot be had, the cache uses the heap instead (and then gives memory back to the engine when the heap runs out,
 * see tex_cache_release).
 */
#include "reimpl/tex_cache.h"
#include "utils/logger.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/clib.h>
#include <vitaGL.h>

#define MAX_ENTRIES 16384   /* r57: 8192 -> 16384 for the prefetched reads */
#define NBUCKETS 8192
#define MAX_ONE (2u << 20)   /* a single read bigger than this is not kept */

typedef struct {
    uint8_t *data;       /* NULL = free */
    int64_t off;
    uint32_t len;
    int32_t next;        /* bucket chain */
    uint8_t obb, ref;
} tentry_t;

static tentry_t e[MAX_ENTRIES];
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

static void *c_alloc(size_t n) { return vram_space ? sceClibMspaceMalloc(vram_space, n) : malloc(n); }
static void c_free(void *p) { if (vram_space) sceClibMspaceFree(vram_space, p); else free(p); }

/* From gl_init, right after vitaGL has started and before the rendering thread exists: vitaGL's allocator is not
 * called from two threads at once here. */
void tex_cache_attach_vram(void) {
    if (!limit || vram_tried) return;
    vram_tried = 1;
    size_t want = limit + (limit >> 4);   /* mspace bookkeeping */
    void *blk = vglAlloc((uint32_t)want, VGL_MEM_VRAM);
    if (blk) vram_space = sceClibMspaceCreate(blk, want);
    if (vram_space) l_info("[texcache] %u MB block in video memory (vitaGL VRAM free now %u MB)", (unsigned)(want >> 20),
                           (unsigned)(vglMemFree(VGL_MEM_VRAM) >> 20));
    else l_warn("[texcache] no %u MB block in video memory: the cache uses the heap", (unsigned)(want >> 20));
}

int tex_cache_in_vram(void) { return vram_space != NULL; }

void tex_cache_init(int mb) {
    limit = mb > 0 ? (uint32_t)mb << 20 : 0;
    for (int i = 0; i < NBUCKETS; i++) bucket[i] = -1;
}

int tex_cache_enabled(void) { return limit != 0; }

static int32_t find(int obb, int64_t off, uint32_t len) {
    for (int32_t i = bucket[hash(obb, off, len)]; i >= 0; i = e[i].next)
        if (e[i].off == off && e[i].len == len && e[i].obb == obb) return i;
    return -1;
}

int tex_cache_get(int obb, int64_t off, void *buf, size_t n) {
    if (!limit || !n || n > MAX_ONE) return 0;
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
        if (*p == i) { *p = e[i].next; break; }
    used -= e[i].len;
    count--;
    c_free(e[i].data);
    e[i].data = NULL;
}

/* Free one slot (or bytes) with the CLOCK rule. Returns 0 if nothing could be freed. */
static int evict_one(void) {
    for (int steps = 0; steps < 2 * MAX_ENTRIES; steps++) {
        int i = hand;
        hand = (hand + 1) % MAX_ENTRIES;
        if (!e[i].data) continue;
        if (e[i].ref) { e[i].ref = 0; continue; }
        unlink_entry(i);
        return 1;
    }
    return 0;
}

void tex_cache_put(int obb, int64_t off, const void *buf, size_t n) {
    if (!limit || !n || n > MAX_ONE || n > limit) return;
    pthread_mutex_lock(&lock);
    if (find(obb, off, (uint32_t)n) >= 0) { pthread_mutex_unlock(&lock); return; }
    while ((used + n > limit || count >= MAX_ENTRIES) && evict_one()) {}
    uint8_t *copy = NULL;
    if (used + n <= limit && count < MAX_ENTRIES)
        while (!(copy = c_alloc(n)) && evict_one()) {}   /* the mspace may be fragmented: make room until it fits */
    int32_t slot = -1;
    if (copy)
        for (int k = 0; k < MAX_ENTRIES; k++) {
            int i = (hand + k) % MAX_ENTRIES;
            if (!e[i].data) { slot = i; break; }
        }
    if (slot < 0) { if (copy) c_free(copy); pthread_mutex_unlock(&lock); return; }
    memcpy(copy, buf, n);
    tentry_t *t = &e[slot];
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
    pthread_mutex_unlock(&lock);
}

/* r57, for the texture prefetch (tex_prefetch.c): is this read already held? */
int tex_cache_has(int obb, int64_t off, size_t n) {
    if (!limit || !n || n > MAX_ONE) return 1;   /* "held": nothing to fetch for it */
    pthread_mutex_lock(&lock);
    int r = find(obb, off, (uint32_t)n) >= 0;
    pthread_mutex_unlock(&lock);
    return r;
}

/* r57: add a prefetched read without pushing anything out: the prefetch only fills free room, so it never costs the
 * cache a read the engine is using now. Returns 1 added, 0 already held or not cacheable, -1 no room left. Prefetched
 * entries start unreferenced, so the CLOCK hand takes them first if the engine never asks for them. */
int tex_cache_fill(int obb, int64_t off, const void *buf, size_t n) {
    if (!limit || !n || n > MAX_ONE) return 0;
    pthread_mutex_lock(&lock);
    int r = 0;
    if (find(obb, off, (uint32_t)n) >= 0) goto out;
    r = -1;
    if (used + n > limit || count >= MAX_ENTRIES) goto out;
    int32_t slot = -1;
    for (int k = 0; k < MAX_ENTRIES; k++) {
        int i = (hand + k) % MAX_ENTRIES;
        if (!e[i].data) { slot = i; break; }
    }
    if (slot < 0) goto out;
    uint8_t *copy = c_alloc(n);
    if (!copy) goto out;
    memcpy(copy, buf, n);
    tentry_t *t = &e[slot];
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
    r = 1;
out:
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
        if (e[i].data) { freed += e[i].len; unlink_entry(i); }
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
