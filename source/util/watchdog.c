/*
 * The watchdog thread, once a second:
 *
 *   Freezes: if no frame has been swapped for 5 s, it says so (once per freeze, then every 30 s) and lists every
 *   engine call stuck in a pthread / semaphore wait, the call sites that kept polling (timed waits, trylock, usleep,
 *   sched_yield), file reads, every engine thread with its state and CPU time, and the game thread's stack. Resolve
 *   the engine offsets with: python3 tools/resolve_addr.py libUnrealEngine3.so 0x<offset> ...
 *
 *   Performance: every REPORT_SECONDS, [cpu] (frame rate and each thread's share of a core), [gpu] (time per frame,
 *   waiting for the GPU, shader links, texture uploads) and [mem] (heap, vitaGL's pools, system memory, lowest values
 *   over the period) lines, for comparing builds.
 *
 *   Thread pinning ("Pin threads" in the settings app, DM_PIN_THREADS): see pin_threads.
 *
 * It also flushes the log and gives up a movie the engine is not presenting.
 */
#include "util/watchdog.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "android/engine.h"
#include "audio/opensl.h"
#include "config.h"
#include "gfx/gl_fixes.h"
#include "gfx/glutil.h"
#include "libc/pthr.h"
#include "storage/io_stats.h"
#include "storage/obb_stream.h"
#include "util/logger.h"
#include "video/movie.h"

#define REPORT_SECONDS 10
#define STALL_MIN_US (5 * 1000 * 1000ULL)
#define MAX_THREADS 64

/* UE3's thread entry points in Dark Meadow 1.4.6.4's libUnrealEngine3.so */
#define UE3_GAME_THREAD_ENTRY 0x12fa4f0

static const char *engine_thread_name(uintptr_t off) {
    switch (off) {
    case UE3_GAME_THREAD_ENTRY: return "UE3GameThread";
    case 0x12e5390: return "UE3ThreadPool";
    case 0x12e5554: return "UE3Runnable";
    case 0x13e945c: return "PhysX";
    default: return "engine";
    }
}

/* the loader's own threads, for the [cpu] lines and pinning */
#define MAX_EXTRA 16
static struct { SceUID uid; char name[24]; } extra[MAX_EXTRA];
static int nextra;

void watchdog_register_thread(const char *name, int uid) {
    if (uid < 0 || nextra >= MAX_EXTRA) return;
    extra[nextra].uid = uid;
    snprintf(extra[nextra].name, sizeof(extra[0].name), "%s", name);
    nextra++;
}

/* ==== freezes ==== */

/* In the kernel library, but missing from some VitaSDK headers (that gave an implicit-declaration warning). */
int sceKernelGetCondInfo(SceUID condId, SceKernelCondInfo *info);

static void report_stalls(void) {
    pthr_stall_t st[16];
    int n = pthr_collect_stalls(STALL_MIN_US, st, 16);
    for (int i = 0; i < n; i++) {
        uintptr_t off = pthr_engine_offset(st[i].caller);
        if (off)
            l_warn("[stall] blocked %u s in %s (object %p) at libUnrealEngine3.so+0x%x",
                   (unsigned)(st[i].blocked_us / 1000000), st[i].what, st[i].obj, (unsigned)off);
        else
            l_warn("[stall] blocked %u s in %s (object %p) called from 0x%x (loader code)",
                   (unsigned)(st[i].blocked_us / 1000000), st[i].what, st[i].obj, (unsigned)st[i].caller);
    }
    if (n == 0) l_warn("[stall] no thread is blocked in a pthread/semaphore call (busy loop or GPU wait?)");
}

/* Call sites that kept polling while no frame came out: a thread spinning on a timed wait, trylock, usleep or
 * sched_yield is waiting for something, and the busiest site usually says what. */
static void report_busy(unsigned seconds) {
    pthr_busy_t b[8];
    int n = pthr_collect_busy(b, 8);
    for (int i = 0; i < n; i++) {
        uintptr_t off = pthr_engine_offset(b[i].caller);
        if (off)
            l_warn("[busy] %u calls in %u s to %s at libUnrealEngine3.so+0x%x", b[i].count, seconds, b[i].what, (unsigned)off);
        else
            l_warn("[busy] %u calls in %u s to %s from 0x%x (loader code)", b[i].count, seconds, b[i].what, (unsigned)b[i].caller);
    }
}

