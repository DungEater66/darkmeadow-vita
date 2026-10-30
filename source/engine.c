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
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "engine.h"
#include <stdio.h>
#include <string.h>
#include "engine_natives.h"
#include "dm_config.h"
#include "utils/logger.h"
#include "utils/dialog.h"
#include "reimpl/pkg_cache.h"
#include "reimpl/save_behind.h"
extern int dm_pin_threads;   /* utils/watchdog.c (r73) */
extern int dm_gc_skip_repeat;   /* reimpl/ue3_trace.c (r74) */
#include "reimpl/tex_cache.h"
#include "reimpl/tex_prefetch.h"
#include "reimpl/io_stats.h"
#include "reimpl/io.h"
extern int dm_song_android_cuts;   /* song.c */
#include <so_util/so_util.h>

extern so_module so_mod;

/* ---- rendering thread (DM_RENDER_THREAD) ----
 * FEngineLoop::PreInit starts UE3's rendering thread only when GIsClient, no -ONETHREAD and GNumHardwareThreads > 1.
 * GNumHardwareThreads is a data variable that is 1 and that nothing on Android ever changes, so the Android game has
 * always rendered on its game thread. With "DM_RENDER_THREAD 1" in config.txt it is set to 3 before the engine starts,
 * and rendering (every GL call, the buffer swap) moves to its own thread, in parallel with the game logic. Never used
 * on Android; on the console r34 measured 52.8 fps average against 45.2 without it (the game thread had been at 100%
 * of a core). On by default since r35; "DM_RENDER_THREAD 0" turns it off. */
static int *g_threaded_rendering;

const char *engine_threaded_rendering(void) {
    if (!g_threaded_rendering) return "unknown";
    return *g_threaded_rendering ? "ON (own thread)" : "off (game thread)";
}

