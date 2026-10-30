/*
 * Native -> Java callbacks (the "JavaCallback_*" methods of BaseUE3JavaApp) that need real behaviour.
 * Everything not defined here falls back to a logging no-op generated in java_tables.c.
 *
 * Callback signature (FalsoJNI): <ret> jcb_<JavaMethodName>(jmethodID id, va_list args)
 * - jstring arguments arrive as FalsoJNI JavaString objects: read them with engine_cstring().
 * - float arguments arrive promoted to double in a va_list: read them with va_arg(args, double).
 * - boolean/byte/short/char arguments arrive promoted to int.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <falso_jni/FalsoJNI_Logger.h>

#include "java_callbacks.h"
#include "engine.h"
#include "engine_natives.h"
#include "song.h"
#include "movie.h"
#include "padinput.h"
#include "dm_config.h"
#include "utils/glutil.h"
#include "utils/gldiag.h"
#include "utils/logger.h"
#include "utils/utils.h"

/* ================================================================================================
 * EGL: the engine asks Java to create/swap/make-current the GL context. vitaGL has exactly one
 * context, created once, so the "make current" calls have nothing to do.
 * ============================================================================================== */

static pthread_once_t gl_once = PTHREAD_ONCE_INIT;
static void gl_init_once(void) {
    l_info("Creating the GL context (vitaGL).");
    gl_init();
}

jboolean jcb_JavaCallback_initEGL(jmethodID id, va_list args) {
    /* arg: BaseUE3JavaApp$EGLConfigParms (the engine's requested bit depths; vitaGL ignores them) */
    pthread_once(&gl_once, gl_init_once);
    return JNI_TRUE;
}

jboolean jcb_JavaCallback_reinitEGL(jmethodID id, va_list args) {
    pthread_once(&gl_once, gl_init_once);
    return JNI_TRUE;
}

jboolean jcb_JavaCallback_makeCurrent(jmethodID id, va_list args)   { return JNI_TRUE; }
jboolean jcb_JavaCallback_unMakeCurrent(jmethodID id, va_list args) { return JNI_TRUE; }

extern volatile int dm_render_tid;   /* utils/watchdog.c (r73: thread pinning) */
jboolean jcb_JavaCallback_swapBuffers(jmethodID id, va_list args) {
    static uint64_t prev_end;
    if (dm_render_tid < 0) dm_render_tid = sceKernelGetThreadId();
    engine_frames++;
    gldiag_frame();
    movie_render();
    overlay_render();   /* r59: the menu cursor of the physical controls */
    uint64_t t0 = sceKernelGetProcessTimeWide();
    engine_swap_since_us = t0;
    gl_swap();
    uint64_t t1 = sceKernelGetProcessTimeWide();
    engine_swap_since_us = 0;
    engine_swap_us_total += t1 - t0;
    if (prev_end && t1 - prev_end > engine_frame_us_max) engine_frame_us_max = (uint32_t)(t1 - prev_end);
    if (prev_end && t1 - prev_end > 250000ull) {   /* r68: long frames, with the time (the cutscene clock may skip them) */
        static int n;
        if (n++ < 400) l_info("[frame] long frame: %u ms (ended now)", (unsigned)((t1 - prev_end) / 1000));
    }
    prev_end = t1;
    return JNI_TRUE;
}

/* The constructor of BaseUE3JavaApp$EGLConfigParms: only has to return a non-NULL object. */
static int g_egl_parms_dummy;
jobject jcb_EGLConfigParms_init(jmethodID id, va_list args) {
    return (jobject)&g_egl_parms_dummy;
}

static float g_fixed_size_scale = 1.0f;
void jcb_JavaCallback_SetFixedSizeScale(jmethodID id, va_list args) {
    g_fixed_size_scale = (float)va_arg(args, double);
    l_info("SetFixedSizeScale(%.3f) requested by the engine (ignored; rendering stays %dx%d).",
           (double)g_fixed_size_scale, DM_SCREEN_W, DM_SCREEN_H);
}