/* File reads since the freeze began: steady reads mean the engine is loading (slowly), none mean it is stuck. */
static void report_io(unsigned seconds) {
    unsigned calls;
    uint64_t bytes;
    uintptr_t last;
    io_stats_collect(&calls, &bytes, &last);
    uintptr_t off = pthr_engine_offset(last);
    l_warn("[io] %u file reads, %u KiB in %u s%s", calls, (unsigned)(bytes / 1024), seconds,
           calls ? "" : " (nothing is being loaded)");
    if (calls && off) l_warn("[io] last read from libUnrealEngine3.so+0x%x", (unsigned)off);
}

static const char *thread_state(int st) {
    switch (st) {
        case SCE_THREAD_RUNNING: return "running";
        case SCE_THREAD_READY:   return "ready (wants CPU)";
        case SCE_THREAD_STANDBY: return "standby";
        case SCE_THREAD_WAITING: return "waiting";
        case SCE_THREAD_DORMANT: return "dormant";
        default:                 return "other";
    }
}

/* What a waiting thread waits on: the kernel object's name says whose it is (vitaGL, pthreads, newlib...). */
static void describe_wait(SceUID id, char *out, size_t n) {
    SceKernelSemaInfo sema;           memset(&sema, 0, sizeof(sema)); sema.size = sizeof(sema);
    if (sceKernelGetSemaInfo(id, &sema) >= 0) { snprintf(out, n, "semaphore \"%s\"", sema.name); return; }
    SceKernelEventFlagInfo evf;       memset(&evf, 0, sizeof(evf)); evf.size = sizeof(evf);
    if (sceKernelGetEventFlagInfo(id, &evf) >= 0) { snprintf(out, n, "event flag \"%s\"", evf.name); return; }
    SceKernelMutexInfo mtx;           memset(&mtx, 0, sizeof(mtx)); mtx.size = sizeof(mtx);
    if (sceKernelGetMutexInfo(id, &mtx) >= 0) { snprintf(out, n, "mutex \"%s\"", mtx.name); return; }
    SceKernelCondInfo cnd;            memset(&cnd, 0, sizeof(cnd)); cnd.size = sizeof(cnd);
    if (sceKernelGetCondInfo(id, &cnd) >= 0) { snprintf(out, n, "condition \"%s\"", cnd.name); return; }
    snprintf(out, n, "object 0x%x", (unsigned)id);
}

/* Stack scan of a frozen thread: the kernel does not give us its registers, but its stack is ordinary memory. Code
 * addresses found there (from the top of the stack down) are its call chain, mixed with some stale values. */

static uintptr_t eboot_lo, eboot_hi;

static void find_eboot_text(void) {
    SceUID mod = sceKernelGetModuleIdByAddr((void *)&find_eboot_text);
    if (mod < 0) return;
    SceKernelModuleInfo mi;
    memset(&mi, 0, sizeof(mi));
    mi.size = sizeof(mi);
    if (sceKernelGetModuleInfo(mod, &mi) < 0) return;
    eboot_lo = (uintptr_t)mi.segments[0].vaddr;
    eboot_hi = eboot_lo + mi.segments[0].memsz;
}

static void dump_stack(const char *label, void *stack, int size) {
    uintptr_t lo = (uintptr_t)stack, hi = lo + (uintptr_t)size;
    if (!stack || size <= 0 || size > 16 * 1024 * 1024 || (lo & 3)) return;
    /* (thread stacks are ordinary user memory; sceKernelFindMemBlockByAddr does not know them, so no check here) */
    char line[200];
    int len = 0, found = 0;
    l_warn("[stack] %s: code addresses on its stack, outermost first (E = libUnrealEngine3.so, L = loader eboot)", label);
    for (uintptr_t a = hi - 4; a >= lo && found < 96; a -= 4) {
        uintptr_t v = *(volatile uintptr_t *)a;
        uintptr_t off = pthr_engine_offset(v & ~1u);
        char tag;
        if (off) tag = 'E';
        else if (eboot_lo && v >= eboot_lo && v < eboot_hi) { tag = 'L'; off = v - eboot_lo; }
        else continue;
        len += snprintf(line + len, sizeof(line) - len, " %c+0x%x", tag, (unsigned)off);
        found++;
        if (found % 8 == 0) { l_warn("[stack] %s", line); len = 0; }
    }
    if (len) l_warn("[stack] %s", line);
}

