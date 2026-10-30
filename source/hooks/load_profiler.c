/*
 * Loading, garbage collection and the files the engine asks for.
 *
 * What the game thread does during a long frame: synchronous package loads (UObject::LoadPackage), waits for the
 * async loader (FlushAsyncLoading) and garbage collection (CollectGarbage) are timed, and anything over HITCH_MS is
 * logged as [hitch], loads with a breakdown of where their time went (storage/load_stats.h). Async load requests are
 * listed too, so a hitch can be matched with what was asked for. The texture prefetch is told when loads start and
 * end, so it keeps off the memory card meanwhile.
 *
 * Back-to-back garbage collections: the game's scripts often ask for a collection right after another (mostly as a
 * fight ends), each one a 30-70 ms freeze that frees next to nothing. A normal collection less than 2 s after the last
 * one is skipped; the memory is freed at the next. Never two skips in a row, and full purges (level changes, which
 * unload areas) always run. Settings app: "Skip repeated clean-ups" (DM_GC_SKIP_REPEAT).
 */
#include <stdio.h>
#include <psp2/kernel/processmgr.h>

#include "config.h"
#include "hooks/hook.h"
#include "hooks/hooks_internal.h"
#include "storage/load_stats.h"
#include "storage/tex_prefetch.h"
#include "util/logger.h"

#define HITCH_MS 30
#define MAX_HITCH_LINES 300
#define GC_REPEAT_US 2000000ull

static unsigned hitch_lines;
static int gc_skip_repeat = 1;

static uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }

static void report(const char *what, const char *detail, uint64_t t0) {
    uint32_t ms = (uint32_t)((now_us() - t0) / 1000);
    if (ms >= HITCH_MS && hitch_lines++ < MAX_HITCH_LINES)
        l_info("[hitch] %s%s%s: %u ms", what, detail ? " " : "", detail ? detail : "", (unsigned)ms);
}

/* Loads can nest (a LoadPackage inside a flush): only the outermost gets a breakdown. */
static volatile int load_depth;

static void report_load(const char *what, const char *detail, const load_snap_t *start, int outermost) {
    uint32_t ms = (uint32_t)((now_us() - start->t_us) / 1000);
    if (ms < HITCH_MS || hitch_lines++ >= MAX_HITCH_LINES) return;
    char breakdown[1100];
    breakdown[0] = 0;
    if (outermost) load_breakdown(start, breakdown, sizeof(breakdown));
    l_info("[hitch] %s%s%s: %u ms%s", what, detail ? " " : "", detail ? detail : "", (unsigned)ms, breakdown);
}

static void *(*o_LoadPackage)(void *, const uint32_t *, unsigned);
static void *h_LoadPackage(void *outer, const uint32_t *file, unsigned flags) {
    load_snap_t start;
    load_snap(&start);
    int outermost = __sync_add_and_fetch(&load_depth, 1) == 1;
    texpf_loading(1);
    void *r = o_LoadPackage(outer, file, flags);
    texpf_loading(-1);
    __sync_sub_and_fetch(&load_depth, 1);
    char name[160];
    ue3_wstr_to_utf8(file, name, sizeof(name));
    report_load("LoadPackage", name, &start, outermost);
    texpf_package_loaded(name);
    return r;
}

static void (*o_FlushAsyncLoading)(uint32_t, uint32_t);
static void h_FlushAsyncLoading(uint32_t a, uint32_t b) {
    load_snap_t start;
    load_snap(&start);
    int outermost = __sync_add_and_fetch(&load_depth, 1) == 1;
    texpf_loading(1);
    o_FlushAsyncLoading(a, b);
    texpf_loading(-1);
    __sync_sub_and_fetch(&load_depth, 1);
    report_load("FlushAsyncLoading (waited for the async loader)", NULL, &start, outermost);
}

static int (*o_LoadPackageAsync)(const ue3_fstring_t *, void *, void *, const void *, uint32_t, uint32_t);
static int h_LoadPackageAsync(const ue3_fstring_t *name, void *cb, void *ud, const void *guid, uint32_t t0, uint32_t t1) {
    static int n;
    if (n++ < 200 && name && name->data) {
        char b[160];
        ue3_wstr_to_utf8(name->data, b, sizeof(b));
        l_info("[hitch] async load requested: %s", b);
    }
    return o_LoadPackageAsync(name, cb, ud, guid, t0, t1);
}

