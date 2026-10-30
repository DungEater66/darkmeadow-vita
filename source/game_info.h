/*
 * Dark Meadow: The Pact (Phosphor Games, UE3 build 9926): values the Android activity (BaseUE3JavaApp) provided, and
 * the loader's fixed sizes.
 */
#ifndef DM_GAME_INFO_H
#define DM_GAME_INFO_H

/* Shown at the top of log.txt, so it is clear which loader build produced a log. Bump on every change. */
#define DM_LOADER_BUILD "r75 (2026-10-30, source cleaned up)"

/* Identity (AndroidManifest.xml). The expansion files are named main/patch.<VERSION_CODE>.<PACKAGE>.obb */
#define DM_PACKAGE_NAME "com.phosphorgames.DarkMeadow"
#define DM_VERSION_CODE 69

#define DM_STR_(x) #x
#define DM_STR(x) DM_STR_(x)
#define DM_OBB_MAIN DATA_PATH "main." DM_STR(DM_VERSION_CODE) "." DM_PACKAGE_NAME ".obb"
#define DM_OBB_PATCH DATA_PATH "patch." DM_STR(DM_VERSION_CODE) "." DM_PACKAGE_NAME ".obb"
#define DM_CMDLINE_FILE DATA_PATH "UE3CommandLine.txt"

/* The directory the engine appends to its storage root (BaseUE3JavaApp.b in the APK) */
#define DM_BASE_DIR "UDKGame"

/* Language passed to NativeCallback_LanguageSet. The port is English only (Coalesced_INT.bin). */
#define DM_LANGUAGE "en"

/* Device identity passed to NativeCallback_InitModel (Build.MANUFACTURER / MODEL / PRODUCT). The engine picks its
 * graphics profile from the manufacturer: "samsung" -> [SystemSettingsAndroid_Samsung], with specular, reflection and
 * normal maps, rim lighting and fog. An unknown one gets a profile with all of those off, where metal renders black. */
#define DM_DEVICE_MANUFACTURER "samsung"
#define DM_DEVICE_MODEL "GT-I9300"
#define DM_DEVICE_PRODUCT "m0xx"

/* The surface the engine renders into, and the screen diagonal in inches (Initialize's float argument) */
#define DM_SCREEN_W 960
#define DM_SCREEN_H 544
#define DM_SCREEN_DIAG 5.0f

/* The "Google account" given to the engine after tap-to-start (it does not go on without one) */
#define DM_ACCOUNT_NAME "player@psvita"

/* Gain on the final audio mix before the soft limiter. Android applies the device volume before mixing; the Vita
 * after, so the mix needs headroom. */
#define DM_AUDIO_MASTER 0.6f

/* What sysconf() / sysinfo() report. UE3 sizes some pools from these. */
#define DM_CPU_CORES 3
#define DM_TOTAL_RAM_MB 256
#define DM_FREE_RAM_MB 140

/* The malloc heap (_newlib_heap_size_user). vitaGL's RAM pool gets what is left. */
#define DM_HEAP_MB 240

#endif