/* ================================================================================================
 * Files: where the expansion data and the optional command line file live.
 * ============================================================================================== */

jobject jcb_JavaCallback_GetExpansionFilename(jmethodID id, va_list args) {
    int is_main = va_arg(args, int);   /* true = main.<code>.<pkg>.obb, false = patch.<code>.<pkg>.obb */
    const char *path = is_main ? DM_OBB_MAIN : DM_OBB_PATCH;
    int found = file_exists(path);
    l_info("GetExpansionFilename(%s) -> %s%s", is_main ? "main" : "patch", found ? path : "(missing) ", found ? "" : path);
    return engine_jstring(found ? path : "");
}

jobject jcb_JavaCallback_GetPackageName(jmethodID id, va_list args) {
    return engine_jstring(DM_PACKAGE_NAME);
}

jobject jcb_JavaCallback_GetCommandLineFilePath(jmethodID id, va_list args) {
    int found = file_exists(DM_CMDLINE_FILE);
    l_info("GetCommandLineFilePath -> %s", found ? DM_CMDLINE_FILE : "(none)");
    return engine_jstring(found ? DM_CMDLINE_FILE : "");
}

/* ================================================================================================
 * App-local values: an in-memory string map the activity creates in surfaceCreated(). The engine
 * reads its storage directories from it (STORAGE_ROOT/BASE_DIR) and may stash its own keys.
 * ============================================================================================== */

#define MAX_LOCALS 32
static struct { char *key; char *val; } g_locals[MAX_LOCALS];
static int g_locals_n;
static pthread_mutex_t g_locals_lock = PTHREAD_MUTEX_INITIALIZER;

static void local_set_locked(const char *key, const char *val) {
    for (int i = 0; i < g_locals_n; i++) {
        if (strcmp(g_locals[i].key, key) == 0) {
            free(g_locals[i].val);
            g_locals[i].val = strdup(val);
            return;
        }
    }
    if (g_locals_n < MAX_LOCALS) {
        g_locals[g_locals_n].key = strdup(key);
        g_locals[g_locals_n].val = strdup(val);
        g_locals_n++;
    }
}

static pthread_once_t locals_once = PTHREAD_ONCE_INIT;
static void locals_init(void) {
    pthread_mutex_lock(&g_locals_lock);
    local_set_locked("STORAGE_ROOT", DATA_PATH);      /* trailing slash, like Android's external storage root */
    local_set_locked("BASE_DIR", DM_BASE_DIR);
    local_set_locked("LOCAL_IP", "");
    pthread_mutex_unlock(&g_locals_lock);
}

static const char *local_get_locked(const char *key) {
    for (int i = 0; i < g_locals_n; i++)
        if (strcmp(g_locals[i].key, key) == 0) return g_locals[i].val;
    return NULL;
}

jboolean jcb_JavaCallback_HasAppLocalValue(jmethodID id, va_list args) {
    pthread_once(&locals_once, locals_init);
    char *key = engine_cstring(va_arg(args, jstring));
    if (!key) return JNI_FALSE;
    pthread_mutex_lock(&g_locals_lock);
    jboolean r = local_get_locked(key) != NULL;
    pthread_mutex_unlock(&g_locals_lock);
    free(key);
    return r;
}

jobject jcb_JavaCallback_GetAppLocalValue(jmethodID id, va_list args) {
    pthread_once(&locals_once, locals_init);
    char *key = engine_cstring(va_arg(args, jstring));
    if (!key) return NULL;
    pthread_mutex_lock(&g_locals_lock);
    const char *v = local_get_locked(key);
    jobject r = v ? engine_jstring(v) : NULL;
    pthread_mutex_unlock(&g_locals_lock);
    l_debug("GetAppLocalValue(\"%s\") -> %s", key, v ? v : "(null)");
    free(key);
    return r;
}

