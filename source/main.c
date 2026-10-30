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
#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "reimpl/controls.h"
#include "dm_config.h"
#include "engine.h"
#include "utils/crash.h"
#include "utils/watchdog.h"
#include "reimpl/opensl.h"
#include "song.h"
#include "padinput.h"

#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

int _newlib_heap_size_user = DM_HEAP_MB * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
#endif

so_module so_mod;

/* Poll the Vita's touch screen and buttons ~120 times per second, independently of the engine's frame rate. */
static int controls_thread(SceSize args, void *argp) {
    while (1) {
        controls_poll();
        padinput_poll();   /* r59: sticks and buttons as touches */
        sceKernelDelayThread(8333);
    }
    return 0;
}

int main() {
    crash_init();
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

/* ---- input ----------------------------------------------------------------------------------- */

void controls_handler_touch(int32_t id, float x, float y, ControlsAction action) {
    /* Android MotionEvent actions: 0 = down, 1 = up, 2 = move */
    int a = (action == CONTROLS_ACTION_DOWN) ? 0 : (action == CONTROLS_ACTION_UP) ? 1 : 2;
    if (a == 0 && padinput_log_touches) l_info("[touch] finger down at (%d, %d)", (int)x, (int)y);   /* DM_LOG_TOUCHES */
    engine_touch(a, id, x, y);
}

void controls_handler_key(int32_t keycode, ControlsAction action) {
    /* TODO: map buttons (BACK, START, ...) to NativeCallback_KeyEvent */
}

void controls_handler_analog(ControlsStickId which, float x, float y, ControlsAction action) {
    /* TODO: drive the engine's on-screen virtual sticks from the analog sticks */
}