static void (*o_CollectGarbage)(uint64_t, unsigned);
static void h_CollectGarbage(uint64_t keep_flags, unsigned full_purge) {
    static uint64_t last_end;
    static int skipped_last;
    uint64_t t0 = now_us();
    if (gc_skip_repeat && !full_purge && !skipped_last && last_end && t0 - last_end < GC_REPEAT_US) {
        skipped_last = 1;
        static unsigned skipped;
        if (skipped++ < 200)
            l_info("[hitch] CollectGarbage skipped: %u ms after the last one (%u skipped so far)",
                   (unsigned)((t0 - last_end) / 1000), skipped);
        return;
    }
    skipped_last = 0;
    o_CollectGarbage(keep_flags, full_purge);
    last_end = now_us();
    report("CollectGarbage", full_purge ? "(full purge)" : NULL, t0);
}

/* ---- the files the engine asks for ---- */

static void *(*o_unzOpen2)(const char *, void *);
static void *h_unzOpen2(const char *path, void *funcs) {
    void *r = o_unzOpen2(path, funcs);
    l_info("[ue3] unzOpen2(\"%s\") -> %p%s", path ? path : "(null)", r, r ? "" : "   <-- FAILED to open as a zip");
    return r;
}

static int (*o_FileSize)(void *, const uint32_t *);
static int h_FileSize(void *self, const uint32_t *file) {
    int r = o_FileSize(self, file);
    char b[512];
    ue3_wstr_to_utf8(file, b, sizeof(b));
    l_info("[ue3] FileSize(\"%s\") -> %d", b, r);
    return r;
}

static void (*o_FindFiles)(void *, void *, const uint32_t *, unsigned, unsigned);
static void h_FindFiles(void *self, void *result, const uint32_t *pattern, unsigned files, unsigned dirs) {
    o_FindFiles(self, result, pattern, files, dirs);
    char b[512];
    ue3_wstr_to_utf8(pattern, b, sizeof(b));
    /* result is a TArray<FString>: { Data, ArrayNum, ArrayMax } */
    l_info("[ue3] FindFiles(\"%s\", files=%u, dirs=%u) -> %d result(s)", b, files, dirs, result ? ((int *)result)[1] : -1);
}

void load_profiler_install(void) {
    gc_skip_repeat = config_bool("DM_GC_SKIP_REPEAT", 1);
    int n = 0;
    n += ue3_hook("_ZN7UObject11LoadPackageEP8UPackagePKwm", 0xE92D4FF0, 0, h_LoadPackage, (void **)&o_LoadPackage);
    n += ue3_hook("_ZN7UObject17FlushAsyncLoadingE5FName", 0xE59F308C, 0xE92D4030, h_FlushAsyncLoading,
                  (void **)&o_FlushAsyncLoading);
    n += ue3_hook("_ZN7UObject14CollectGarbageEyj", 0xE92D4FF0, 0, h_CollectGarbage, (void **)&o_CollectGarbage);
    n += ue3_hook("_ZN7UObject16LoadPackageAsyncERK7FStringPFvPS_PvES4_PK5FGuid5FName", 0xE92D4FF0, 0xE24DD034,
                  h_LoadPackageAsync, (void **)&o_LoadPackageAsync);
    l_info("[hitch] profiler: %d of 4 engine functions timed (over %d ms logged); repeated clean-ups %s", n, HITCH_MS,
           gc_skip_repeat ? "skipped" : "kept");

    int f = 0;
    f += ue3_hook("unzOpen2", 0xE92D4FF0, 0xE251E000, h_unzOpen2, (void **)&o_unzOpen2);
    f += ue3_hook("_ZN19FFileManagerAndroid8FileSizeEPKw", 0xE92D4FF0, 0xE1A05000, h_FileSize, (void **)&o_FileSize);
    f += ue3_hook("_ZN19FFileManagerAndroid9FindFilesER6TArrayI7FString17FDefaultAllocatorEPKwjj", 0xE92D4FF0,
                  0xE1A05000, h_FindFiles, (void **)&o_FindFiles);
    l_info("[ue3] tracing %d of 3 engine file functions", f);
}
