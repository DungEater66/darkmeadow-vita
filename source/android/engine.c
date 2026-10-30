/*
 * Emulates the part of the Android activity (com.Phosphor.Engine.BaseUE3JavaApp) that drives libUnrealEngine3.so.
 *
 * The engine registers its Java->native entry points with RegisterNatives in JNI_OnLoad. FalsoJNI ignores that
 * call, so we capture the table instead (engine_install_jni_hooks) and bind typed pointers to it
 * (engine_natives.c, generated). engine_start() then calls them in the same order Java does:
 *
 *   onStart()         InitModel(manufacturer, model, product); InitZipVFS(true)
 *   surfaceCreated()  InitEGLCallback()              -> engine calls back JavaCallback_initEGL
 *   surfaceChanged()  PostInitUpdate(w, h)
 *   PostStartup()     LanguageSet(code); Initialize(w, h, diagonalInches, multitouch)
 *
 * After Initialize() the engine runs its own game and render threads; Java is not involved per frame.
 */
#include "android/engine.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

#include "android/engine_natives.h"
#include "game_info.h"
#include "storage/pkg_cache.h"
#include "storage/save_behind.h"
#include "storage/tex_cache.h"
#include "storage/tex_prefetch.h"
#include "util/fatal.h"
#include "util/logger.h"

extern so_module so_mod;

/* ---- rendering thread ----
 * FEngineLoop::PreInit starts UE3's rendering thread only when GNumHardwareThreads > 1. That variable is 1 and nothing
 * on Android changes it, so the Android game renders on its game thread. Setting it to 3 before the engine starts
 * moves rendering (every GL call, the buffer swap) to a thread of its own, in parallel with the game logic: on the
 * Vita about 53 fps on average against 45 without it (the game thread was at 100% of a core). */
static int *threaded_rendering;

const char *engine_threaded_rendering(void) {
    if (!threaded_rendering) return "unknown";
    return *threaded_rendering ? "on (own thread)" : "off (game thread)";
}

static void enable_render_thread(void) {
    threaded_rendering = (int *)so_symbol(&so_mod, "GIsThreadedRendering");
    int *hw = (int *)so_symbol(&so_mod, "GNumHardwareThreads");
    if (!hw) {
        l_warn("[render] GNumHardwareThreads not found: rendering on the game thread");
        return;
    }
    *hw = 3;
}

extern struct JNINativeInterface *_jni;   /* FalsoJNI's function table (heap allocated, writable) */

#define MAX_NATIVES 64
static JNINativeMethod captured[MAX_NATIVES];
static int captured_n;

static int g_activity_dummy;              /* address only; stands in for the activity instance */
static volatile int g_engine_ready;

volatile unsigned engine_frames;    /* frames swapped so far (JavaCallback_swapBuffers) */
volatile unsigned engine_touches;   /* touch events handed to the engine */
volatile uint64_t engine_swap_since_us;
volatile uint64_t engine_swap_us_total;
volatile uint32_t engine_frame_us_max;   /* != 0 while the game thread is inside vglSwapBuffers */

/* ---- RegisterNatives capture ---- */

static jint capture_RegisterNatives(JNIEnv *env, jclass clazz, const JNINativeMethod *methods, jint n) {
    for (jint i = 0; i < n && captured_n < MAX_NATIVES; i++)
        captured[captured_n++] = methods[i];
    l_info("RegisterNatives: captured %d methods (total %d).", (int)n, captured_n);
    return JNI_OK;
}

/* The engine creates BaseUE3JavaApp$EGLConfigParms with AllocObject (not NewObject) and then fills its fields in.
 * FalsoJNI answers AllocObject with NULL ("not implemented"); hand out a scratch object instead. */
static jobject alloc_object_hook(JNIEnv *env, jclass clazz) {
    return (jobject)calloc(1, 64);
}

void engine_install_jni_hooks(void) {
    _jni->RegisterNatives = capture_RegisterNatives;
    _jni->AllocObject = alloc_object_hook;
}

int engine_find_native(const char *name, void **out) {
    for (int i = 0; i < captured_n; i++) {
        if (strcmp(captured[i].name, name) == 0) {
            *out = captured[i].fnPtr;
            return 1;
        }
    }
    return 0;
}

/* ---- helpers ---- */

jobject engine_activity(void) { return (jobject)&g_activity_dummy; }

jstring engine_jstring(const char *utf8) { return (*ENV)->NewStringUTF(ENV, utf8); }

char *engine_cstring(jstring s) {
    if (!s) return NULL;
    const char *c = (*ENV)->GetStringUTFChars(ENV, s, NULL);
    if (!c) return NULL;
    char *r = strdup(c);
    (*ENV)->ReleaseStringUTFChars(ENV, s, (char *)c);
    return r;
}

