#ifndef DM_ENGINE_H
#define DM_ENGINE_H

#include <stdint.h>
#include <falso_jni/FalsoJNI.h>

#define ENV (&jni)   /* the single fake JNIEnv* */

/* Call after jni_init() and before JNI_OnLoad: lets us capture the natives the engine registers. */
void engine_install_jni_hooks(void);

/* Run what BaseUE3JavaApp does on Android to start the engine (model, VFS, EGL, size, language, initialize). */
void engine_start(void);

/* Forward a touch to the engine. action: 0 = down, 1 = up, 2 = move (Android MotionEvent values). */
void engine_touch(int action, int id, float x, float y);

/* Tell the engine that a movie started by JavaCallback_StartVideo has ended. */
void engine_movie_finished(void);

/* Give the engine a (Google) account name, as BaseUE3JavaApp does from JavaCallback_GetAvailableAccounts. */
void engine_received_account(const char *name);

/* Progress counters, read by the watchdog. */
extern volatile unsigned engine_frames;
extern volatile unsigned engine_touches;
extern volatile uint64_t engine_swap_since_us;
/* GPU wait: total time spent inside vglSwapBuffers (it blocks while the GPU is still busy with earlier frames), and
 * the longest gap between two swaps since the last [cpu] report (reset by the watchdog). */
extern volatile uint64_t engine_swap_us_total;
extern volatile uint32_t engine_frame_us_max;

/* Dummy "this" object handed to natives (the Java activity instance). */
jobject engine_activity(void);

/* Create a Java string for the engine (leaks by design: FalsoJNI strings are small and rare). */
jstring engine_jstring(const char *utf8);

/* Convert a Java string from the engine into a malloc'ed C string (caller frees); NULL stays NULL. */
char *engine_cstring(jstring s);

#endif