/* Every engine thread with its state and the CPU time it used since the previous report: a thread that is
 * "running"/"ready" and burning CPU while nothing is drawn is the one to look at. */
static int prev_tid[MAX_THREADS];
static uint64_t prev_clock[MAX_THREADS];

static void report_threads(unsigned seconds, int quiet, int dump_stacks) {
    pthr_thread_t t[MAX_THREADS];
    int n = pthr_list_threads(t, MAX_THREADS);
    for (int i = 0; i < n; i++) {
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(t[i].tid, &info) < 0) {
            if (!quiet) l_warn("[thread] tid 0x%08x entry +0x%-7x has exited", (unsigned)t[i].tid, (unsigned)pthr_engine_offset(t[i].start));
            continue;
        }
        uint64_t clk = info.runClocks;
        uint64_t used = (prev_tid[i] == t[i].tid && clk >= prev_clock[i]) ? clk - prev_clock[i] : clk;
        prev_tid[i] = t[i].tid;
        prev_clock[i] = clk;
        if (quiet) continue;
        char wait[96] = "";
        if (info.status == SCE_THREAD_WAITING) {
            char what[48];
            describe_wait(info.waitId, what, sizeof(what));
            snprintf(wait, sizeof(wait), " on %s (type 0x%x)", what, (unsigned)info.waitType);
        }
        l_warn("[thread] tid 0x%08x entry +0x%-7x %-17s prio %3d cpu %5u ms in %u s%s", (unsigned)t[i].tid,
               (unsigned)pthr_engine_offset(t[i].start), thread_state(info.status),
               (int)info.currentPriority, (unsigned)(used / 1000), seconds, wait);
        if (dump_stacks && pthr_engine_offset(t[i].start) == UE3_GAME_THREAD_ENTRY)
            dump_stack("UE3GameThread", info.stack, info.stackSize);
    }
}

/* ==== performance reports ==== */

/* [cpu]: every engine thread and every registered loader thread, with the share of one core it used over the period,
 * the core it last ran on, and its affinity mask (0 = any core, the kernel decides). */
#define CPU_SLOTS 96
static SceUID cpu_uid[CPU_SLOTS];
static uint64_t cpu_prev[CPU_SLOTS];

static uint64_t cpu_delta(SceUID uid, uint64_t now) {
    int free_i = -1;
    for (int i = 0; i < CPU_SLOTS; i++) {
        if (cpu_uid[i] == uid) { uint64_t d = now >= cpu_prev[i] ? now - cpu_prev[i] : now; cpu_prev[i] = now; return d; }
        if (!cpu_uid[i] && free_i < 0) free_i = i;
    }
    if (free_i >= 0) { cpu_uid[free_i] = uid; cpu_prev[free_i] = now; }
    return now;
}

static void cpu_line(const char *name, SceUID uid, unsigned period_s, uint64_t *per_core) {
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(uid, &info) < 0) return;
    uint64_t used = cpu_delta(uid, info.runClocks);
    unsigned pct10 = (unsigned)(used / (period_s * 1000ull));   /* tenths of a percent of one core */
    int core = info.lastExecutedCpuId;
    if (core >= 0 && core < 4) per_core[core] += used;
    l_info("[cpu]   %-16s 0x%08x %3u.%u%%  core %d  mask 0x%x  prio %d", name, (unsigned)uid, pct10 / 10, pct10 % 10,
           core, (unsigned)info.currentCpuAffinityMask, (int)info.currentPriority);
}

static unsigned frames_in_period;