void jcb_JavaCallback_SetAppLocalValue(jmethodID id, va_list args) {
    pthread_once(&locals_once, locals_init);
    char *key = engine_cstring(va_arg(args, jstring));
    char *val = engine_cstring(va_arg(args, jstring));
    if (key && val) {
        pthread_mutex_lock(&g_locals_lock);
        local_set_locked(key, val);
        pthread_mutex_unlock(&g_locals_lock);
    }
    free(key);
    free(val);
}

/* ================================================================================================
 * App lifecycle / UI that has no Vita equivalent.
 * ============================================================================================== */

void jcb_JavaCallback_ShutDownApp(jmethodID id, va_list args) {
    l_info("The engine asked to shut down.");
    sceKernelExitProcess(0);
}

jboolean jcb_JavaCallback_IsVideoPlaying(jmethodID id, va_list args) { return movie_engine_playing() ? JNI_TRUE : JNI_FALSE; }
void jcb_JavaCallback_StopVideo(jmethodID id, va_list args) { movie_stop(); }

/* Movies (.m4v inside the expansion file) are not played yet: report them finished right away from a
 * short-lived thread so the engine does not wait forever (e.g. on the intro logos). */
static int movie_finished_thread(SceSize args, void *argp) {
    sceKernelDelayThread(100 * 1000);
    engine_movie_finished();
    return sceKernelExitDeleteThread(0);
}

void jcb_JavaCallback_StartVideo(jmethodID id, va_list args) {
    char *path = engine_cstring(va_arg(args, jstring));
    int ok = movie_request(path);   /* movie.c: hardware playback of a converted .mp4 */
    free(path);
    if (ok) return;
    SceUID t = sceKernelCreateThread("dm_movie_done", movie_finished_thread, 0x40, 0x4000, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
}

/* ================================================================================================
 * Account / date. After "tap to start" the game asks Java for the device's Google accounts and will not go
 * on until at least one arrives through NativeCallback_ReceivedNewAccount (on Android, with no account it
 * shows "A Google account must be signed in" and exits). Hand it one fixed local account.
 * ============================================================================================== */

void jcb_JavaCallback_GetAvailableAccounts(jmethodID id, va_list args) {
    l_info("GetAvailableAccounts -> \"%s\"", DM_ACCOUNT_NAME);
    engine_received_account(DM_ACCOUNT_NAME);
}

/* Java: new SimpleDateFormat("MM/yy/dd").format(new Date()) */
jstring jcb_JavaCallback_GetJavaDate(jmethodID id, va_list args) {
    char buf[16];
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, sizeof(buf), "%m/%y/%d", &tmv);
    return engine_jstring(buf);
}

/* In-app purchases: billing is reported as supported and every store purchase (the money bags and Sun Coin packs)
 * succeeds at once, for free. "DM_FREE_IAP 0" in config.txt turns the store's purchases off again. */
static int config_int(const char *key, int def) {
    FILE *f = fopen(DATA_PATH "config.txt", "r");
    if (!f) return def;
    char line[128], k[64];
    int v, r = def;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "%63s %d", k, &v) == 2 && !strcmp(k, key)) r = v;
    fclose(f);
    return r;
}

jboolean jcb_JavaCallback_AndroidIAPIsBillingSupported(jmethodID id, va_list args) {
    static int v = -1;
    if (v < 0) { v = config_int("DM_FREE_IAP", 1) != 0; l_info("[iap] billing %s (DM_FREE_IAP)", v ? "on: purchases are free" : "off"); }
    return v ? JNI_TRUE : JNI_FALSE;
}

void jcb_JavaCallback_AndroidIAPRequestPurchase(jmethodID id, va_list args) {
    char *product = engine_cstring(va_arg(args, jstring));
    l_info("[iap] purchase \"%s\": granted", product ? product : "(null)");
    if (product && ue3_AndroidIAPPurchaseSucceeded)
        ue3_AndroidIAPPurchaseSucceeded(ENV, engine_activity(), engine_jstring(product));
    free(product);
}

