/*
 * Dark Meadow: The Pact (Phosphor Games, UE3 build 9926) - values the Java activity used to provide.
 * Everything here is something BaseUE3JavaApp / UE3JavaApp decides on Android; change it here, not in the glue.
 */
#ifndef DM_CONFIG_H
#define DM_CONFIG_H

/* Identity (from AndroidManifest.xml). The expansion files are named main/patch.<VERSION_CODE>.<PACKAGE>.obb */
/* Shown in log.txt at start, so it is clear which loader build produced a log. Bump on every change. */
#define DM_LOADER_BUILD "r74 (2026-10-29, back-to-back garbage collections skipped)"

#define DM_PACKAGE_NAME  "com.phosphorgames.DarkMeadow"
#define DM_VERSION_CODE  69

/* Directory name the engine appends to STORAGE_ROOT (BaseUE3JavaApp.b in the APK) */
#define DM_BASE_DIR      "UDKGame"

/* Language code the activity passes to NativeCallback_LanguageSet (Locale.getLanguage()).
 * The engine maps it to Coalesced_<LANG>.bin: "en" -> INT, "fr" -> FRA, "de" -> DEU, "es" -> ESN, ... */
#define DM_LANGUAGE      "en"

/* Device identity passed to NativeCallback_InitModel (Build.MANUFACTURER / MODEL / PRODUCT).
 * The engine picks its graphics profile from it (Android-UDKEngine.ini in Coalesced_INT.bin): "samsung" ->
 * [SystemSettingsAndroid_Samsung], based on [SystemSettingsMobile], with specular, environment (reflection) maps,
 * normal maps, rim lighting and fog. An unknown manufacturer gets [SystemSettingsAndroid], based on
 * [SystemSettingsMobileTextureBias], which turns all of those off: metal whose look comes from the reflection renders
 * black and characters lose their rim light (r1-r25 reported "Sony"). config.txt can override these at run time:
 * DM_DEVICE_MANUFACTURER / DM_DEVICE_MODEL / DM_DEVICE_PRODUCT followed by one word. */
#define DM_DEVICE_MANUFACTURER "samsung"
#define DM_DEVICE_MODEL        "GT-I9300"
#define DM_DEVICE_PRODUCT      "m0xx"

/* Surface the engine renders into, and the physical screen diagonal in inches (Initialize's float argument). */
#define DM_SCREEN_W      960
#define DM_SCREEN_H      544
#define DM_SCREEN_DIAG   5.0f

/* Where the expansion files live: DATA_PATH + main.<code>.<package>.obb (and patch.<...>) */
#define DM_STR_(x)       #x
#define DM_STR(x)        DM_STR_(x)
#define DM_OBB_MAIN      DATA_PATH "main."  DM_STR(DM_VERSION_CODE) "." DM_PACKAGE_NAME ".obb"
#define DM_OBB_PATCH     DATA_PATH "patch." DM_STR(DM_VERSION_CODE) "." DM_PACKAGE_NAME ".obb"

/* Packages kept unpacked in RAM (reimpl/pkg_cache.c). The battle sound tables are loaded again at every battle start.
 * config.txt: DM_PACKAGE_CACHE_FILES replaces this list, DM_PACKAGE_CACHE_ADD appends to it (a trailing * matches a
 * name prefix, e.g. HOR_ai_light_0*), DM_PACKAGE_CACHE_MB caps the RAM used (default 48; r48-r53: 64), DM_PACKAGE_CACHE 0 = off.
 * Other packages up to 8 MB are kept after their first load (DM_PACKAGE_CACHE_KEEP 0 turns that off). */
#define DM_PACKAGE_CACHE_DEFAULT "light_sound_table_SF.xxx heavy_sound_table_SF.xxx witch_sound_table_SF.xxx " \
                                 "trickster_sound_table_SF.xxx ct_boss_sound_table_SF.xxx"
#define DM_CMDLINE_FILE  DATA_PATH "UE3CommandLine.txt"

/* Experiment for objects drawn solid black (shiny/reflective ones such as bedpans): the mobile shaders compute
 * specular and fresnel terms with pow(x, y) where x can be exactly 0. Sony's shader compiler evaluates pow as
 * exp2(y * log2(x)), so pow(0, 0) is NaN, which renders black. With this on, every shader gets
 * "#define pow(a, b) pow(max(a, 1e-7), b)" prepended. Changing it makes vitaGL recompile all shaders once (slow
 * first start). Set to 0 to compare. */
#define DM_SAFE_POW  1

/* Gain applied to the final audio mix (sound effects + music + dialogue) before the soft limiter. Android applies
 * the device volume before mixing; the Vita applies its volume after, so the mix needs headroom. Raise it towards
 * 1.0 for louder output, lower it if loud scenes still sound harsh. */
#define DM_AUDIO_MASTER  0.6f

/* The "Google account" given to the engine after tap-to-start (it refuses to continue without one). */
#define DM_ACCOUNT_NAME  "player@psvita"


/* What sysconf()/sysinfo() report. UE3 sizes some pools from these, so keep them modest. */
#define DM_CPU_CORES        3      /* cores usable by a Vita title */
#define DM_TOTAL_RAM_MB     256
#define DM_FREE_RAM_MB      140

/* Size of the malloc heap handed to the engine (_newlib_heap_size_user). vitaGL and the .so image
 * are allocated outside of it, so this needs tuning against the console's memory once it runs. */
#define DM_HEAP_MB          240   /* r58: 200 -> 240: vitaGL's RAM pool (the rest) never went below 80 MB free; now ~40 MB */

#endif