/* ---- startup ---- */

void engine_start(void) {
    engine_bind_natives();

    if (!ue3_InitModel || !ue3_InitZipVFS || !ue3_InitEGLCallback || !ue3_PostInitUpdate ||
        !ue3_LanguageSet || !ue3_Initialize) {
        fatal_error("The engine did not register the expected entry points. Is this the right libUnrealEngine3.so "
                    "(Dark Meadow 1.4.6.4)?");
    }

    JNIEnv *env = ENV;
    jobject self = engine_activity();

    enable_render_thread();
    tex_cache_init();
    texpf_init();
    pkg_cache_start();
    save_behind_init();

    const char *manu = DM_DEVICE_MANUFACTURER, *model = DM_DEVICE_MODEL, *product = DM_DEVICE_PRODUCT;
    l_info("onStart: InitModel(\"%s\", \"%s\", \"%s\")", manu, model, product);
    ue3_InitModel(env, self, engine_jstring(manu), engine_jstring(model), engine_jstring(product));
    l_info("onStart: InitZipVFS(true)");
    ue3_InitZipVFS(env, self, JNI_TRUE);

    l_info("surfaceCreated: InitEGLCallback()");
    jboolean egl = ue3_InitEGLCallback(env, self);
    l_info("InitEGLCallback returned %d", (int)egl);

    l_info("surfaceChanged: PostInitUpdate(%d, %d)", DM_SCREEN_W, DM_SCREEN_H);
    ue3_PostInitUpdate(env, self, DM_SCREEN_W, DM_SCREEN_H);

    l_info("PostStartup: LanguageSet(\"%s\")", DM_LANGUAGE);
    ue3_LanguageSet(env, self, engine_jstring(DM_LANGUAGE));
    l_info("PostStartup: Initialize(%d, %d, %.1f, multitouch)", DM_SCREEN_W, DM_SCREEN_H, (double)DM_SCREEN_DIAG);
    jboolean ok = ue3_Initialize(env, self, DM_SCREEN_W, DM_SCREEN_H, DM_SCREEN_DIAG, JNI_TRUE);
    l_info("Initialize returned %d", (int)ok);

    if (!ok)
        fatal_error("The engine failed to initialize. Check that main.%d.%s.obb (and patch.*) are in %s and that "
                    "log.txt shows no earlier errors.", DM_VERSION_CODE, DM_PACKAGE_NAME, DATA_PATH);

    g_engine_ready = 1;
}

/* ---- input and events ---- */

/* Android pointer ids are small and reused (the lowest free one is taken); the Vita's touch report ids keep growing.
 * Translate them so the engine sees 0, 1, 2... like on a phone. */
#define MAX_POINTERS 8
static int pointer_vita_id[MAX_POINTERS] = { -1, -1, -1, -1, -1, -1, -1, -1 };

static int pointer_for(int vita_id, int action) {
    for (int i = 0; i < MAX_POINTERS; i++)
        if (pointer_vita_id[i] == vita_id) return i;
    if (action != 0) return -1;                  /* move/up for a finger we never saw go down */
    for (int i = 0; i < MAX_POINTERS; i++)
        if (pointer_vita_id[i] < 0) { pointer_vita_id[i] = vita_id; return i; }
    return -1;
}

void engine_touch(int action, int id, float x, float y) {
    if (!g_engine_ready || !ue3_InputEvent) return;
    int p = pointer_for(id, action);
    if (p < 0) return;
    if (action == 1) pointer_vita_id[p] = -1;
    /* Android only reports a move when the finger actually moved. The controls thread polls at 120 Hz and reported
     * every held finger on every poll, so scrolling menus (Scaleform) saw runs of zero-distance moves a few ms apart,
     * read that as "not dragging" and scrolled in jerks. */
    static int last_x[MAX_POINTERS], last_y[MAX_POINTERS];
    int ix = (int)x, iy = (int)y;
    if (action == 2 && ix == last_x[p] && iy == last_y[p]) return;
    last_x[p] = ix;
    last_y[p] = iy;
    jlong now_ms = (jlong)(sceKernelGetProcessTimeWide() / 1000);
    engine_touches++;
    ue3_InputEvent(ENV, engine_activity(), action, (jint)x, (jint)y, p, now_ms);
}

void engine_received_account(const char *name) {
    if (ue3_ReceivedNewAccount) ue3_ReceivedNewAccount(ENV, engine_activity(), engine_jstring(name));
    else l_warn("NativeCallback_ReceivedNewAccount was not registered by the engine.");
}

void engine_movie_finished(void) {
    if (ue3_MovieFinished) ue3_MovieFinished(ENV, engine_activity());
}
