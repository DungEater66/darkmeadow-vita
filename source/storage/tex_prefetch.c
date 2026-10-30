/*
 * Texture prefetch.
 *
 * The engine reads its textures from the texture caches (.tfc) in the patch .obb in small pieces, about 2.4 MB/s from
 * the memory card. At a battle start that is a large share of the stall. The texture-data cache (tex_cache.c) makes a
 * read the engine has made before a memcpy, but only within one session, and only after the first time.
 *
 * Here every texture read that went to the card is remembered with the floor it happened on (the floor is taken from
 * the last "HOR_fl<N>_persistent" package loaded; the front end counts as floor 0) and saved to
 * ux0:data/darkmeadow/texprefetch.bin. When a floor is entered, a low-priority thread waits until loading has been
 * quiet for a second, then reads everything recorded for that floor in earlier sessions into the texture-data cache:
 * sorted by position and merged into reads of up to 512 KB, so it goes at the card's full speed, pausing whenever the
 * engine loads something. It only fills free room in the cache (it never pushes out what the engine is using), so the
 * worst case is that it is of no use.
 *
 * The file grows as more of the game is played (16 bytes per read; a floor is a few thousand reads) and is written
 * at each floor change and every minute while new reads are coming in. It is checked against the .obb sizes, so a
 * different game version starts over. Deleting the file resets it.
 */
#include "storage/tex_prefetch.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "storage/io_stats.h"
#include "storage/obb_stream.h"
#include "storage/tex_cache.h"
#include "util/logger.h"

#define PF_FILE DATA_PATH "texprefetch.bin"
#define PF_TMP DATA_PATH "texprefetch.tmp"

#define MAX_RECS 200000
#define MAX_KINDS 4
#define SPAN_MAX (512 * 1024)
#define SPAN_GAP (32 * 1024)
#define QUIET_US 1000000

typedef struct {
    int64_t off;
    uint32_t len;
    uint8_t kind;    /* index into names[] (which .obb) */
    uint8_t floor;
    uint16_t pad;
} rec_t;

typedef struct {
    char magic[4];
    uint32_t version, count, nkinds;
    char names[MAX_KINDS][64];
    int64_t sizes[MAX_KINDS];
} hdr_t;

static int on;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static rec_t *recs;
static uint32_t nrecs, caprecs, saved_n;
static uint32_t *ht;          /* open addressing: record index + 1, 0 = empty */
static uint32_t htsize;
static char names[MAX_KINDS][64];
static int64_t sizes[MAX_KINDS];
static int nkinds;
static int8_t kind_of_obb[4] = {-1, -1, -1, -1};

static volatile int cur_floor, want_floor = -1;
static volatile unsigned gen;
static volatile int loading_depth;
static volatile uint64_t last_load_end;
static SceUID sema = -1;

static uint32_t hkey(int kind, int floor, int64_t off, uint32_t len) {
    uint64_t h = (uint64_t)off * 0x9E3779B97F4A7C15ull ^ (uint64_t)len * 0xC2B2AE3D27D4EB4Full ^
                 ((uint64_t)kind << 8 | (uint64_t)floor) * 0x165667B19E3779F9ull;
    return (uint32_t)(h >> 32);
}

static int ht_find(int kind, int floor, int64_t off, uint32_t len, uint32_t *slot) {
    uint32_t m = htsize - 1, i = hkey(kind, floor, off, len) & m;
    for (;; i = (i + 1) & m) {
        uint32_t v = ht[i];
        if (!v) { *slot = i; return 0; }
        rec_t *r = &recs[v - 1];
        if (r->off == off && r->len == len && r->kind == kind && r->floor == floor) { *slot = i; return 1; }
    }
}

static int ht_grow(void) {
    uint32_t ns = htsize ? htsize * 2 : 8192;
    uint32_t *n = calloc(ns, sizeof(uint32_t));
    if (!n) return 0;
    free(ht);
    ht = n;
    htsize = ns;
    for (uint32_t k = 0; k < nrecs; k++) {
        uint32_t s;
        ht_find(recs[k].kind, recs[k].floor, recs[k].off, recs[k].len, &s);
        ht[s] = k + 1;
    }
    return 1;
}

