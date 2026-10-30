/*
 * Start-up: the settings app on request, clocks, checks, and loading libUnrealEngine3.so.
 * Based on the so-loader template (Copyright (C) 2021 Andy Nguyen, 2021-2022 Rinnegatamante, 2022-2024 Volodymyr
 * Atamanenko; MIT license, see LICENSE).
 */
#include "util/init.h"

#include <string.h>
#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>
#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/kernel/clib.h>
#include <psp2/power.h>

#include "gfx/glutil.h"
#include "input/touch.h"
#include "util/fatal.h"
#include "util/logger.h"
#include "util/utils.h"

#define LOAD_ADDRESS 0x98000000   /* where the .so is loaded */

extern so_module so_mod;

/* The LiveArea's "Settings" button starts the game with "-config": run the settings app instead. */
static void maybe_run_configurator(void) {
    sceAppUtilInit(&(SceAppUtilInitParam){}, &(SceAppUtilBootParam){});
    SceAppUtilAppEventParam event;
    sceClibMemset(&event, 0, sizeof(event));
    sceAppUtilReceiveAppEvent(&event);
    if (event.type == 0x05) {
        char buffer[2048];
        sceAppUtilAppEventParseLiveArea(&event, buffer);
        if (strstr(buffer, "-config")) sceAppMgrLoadExec("app0:/configurator.bin", NULL, NULL);
    }
}

void soloader_init_all(void) {
    maybe_run_configurator();

    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    if (!module_loaded("kubridge")) fatal_error("Error: kubridge.skprx is not installed.");
    if (!file_exists(SO_PATH))
        fatal_error("Looks like you haven't installed the data files for this port, or they are in an incorrect "
                    "location. Please make sure that you have %s file exactly at that path.", SO_PATH);
    if (so_file_load(&so_mod, SO_PATH, LOAD_ADDRESS) < 0) fatal_error("Error: could not load %s.", SO_PATH);

    so_relocate(&so_mod);
    resolve_imports(&so_mod);
    so_patch();
    so_flush_caches(&so_mod);
    so_initialize(&so_mod);
    l_success("libUnrealEngine3.so loaded, relocated and patched.");

    gl_preload();
    jni_init();
    touch_init();
}
