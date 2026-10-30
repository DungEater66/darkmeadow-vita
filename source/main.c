/*
 * Dark Meadow: The Pact - PS Vita so-loader entry point.
 *
 * soloader_init_all()      load + relocate libUnrealEngine3.so, resolve its imports, init FalsoJNI and controls
 * engine_install_jni_hooks capture the natives the engine registers
 * JNI_OnLoad               engine registers natives and caches the Java callback method IDs
 * engine_start()           call the engine the way BaseUE3JavaApp does (see engine.c)
 *
 * After that the engine runs on its own threads; this thread only has to stay out of the way.
 */
#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>

#include "android/engine.h"
#include "audio/opensl.h"
#include "audio/song.h"
#include "config.h"
#include "game_info.h"
#include "input/padinput.h"
#include "input/touch.h"
#include "util/crash.h"
#include "util/init.h"
#include "util/logger.h"
#include "util/watchdog.h"

int _newlib_heap_size_user = DM_HEAP_MB * 1024 * 1024;

so_module so_mod;

/* Poll the Vita's touch screen and buttons ~120 times per second, independently of the engine's frame rate. */
static int controls_thread(SceSize args, void *argp) {
    while (1) {
        touch_poll();
        padinput_poll();   /* sticks and buttons as touches */
        sceKernelDelayThread(8333);
    }
    return 0;
}

int main() {
    crash_init();
    config_load();
    l_info("Dark Meadow loader %s, built " __DATE__ " " __TIME__, DM_LOADER_BUILD);
    soloader_init_all();
    engine_install_jni_hooks();

    int (*JNI_OnLoad)(void *jvm, void *reserved) = (void *)so_symbol(&so_mod, "JNI_OnLoad");
    if (!JNI_OnLoad) {
        l_fatal("JNI_OnLoad not found: this does not look like libUnrealEngine3.so.");
        return -1;
    }
    JNI_OnLoad(&jvm, NULL);
    l_success("JNI_OnLoad done.");

    sceIoMkdir(DATA_PATH DM_BASE_DIR, 0777);   /* the engine writes configs, logs and saves under here */

    song_init();          /* music, ambience and dialogue (the Java MediaPlayer side) */
    opensl_init();        /* sound effects: the loader's own OpenSL ES mixer */
    engine_start();
    l_success("Engine started.");

    watchdog_start();

    padinput_init();
    SceUID ct = sceKernelCreateThread("dm_controls", controls_thread, 0x40, 0x10000, 0, 0, NULL);
    if (ct >= 0) sceKernelStartThread(ct, 0, NULL);
    watchdog_register_thread("dm_controls", ct);
    watchdog_register_thread("main", sceKernelGetThreadId());

    sceKernelExitDeleteThread(0);
    return 0;
}