/* Sound effects the Java side plays through Android's SoundPool. Not implemented yet: hand out ids so the engine
 * keeps going, and log which files it asks for (useful for deciding what actually needs playing). */
static int g_next_sound_id = 1;

jint jcb_JavaCallback_LoadSoundFile(jmethodID id, va_list args) {
    char *path = engine_cstring(va_arg(args, jstring));
    int sid = g_next_sound_id++;
    l_info("LoadSoundFile(\"%s\") -> id %d (not played yet)", path ? path : "(null)", sid);
    free(path);
    return sid;
}

jint jcb_JavaCallback_PlaySound(jmethodID id, va_list args) {
    int sid = va_arg(args, int);
    return sid;   /* stream id; nothing is played yet */
}

void jcb_JavaCallback_UnloadSoundID(jmethodID id, va_list args) {}
void jcb_JavaCallback_StopSound(jmethodID id, va_list args) {}
void jcb_JavaCallback_SetVolume(jmethodID id, va_list args) {}

/* Savegame indicator the Java side shows; nothing to do. */
void jcb_JavaCallback_StartSavegame(jmethodID id, va_list args) { l_debug("StartSavegame"); }
void jcb_JavaCallback_EndSavegame(jmethodID id, va_list args)   { l_debug("EndSavegame"); }

/* ================================================================================================
 * Songs: music, room ambience and all spoken dialogue (UDKGame/Music/<name>.mp3 in the .obb), played on Android
 * with a MediaPlayer. See song.c.
 * ============================================================================================== */

void jcb_JavaCallback_PlaySong(jmethodID id, va_list args) {
    char *name = engine_cstring(va_arg(args, jstring));
    int loop = va_arg(args, int);
    /* Some cutscenes pass "name 1.000000 0.000000 0.000000" (extra numbers after the file name). Only the name is a
     * file; drop the rest so those cutscenes have their music. */
    char *sp = name ? strchr(name, ' ') : NULL;
    if (sp) {
        l_debug("[song] PlaySong(\"%s\"): ignoring the extra parameters", name);
        *sp = 0;
    }
    song_play(name, loop);
    free(name);
}
void jcb_JavaCallback_StopSong(jmethodID id, va_list args)   { song_stop(); }
void jcb_JavaCallback_PauseSong(jmethodID id, va_list args)  { song_pause(1); }
void jcb_JavaCallback_ResumeSong(jmethodID id, va_list args) { song_pause(0); }
void jcb_JavaCallback_SetSongBaseVolume(jmethodID id, va_list args) { song_set_base_volume((float)va_arg(args, double)); }
void jcb_JavaCallback_SetSongFadeValues(jmethodID id, va_list args) {
    float in = (float)va_arg(args, double);
    float out = (float)va_arg(args, double);
    song_set_fades(in, out);
}
void jcb_JavaCallback_SetSongSoundClassVolume(jmethodID id, va_list args) { song_set_class_volume((float)va_arg(args, double)); }
void jcb_JavaCallback_UpdateMusicVolume(jmethodID id, va_list args)       { song_set_music_volume((float)va_arg(args, double)); }

/* ================================================================================================
 * Tapjoy (the "Free Sun Coins" offer wall). There is no Tapjoy server here, so the loader plays it:
 * opening the offer wall (JavaCallback_TapjoyShowOffers) earns DM_FREE_SUNCOINS (100) tap points,
 * and the game is told its balance the way the real SDK would (NativeCallback_TapjoyGetUpdatePoints).
 * The game turns tap points into Sun Coins by spending them (JavaCallback_TapjoySpendTapPoints ->
 * NativeCallback_TapjoyGetSpendPointsResponse with the new balance). The balance is kept in
 * ux0:data/darkmeadow/tapjoy.txt so unspent points survive a restart. The engine queues these
 * callbacks for its game thread, so calling them from inside a Java callback is safe.
 * ============================================================================================== */