/* called with lock held */
static void add_rec(int kind, int floor, int64_t off, uint32_t len) {
    if (nrecs >= MAX_RECS) return;
    if ((nrecs + 1) * 2 > htsize && !ht_grow()) return;
    uint32_t s;
    if (ht_find(kind, floor, off, len, &s)) return;
    if (nrecs >= caprecs) {
        uint32_t nc = caprecs ? caprecs * 2 : 4096;
        rec_t *n = realloc(recs, nc * sizeof(rec_t));
        if (!n) return;
        recs = n;
        caprecs = nc;
    }
    recs[nrecs] = (rec_t){off, len, (uint8_t)kind, (uint8_t)floor, 0};
    ht[s] = ++nrecs;
}

static const char *base_name(const char *p) {
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

static int kind_for_name(const char *bn) {
    for (int k = 0; k < nkinds; k++) if (!strcmp(names[k], bn)) return k;
    if (nkinds >= MAX_KINDS) return -1;
    char full[256];
    snprintf(full, sizeof(full), DATA_PATH "%s", bn);
    snprintf(names[nkinds], sizeof(names[0]), "%s", bn);
    sizes[nkinds] = obb_shared_size(full);
    return nkinds++;
}

void texpf_note_read(int obb, int64_t off, uint32_t len) {
    if (!on || obb < 0 || obb > 3) return;
    pthread_mutex_lock(&lock);
    int k = kind_of_obb[obb];
    if (k < 0) {
        const char *p = io_obb_path(obb);
        if (p) k = kind_of_obb[obb] = (int8_t)kind_for_name(base_name(p));
    }
    if (k >= 0) add_rec(k, cur_floor, off, len);
    pthread_mutex_unlock(&lock);
}

int texpf_busy(void) { return loading_depth > 0; }
uint64_t texpf_quiet_us(void) { return loading_depth > 0 ? 0 : sceKernelGetProcessTimeWide() - last_load_end; }
void texpf_loading(int delta) {
    if (__sync_add_and_fetch(&loading_depth, delta) <= 0) last_load_end = sceKernelGetProcessTimeWide();
}

static int parse_floor(const char *name) {
    const char *b = name;
    for (const char *p = name; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    if (!strncasecmp(b, "HOR_frontEnd", 12)) return 0;
    if (strncasecmp(b, "HOR_fl", 6) || !isdigit((unsigned char)b[6])) return -1;
    char *end;
    long f = strtol(b + 6, &end, 10);
    if (strncasecmp(end, "_persistent", 11) || f < 0 || f > 250) return -1;
    return (int)f;
}

void texpf_package_loaded(const char *name) {
    if (!on || !name) return;
    int f = parse_floor(name);
    if (f < 0) return;
    cur_floor = f;
    want_floor = f;
    __sync_fetch_and_add(&gen, 1);
    if (sema >= 0) sceKernelSignalSema(sema, 1);
}

/* ---- file ---- */
static void load_file(void) {
    FILE *f = fopen(PF_FILE, "rb");
    if (!f) { l_info("[texpf] no %s yet: texture reads will be recorded from now on", PF_FILE); return; }
    hdr_t h;
    if (fread(&h, sizeof(h), 1, f) != 1 || memcmp(h.magic, "DMTP", 4) || h.version != 1 || h.nkinds > MAX_KINDS) {
        l_warn("[texpf] %s is not a texture prefetch file of this version, starting over", PF_FILE);
        fclose(f);
        return;
    }
    int map[MAX_KINDS];
    for (uint32_t k = 0; k < h.nkinds; k++) {
        h.names[k][63] = 0;
        map[k] = kind_for_name(h.names[k]);
        if (map[k] >= 0 && sizes[map[k]] != h.sizes[k]) {
            l_warn("[texpf] %s has changed size since it was recorded: its reads are dropped", h.names[k]);
            map[k] = -1;
        }
    }
    unsigned per_floor[32] = {0}, n = 0, dropped = 0;
    rec_t buf[256];
    uint32_t left = h.count < MAX_RECS ? h.count : MAX_RECS;
    while (left) {
        size_t want = left < 256 ? left : 256, got = fread(buf, sizeof(rec_t), want, f);
        if (!got) break;
        for (size_t i = 0; i < got; i++) {
            if (buf[i].kind >= h.nkinds || map[buf[i].kind] < 0) { dropped++; continue; }
            add_rec(map[buf[i].kind], buf[i].floor, buf[i].off, buf[i].len);
            per_floor[buf[i].floor < 31 ? buf[i].floor : 31]++;
            n++;
        }
        left -= (uint32_t)got;
    }
    fclose(f);
    saved_n = dropped ? 0 : nrecs;
    char fl[256];
    int o = 0;
    for (int i = 0; i < 32 && o < (int)sizeof(fl) - 24; i++)
        if (per_floor[i]) o += snprintf(fl + o, sizeof(fl) - o, "%s%s%d: %u", o ? ", " : "", i == 31 ? "31+" : "", i, per_floor[i]);
    l_info("[texpf] %u recorded texture reads loaded from %s (by floor: %s)", n, PF_FILE, o ? fl : "none");
}

static void save_file(void) {
    pthread_mutex_lock(&lock);
    uint32_t n = nrecs;
    if (n == saved_n) { pthread_mutex_unlock(&lock); return; }
    rec_t *copy = malloc((size_t)n * sizeof(rec_t) + 1);
    if (copy) memcpy(copy, recs, (size_t)n * sizeof(rec_t));
    hdr_t h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "DMTP", 4);
    h.version = 1;
    h.count = n;
    h.nkinds = (uint32_t)nkinds;
    memcpy(h.names, names, sizeof(names));
    memcpy(h.sizes, sizes, sizeof(sizes));
    pthread_mutex_unlock(&lock);
    if (!copy) return;
    uint64_t t0 = sceKernelGetProcessTimeWide();
    FILE *f = fopen(PF_TMP, "wb");
    int ok = f && fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(copy, sizeof(rec_t), n, f) == n;
    if (f) ok = fclose(f) == 0 && ok;
    free(copy);
    if (ok) {
        remove(PF_FILE);
        ok = rename(PF_TMP, PF_FILE) == 0;
    }
    if (ok) {
        saved_n = n;
        l_info("[texpf] %u texture reads saved to %s (%u ms)", (unsigned)n, PF_FILE,
               (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000));
    } else {
        l_warn("[texpf] could not write %s", PF_FILE);
    }
}

/* ---- the prefetch ---- */

/* wait until no load has been running for QUIET_US; 0 if a newer floor change came in meanwhile */
static int wait_quiet(unsigned g) {
    for (;;) {
        if (gen != g) return 0;
        if (loading_depth <= 0 && sceKernelGetProcessTimeWide() - last_load_end >= QUIET_US) return 1;
        sceKernelDelayThread(50 * 1000);
    }
}

static int cmp_rec(const void *a, const void *b) {
    const rec_t *x = a, *y = b;
    if (x->kind != y->kind) return x->kind - y->kind;
    return x->off < y->off ? -1 : x->off > y->off;
}

static void prefetch_floor(int floor, unsigned g) {
    pthread_mutex_lock(&lock);
    uint32_t n = 0;
    for (uint32_t i = 0; i < nrecs; i++) if (recs[i].floor == floor) n++;
    rec_t *list = n ? malloc(n * sizeof(rec_t)) : NULL;
    uint32_t m = 0;
    if (list) for (uint32_t i = 0; i < nrecs; i++) if (recs[i].floor == floor) list[m++] = recs[i];
    char paths[MAX_KINDS][256];
    int obb_i[MAX_KINDS];
    for (int k = 0; k < nkinds; k++) {
        snprintf(paths[k], sizeof(paths[k]), DATA_PATH "%s", names[k]);
        obb_i[k] = -1;
    }
    int nk = nkinds;
    pthread_mutex_unlock(&lock);
    if (!m) { free(list); l_info("[texpf] floor %d: nothing recorded yet", floor); return; }
    for (int k = 0; k < nk; k++) obb_i[k] = io_obb_index(paths[k]);

    /* only what the cache does not hold already */
    uint32_t w = 0;
    for (uint32_t i = 0; i < m; i++)
        if (obb_i[list[i].kind] >= 0 && !tex_cache_has(obb_i[list[i].kind], list[i].off, list[i].len)) list[w++] = list[i];
    uint32_t held = m - w;
    m = w;
    qsort(list, m, sizeof(rec_t), cmp_rec);

    uint64_t t0 = sceKernelGetProcessTimeWide(), card_us = 0, bytes_read = 0;
    unsigned added = 0, spans = 0, pauses = 0;
    uint64_t added_bytes = 0;
    int full = 0;
    uint8_t *buf = NULL;
    size_t bufsz = 0;
    for (uint32_t i = 0; i < m && !full;) {
        int64_t start = list[i].off, end = start + list[i].len;
        uint32_t j = i + 1;
        while (j < m && list[j].kind == list[i].kind && list[j].off <= end + SPAN_GAP) {
            int64_t e = list[j].off + list[j].len;
            int64_t ne = e > end ? e : end;
            if (ne - start > SPAN_MAX) break;
            end = ne;
            j++;
        }
        size_t len = (size_t)(end - start);
        if (len > bufsz) {
            free(buf);
            buf = malloc(len);
            bufsz = buf ? len : 0;
            if (!buf) break;
        }
        if (loading_depth > 0 || gen != g) {
            pauses++;
            if (!wait_quiet(g)) break;
        }
        uint64_t r0 = sceKernelGetProcessTimeWide();
        int got = obb_shared_pread(paths[list[i].kind], buf, (int)len, start);
        card_us += sceKernelGetProcessTimeWide() - r0;
        spans++;
        if (got > 0) bytes_read += (uint64_t)got;
        for (uint32_t k = i; k < j && got > 0; k++) {
            int64_t rel = list[k].off - start;
            if (rel + list[k].len > got) continue;
            int r = tex_cache_fill(obb_i[list[k].kind], list[k].off, buf + rel, list[k].len);
            if (r < 0) { full = 1; break; }
            if (r > 0) { added++; added_bytes += list[k].len; }
        }
        i = j;
        sceKernelDelayThread(2000);   /* leave the card to the game between reads */
    }
    free(buf);
    free(list);
    uint32_t ce, cb, cl;
    tex_cache_stats(&ce, &cb, &cl);
    l_info("[texpf] floor %d: %u texture reads put in the cache (%u KB; %u were there already) from %u KB read in %u "
           "pieces, %u ms on the card, %u ms in all (%u pauses for loads)%s; cache holds %u KB of %u",
           floor, added, (unsigned)(added_bytes / 1024), (unsigned)held, (unsigned)(bytes_read / 1024), spans,
           (unsigned)(card_us / 1000), (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000), pauses,
           full ? ", stopped: cache full" : gen != g ? ", stopped: floor changed" : "", (unsigned)(cb / 1024), (unsigned)(cl / 1024));
}

static int pf_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    for (;;) {
        SceUInt timeout = 60 * 1000 * 1000;
        int r = sceKernelWaitSema(sema, 1, &timeout);
        if (r < 0) {   /* timeout: save what came in, if nothing is loading */
            if (loading_depth <= 0) save_file();
            continue;
        }
        unsigned g = gen;
        int floor = want_floor;
        save_file();
        if (floor < 0 || !wait_quiet(g)) continue;
        prefetch_floor(floor, g);
    }
    return 0;
}

void texpf_init(void) {
    pthread_mutex_lock(&lock);
    ht_grow();
    load_file();
    pthread_mutex_unlock(&lock);
    sema = sceKernelCreateSema("dm_texpf", 0, 0, 1000, NULL);
    SceUID t = sema >= 0 ? sceKernelCreateThread("dm_texprefetch", pf_thread, 190, 16 * 1024, 0, 0, NULL) : -1;
    if (t < 0 || sceKernelStartThread(t, 0, NULL) < 0) {
        l_warn("[texpf] could not start the prefetch thread: off");
        return;
    }
    last_load_end = sceKernelGetProcessTimeWide();
    on = 1;
}