static void cpu_report(unsigned period_s, int baseline_only) {
    uint64_t per_core[4] = { 0 };
    if (baseline_only) {
        pthr_thread_t t[MAX_THREADS];
        int n = pthr_list_threads(t, MAX_THREADS);
        SceKernelThreadInfo info;
        for (int i = 0; i < n; i++) {
            memset(&info, 0, sizeof(info)); info.size = sizeof(info);
            if (sceKernelGetThreadInfo(t[i].tid, &info) >= 0) cpu_delta(t[i].tid, info.runClocks);
        }
        for (int i = 0; i < nextra; i++) {
            memset(&info, 0, sizeof(info)); info.size = sizeof(info);
            if (sceKernelGetThreadInfo(extra[i].uid, &info) >= 0) cpu_delta(extra[i].uid, info.runClocks);
        }
        return;
    }
    l_info("[cpu] last %u s, %u frames (%u.%u fps); threaded rendering: %s", period_s, frames_in_period,
           frames_in_period * 10 / period_s / 10, frames_in_period * 10 / period_s % 10,
           engine_threaded_rendering());
    {
        static uint64_t prev_swap;
        uint64_t sw = engine_swap_us_total, d = sw - prev_swap;
        prev_swap = sw;
        uint32_t mx = engine_frame_us_max;
        engine_frame_us_max = 0;
        if (frames_in_period) {
            unsigned frame_us = (unsigned)(period_s * 1000000ull / frames_in_period);
            unsigned swap_us = (unsigned)(d / frames_in_period);
            static uint32_t prev_fbo;
            uint32_t fb = dm_fbo_binds, dfb = fb - prev_fbo;
            prev_fbo = fb;
            l_info("[gpu] per frame: %u.%u ms in all, %u.%u ms waiting in vglSwapBuffers (%u%%), longest frame %u.%u ms; "
                   "offscreen framebuffer binds %u", frame_us / 1000, frame_us / 100 % 10, swap_us / 1000, swap_us / 100 % 10,
                   frame_us ? swap_us * 100 / frame_us : 0, (unsigned)(mx / 1000), (unsigned)(mx / 100 % 10), (unsigned)dfb);
        }
        static uint32_t p_ln, p_lu, p_tn, p_tu, p_tb;
        uint32_t ln = dm_link_n - p_ln, lu = dm_link_us - p_lu, tn = dm_tex_n - p_tn, tu = dm_tex_us - p_tu,
                 tb = dm_tex_bytes - p_tb, lm = dm_link_max_us;
        p_ln = dm_link_n; p_lu = dm_link_us; p_tn = dm_tex_n; p_tu = dm_tex_us; p_tb = dm_tex_bytes;
        dm_link_max_us = 0;
        if (ln || tn)
            l_info("[gpu] render thread: %u shader programs linked in %u ms (longest %u ms), %u texture levels uploaded "
                   "(%u KB) in %u ms", (unsigned)ln, (unsigned)(lu / 1000), (unsigned)(lm / 1000), (unsigned)tn,
                   (unsigned)(tb / 1024), (unsigned)(tu / 1000));
    }
    pthr_thread_t t[MAX_THREADS];
    int n = pthr_list_threads(t, MAX_THREADS);
    for (int i = 0; i < n; i++) cpu_line(engine_thread_name(pthr_engine_offset(t[i].start)), t[i].tid, period_s, per_core);
    for (int i = 0; i < nextra; i++) cpu_line(extra[i].name, extra[i].uid, period_s, per_core);
    l_info("[cpu]   by last core: 0 %u%%, 1 %u%%, 2 %u%%, 3 %u%% (only the threads above)",
           (unsigned)(per_core[0] / (period_s * 10000ull)), (unsigned)(per_core[1] / (period_s * 10000ull)),
           (unsigned)(per_core[2] / (period_s * 10000ull)), (unsigned)(per_core[3] / (period_s * 10000ull)));
}