#ifndef DM_FREE_SUNCOINS
#define DM_FREE_SUNCOINS 100
#endif
#define TAPJOY_CURRENCY "Sun Coins"
#define TAPJOY_FILE DATA_PATH "tapjoy.txt"

static int tapjoy_balance = -1;

static void tapjoy_load(void) {
    if (tapjoy_balance >= 0) return;
    tapjoy_balance = 0;
    FILE *f = fopen(TAPJOY_FILE, "r");
    if (f) {
        if (fscanf(f, "%d", &tapjoy_balance) != 1 || tapjoy_balance < 0) tapjoy_balance = 0;
        fclose(f);
    }
}

static void tapjoy_save(void) {
    FILE *f = fopen(TAPJOY_FILE, "w");
    if (f) { fprintf(f, "%d\n", tapjoy_balance); fclose(f); }
}

static void tapjoy_report_balance(void) {
    if (ue3_TapjoyGetUpdatePoints)
        ue3_TapjoyGetUpdatePoints(ENV, engine_activity(), engine_jstring(TAPJOY_CURRENCY), tapjoy_balance);
}

void jcb_JavaCallback_TapjoyConnect(jmethodID id, va_list args) { tapjoy_load(); l_info("[tapjoy] connect (offline)"); }

void jcb_JavaCallback_TapjoyShowOffers(jmethodID id, va_list args) {
    tapjoy_load();
    tapjoy_balance += DM_FREE_SUNCOINS;
    tapjoy_save();
    l_info("[tapjoy] offer wall opened: +%d tap points, balance %d", DM_FREE_SUNCOINS, tapjoy_balance);
    tapjoy_report_balance();
}

void jcb_JavaCallback_TapjoyGetTapPoints(jmethodID id, va_list args) {
    tapjoy_load();
    l_info("[tapjoy] balance asked: %d", tapjoy_balance);
    tapjoy_report_balance();
}

void jcb_JavaCallback_TapjoySpendTapPoints(jmethodID id, va_list args) {
    int amount = va_arg(args, int);
    tapjoy_load();
    if (amount <= 0 || amount > tapjoy_balance) {
        l_info("[tapjoy] spend %d refused (balance %d)", amount, tapjoy_balance);
        if (ue3_TapjoyGetSpendPointsResponseFailed)
            ue3_TapjoyGetSpendPointsResponseFailed(ENV, engine_activity(), engine_jstring("Balance too low"));
        return;
    }
    tapjoy_balance -= amount;
    tapjoy_save();
    l_info("[tapjoy] spent %d, balance %d", amount, tapjoy_balance);
    if (ue3_TapjoyGetSpendPointsResponse)
        ue3_TapjoyGetSpendPointsResponse(ENV, engine_activity(), engine_jstring(TAPJOY_CURRENCY), tapjoy_balance);
}

void jcb_JavaCallback_TapjoyAwardTapPoints(jmethodID id, va_list args) {
    int amount = va_arg(args, int);
    tapjoy_load();
    if (amount > 0) tapjoy_balance += amount;
    tapjoy_save();
    l_info("[tapjoy] awarded %d, balance %d", amount, tapjoy_balance);
    if (ue3_TapjoyGetAwardPointsResponse)
        ue3_TapjoyGetAwardPointsResponse(ENV, engine_activity(), engine_jstring(TAPJOY_CURRENCY), tapjoy_balance);
}

void jcb_JavaCallback_TapjoySetBannerSize(jmethodID id, va_list args) {}
void jcb_JavaCallback_TapjoyShowBannerAd(jmethodID id, va_list args) {}
void jcb_JavaCallback_TapjoyHideBannerAd(jmethodID id, va_list args) {}
