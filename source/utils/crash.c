#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <so_util/so_util.h>

#include "utils/crash.h"
#include "utils/logger.h"

extern so_module so_mod;

/* Same layout as KuKernelAbortContext in upstream kubridge.h (r0-r12, sp, lr, pc, vfp, status registers). */
typedef struct {
    uint32_t r[13];
    uint32_t sp, lr, pc;
    uint64_t vfp[32];
    uint32_t spsr, fpscr, fpexc, fsr, far, abort_type;
} dm_abort_context;

typedef void (*dm_abort_handler)(dm_abort_context *);

/* Declared under our own name and bound to kubridge's symbol, so this builds against any kubridge.h
 * (the prototype gained a third parameter in newer releases). */
extern int dm_register_abort_handler(dm_abort_handler handler, dm_abort_handler *old_handler, void *opt)
    __asm__("kuKernelRegisterAbortHandler");

static int in_engine(uint32_t a) {
    return a >= so_mod.text_base && a < so_mod.text_base + so_mod.text_size;
}

static void describe(char *out, size_t n, uint32_t a) {
    if (in_engine(a))
        snprintf(out, n, "libUnrealEngine3.so+0x%x", (unsigned)(a - so_mod.text_base));
    else
        snprintf(out, n, "(not in the engine code)");
}

static void abort_handler(dm_abort_context *c) {
    char w[64];
    SceKernelThreadInfo ti;
    memset(&ti, 0, sizeof(ti));
    ti.size = sizeof(ti);
    const char *tname = (sceKernelGetThreadInfo(sceKernelGetThreadId(), &ti) >= 0) ? ti.name : "?";

    l_fatal("==================== CRASH ====================");
    l_fatal("Thread: %s   %s", tname, c->abort_type == 0 ? "data abort" : c->abort_type == 1 ? "prefetch abort" : "abort");
    describe(w, sizeof(w), c->pc);
    l_fatal("PC  = 0x%08x   %s", (unsigned)c->pc, w);
    describe(w, sizeof(w), c->lr);
    l_fatal("LR  = 0x%08x   %s", (unsigned)c->lr, w);
    l_fatal("SP  = 0x%08x   FAR (address touched) = 0x%08x   FSR = 0x%08x   SPSR = 0x%08x",
            (unsigned)c->sp, (unsigned)c->far, (unsigned)c->fsr, (unsigned)c->spsr);
    l_fatal("r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x", (unsigned)c->r[0], (unsigned)c->r[1], (unsigned)c->r[2],
            (unsigned)c->r[3], (unsigned)c->r[4], (unsigned)c->r[5]);
    l_fatal("r6=%08x r7=%08x r8=%08x r9=%08x r10=%08x r11=%08x r12=%08x", (unsigned)c->r[6], (unsigned)c->r[7],
            (unsigned)c->r[8], (unsigned)c->r[9], (unsigned)c->r[10], (unsigned)c->r[11], (unsigned)c->r[12]);

    /* Rough call stack: words on the stack that point into the engine's code. Only scan a plausible stack. */
    uint32_t sp = c->sp;
    if ((sp & 3) == 0 && sp >= 0x81000000u && sp < 0xE0000000u) {
        const uint32_t *p = (const uint32_t *)sp;
        int found = 0, words = 512;
        /* never read past the top of the thread's stack (that faulted inside this handler once) */
        uint32_t top = (uint32_t)(uintptr_t)ti.stack + (uint32_t)ti.stackSize;
        if (ti.stack && top > sp && (top - sp) / 4 < (uint32_t)words) words = (int)((top - sp) / 4);
        for (int i = 0; i < words && found < 24; i++) {
            if (in_engine(p[i])) {
                l_fatal("  stack+0x%03x: 0x%08x  libUnrealEngine3.so+0x%x", i * 4, (unsigned)p[i],
                        (unsigned)(p[i] - so_mod.text_base));
                found++;
            }
        }
    }
    l_fatal("Resolve the offsets with: python3 tools/resolve_addr.py libUnrealEngine3.so 0x<offset> ...");
    sceKernelExitProcess(0);
}

void crash_init(void) {
    int r = dm_register_abort_handler(abort_handler, NULL, NULL);
    if (r < 0) l_warn("Could not install the crash handler (0x%08x); crashes will not be logged.", r);
    else l_success("Crash handler installed.");
}