/* [mem]:
 *   heap: the newlib heap that holds the engine's and the loader's allocations (reserved at start, DM_HEAP_MB);
 *   vitaGL: free video RAM (CDRAM) and free main RAM in vitaGL's own pools (textures, buffers, shaders), sampled on
 *     the rendering thread (gl_fixes_frame): the pools are not meant to be walked while vitaGL allocates;
 *   system: what the kernel still has free for the process.
 * Sampled every second; the lowest value of the period is shown. */
extern int _newlib_heap_size_user;
typedef struct { uint32_t heap_used, heap_free, vram, vgl_ram, sys_user, sys_cdram, sys_phycont; } memsnap_t;
static memsnap_t mem_min;
static int mem_have;

static void mem_sample(memsnap_t *m) {
    struct mallinfo mi = mallinfo();
    uint32_t arena = (uint32_t)mi.arena, free_in = (uint32_t)mi.fordblks;
    m->heap_used = arena - free_in;
    m->heap_free = (uint32_t)_newlib_heap_size_user > m->heap_used ? (uint32_t)_newlib_heap_size_user - m->heap_used : 0;
    m->vram = dm_vram_free;
    m->vgl_ram = dm_vglram_free;
    SceKernelFreeMemorySizeInfo fi;
    memset(&fi, 0, sizeof(fi));
    fi.size = sizeof(fi);
    if (sceKernelGetFreeMemorySize(&fi) >= 0) {
        m->sys_user = (uint32_t)fi.size_user;
        m->sys_cdram = (uint32_t)fi.size_cdram;
        m->sys_phycont = (uint32_t)fi.size_phycont;
    }
}

static void mem_tick(void) {
    memsnap_t m;
    mem_sample(&m);
    if (!mem_have) { mem_min = m; mem_have = 1; return; }
    if (m.heap_free < mem_min.heap_free) { mem_min.heap_free = m.heap_free; mem_min.heap_used = m.heap_used; }
    if (m.sys_user < mem_min.sys_user) mem_min.sys_user = m.sys_user;
    if (m.sys_cdram < mem_min.sys_cdram) mem_min.sys_cdram = m.sys_cdram;
    if (m.sys_phycont < mem_min.sys_phycont) mem_min.sys_phycont = m.sys_phycont;
}

#define MB10(x) (unsigned)((x) / (1024u * 1024u)), (unsigned)(((x) % (1024u * 1024u)) * 10u / (1024u * 1024u))
static void mem_report(void) {
    memsnap_t now;
    mem_sample(&now);
    mem_min.vram = dm_vram_min ? dm_vram_min : now.vram;
    mem_min.vgl_ram = dm_vglram_min ? dm_vglram_min : now.vgl_ram;
    dm_vram_min = 0;
    dm_vglram_min = 0;
    l_info("[mem] heap %u.%u MB used, %u.%u MB free (lowest %u.%u) | vitaGL free: VRAM %u.%u MB (lowest %u.%u), RAM %u.%u MB "
           "(lowest %u.%u) | system free: user %u.%u MB, CDRAM %u.%u MB, phycont %u.%u MB",
           MB10(now.heap_used), MB10(now.heap_free), MB10(mem_min.heap_free), MB10(now.vram), MB10(mem_min.vram),
           MB10(now.vgl_ram), MB10(mem_min.vgl_ram), MB10(now.sys_user), MB10(now.sys_cdram), MB10(now.sys_phycont));
    mem_have = 0;
}

/* ==== thread pinning ====
 * With every thread free to run on any of the three cores (mask 0), the scheduler sometimes puts the game thread and
 * the rendering thread on the same core. Pinned: the game thread (game logic, and the package loading it does itself
 * while it waits in FlushAsyncLoading) alone on core 0, the rendering thread on core 1, everything else on cores 1-2 -
 * the engine's I/O and worker threads, PhysX, the loader's audio, controls and watchdog threads - so they can use the
 * rendering core whenever it waits (as it does through a load) but never take time from the game thread. Checked every
 * second, so threads started later are pinned too. */