static void setup_render_thread(int want) {
    g_threaded_rendering = (int *)so_symbol(&so_mod, "GIsThreadedRendering");
    int *hw = (int *)so_symbol(&so_mod, "GNumHardwareThreads");
    if (!hw) { l_warn("[render] GNumHardwareThreads not found"); return; }
    if (want) {
        *hw = 3;
        l_info("[render] DM_RENDER_THREAD 1 (default): GNumHardwareThreads = 3, the engine will start its rendering thread");
    } else {
        l_info("[render] rendering on the game thread (DM_RENDER_THREAD 0)");
    }
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

/* ---- RegisterNatives capture ------------------------------------------------------------------ */

static jint capture_RegisterNatives(JNIEnv *env, jclass clazz, const JNINativeMethod *methods, jint n) {
    for (jint i = 0; i < n && captured_n < MAX_NATIVES; i++)
        captured[captured_n++] = methods[i];
    l_info("RegisterNatives: captured %d methods (total %d).", n, captured_n);
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

/* ---- helpers ---------------------------------------------------------------------------------- */

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

/* ---- startup ---------------------------------------------------------------------------------- */

void engine_start(void) {
    engine_bind_natives();

    if (!ue3_InitModel || !ue3_InitZipVFS || !ue3_InitEGLCallback || !ue3_PostInitUpdate ||
        !ue3_LanguageSet || !ue3_Initialize) {
        fatal_error("The engine did not register the expected entry points. Is this the right libUnrealEngine3.so "
                    "(Dark Meadow 1.4.6.4)?");
    }

    JNIEnv *env = ENV;
    jobject self = engine_activity();

    /* device identity: picks the engine's graphics profile (see dm_config.h); config.txt may override it */
    char manu[48] = DM_DEVICE_MANUFACTURER, model[48] = DM_DEVICE_MODEL, product[48] = DM_DEVICE_PRODUCT;
    int render_thread = 1, pcache_on = 1, pcache_mb = 96, pcache_keep = 1, texcache_mb = 44, prefetch_on = 1;   /* r57: 24 -> 32 MB, room for the texture prefetch; r64: package cache 80 -> 96 MB (it was full), texture cache 32 -> 44 MB */
    int pkgpf_on = 1, pkgpf_mb = 24;   /* r64: package prefetch */
    char pcache_files[512] = DM_PACKAGE_CACHE_DEFAULT;
    FILE *cfg = fopen(DATA_PATH "config.txt", "r");
    if (cfg) {
        char line[600], k[64], v[48];
        while (fgets(line, sizeof(line), cfg)) {
            if (sscanf(line, "%63s %47s", k, v) != 2) continue;
            if (!strcmp(k, "DM_DEVICE_MANUFACTURER")) snprintf(manu, sizeof(manu), "%s", v);
            else if (!strcmp(k, "DM_DEVICE_MODEL")) snprintf(model, sizeof(model), "%s", v);
            else if (!strcmp(k, "DM_DEVICE_PRODUCT")) snprintf(product, sizeof(product), "%s", v);
            else if (!strcmp(k, "DM_RENDER_THREAD")) render_thread = atoi(v) != 0;
            else if (!strcmp(k, "DM_PACKAGE_CACHE")) pcache_on = atoi(v) != 0;
            else if (!strcmp(k, "DM_PACKAGE_CACHE_MB")) pcache_mb = atoi(v);
            else if (!strcmp(k, "DM_PACKAGE_CACHE_KEEP")) pcache_keep = atoi(v) != 0;
            else if (!strcmp(k, "DM_PACKAGE_CACHE_KEEP_ONLY")) {
                const char *rest = strstr(line, k) + strlen(k);
                while (*rest == ' ' || *rest == '\t') rest++;
                char tmp[256];
                snprintf(tmp, sizeof(tmp), "%s", rest);
                tmp[strcspn(tmp, "\r\n")] = 0;
                pkg_cache_set_keep_only(tmp);
            }
            else if (!strcmp(k, "DM_SONG_ANDROID_CUTS")) dm_song_android_cuts = atoi(v) != 0;
            else if (!strcmp(k, "DM_TEXTURE_CACHE_MB")) texcache_mb = atoi(v);
            else if (!strcmp(k, "DM_TEXTURE_PREFETCH")) prefetch_on = atoi(v) != 0;
            else if (!strcmp(k, "DM_PACKAGE_PREFETCH")) pkgpf_on = atoi(v) != 0;
            else if (!strcmp(k, "DM_PIN_THREADS")) dm_pin_threads = atoi(v) != 0;   /* r73 */
            else if (!strcmp(k, "DM_GC_SKIP_REPEAT")) dm_gc_skip_repeat = atoi(v) != 0;   /* r74 */
            else if (!strcmp(k, "DM_SAVE_BEHIND")) save_behind_set(atoi(v) != 0);  /* r73 */
            else if (!strcmp(k, "DM_PACKAGE_PREFETCH_MB")) pkgpf_mb = atoi(v);
            else if (!strcmp(k, "DM_OBB_KEEP_OPEN")) io_set_keep_obb_open(atoi(v) != 0);
            else if (!strcmp(k, "DM_DIR_CACHE")) io_set_dir_cache(atoi(v) != 0);
            else if (!strcmp(k, "DM_PACKAGE_CACHE_FILES") || !strcmp(k, "DM_PACKAGE_CACHE_ADD")) {
                /* _FILES replaces the default list, _ADD appends to it */
                const char *rest = strstr(line, k) + strlen(k);
                char tmp[512];
                snprintf(tmp, sizeof(tmp), "%s", rest);
                char *nl = strpbrk(tmp, "\r\n");
                if (nl) *nl = 0;
                if (k[17] == 'F') snprintf(pcache_files, sizeof(pcache_files), "%s", tmp);
                else strncat(pcache_files, tmp, sizeof(pcache_files) - strlen(pcache_files) - 1);
            }
        }
        fclose(cfg);
    }
    setup_render_thread(render_thread);
    tex_cache_init(texcache_mb);
    if (texcache_mb > 0) l_info("[texcache] texture-data cache: up to %d MB (DM_TEXTURE_CACHE_MB)", texcache_mb);
    else l_info("[texcache] texture-data cache off (DM_TEXTURE_CACHE_MB 0)");
    texpf_init(prefetch_on);
    if (pcache_on && pcache_mb > 0) {
        l_info("[pcache] up to %d MB; preloading in the background: %s; keeping packages after their first load: %s",
               pcache_mb, pcache_files, pcache_keep ? "on" : "off");
        pkg_prefetch_config(pkgpf_on, pkgpf_mb);
        pkg_cache_start(DM_OBB_MAIN, pcache_files, pcache_mb, pcache_keep);
    } else {
        l_info("[pcache] off (DM_PACKAGE_CACHE 0)");
    }
    l_info("onStart: InitModel(\"%s\", \"%s\", \"%s\")", manu, model, product);
    ue3_InitModel(env, self, engine_jstring(manu), engine_jstring(model), engine_jstring(product));
    l_info("onStart: InitZipVFS(true)");
    ue3_InitZipVFS(env, self, JNI_TRUE);

    l_info("surfaceCreated: InitEGLCallback()");
    jboolean egl = ue3_InitEGLCallback(env, self);
    l_info("InitEGLCallback returned %d", (int)egl);
    (void)egl;

    l_info("surfaceChanged: PostInitUpdate(%d, %d)", DM_SCREEN_W, DM_SCREEN_H);
    ue3_PostInitUpdate(env, self, DM_SCREEN_W, DM_SCREEN_H);

    l_info("PostStartup: LanguageSet(\"%s\")", DM_LANGUAGE);   /* English only */
    ue3_LanguageSet(env, self, engine_jstring(DM_LANGUAGE));
    l_info("PostStartup: Initialize(%d, %d, %.1f, multitouch)", DM_SCREEN_W, DM_SCREEN_H, (double)DM_SCREEN_DIAG);
    jboolean ok = ue3_Initialize(env, self, DM_SCREEN_W, DM_SCREEN_H, DM_SCREEN_DIAG, JNI_TRUE);
    l_info("Initialize returned %d", (int)ok);

    if (!ok)
        fatal_error("The engine failed to initialize. Check that main.%d.%s.obb (and patch.*) are in %s and that "
                    "log.txt shows no earlier errors.", DM_VERSION_CODE, DM_PACKAGE_NAME, DATA_PATH);

    g_engine_ready = 1;
}

/* ---- input / events ----------------------------------------------------------------------------- */

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
    unsigned n = ++engine_touches;
    if (n <= 40) l_debug("[input] touch %u: action %d pointer %d at (%d, %d)", n, action, p, (int)x, (int)y);
    ue3_InputEvent(ENV, engine_activity(), action, (jint)x, (jint)y, p, now_ms);
    if (n <= 40) l_debug("[input] touch %u handled", n);
}

void engine_received_account(const char *name) {
    if (ue3_ReceivedNewAccount) ue3_ReceivedNewAccount(ENV, engine_activity(), engine_jstring(name));
    else l_warn("NativeCallback_ReceivedNewAccount was not registered by the engine.");
}

void engine_movie_finished(void) {
    if (ue3_MovieFinished) ue3_MovieFinished(ENV, engine_activity());
}
