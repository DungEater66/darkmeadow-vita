/*
 * Hooking functions inside libUnrealEngine3.so (see hook.h).
 *
 * so_util's hook_addr overwrites the start of a function with a jump; calling the original afterwards (SO_CONTINUE)
 * restores and re-patches it, which is not thread safe. ue3_hook instead copies the first two instructions into a
 * trampoline that ends with a jump back into the function, so the original can be called from any thread.
 */
#include "hooks/hook.h"
#include "hooks/hooks_internal.h"

#include <kubridge.h>
#include <so_util/so_util.h>

#include "util/logger.h"

extern so_module so_mod;

/* "ldr rX, [pc, #imm]" (a literal load): can be moved if its literal is copied along */
static int is_ldr_literal(uint32_t ins) {
    return (ins & 0x0F7F0000u) == 0x051F0000u && (ins >> 28) != 15 && ((ins >> 12) & 15) != 15;
}

/* anything else that reads pc gives a different result once moved */
static int is_pc_dependent(uint32_t ins) {
    if ((ins & 0xF0000000u) == 0xF0000000u) return 1;                       /* unconditional-space instructions */
    if ((ins & 0x0E000000u) == 0x0A000000u) return 1;                       /* b / bl */
    if (((ins >> 16) & 15) == 15 || ((ins >> 12) & 15) == 15) return 1;     /* reads or writes pc */
    if ((ins & 0x0E000010u) == 0x00000000u && (ins & 15) == 15) return 1;   /* pc as a shifted operand */
    return 0;
}

int ue3_hook(const char *sym, uint32_t w0, uint32_t w1, void *dst, void **orig) {
    uintptr_t addr = (uintptr_t)so_symbol(&so_mod, sym);
    if (!addr) {
        l_warn("[hook] %s not found, not hooked", sym);
        return 0;
    }
    const uint32_t *code = (const uint32_t *)addr;
    if ((addr & 1) || code[0] != w0 || (w1 && code[1] != w1)) {
        l_warn("[hook] %s starts differently than expected (%08x %08x), not hooked", sym, (unsigned)code[0],
               (unsigned)code[1]);
        return 0;
    }

    /* the two copied instructions, a jump back to the third, and room for two literals */
    uint32_t tramp[6] = { code[0], code[1], 0xE51FF004 /* ldr pc, [pc, #-4] */, (uint32_t)(addr + 8), 0, 0 };
    for (int k = 0; k < 2; k++) {
        uint32_t ins = code[k];
        if (is_ldr_literal(ins)) {
            unsigned imm = ins & 0xFFF;
            uintptr_t lit = addr + 4 * k + 8;
            lit = (ins & (1u << 23)) ? lit + imm : lit - imm;
            tramp[4 + k] = *(const uint32_t *)lit;
            /* instruction k sits at +4k and reads pc = +4k+8; its literal is now at +16+4k, so imm becomes 8 */
            tramp[k] = (ins & 0xFFFFF000u) | (1u << 23) | 8u;
        } else if (is_pc_dependent(ins)) {
            l_warn("[hook] %s starts with a pc-relative instruction (%08x), not hooked", sym, (unsigned)ins);
            return 0;
        }
    }

    uintptr_t head = (so_mod.patch_head + 3) & ~(uintptr_t)3;
    if (head + sizeof(tramp) > so_mod.patch_base + so_mod.patch_size) {
        l_warn("[hook] no room for a trampoline for %s", sym);
        return 0;
    }
    kuKernelCpuUnrestrictedMemcpy((void *)head, tramp, sizeof(tramp));
    kuKernelFlushCaches((void *)head, sizeof(tramp));
    so_mod.patch_head = head + sizeof(tramp);
    *orig = (void *)head;
    hook_addr(addr, (uintptr_t)dst);
    return 1;
}

void ue3_wstr_to_utf8(const uint32_t *w, char *out, size_t n) {
    size_t o = 0;
    for (; w && *w && o + 5 < n; w++) {
        uint32_t c = *w;
        if (c < 0x80) {
            out[o++] = (char)c;
        } else if (c < 0x800) {
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out[o++] = (char)(0xE0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (c >> 18));
            out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = 0;
}

void hooks_install(void) {
    load_profiler_install();
    engine_log_install();
    text_scale_install();
    game_state_install();
}