static int pin_enabled;
volatile int dm_render_tid = -1;   /* set by JavaCallback_swapBuffers: the thread that swaps buffers */
#define CORE0 0x10000
#define CORE1 0x20000
#define CORE2 0x40000
static void pin_one(int tid, int mask, const char *name) {
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(tid, &info) < 0 || (int)info.currentCpuAffinityMask == mask) return;
    int r = sceKernelChangeThreadCpuAffinityMask(tid, mask);
    static int n;
    if (n++ < 60) l_info("[cpu] pinned %s (0x%08x) to %s%s", name, (unsigned)tid,
                         mask == CORE0 ? "core 0" : mask == CORE1 ? "core 1" : "cores 1-2", r < 0 ? " - FAILED" : "");
}
static void pin_threads(void) {
    if (!pin_enabled) return;
    pthr_thread_t t[MAX_THREADS];
    int n = pthr_list_threads(t, MAX_THREADS);
    for (int i = 0; i < n; i++) {
        const char *name = engine_thread_name(pthr_engine_offset(t[i].start));
        int mask = !strcmp(name, "UE3GameThread") ? CORE0 : t[i].tid == dm_render_tid ? CORE1 : (CORE1 | CORE2);
        pin_one(t[i].tid, mask, t[i].tid == dm_render_tid ? "rendering thread" : name);
    }
    for (int i = 0; i < nextra; i++) pin_one(extra[i].uid, extra[i].uid == dm_render_tid ? CORE1 : (CORE1 | CORE2), extra[i].name);
}

/* ==== the thread ==== */

static int watchdog_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    find_eboot_text();
    if (eboot_lo) l_debug("[watchdog] loader code at 0x%x-0x%x", (unsigned)eboot_lo, (unsigned)eboot_hi);
    unsigned last_frames = engine_frames, still = 0, tick = 0, reported = 0, cpu_frames = engine_frames;
    cpu_report(0, 1);
    while (1) {
        sceKernelDelayThread(1000 * 1000);
        log_flush();   /* buffered log lines reach the card within a second */
        pin_threads();
        tick++;
        unsigned f = engine_frames;
        if (f != last_frames) {
            if (reported) l_info("[watchdog] frames are moving again (%u)", f);
            last_frames = f;
            still = 0;
            reported = 0;
            pthr_busy_t discard[1];
            pthr_collect_busy(discard, 1);   /* only count polling and reads done while frozen */
            unsigned c; uint64_t b; uintptr_t l;
            io_stats_collect(&c, &b, &l);
            report_threads(1, 1, 0);            /* only sets the CPU-time baseline */
        } else if (++still == 5 || (still > 5 && still % 30 == 0)) {
            l_warn("[watchdog] no frame swapped for %u s (frames so far %u, touches %u)", still, f, engine_touches);
            uint64_t sw = engine_swap_since_us;
            if (sw)
                l_warn("[gpu] the game thread has been inside vglSwapBuffers for %u s (GPU hang?)",
                       (unsigned)((sceKernelGetProcessTimeWide() - sw) / 1000000));
            report_stalls();
            report_busy(reported ? 30 : 5);
            report_io(reported ? 30 : 5);
            report_threads(reported ? 30 : 5, 0, still == 5 || still == 30);
            opensl_report();
            obb_report();
            reported = 1;
        }
        if (tick % 30 == 0) l_debug("[watchdog] alive: %u frames, %u touches", f, engine_touches);
        mem_tick();
        movie_check_stalled();
        if (tick % REPORT_SECONDS == 0) {
            frames_in_period = f - cpu_frames;
            cpu_frames = f;
            cpu_report(REPORT_SECONDS, 0);
            mem_report();
            io_time_report(REPORT_SECONDS);
        }
    }
    return 0;
}

void watchdog_start(void) {
    pin_enabled = config_bool("DM_PIN_THREADS", 0);
    if (pin_enabled) l_info("[cpu] threads pinned: game thread on core 0, rendering thread on core 1, the rest on 1-2");
    SceUID t = sceKernelCreateThread("dm_watchdog", watchdog_thread, 0x10000100, 0x4000, 0, 0, NULL);
    if (t >= 0) {
        sceKernelStartThread(t, 0, NULL);
        watchdog_register_thread("dm_watchdog", t);
    }
}
