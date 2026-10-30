/*
 * Copyright (C) 2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/*
 * Patches to libUnrealEngine3.so after it is loaded: Android's kernel helpers for atomics, and every hook of the
 * loader's own.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <kubridge.h>
#include <so_util/so_util.h>
#include <vitasdk.h>

#include "gfx/gl_fixes.h"
#include "hooks/hook.h"
#include "libc/sys.h"
#include "libc/ue3_stdio.h"
#include "storage/pkg_cache.h"
#include "util/fatal.h"
#include "util/init.h"
#include "util/logger.h"

extern so_module so_mod;

#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX (0x0C20D050)

#include "util/logger.h"
#include "util/fatal.h"
#include "libc/sys.h"
#include <stdbool.h>

/* Android's "kuser helpers" live at fixed addresses (0xFFFF0FA0 memory barrier, 0xFFFF0FC0 cmpxchg) that do not
 * exist on the Vita. A page is mapped at 0x9A000000 with the same layout, and every reference to them is redirected. */
void __kuser_memory_barrier(void) {
	__sync_synchronize();
}

static void kuser_patch(void) {
	SceKernelAllocMemBlockKernelOpt opt;
	memset(&opt, 0, sizeof(SceKernelAllocMemBlockKernelOpt));
	opt.size = sizeof(SceKernelAllocMemBlockKernelOpt);
	opt.attr = 0x1;
	opt.field_C = (SceUInt32)0x9A000000;
	if (kuKernelAllocMemBlock("atomic", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, 0x1000, &opt) < 0)
		fatal_error("Error could not allocate atomic block.");
	kuKernelMemProtect((void *)0x9A000000, (SceSize)0x1000, KU_KERNEL_PROT_EXEC | KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE);

	hook_addr(0x9A000FA0, (uintptr_t)__kuser_memory_barrier);
	hook_addr(0x9A000FC0, (uintptr_t)__atomic_cmpxchg);

	uint32_t patched_addr;
	for (uint32_t addr = so_mod.text_base; addr < so_mod.text_base + so_mod.text_size; addr += 4) {
		uint32_t *a = (uint32_t *)addr;
		if (*a == 0xFFFF0FC0) {
			l_debug("Patching %p -> __kuser_cmpxchg", (void *)a);
			patched_addr = 0x9A000FC0;
			kuKernelCpuUnrestrictedMemcpy((void *)(addr), &patched_addr, sizeof(uint32_t));
		}
		else if (*a == 0xFFFF0FA0) {
			l_debug("Patching %p -> __kuser_memory_barrier", (void *)a);
			patched_addr = 0x9A000FA0;
			kuKernelCpuUnrestrictedMemcpy((void *)(addr), &patched_addr, sizeof(uint32_t));
		}
	}
}

void so_patch(void) {
	kuser_patch();
	ue3_stdio_hook_all();        // the engine carries its own bionic stdio: see libc/ue3_stdio.h
	hooks_install();             // engine log, load profiler, text size, game state (hooks/)
	gl_fixes_install_hooks();    // graphics profile (gfx/gl_fixes.c)
	pkg_cache_install_hooks();   // package cache (storage/pkg_cache.c)
}