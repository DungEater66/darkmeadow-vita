# Dark Meadow: The Pact - PS Vita (so-loader)

Runs the Android build of *Dark Meadow: The Pact* (Phosphor Games, Unreal Engine 3 build 9926,
v1.4.6.4 "PowerVR", package `com.phosphorgames.DarkMeadow`) on the PS Vita by loading
`libUnrealEngine3.so` directly. Built on [soloader-boilerplate](README.boilerplate.md).

**Status: playable from start to every ending** on hardware: movies, music and dialogue, physical controls with
remapping, a settings app on the LiveArea. You need your own copy of the game (Android, v1.4.6.4,
version code 69). **How to build and install: [BUILD.md](BUILD.md).** The rest of this file is the development log,
newest builds further down.

## What you need

* VitaSDK-softfp (`VITASDK` set) with the packages the boilerplate links: OpenSLES, sndfile, mp3lame, mpg123,
  opus, vorbis, ogg, vorbisenc, vorbisfile, FLAC, vitashark, SceShaccCgExt, mathneon.
* **A current kubridge**, both in the SDK and on the Vita (RWX memory, `kuKernelMemProtect`):
  * SDK: `grep -c kuKernelMemProtect "$VITASDK/arm-vita-eabi/include/kubridge.h"` must print 1 or more. If it prints 0,
    install upstream: `git clone https://github.com/bythos14/kubridge && cd kubridge && cmake -B build && cmake --build build && cmake --install build`
    (it installs `kubridge.h` and the stub libraries into the SDK).
  * Vita: install the matching `kubridge.skprx` from the same project. An older plugin does not export
    `kuKernelMemProtect` and the app will not start.
  * `lib/kubridge/` in this tree is a stale copy from the boilerplate zip and is not used by the build (the code
    includes `<kubridge.h>` from the SDK).
* From the game (version code **69**):
  * `lib/armeabi-v7a/libUnrealEngine3.so` from the APK
  * the expansion files `main.69.com.phosphorgames.DarkMeadow.obb` and `patch.69.com.phosphorgames.DarkMeadow.obb`

Put all three in **`ux0:data/darkmeadow/`**. Leave the `.obb` files as they are (they are zip archives the engine reads itself).

## Build

Run these from the project folder (the one containing `CMakeLists.txt`), in a terminal where `VITASDK` is set and
`$VITASDK/bin` is on your `PATH` (the same setup you used for the `bio4` build, with the softfp SDK).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

What each part means:

| Part | Meaning |
|---|---|
| `cmake -S . -B build` | **Configure.** `-S .` = the source is this folder, `-B build` = put all generated files in a new `build/` folder. Replaces `mkdir build && cd build && cmake ..` |
| `-DCMAKE_BUILD_TYPE=Debug` | Debug build: full logging (debug lines too), `-O1`, vitaGL with its checks (`TEST=1 LOG_ERRORS=1`). |
| `-DCMAKE_BUILD_TYPE=Profile` | **Recommended for play-testing (r37):** optimised like Release (`-O3`, vitaGL `NO_DEBUG`) but keeps the log (info/warnings, `[cpu]`/`[gpu]`/`[mem]`/`[io]`/`[text]` lines). Debug lines and the per-draw diagnostics are compiled out. Use a new build folder (e.g. `-B build-profile`) so vitaGL is rebuilt with its fast flags. |
| `-DCMAKE_BUILD_TYPE=Release` | Optimised, no log except errors. |
| `cmake --build build` | **Compile and link.** Replaces `make`. Add `-j4` to use 4 CPU cores. |

You do not pass a toolchain: `CMakeLists.txt` takes it from `$VITASDK` itself. If `VITASDK` is not set you get
"Please define VITASDK env var...". Delete the `build/` folder (`rm -rf build`) when you change `VITASDK` or any
`-D` option, or when a build behaves strangely, then run both commands again.

The result is `build/DarkMeadow.vpk`. Copy it to the Vita and install it with VitaShell, then put the game files in
`ux0:data/darkmeadow/` (see above). Changing the game folder or the title needs `DATA_PATH` etc. in `CMakeLists.txt`.

Typical failures:

| Message | Cause |
|---|---|
| `Please define VITASDK env var` | `VITASDK` not set in this terminal |
| `cannot find -lOpenSLES` (or `-lmpg123`, `-lvitashark`, `-lmathneon`, ...) | that library is not installed in your SDK (see "What you need") |
| `uses VFP register arguments` | you are using the hard-float SDK instead of VitaSDK-softfp |
| `kuKernelMemProtect` undeclared / undefined | your SDK's kubridge is too old (see "What you need") |
| `undefined reference to shark_*` while linking | your `libvitashark.a` is older than the vitaGL in `lib/vitagl`. One known case is already covered by a weak stub in `source/utils/glutil.c`; for others update vitaShaRK in your SDK |
| `fatal: not a git repository` during "Building VitaGL" | harmless (vitaGL asks git for a version hash; falls back to "unknown") |

## Logs

A **Debug** build writes everything the loader and FalsoJNI report to `ux0:data/darkmeadow/log.txt` (plain text,
overwritten on every start) and to the Vita's debug console. A **Release** build only logs errors. When something
goes wrong, send `log.txt`. Each Java callback that is not implemented yet is listed once as `not implemented`.

Log lines are collected in memory and written to the card in batches: when 16 KB have piled up, right away for
warnings and errors, and at least once a second (the watchdog flushes). Earlier builds wrote every line to the card
on its own, and each logging thread had to wait for that. The audio mixer thread never logs at all.

Debug builds are slower than Release builds for other reasons too: `-O1` instead of `-O3`, and vitaGL built with
error checking (`LOG_ERRORS=1`) that validates every GL call. To judge speed, build Release
(`cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release`).

## If it crashes

The loader installs a crash handler. A crash writes a block like this to `log.txt` and exits:

```
==================== CRASH ====================
Thread: <name>   data abort
PC  = 0x99xxxxxx   libUnrealEngine3.so+0x1234567
LR  = ...
  stack+0x010: 0x99xxxxxx  libUnrealEngine3.so+0x89abcd      (rough call stack)
```

Turn the `libUnrealEngine3.so+0x...` offsets into function names with
`python3 tools/resolve_addr.py libUnrealEngine3.so 0x1234567 0x89abcd` (needs `pyelftools`), or just send `log.txt`.

## If it freezes

A watchdog thread checks once a second whether the engine is still presenting frames. After 5 s without a frame it
writes to `log.txt`:

```
[watchdog] no frame swapped for 5 s (frames so far 1234, touches 3)
[stall] blocked 7 s in pthread_cond_wait (object 0x8xxxxxxx) at libUnrealEngine3.so+0x12e6438
```

The `[stall]` lines are every engine call that has been stuck in a mutex, condition variable, semaphore or
`pthread_join` for 5 s or more, with the engine code that made the call (resolve it with `tools/resolve_addr.py`).
`[busy] N calls in 5 s to ...` lines list the call sites that kept polling during the freeze (timed waits, failed
`trylock`, `usleep`, `sched_yield`): a thread stuck in such a loop is waiting for something that never happens.
`[io]` says how much the engine read from files during the freeze (steady reads = it is loading, none = it is stuck),
and one `[thread]` line per engine thread gives its state (running / ready / waiting), priority and the CPU time it used:
a thread that keeps burning CPU while nothing is drawn is the one that hangs. For a waiting thread the line also names the
kernel object it waits on (semaphore / event flag / mutex / condition and its name), which tells whose wait it is
(vitaGL, pthreads, newlib...). `[gpu]` appears if the game thread is stuck inside `vglSwapBuffers`.
`[opensl]` shows how many sound-effect players exist and that the mixer is running (see "Sound effects").
`[stack]` lines (5 s and 30 s into a freeze) list the code addresses found on `UE3GameThread`'s stack, outermost first:
`E+0x...` are engine offsets (`tools/resolve_addr.py`), `L+0x...` are offsets into the loader itself; resolve those with
`arm-vita-eabi-addr2line -f -C -e build/so_loader 0x<0x81000000 + offset>` (the unstripped ELF from your build),
assuming the log's `loader code at 0x81000000-...` line shows that base. The first
40 touches are also logged as `[input] touch N ...` / `[input] touch N handled`.

## Touch input

Moves are only forwarded when the finger actually moved (as on Android); repeated identical positions from the 120 Hz
poll made Scaleform's scrolling menus think the finger had stopped.

At startup the engine reads `MotionEvent.ACTION_*` and `KeyEvent.KEYCODE_*` from Java with `GetStaticIntField`. FalsoJNI
did not know them and answered 0 for all of them, so the engine took every touch-up for a move: as far as it knew, no
finger was ever lifted. The real Android values are now in the generated field table (`tools/gen_java.py`,
`ANDROID_INT_CONSTANTS`). Touch ids are also translated to small Android-style pointer ids (0, 1, 2...), because the
Vita's touch report ids keep counting up. The engine's `sched_yield` now really yields (a 100 µs sleep).

Threads get the stack size the engine asks for (between 512 KiB and 4 MiB; Android's default is 1 MiB). The boilerplate
forced 512 KiB on every thread, less than PhysX and the engine request.

## Threads and locks

Bionic's `pthread_mutex_t` and `pthread_cond_t` are 4 bytes, so `reimpl/pthr.c` keeps a pointer to a real newlib object in
them and creates it the first time it is used (the engine often uses static initializers instead of `*_init`).
The boilerplate tracked every created object in a fixed table of 1024 entries. Unreal Engine 3 makes many more than that;
once the table was full, each lock, wait and signal quietly got a brand-new object, so a thread waiting for a signal never
received it. Now "already created?" is just "does it hold a pointer?", set with an atomic compare-and-swap, with no limit.
`sem_timedwait` also had its deadline test backwards (it gave up after 1 ms whenever the deadline was still ahead).

## Dialogue cut short (r54)

All dialogue, music and ambience share the one song player (as on Android: one MediaPlayer). A `PlaySong` while a
song plays fades that song out and starts the new one afterwards. The engine gets no "song finished" callback: it
times its lines itself, and asks for the room ambience (`sfx_amb_*`) or the next music when it thinks a line is over.
In the r53 log, `vo_trickster_part1_08`, `_09` and `_10` (43, 33 and 30 s) were each ended by
`sfx_amb_hallway_*` or `mus_sunroom_radio_01` before the decoder had reached their end, so they lost their last
1-3 s. One cause is that a line asked for while ambience plays only starts after the ambience has faded out, which
makes it start late against the engine's timing. Since r54:
- background music (`mus_*`) or ambience (`sfx_amb_*`) requested while a `vo_` line plays waits until that line has
  finished. The line is no longer faded out, and the request is logged as `waits for "vo_..." to finish`. Cutscene
  music (`cin_*`) does not wait: r54 held `cin_witch_2nd_encounter_flmx` back by ~8 s behind a trickster line;
- a `vo_` line replacing music or ambience starts after a 0.3 s fade instead of the full fade-out;
- a `vo_` line replacing another `vo_` line, and `StopSong` (e.g. a skipped cutscene), behave as before.

`DM_SONG_ANDROID_CUTS 1` in config.txt restores the exact Android behaviour. The requests are logged at info level
with a time stamp, along with what they end and how long that had played:
`[song] t=812.4 s PlaySong("vo_trickster_part1_10") ends "sfx_amb_hallway_ct_1" after 19840 ms (fading out over 1.0 s)`,
`[song] t=... StopSong ends "..." after N ms`, and `[song] t=... "X" decoded to the end (N ms)`. Playback ends up to
1.4 s after decoding does, because of the buffer.

## Sound effects (OpenSL ES)

The engine plays its sound effects through OpenSL ES. VitaSDK's `libOpenSLES` (Android's "wilhelm") did not work here:
its mixer thread never started on the console, and in that library stopping, destroying and even creating a player
waits for the mixer, so the game thread hung (first after "tap to start", later in `CreateAudioPlayer`).
`reimpl/opensl.c` is a small OpenSL ES of the loader's own with exactly what `UOpenSLAudioDevice` uses: engine, output
mix, buffer-queue audio players with 16-bit PCM, and the PLAY / VOLUME / BUFFERQUEUE interfaces (`dynlib.c` hands
`slCreateEngine` and those `SL_IID_*` to the engine). One thread mixes all players (linearly interpolated, volume
changes ramped over a block) together with the music/dialogue from `song.c` to 48 kHz on one audio port, then applies
`DM_AUDIO_MASTER` (`dm_config.h`, default 0.6) and a soft limiter. That headroom matters: Android applies the device
volume to each track before mixing, the Vita applies its volume after our mix, and the dialogue files already peak at
full scale, so without it loud scenes clipped hard ("blown out", crackly);
state changes take effect at once and nothing waits for the mixer. A one-shot player (no buffer callback) whose queue
runs dry switches to `SL_PLAYSTATE_STOPPED`: the engine polls `GetPlayState` (`FOpenSLSoundSource::IsSourceFinished`)
and frees the sound source when it reads STOPPED. Until r15 such players stayed PLAYING, so every finished sound held
its source until the engine's own time-out (about 2.5 times its length) and quick repeats - sword swipes - ran out of
sources after 5 or 6 and went silent. (An early build blamed STOPPED for sounds playing three times; that was the
position overflow below.) Looping sounds are refilled from their buffer-queue callback, which runs with no lock held.
The `[opensl]` trace logs the first 400 player operations, and after that still every sound of 1.5 s or longer. The play position
is 48.16 fixed point in 64 bits (a 32-bit 16.16 position wrapped after 65536 frames and replayed every sound longer
than about 1.5 s - heard as sounds playing three times). The output alternates between two buffers:
`sceAudioOutOutput` returns once the previous buffer has played and keeps reading the new one, so mixing the next
block into the same buffer overwrote audio still being played (crackle and buzz on the console; emulators copy the
buffer immediately, so Vita3K was clean). The watchdog's `[opensl]` line shows how many players
exist and whether the mixer is running.

## Music, ambience and dialogue

Everything in `UDKGame/Music/` of the .obb (music `mus_*`/`cin_*`, room ambience `sfx_amb_*` and all spoken dialogue
`vo_*`) is played on Android by the Java side with a MediaPlayer (`JavaCallback_PlaySong` and friends), not through
OpenSL ES. `source/song.c` reimplements that player: it finds the .mp3 inside the .obb zips (stored uncompressed; the
patch file wins), decodes it with libmpg123, resamples to 48 kHz and mixes it with the sound effects, with the Java
code's volume and fade rules (cross-fade between songs, fade out on stop, base/sound-class/music volume, pause/resume).
Decoding runs in its own thread that keeps about 1.4 s of audio buffered, so memory-card reads and MP3 decoding can not
starve the output (an earlier version decoded on the output thread and crackled). `[song] decoder fell behind` in the
log means the buffer ran empty anyway.

The .mp3 data is read with `sceIoPread` at explicit offsets on one descriptor per .obb that stays open, so nothing is
opened or allocated while the engine is loading. A song that produces no audio at all (r14 lost two dialogue lines
during level loads this way: the line ended at once and only its subtitle showed) is retried up to 3 times over
0.6 s before the game is told it ended. The log then says why:
`[song] "<name>": read failed / mpg123_feed failed / decode error / no audio frames found after N of M bytes`, and
`[song] "<name>" finished (frames = ms)` gives the length of every song that played to the end.

Every request from the engine is logged too (`[song] PlaySong(...)`, `StopSong`, `PauseSong`, `ResumeSong`, music
volume), with what became of it: played, queued behind a fade-out, replacing an earlier queued song, or ignored
because the music volume is 0. A voice line that never shows up there was not sent as a song (it is then an OpenSL
sound: see the `[opensl]` trace).

As in the Java original, the 50 ms fade timer keeps running when no song is playing: if a song ends by itself while
it is fading out, the song queued behind the fade still starts when the fade time is up. (r16 stopped the timer with
the song, so after reawakening `vo_trickster_part1_03`, queued behind the fade of `cin_awaken_flmx`, was lost.)

## Expansion-file streams

The engine fopen()s the .obb once per open package and keeps well over a hundred open while a level loads. With a real
descriptor each, a level reload after death ran out (fopen returned NULL, crash in unzReadFile). `reimpl/obb_stream.c`
makes those read-only streams with newlib `funopen()` over one shared descriptor per .obb (`sceIoPread`, each stream
keeps its own position); `_fileno` on such a stream opens a private descriptor on demand.

## Saves

`stat`/`fstat`/`lstat` hand the engine bionic's ARM `struct stat`. The boilerplate declared that struct packed and with
16-bit fields, so `st_size` landed at offset 38 instead of 48 and the engine read garbage sizes; it could not load its
save (it replayed the intro and overwrote `PactSave.bin` with a fresh game). Fixed in `reimpl/io.h`.

The game autosaves on its own (`files/TheDarkMeadow/PactSave.bin`, plus `PactMetadata.bin`; it also probes
`PactSave1..3.bin`, which do not exist until used). Every file the engine writes is reported when it is closed, e.g.
`close(ux0:data/darkmeadow//files/TheDarkMeadow/PactSave.bin): wrote 12345 bytes`, with a warning on any failed write.
`rename()` follows POSIX (it replaces an existing destination, which `sceIoRename` does not).

## Google account

After "tap to start" the game asks Java for the device's Google accounts (`JavaCallback_GetAvailableAccounts`) and does not go
on until one comes back through `NativeCallback_ReceivedNewAccount`; on Android, with no account it shows "A Google account must
be signed in" and quits. The loader answers with a fixed local name, `DM_ACCOUNT_NAME` in `dm_config.h`. Nothing is sent
anywhere. `JavaCallback_GetJavaDate` returns today's date in the same `MM/yy/dd` form the Java code uses.

## Graphics profile (the real cause of the black metal props)

The engine picks its graphics settings from the device identity the Java side reports (`NativeCallback_InitModel`:
manufacturer, model, product) and the GPU name. The profiles are in `Android-UDKEngine.ini` inside
`Coalesced_INT.bin` (AES-256 encrypted; the key is in `SetupAESKey`, stored reversed). Every device family the game
knows - `[SystemSettingsAndroid_Samsung]`, `_Qualcomm`, `_Tegra3`, `_Motorola`, `_LG` - is based on
`[SystemSettingsMobile]`, with specular, environment (reflection) maps, normal maps, rim lighting and fog. An unknown
manufacturer gets `[SystemSettingsAndroid]`, based on `[SystemSettingsMobileTextureBias]`, which turns all of those
off. r1-r25 reported "Sony", so the game ran in that low mode: the metal props (bedpan, medical table, overbed table,
kidney dish), whose 4 bpp RGBA base textures are very dark because their look comes from the reflection and specular,
rendered black, characters lost their rim lighting, and some glow effects were missing.

r26 reports **samsung / GT-I9300** (`dm_config.h`), which selects `[SystemSettingsAndroid_Samsung]`. To compare, or if
a Samsung-specific workaround in the engine causes trouble, override it in `ux0:data/darkmeadow/config.txt`:

```
DM_DEVICE_MANUFACTURER Sony
DM_DEVICE_MODEL Vita
```

The richer shaders cost GPU time; the old low profile is one config line away.

## The black metal props (r27-r29)

The base textures of the bedpan, medical table, overbed table and IV stand are PVRTC 4 bpp RGBA with near-black
colour and a picture in the alpha channel. The game's reflection shaders use that alpha as the reflection mask
(`PolyColor.rgb += EnvironmentColor.rgb * EnvironmentColorScale * EnvironmentAmount * BaseColor.a`), so the props
need the reflection shaders, which the engine only picks with the right graphics profile (r29, below). r27's
alpha-as-grey-colour workaround and r28's poke of a GSystemSettings flag (it turned out to be rim lighting) were
removed in r38.

### r29: the profile was never applied

r28's dump of `GSystemSettings` showed the mobile feature flags all off, as in the low `[SystemSettingsAndroid]`
profile, even though InitModel was told "samsung" since r26. Forcing the word at 0x43c turned out to be the wrong
switch (characters glowed white and the weapon vanished; the witch's glow came back, so it is most likely rim
lighting). Instead of poking flags, r29 replaces `appGetMobileSystemSettingsSectionName()` (the engine function
that picks the profile section, from device checks that cache their first answer) so it returns
`SystemSettingsAndroid_Samsung`. config.txt:

```
DM_SETTINGS_SECTION SystemSettingsAndroid_Samsung   (default; any section of Android-UDKEngine.ini)
DM_SETTINGS_SECTION engine                          (leave the choice to the engine, as before r29)
```

The log shows the answer given (`[gl] engine asked for its settings section: answering ...`).

Why the engine never chose Samsung on its own: `GetTegraDeviceType()` reads `/sys/devices/system/cpu/present`,
`/proc/cpuinfo` and `/proc/config.gz`; on the Vita those fail and it answers "Tegra 2", which is checked before
the manufacturer, so the engine took `[SystemSettingsAndroid_Tegra2]` (low profile). The Tegra 2 answer is used only
for that choice.

### r30: decal flicker

With the full profile, static decals (the rubble piles) are drawn, offset towards the camera with
`glPolygonOffset(slope, DepthBias * 65535)`. vitaGL passes both to `sceGxmSetFrontDepthBias` as integers, so the
engine's fractional values became 0 and the decal edges fought the floor. `glPolygonOffset_soloader` scales them by
`DM_DECAL_BIAS_SCALE` (default 2, `0` = unchanged) and rounds away from zero; the first values are logged as
`[gl] glPolygonOffset(a, b) -> (c, d)`. If edges still flicker, try 4 or 8; if decals float or show through
objects, try 1.

### r31: decal bias defaults

The console tests found 6 the best constant scale (r30 log: the engine asks for `glPolygonOffset(0, -3.93)`), now
the default. The engine asks for no slope term, and a constant offset cannot cover floors seen at a grazing angle,
so a slope offset of `DM_DECAL_SLOPE` (default 1, `0` = none, 2-3 if edges still flicker far away) is added.

## Free Sun Coins (Tapjoy, r31)

The "Free Sun Coins" button opens the Tapjoy offer wall (`JavaCallback_TapjoyShowOffers`). There is no Tapjoy here,
so `java_impl.c` plays the server: every press earns 100 tap points (`DM_FREE_SUNCOINS`) and the game is told its
balance (`NativeCallback_TapjoyGetUpdatePoints`); the game converts points into Sun Coins by spending them
(`JavaCallback_TapjoySpendTapPoints` -> `NativeCallback_TapjoyGetSpendPointsResponse`). The unspent balance is kept in
`ux0:data/darkmeadow/tapjoy.txt`. Every step is logged with `[tapjoy]`.

In-app purchases (r33): billing is reported as supported and every store purchase (money bags, Sun Coin packs)
succeeds immediately and for free (`JavaCallback_AndroidIAPRequestPurchase` ->
`NativeCallback_AndroidIAPPurchaseSucceeded`), logged with `[iap]`. `DM_FREE_IAP 0` in config.txt turns it off.

The `?INT?HOR_obj_GPTemplates.GT_GemPack5...ItemName?` labels in the store are not a port problem: the game's own
English localization (`HOR_obj_GPTemplates.int` etc. inside `Coalesced_INT.bin`) has no `ItemName` for those packs.

## CPU cores and the rendering thread (r34)

Every 10 s the log gets a `[cpu]` block: frames and fps over the period, whether the engine renders on its own
thread, then one line per thread (engine threads by entry point: UE3GameThread, UE3ThreadPool, UE3Runnable, PhysX;
loader threads: main, dm_controls, dm_opensl_mixer, dm_song_decoder, dm_watchdog) with the share of one core it used,
the core it last ran on, its affinity mask (0 = any core, the kernel decides) and priority, and a per-core total.

Why everything runs on one thread: `FEngineLoop::PreInit` starts UE3's rendering thread only when `GIsClient`, no
`-ONETHREAD` and `GNumHardwareThreads > 1`. `GNumHardwareThreads` is a data variable that is 1 and that nothing in the
Android build ever changes, so the game has always rendered on its game thread (game logic + every GL call + the
swap). `DM_RENDER_THREAD 1` in config.txt sets it to 3 before the engine starts, so rendering gets its own thread
(`engine.c`). That path was never used on Android. Measured in r34: without it the game thread sat at ~100% of a core
and the game averaged 45.2 fps; with it the work split into game thread ~41-57% and rendering thread ~72-75%, and the
average rose to 52.8 fps. On by default since r35 (`DM_RENDER_THREAD 0` turns it off).

Pinning threads to fixed cores (r35) measured the same as letting the kernel choose and felt worse; removed in r38.

### r36: GPU wait and duplicate links

Each `[cpu]` block is followed by a `[gpu]` line: average frame time, average time spent waiting inside
`vglSwapBuffers` (it blocks while the GPU is still busy with earlier frames, so a large share means the GPU is the
limit) and the longest frame of the period. The engine links every program twice; vitaGL ignores the second link
and only logged a warning (~340 times during loading), so `glLinkProgram_soloader` skips links of programs that are
already linked.

### r37/r38: memory, loading time, text trace

- `[mem]` after each `[cpu]` block: newlib heap used/free (of `DM_HEAP_MB`), vitaGL's free video RAM and main RAM
  (sampled on the rendering thread), and the kernel's free user RAM / CDRAM / phycont, with the lowest value of the
  period.
- `[io]` (only for periods where something loaded): time spent in zlib `inflate` (packages in the .obb are deflated),
  in `sceIoPread` for the expansion-file streams. (r37 measured expansion-file reads at about 2.5 MB/s through the
  engine's 8 KB stream reads.)
- r37 also kept the battle sound tables in RAM (about 8 MB); removed in r38, back in r40 (below).
- `[text]` lines: `UCanvas::WrappedPrint` (measure and draw calls) and the static `UCanvas::ClippedStrLen` (used by
  the game's own `UCanvas::Horror_TextSize`; r37 hooked it with a wrong signature and crashed) are hooked; each
  distinct text/width/result is logged once with the width it was wrapped to and the size that came out, to find why
  journal paragraphs and save-slot lines overlap by one line.

### r39: text fit, read-ahead, MSAA and 30 fps

- **Overlapping text.** r38's `[text]` trace showed the cause: the game sizes multi-line text by measuring it on one
  unwrapped line (`UCanvas::Horror_TextSize` -> `ClippedStrLen`) and dividing by the box width, which ignores the room
  lost at each wrapped line end, so the estimate is often one line short (2902 / 749 -> 4 lines estimated, 5 drawn).
  The loader now remembers, per text, the width and height of the real wrapped draw (`WrappedPrint`), and when the
  same text is measured again reports a width just large enough that the game's estimate matches. Logged once per
  text as `[text] fixed: ...`. `DM_TEXT_FIX 0` turns it off; `DM_TEXT_TRACE 0` stops the `[text]` trace lines.
- **Read-ahead** (`reimpl/obb_stream.c`): each .obb gets a shared cache of 8 blocks of 128 KB; the engine's 8 KB
  stream reads are served from it, misses load a whole block, reads of 128 KB or more go straight to the card.
  `[io]` now shows the read speed and the hit/load counts. `DM_OBB_READAHEAD 0` turns it off.
- **`DM_MSAA 2` / `4`** (default 0): multisampled screen (`vglInitExtended`). It does not cover anything the engine
  renders into its own framebuffers; the first such attachments are logged as `[fbo] ...` and the `[gpu]` line counts
  offscreen framebuffer binds per period.
- **`DM_FPS_LIMIT 30`** (default 60): presents every second screen refresh (`eglSwapInterval(.., 2)`).
- The line `[gl] screen: MSAA ..., frame rate limit ...` confirms both at startup.
- r39 results: no `[fbo]` lines (the engine draws straight to the screen, so MSAA covers everything); average fps
  51.3 without MSAA, 51.6 with 2x, 54.0 with 4x (different routes, so: no measurable cost). Read-ahead raised
  expansion-file reads from 1.5-2.5 MB/s to ~11 MB/s.

### r40: journal spacing and the battle sound tables

- The journal does not round its estimate up: it places the next paragraph (unwrapped width / box width) x ~29 px
  further down while lines are 31 px apart, so r39's fix was not enough. The text hook now also watches consecutive
  draws in the same column: if a text is followed by another closer than its real height, its reported width is
  scaled up by the missing ratio (max 1.6x), which settles within a frame or two. Logged once per text as
  `[text] spacing: ...`.
- The battle sound tables are preloaded into RAM again (about 8 MB, `[pcache]`), on top of the read-ahead: r39 showed
  that roughly half of each 0.7-0.9 s battle-start freeze is reading them (the rest is unpacking, which a cache of
  the compressed bytes cannot remove). `DM_PACKAGE_CACHE 0` / `DM_PACKAGE_CACHE_FILES` as in r37; the `[io]` line
  shows how much the cache served.

### r41: smaller wrapped text instead of measurement fixes

r40's logs pin the journal layout down: the next paragraph is placed (unwrapped width / box width) x 29.1 px lower
(31 px lines x 960/1024), never less than 65 px, and the layout is computed once when a page opens, so correcting
later measurements (r39 `fixed`, r40 `spacing`) cannot move it. Both corrections are removed. Instead, text that has
to wrap (its unwrapped width is larger than its box) is drawn, and wrap-measured, at `DM_TEXT_SCALE` (default 0.8) of
its size, while the game's estimate keeps using the full size. At 0.8 a paragraph needs about (width / box) x 21 px +
12 px of the ~29 px per unit the game reserves, so it no longer runs into the next one; long subtitles take fewer,
smaller lines and stay on screen. Text that fits on one line (menus, labels, centred messages) is unchanged.
`DM_TEXT_SCALE 0.85` / `0.9` gives larger text (0.85 can still overlap short paragraphs by a few pixels; 0.9 often
does); `1.0` turns it off.

r42: `WrappedPrint` centres each line with its unscaled width, so scaled centred text (subtitles, journal headings)
drifted left by about an eighth of the line width, and long first lines sat against the left edge. For scaled,
centred text the hook now wraps the lines itself (greedy, by words, honouring line breaks, measured with
`ClippedStrLen` at the drawn scale) and draws each line with centring off at the true centre of the box. The text
trace now also logs short labels (6+ characters), with a `centred` marker, to size the shop texts that overflow
their panels.

r43: the centring did not come from `WrappedPrint`'s Center argument at all (`UCanvas::DrawWrappedText` ignores it)
but from the canvas's `bCenter` flag (bit 0 of the byte at 0x6c), so in r42 every line after the first was centred
twice and sat too far right. The flag is now cleared while the re-centred lines are drawn, and also used to decide
which texts are centred.

r40 also showed the sound-table preload serving ~4 MB at every battle start; the longest frame at a battle start
went from ~750-900 ms to ~450-1000 ms, so the rest of that freeze is unpacking and the engine's own loading.

## Movies (r43)

The game's movies (`UDKGame/Movies/Startup.m4v` 1024x768, `Ending.m4v`, `end_meadow.m4v`, `end_psychWard.m4v`
1366x720, all 30 fps) are MPEG-4 Part 2 + AAC. The Vita's hardware decoder (SceAvPlayer) only plays H.264, so they
are converted once on a PC: `tools/convert_movies.sh <folder with the .m4v> <out>` (ffmpeg + libx264: H.264 Main,
960x544 with black bars where the shape differs, BT.709, AAC kept). Copy the `.mp4` files to
`ux0:data/darkmeadow/movies/`. A movie with no converted file is skipped, as before; the game's "Loading" movie does
not exist in the data and is always skipped.

`movie.c`: `JavaCallback_StartVideo` queues the file; the rendering thread, at every buffer swap, opens it with
SceAvPlayer, takes the newest decoded frame and draws it over the screen as a YUV texture (the GPU converts to RGB)
with a small shader and its own vertex array object. Every GL state it touches is saved and restored, so the engine's
cached state stays valid. Audio plays on its own audio port. `IsVideoPlaying` reports it until it ends, then
`NativeCallback_MovieFinished` is sent. Tap the screen or press X / O / START to skip. Decoder memory comes from
physically contiguous memory if any is free, else from vitaGL's video RAM pool. If no frame is presented within 3 s
of the request (the engine not rendering), the movie is given up. Logged with `[movie]`.

Since r46 the movie name the engine passes is logged (`[movie] engine asked for "..."`) and reduced to its bare
name before `.mp4` is added: any folder part (`/` or `\`) and any extension (`.m4v`) are stripped. r43-r45 only
stripped `/` folders, so a name passed as `..\..\UDKGame\Movies\Startup.m4v` or `Startup.m4v` was looked up as the
wrong file and the movie was skipped.

## Unpacked package cache (r48)

Some packages are loaded again and again: the sound tables (`light_sound_table_SF.xxx`, `heavy_sound_table_SF.xxx`,
about 5 MB each) at every battle start, and the same enemy package (`HOR_ai_light_00a.xxx`: AI, mesh, animations,
materials, 1.3-3 MB) for each battle on a floor. In the r47 log each battle start froze the game for 0.65-1.4 s, and
roughly half of that was file work: reading the deflated zip entry from the .obb, then inflating it (about 45 ms
per MB). r40-r47 kept the *compressed* bytes of the two sound tables in RAM, which removed the reading but not the
inflating. `source/reimpl/pkg_cache.c` keeps packages **unpacked**:
- **Preloaded:** the listed packages are read and inflated once in a background thread (priority 160, just below the
  engine's) right after start, and stay for the whole session. The default list (`DM_PACKAGE_CACHE_DEFAULT` in
  `dm_config.h`) is the five sound tables: light, heavy, witch, trickster and `ct_boss`, about 22 MB.
- **Kept after first load:** while the engine reads any other package of 32 KB-8 MB, a copy of the bytes it has just
  unpacked is kept, so the next load of it comes from RAM. The async loader reads a package in pieces: it seeks
  back, which closes and reopens the zip entry, and every handle starts again from the beginning. In r48 a copy had
  to come from one uninterrupted read, so no enemy package was ever kept. Since r49 the copy belongs to the package
  and grows with whichever handle reads furthest. A partly filled copy already serves the part it holds; past that,
  the engine's own handle is brought up to the same point and carries on, and its output extends the copy. This adds no extra reading or
  inflating. Bigger packages (whole areas, loaded once per visit) are not kept. When the limit is reached, kept packages
  that were never used again go first (oldest first), then the least recently used. Preloaded ones are never dropped.

The engine reads a zip entry through `ZipFS_openFileFromDirectoryEntry`, `ZipFS_readFile` and `ZipFS_closeFile`. These
three are hooked in memory (the .so file is not changed). For a cached entry (same name and uncompressed size),
`ZipFS_readFile` is answered with a memcpy. The engine's own handle is still opened and closed normally; it just never
inflates. Textures (`.tfc`, stored uncompressed in the patch obb) are read directly from the card, as before.

Log: `[pcache] ... unpacked in RAM` (preload), `[pcache] kept X (N KB) after its first load; cache A KB of B`,
`[pcache] X served from RAM (use N)`, `[pcache] dropped X ... to make room`. The `[io]` line's
`package cache served N KB` counts everything the engine got from RAM.

**Hitch profiler (r49).** r48 showed battle-start freezes of 0.6-1.4 s even when that 10-second window did under
100 ms of file work, so the rest is engine work. `UObject::LoadPackage`, `FlushAsyncLoading` (the game thread waiting
for the async loader) and `CollectGarbage` are timed, and any call over 30 ms is logged as `[hitch] ...: N ms`. The
first 200 `LoadPackageAsync` requests are listed as `[hitch] async load requested: ...`.

r49's profiler put the battle-start freeze at `FlushAsyncLoading`: the game thread waits (spinning on `sched_yield`)
for the async loader to finish the enemy package. That took 600-1200 ms for an enemy and ~100 ms for a sound table
served from RAM, while the same enemy package (`HOR_ai_light_04a`) took 969 ms and then 960 ms even when served from
RAM. So the remaining time is the loader turning the package into objects, not reading or unpacking it. Most
enemies are a different variant at every encounter anyway (`_00a`, `_00b`, `_CT01`...). Note that the r49 run had
`DM_TEST_DUMP_TEXTURES` on, which writes a file per texture during exactly those loads. The `[pcache] ... partly`
lines during a *first* load are the async loader reopening the entry: each reopen used to inflate again from the
start, and now reads the part already unpacked from RAM.

Since r50 the `[gpu]` line is followed by `[gpu] render thread: N shader programs linked in X ms (longest Y ms), N
texture levels uploaded (K KB) in Z ms`. The `[io]` line ends with `texture-cache reads N ms for K KB` (the `.tfc`
reads through the engine's own `open()` of the patch obb, which were not timed before). These show whether the wait
is shader linking or texture uploads on the render thread, or texture reads.

config.txt:
- `DM_PACKAGE_CACHE 0` turns it off.
- `DM_PACKAGE_CACHE_MB 48` is the RAM limit for both kinds together (default 48 MB since r54; 64 before). Peak heap use in r47 was about
  86 MB of 200.
- `DM_PACKAGE_CACHE_KEEP 0` keeps only the preloaded list.
- `DM_PACKAGE_CACHE_FILES ...` replaces the preload list.
- `DM_PACKAGE_CACHE_ADD ...` adds to the preload list. A trailing `*` matches a prefix, e.g.
  `DM_PACKAGE_CACHE_ADD HOR_ai_light_0*`.

**Loose-file mode (r43-r47) was removed.** Extracting the .obb files and reading loose files (`DM_ZIP_VFS 0`) would
only have traded inflate time for reading about 60% more bytes at the card's 9-12 MB/s. It also ran into a latent
engine bug: the plain-file reader's 4 KB buffer goes stale after large reads, so seeks return wrong data. Shipping
Android builds never take that path. What remains from that work:
- vitaGL's GLSL preprocessor no longer ends the game when it throws on a shader. It saves the source as
  `shader_error_N.txt`, returns a blank shader and logs `[gl] ... vitaGL's GLSL preprocessor failed`.
- The engine's `lseek` and `ftruncate` go through wrappers with bionic's 32-bit `off_t` arguments.
- `ue3_hook` (in `ue3_trace.c`) can relocate a pc-relative literal load in a function's first two instructions.

## Shader test switches (config.txt) and black props

**The black metal props (bedpan, bed frame, cabinet, folding stand): the depth pre-pass.** The engine draws some
objects twice: first with a "depth-only" program to fill the depth buffer (its pixel shader outputs
`vec4(0,0,0,0)` and the engine never masks colour writes, so it really draws black), then with the material, depth
test LessEqual (the engine's only depth states are LessEqual and Always). Most depth-only vertex shaders compute the
position as `LocalToProjection * Position`, but every material that needs the world position - environment-mapped
metal, dynamically lit characters - computes `ViewProjection * (LocalToWorld * Position)`. On the Vita the two results
differ in the last bits, the material's depth lands a hair behind the pre-pass and fails the test, and the object
keeps the pre-pass black. `utils/gldiag.c` recognises the depth-only programs (a pixel shader writing `vec4(0,0,0,0)`
paired with a camera-space vertex shader; shadow-depth passes are left alone) and pushes their depth back by
`DM_DEPTH_PREPASS_BIAS` (config.txt, default `0.0002` of the clip range; `0` turns it off). `DM_TEST_NO_DEPTH_PREPASS 1`
instead removes the pre-pass geometry entirely. The test switches below could not change those props, because what
showed on screen was the pre-pass, not the material.

The engine does not build its shaders from the `.msf` sources with `#define USE_xxx` switches: it sends text that was
preprocessed when the game was cooked (`MobileUsePreprocessedShaders`; `PreprocessedShadersInfo.bin` indexes 964 of
them inside `AllShaders.bin` in the main .obb). Material features are already resolved in that text, so they can only
be changed by editing it. (r15/r16's `USE_ENVIRONMENT_MAPPING 0` switch could not work for that reason.)

**What r20 showed**: in false colour the bedpan, bed frame and stand are dark green: they use the light-mapped
material (not the reflective one), and their base texture contributes nothing, i.e. it samples black. The textures
themselves decode correctly (r19), so either the props' texture coordinates are broken or they are drawn with the wrong
or an empty texture. `DM_TEST_SHOW_UV 1` (r21) shows each world surface's texture coordinates as colour (red = U,
green = V): normal surfaces show red/green gradients, a surface with broken coordinates one flat colour. The log lists
the first draw with every texture (`[draw] program N first use of ...`) and warns about textures drawn without ever
having been uploaded.

**What r21 showed**: with `DM_TEST_SHOW_UV` the props show sensible texture coordinates, so they sample the right
place; their texture decodes correctly (r19) - yet it samples black. The remaining suspect is the texture data the
engine reads: textures whose full-size mips were moved to the texture caches (`Textures.tfc`, `Lighting.tfc`,
`CharTextures.tfc` in the patch .obb) are read by opening the .obb, seeking and reading; data that comes back short or
as zeros is uploaded anyway and zero PVRTC data is solid black. r22 checks every read from an .obb descriptor and
logs `[obb read] ... ALL ZEROS` / short reads (with the engine address that asked), plus the first 120 normal reads.

**What r22 showed**: the texture-cache reads (`Textures.tfc`, `CharTextures.tfc`) return full, non-zero data. The
engine decompresses that data with zlib `uncompress()` on its worker threads (`appUncompressMemoryZLIB`; it supports
no other method). r23 wraps `uncompress` (`reimpl/io_stats.c`) and logs failures, wrong output sizes and all-zero
output as `[zlib] ...`.

**What r23 showed**: `uncompress` is never called; the texture caches hold raw PVRTC (no decompression involved).
The game's settings (`Coalesced_INT.bin`, AES-256 with the key from `SetupAESKey`, stored reversed) give the default
`[SystemSettingsAndroid]` profile (what "Sony / PlayStation Vita" gets): it does not switch off any material feature.
`DM_TEST_DUMP_TEXTURES 1` (r24) writes a 64x64 preview of every PVRTC texture the game uploads to
`ux0:data/darkmeadow/texdump/tex<id>_<w>x<h>_<n>.tga` (ids as in the log), to see whether the props' own texture data
is dark. The log also warns about textures uploaded without data and lists `glTexImage2D` calls. Since r50 the
file name is `tex<id>_<w>x<h>_<format>_<n>.tga`. Uncompressed RGB/RGBA uploads are dumped too, an image identical to
one already written is skipped, and up to 3000 files are written (r49 stopped at 600, before floor 17).

**Texture ID tint (r25)**: `DM_TEST_TEX_TINT 1` together with `DM_TEST_BASE_ONLY 1` replaces, at draw time, each
program's base texture with a solid colour that encodes the texture's number: red = (n % 8) * 36, green =
((n / 8) % 8) * 36, blue = ((n / 64) % 8) * 36. The colour of an object in a screenshot gives the texture it is drawn
with; the log lists each mapping (`[tint] texture N is colour (r, g, b)`), and `DM_TEST_DUMP_TEXTURES` shows that
texture's content.

**False colour (r20)**: `DM_TEST_FALSE_COLOUR 1` paints every world-material shader by kind (lightly mixed with its
base texture so the room stays readable): **red** = depth-only pre-pass (normally drawn black), **magenta** =
environment-mapped (metal), **cyan** = emissive, **green** = light-mapped, **blue** = dynamically lit, **yellow** =
other material. UI, particles and post-processing keep their normal look. An object that stays black is drawn by
none of these.

**Texture switches (r19)**: with `DM_TEST_BASE_ONLY` the metal props stayed black, so their base texture samples
black. Two more switches tell why:

| Switch | What it does | If the props show up... |
|---|---|---|
| `DM_TEST_NO_MIPMAPS` | every texture samples only its full-size level | the smaller mip levels are broken (vitaGL's PVRTC mip layout) |
| `DM_TEST_PVRTC_SOFTWARE` | PVRTC is decoded by the loader (`utils/pvrtc.c`, checked bit for bit against an independent decoder) and uploaded as plain RGBA | the console's GPU reads those PVRTC textures differently (uses about 4-8x the texture memory, so only for testing) |

The log lists every PVRTC texture's level 0 (`[tex] texture N: PVRTC 4bpp RGBA 256x256`), warns about levels whose
data size is not what PVRTC needs and about textures whose first upload is not level 0, and the per-program report
names each texture's format and how many mip levels it received.

**Test switches**: one per line in `ux0:data/darkmeadow/config.txt`, e.g. `DM_TEST_BASE_ONLY 1`. Each replaces one
piece of every pixel shader, so a cause can be ruled in or out on the console without rebuilding. Use one at a time:

| Switch | What it does | If the props change... |
|---|---|---|
| `DM_TEST_WHITE` | every surface pure white | still black/missing: they are not drawn properly at all |
| `DM_TEST_BASE_ONLY` | only the colour texture, no lighting | still black: the colour texture itself is the problem |
| `DM_TEST_NO_LIGHTMAP` | light maps replaced by mid grey | they show up: their light map (static lighting) is broken |
| `DM_TEST_NO_LIGHTING` | dynamic lighting skipped | they show up: the dynamic light values are wrong |
| `DM_TEST_NO_ENV` | reflection texture replaced by black | they show up: the reflection is the problem |

The log confirms an active switch (`[gl] DM_TEST_... is changing pixel shaders`). Shaders are compiled again on the
first start after a change (slow once). Delete the line to go back.

**Diagnostics** (Debug builds): for each shader program, at its first real draw, the log shows the textures it samples
with size and format (`NO TEXTURE` is a red flag) and the lighting uniforms it was given. The engine first draws every
program once without textures while loading, to warm the shaders up; those draws are skipped. (vitaGL's uniform
locations are pointer-sized and often negative; r16 stored them in 16 bits and missed every sampler assignment.)

`glDrawArrays` calls over 49152 vertices (vitaGL's built-in index buffer) are split into pieces; the log warns once.

`GL_UNPACK_ALIGNMENT`: vitaGL ignores it and always reads texture rows tightly packed, while the engine uploads with
an alignment of min(width, 8). `source/utils/gldiag.c` repacks uploads whose rows are padded, so they are not sheared.

## Shader cache

Compiled shaders are cached by vitaGL itself (`HAVE_SHADER_CACHE`, `vglSetShaderCachePath`) in
`ux0:data/darkmeadow/shader_cache/` and re-used on later starts, so the first run is slow and later ones are fast.
The boilerplate's own cache (`DUMP_COMPILED_SHADERS`, folder `gxp/`) is OFF: it calls `vglGetShaderBinary` right after
`glCompileShader`, but with `VGL_MODE_POSTPONED` vitaGL only compiles in `glLinkProgram`, so it saved empty files. You can
delete `ux0:data/darkmeadow/gxp/`. Debug builds also send vitaGL's shader compiler messages to `log.txt`.

## Saves and other writable folders

Android already has the game's folders (for example `files/TheDarkMeadow/`); here nothing does. Whenever the engine opens a
file for writing (`open` with `O_CREAT`, or `fopen` in `w`/`a`/`+` mode), the loader first creates the missing parent
folders. Without that the save writer came back NULL and the engine crashed in `UEngine::BasicSaveObject`.

## Why only PVRTC is advertised

The engine builds its content folder name in `appAndroidGetCookedDirectoryName()` from a bitmask filled in by looking for
compressed texture extensions in `GL_EXTENSIONS`, and takes the first match in the order DXT, PVRTC, ATITC, ETC
(folder `CookedAndroid_<format>`). vitaGL advertises all four, so the engine looked in `CookedAndroid_DXT`, which is empty in
the PowerVR build's `.obb` (the data is in `CookedAndroid_PVRTC`), and `Coalesced_INT.bin` was "not found". `glGetString_soloader`
(`source/utils/glutil.c`) hides the DXT/ATC/ETC1 names. If you use content cooked for another format, change
`hidden_extensions` there.

## Tracing file lookups

`source/reimpl/ue3_trace.c` wraps three engine functions with pass-through loggers (`[ue3]` lines in `log.txt`):
`unzOpen2` (did the engine open the `.obb` as a zip?), `FFileManagerAndroid::FileSize` and `FindFiles` (which paths and
patterns it asks for, and what came back). A wrapper is only installed when the function's first two instructions match
what was expected; otherwise a `hook:` warning is logged and the function is left alone.

## Read-ahead for the texture caches (r51)

The engine reads textures from the `.tfc` files in the patch obb with its own `open()`/`lseek()`/`read()`: it opens
the obb once per texture and reads it in pieces. r50 timed those reads at 2-4 MB/s (37 MB in 8.9 s over a run), against
9-12 MB/s for the main obb, whose streams have read-ahead. Since r51 `read_counted` (`reimpl/io_stats.c`) sends reads
smaller than 128 KB on such a descriptor through the same shared read-ahead blocks (`obb_ra_pread` in
`obb_stream.c`: 8 x 128 KB per obb). It then moves the descriptor to where a real read would have left it. Larger
reads still go straight to the card. `DM_OBB_READAHEAD 0` turns both off. The `[io]` line's
`texture-cache reads N ms for K KB` shows the effect.

## vitaGL splash: garbled characters in the corner (r53)

vitaGL's start-up splash prints its git commit as `#` + 7 hex digits in the lower left, using a tiny 0-9/a-f font.
The Makefile takes the hash from `git rev-parse` and falls back to `unknown` when vitaGL is not a git checkout (as
here, a copied source tree). The splash then turned `u`, `n`, `k`... into font indexes far past the end of the table and
drew whatever memory followed. Since r53 the Makefile falls back to the commit this copy was taken from (`dca4b9d`,
see `lib/SUBMODULES.txt`), and `splashscreen.c` stops at any character it has no glyph for. `make` does not notice a
changed `-D` flag, but `splashscreen.c` itself changed, so it is rebuilt either way. A fresh folder (or
`make -C lib/vitagl clean`) rebuilds everything.

## Texture-data cache, memory valve (r54)

r53's measurement settled it: 66% of the texture data the engine read (44.5 of 67.1 MB over a 26-minute run) had
been read before in the same session, and the distinct texture data came to 22.8 MB. `reimpl/tex_cache.c` keeps each
read the engine makes through its own `open()` of an .obb, keyed by (obb, offset, length), since the engine repeats
exactly the same reads. A repeat is answered from memory (`read_counted` in `io_stats.c` checks the cache before the
read-ahead and the card). The limit is `DM_TEXTURE_CACHE_MB` (default 32 since r57, was 24; 0 = off), with single reads above 2 MB not
kept. When it is full, entries are dropped with the CLOCK rule.

**Where it lives:** vitaGL's video-memory pool (76 MB) had 59+ MB free at its lowest in every run, while the heap got
down to 37 MB free. So `gl_init` takes one block of the cache's size from that pool right after vitaGL starts, before
the rendering thread exists (vitaGL's allocator is never called from two threads at once here). The cache allocates
inside that block with its own `sceClibMspace`, logged as `[texcache] N MB block in video memory`. The CPU reads video
memory more slowly than main RAM, but still far faster than the card. If the block cannot be had, the cache uses the
heap. With the texture data out of the heap, the package cache's default went from 64 to 48 MB
(`DM_PACKAGE_CACHE_MB`), which should leave about 50 MB of heap free at the r53 peak instead of 37.

**Memory valve:** the engine's `malloc`, `realloc`, `calloc` and `memalign` go through `reimpl/mem_valve.c`. If one
fails, the caches let go of what they hold on the heap: kept packages first, then preloaded ones, then heap-mode
texture data. Then the allocation is tried again. Running short of memory costs cache hits instead of a crash, logged
as `[mem] engine malloc of N bytes failed: caches gave back K KB, retry succeeded`.

The `[io]` line adds `texture-data cache served N KB, holds H KB of L`.

## r73: English only, build guide

The language setting of r72 is gone again: the game always runs in English (`DM_LANGUAGE` "en" in dm_config.h).
BUILD.md describes building the VPK and installing it; `.gitignore` keeps game files and build output out of git.

**Saves in the background** (`DM_SAVE_BEHIND`, on by default; settings app: Performance). r71's log: every save wrote
`PactSave<N>.bin` (13 KB) 2-5 times in a row, ~70 ms each, on the game thread - 13 frames of 366-780 ms, mostly in
shop, upgrade and dialog screens, some just before fights. `reimpl/save_behind.c`: open() for writing (create or
truncate) of `files/.../Pact*.bin` gives a virtual descriptor collecting the data in memory (write, read, lseek,
fstat, ftruncate, fsync and close know it); close() queues the file and returns. The `dm_save` thread writes the newest
version of each file 150 ms after its last close (`[save] ...: N bytes written in M ms (K save(s) merged)`). open,
fopen, stat, rename, remove and unlink of a file still queued wait for its write first.

**Thread pinning** (`DM_PIN_THREADS`, off by default, experimental; settings app: Performance): the game thread alone
on core 0, the rendering thread (the one that swaps buffers) on core 1, everything else on cores 1-2 - the engine's I/O
and worker threads, PhysX, the loader's audio, controls and watchdog threads - so they use the rendering core whenever
it waits (through a load it mostly does) but never take time from the game thread. Re-checked every second
(`[cpu] pinned ...`).

## r72: prefetch window, stick camera after fights

r72 also had a Language setting in the settings app (`DM_LANGUAGE`); removed in r73.

**Prefetch window: packages held open.** r71's log named them: 13-15 area packages (fl1/fl2 art, stairs, hot spots,
pick-ups, a few climb floors; 21-24 MB) that the engine opened once and kept open for the rest of the session, three
endings and new games included, without reading them to the end. They filled the 24 MB window, so the larger packages
of later floors (fl2/cr audio 10 MB, ct hot spots 11 MB) were never prefetched. A window entry the engine has open but
has not read for a minute can now be let go when room is needed (`[pkgpf] ... let go: open in the engine ...`): its
handles carry on with the engine's own reader, which first catches up to where the engine is (unpacking from the start
of that package once, should the engine ever read it again). Reads in progress are counted, so an entry is never taken
from under one.

**After a fight**, the level / gold / exp / kill streak texts (`HorrorMenuCombatSummary`) put the controls in the menu
profile, where the right stick scrolled - a drag the opposite way to the camera look, and the game, which still turns
the camera under those texts, turned it inverted on both axes until the last text faded. There the right stick is now
the camera, as in exploration (with the same pause while the game moves the camera), or does nothing with the stick
camera off.

**Right stick camera:** when exploration resumes, the pause lasts at least 0.4 s (r71 lifted it "after 0 ms", a few
frames before the camera started swinging back).

## r71: right stick camera waits for the game's camera

For "Right stick camera" (`DM_STICK_LOOK 1`; off by default, the touch screen turns the camera). The stick look used
to fight the game whenever the game turned the camera itself - after fights (the camera swings back), after menus and
text boxes, while walking to a tapped spot - and felt inverted or stuck.

`ULocalPlayer::CalcSceneView` is now hooked: it computes the camera's final rotation every frame, and a frame where the
camera turned (more than ~0.13 degrees) is noted. When the camera turns with no look finger of ours down (and not in
the glide just after one), or when exploration starts again after a menu, fight or cutscene, the stick look pauses
(`[pad] right stick look paused: ...`). It comes back once the camera has held still for 0.2 s (at most 4 s) and the
stick has returned to the middle (`[pad] right stick look back (camera settled after N ms)`).

Every player controller state change is logged with its time (`[ui] player controller: A -> B`), so a misbehaving
moment can be matched to the state the game was in.

**Packages open during a movie.** r70 moved window entries the engine still had open to the heap when a movie needed
the window's memory: 11 packages (about 20 MB) at the ending, which stayed for the rest of the session (heap cache 115
MB of 96, heap free down to 34 MB, and more with every further ending). Now:
- they go to a spill block in vitaGL's RAM pool (`DM_PACKAGE_PREFETCH_MB`, 24 MB, taken at start; about 39 MB of that
  pool sat unused in every log), each logged (`[pkgpf]   still open for the movie: ...`); the heap only if it is full;
- after the movie they are copied back into the window, or dropped if the engine has let go of them;
- every 2 s, a heap cache over its limit drops entries the engine no longer has open (`[pcache] over the limit: ...`);
- "not prefetched: no room" now lists the window entries the engine still has open, to see what fills the window.

## r70: movies, credits text

- **Ending movie** (`end_meadow`) did not play in r69: a prefetched package in the window was still open in the engine,
  so the window was not given back and the decoder got no physically contiguous memory ("no frame decoded within 5 s").
  Entries still open after 0.5 s are now moved to the heap and the block is freed.
- **Final credits** changed text size as they scrolled: r64's subtitle rule (centred text in the y 400-480 band, drawn
  at `DM_TEXT_SCALE`) caught each credit line on its way through. Subtitles are now recognised by their exact box (x
  48-912, y 435.2).
- **Opening movie:** plays (r69 played it to the end). The first drawn frame logs its GL state (`[movie] first frame
  drawn: ...`), and a movie can no longer be skipped in its first second (r68 skipped it 0.06 s in, with a button still
  held from the title).

## r69: L/R fixed, touch camera, steadier menus, intro audio on time

- **L / R did nothing** (r65-r68): `sceCtrlPeekBufferPositiveExt2` reports the Vita's shoulder buttons as `L1` / `R1`;
  `LTRIGGER` / `RTRIGGER` are L2 / R2 there. Both are now read as L / R.
- **Camera while exploring:** the touch screen by default. The right stick look kept fighting the game's own camera
  moves (after fights, at cutscene ends); `DM_STICK_LOOK 1` (settings app: "Right stick camera") brings it back.
- **Menus that stop redrawing** (inventory, pause, dialogs) dropped to the cutscene profile for a moment every few
  seconds in r68's log (cursor gone, held buttons let go). A menu stays a menu while it is the last thing drawn, for up
  to 10 s, unless a cutscene track starts.
- **Intro audio:** r68's timing log showed `cin_intro_flmx` audible 567 ms after the game asked for it - 555 ms of that
  was the first-time scan of the expansion files for the song list. The list is now built when the song thread starts,
  so the track (which carries all of the trickster's first lines) starts with its subtitles. No long frames were logged
  during the scene, so the cutscene clock did not skip.

## r68: timing in the log

To find out why the trickster's early lines in the intro are off from their subtitles (the whole scene is one audio
track, `cin_intro_flmx`, while the subtitles run on the cutscene's own clock): every log line now carries the time
since start (seconds.milliseconds, after the level tag); every frame longer than 250 ms is logged (`[frame] long
frame`); and each song logs when its first audio is mixed (`[song] "..." audible now: N ms after it was asked for`).
With the subtitle trace (first draw of each line), one pass through a scene gives the offset of every line.

## r67: voice delay removed, package prefetch fixes

r66's log: the voice delay was set to 0 (and r64's never applied to cutscene tracks, `cin_*`, which carry most lines),
so it is gone: no lead-in silence, no setting.

The sound that came late was the package prefetch's doing. When "cin_intro_flmx" was asked for, the prefetch thread
was unpacking five packages from the memory card, and the song (whose first start also scans the expansion file for
its 140 songs) began about 1.5 s late. The prefetch now pauses whenever a song has been asked for and has less than
0.5 s buffered (`song_io_busy`, song.c).

Two more prefetch fixes from the same log:
- At the title it had filled 21 MB of the window (floor 1's audio, pick-ups, intro cutscene ...), then the opening
  movie took the memory back and all of it was thrown away; floor 1 then loaded from the card as before. The window is
  now only taken once the opening movie has played (`movie_opening_pending()`, movie.c).
- After any movie, the list of the last package opened is run again, so what the movie threw out is fetched again.

## r66: voice delay 50 ms

The lead-in silence before each dialogue line (`DM_VOICE_DELAY_MS`) is 50 ms by default (r64/r65: 500, too long); the
settings app offers 0-500 ms in 10 ms steps. The value in use is logged at start (`[song] voice delay N ms`), and the
first 20 lines it is applied to are logged too (`[song] N ms of silence before "vo_..."`).

## r65: no reversed camera after fights, L/R tap, button remapping

After a fight the camera still went the wrong way for about 2 s. The cause was not the fight itself: once the combat
summary was dismissed, the menu profile was held for another 1.5 s (`MENU_HOLD_US`, there because menus are not drawn
every frame), and in the menu profile the right stick scrolls - a drag the opposite way to the exploration look.
Now the menu hold ends as soon as the player controller leaves `PostCombatTransition` (the summary is gone), and in
any menu the right stick only scrolls while a menu has been drawn in the last 250 ms, so closing any menu (pause,
inventory) no longer leaves a reversed right stick behind.

**L and R** tap at the cursor (exploration, menus, crossbow aiming) - a "tap only" function: they do nothing in sword
fights, where Cross blocks.

**Button remapping.** The settings app has a Buttons section under Controls: every function can be put on any button
(or none). It writes `DM_KEY_<function> <button>` lines to config.txt - functions `TAP` (tap at the cursor / block),
`TAP_ONLY` (tap at the cursor, no block), `BACK`, `BLOCK`, `DODGE_LEFT`, `DODGE_RIGHT`, `MEDKIT`, `BOMB`, `MENU`, `TOGGLE` (controls on/off);
buttons `CROSS CIRCLE SQUARE TRIANGLE UP DOWN LEFT RIGHT L R START SELECT NONE`, several joined with `+`
(the default for `TAP_ONLY` is `L+R`). In the settings app a button is assigned by selecting the line, pressing Cross
and then pressing the button (for Tap, several buttons pressed together, each of which then works alone - shown as "L or R"; any button can be assigned this way, Square and Circle included); outside of assigning, Square clears it, Triangle restores the
default. The loader turns the physical buttons
into the default layout's buttons before anything else, so every profile follows the remap. Two functions on one
button both happen; the settings app marks that "(shared)".

## r64: controls after battles, Select toggle, subtitles, voice timing, opening movie before the title, package prefetch

**Controls lost after battles.** r62 switched to the cutscene profile (no cursor, no sticks) for 0.5 s after the fight
screen (`HorrorMenuHUD`) was drawn - but that screen keeps being drawn after the fight, so the controls never came back.
The guard is now keyed to the player controller's `PostCombatTransition` state (the few seconds after a fight while
the game swings the camera back, where the right stick turned it the wrong way): no sticks until it leaves that state,
at most 8 s. The combat summary on top is a menu and keeps its cursor.

**Select** turns the physical controls off (no cursor; buttons and sticks do nothing; the touch screen works as always)
and on again.

**Camera spin (elevator on floor 2).** A look finger that was moving when the controls changed was lifted at once, and
the game kept turning the camera at the drag's speed. A look finger is now held still for 150 ms before it lifts.

**Subtitles** in the subtitle band (centred, y ~435, 864 px wide) are always drawn at `DM_TEXT_SCALE` (0.8), so short
lines are no longer bigger than long ones. The Text trace toggle is gone from the settings app (`DM_TEXT_TRACE` in
config.txt still works).

**Voice timing.** Each dialogue line (`vo_*`) starts after `DM_VOICE_DELAY_MS` of silence (default 500, 0-1000, in the
settings app under Sound): the engine shows the subtitle about 0.5 s after it asks for the line.

**Opening movie** plays when the title screen is first drawn, so skipping it goes straight to the title.

**Package prefetch.** In r63 about 19 s of the 51 s of load waits were memory-card reads and inflating for packages
loaded for the first time (area art, hot spots, audio, enemies). The packages the engine opens are now remembered
across sessions in `pkgprefetch.txt`: for each package, the next six opened at least 2 s after it (not in the same
load) and within 5 minutes. When the engine opens a package with a record, a low-priority thread (`dm_pkgpf`) waits for
the load to finish, then unpacks the successors into RAM, pausing whenever the engine loads something else (unless the
engine is already reading the package being unpacked). Enemy and weapon packages go into the heap cache and stay;
everything else into a 24 MB window (`DM_PACKAGE_PREFETCH_MB`) of physically contiguous RAM - the memory vitaGL leaves
for the movie decoder, idle after the opening movie (26 MB free in all of r63). Before any movie the window is emptied
and the block given back; it is taken again afterwards. `DM_PACKAGE_PREFETCH 0` turns it off; the settings app's
"Forget recorded textures and packages" deletes both records. The first session only records; later ones prefetch.

**Bigger caches.** The package cache (`DM_PACKAGE_CACHE_MB`) goes from 80 to 96 MB: it was full at the end of r63
(21 MB of sound tables plus 40 enemy packages). The texture-data cache (`DM_TEXTURE_CACHE_MB`) goes from 32 to 44 MB
(it held 31 MB at the end; its block is in video memory, which had at least 21.6 MB to spare).

## r63: movies through physically contiguous memory, opening movie during the load, cursor at boot, MSAA 4x

**Movies.** vitaGL's own `samples/video_playback` gives sceAvPlayer its frame memory as physically contiguous RAM
blocks of its own (`SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW`, 1 MB aligned, mapped for the GPU) and starts
vitaGL with `vglInitWithCustomThreshold(..., phycont_threshold = 32 MB, ...)` so that memory is left free.
`vglInitExtended` had taken all of it, the decoder got vitaGL's pool memory instead, and the player stopped at once
(event 0x1) in r60-r62. `gl_init` now leaves it (`DM_MOVIE_PHYCONT_MB`, default 32 = all; vitaGL does not need it:
textures live in video memory and its RAM pool), and `alloc_gpu` takes the decoder's blocks from there first. (r61's
`alloc_gpu` also replaced a successful video-memory block with pool memory; fixed.)

**Opening movie during the load.** The engine draws almost nothing while it loads its packages (2 frames in the
first ~20 s), and the movie was only drawn at the engine's swaps. The first swap is now held: the movie plays on the
rendering thread with its own swaps until it ends or is skipped, while the game thread keeps loading
(`DM_STARTUP_MOVIE_HOLD 0`: back to drawing only at the engine's swaps).

**Cursor.** Hidden until the first game screen (the title screen) and while any movie plays.

**Settings app.** The "Loading and memory" section and "Video memory for movies" are gone (they still work in
config.txt). "Anti-aliasing" is now "MSAA", default 4x - and the loader's default is now 4x too (`DM_MSAA 0` / `2` for
less).

## r62: control defaults, fight start, movies

**Control defaults** (also in the Settings app): camera 250 px/s (was 900), cursor 420 (650), menu scroll 900 (700),
sword swipe 80 ms (90). The Settings app no longer shows sword swipe length and duration, text size and Android dialogue
timing; they still work in config.txt (`DM_SWIPE_LENGTH`, `DM_SWIPE_MS`, `DM_TEXT_SCALE`, `DM_SONG_ANDROID_CUTS`), and
the Settings app leaves those lines alone.

**Fight start.** For the first 1.5-2 s of an encounter the player has no weapon state yet, so the profile stayed
exploration, and the right stick's look drag turned the camera the wrong way. The fight is now recognised earlier, from
the weapon being drawn (`HorrorWeapon_Sword` / `HorrorWeapon_Crossbow` going to `WeaponEquipping` or `Active`), and
while the fight screen (`HorrorMenuHUD`) is up without a weapon state the sticks do nothing (cutscene profile).

**Package cache: only enemy and weapon packages are kept.** r61 unpacked a kept package completely on first open,
which took ~200 ms off the battles whose package was kept (light_00a 595 -> 385 ms, light00_ct01 599 -> 392). But
from the climb on nothing new was kept ("not kept: cache full (21 MB preloaded, 60 MB in 30 packages the engine still
has open)"): the engine keeps the floor's area packages (art, pickups, hot spots, audio, cutscenes, up to 7.6 MB each)
open for as long as the floor is loaded, so they could never be dropped. Those are loaded once per visit anyway, so
now only `HOR_ai_*` (enemies) and `HOR_Weapon*` are kept after their first load; `DM_PACKAGE_CACHE_KEEP_ONLY <prefixes>`
in config.txt replaces that list (`*` = everything, as before).

**Movies.** r61's player log: the player reported event 0x1 (stop) straight after the file was added and never became
ready, with the decoder's memory in vitaGL's video memory and also in its RAM pool, so the memory was not the
problem: the files were. `tools/convert_movies.sh` now makes H.264 Constrained Baseline (no B-frames, one reference
frame), AAC-LC 48 kHz, `mp42` brand; the Main-profile files from earlier versions should be converted again. The
loader-started opening movie can no longer be stopped by the engine's StopVideo (r61's was stopped right after it
began), and a failed video-memory block for the decoder is logged with its error code.

## Settings app (r61)

A companion app in the same LiveArea bubble: the **Settings** button on the game's LiveArea page starts
`app0:/configurator.bin` (`configurator/configurator.c`) instead of the game - the LiveArea item's target is
`psla:-config`, and `soloader_init_all` (`source/utils/init.c`, from the boilerplate) runs the configurator when it sees
`-config`. It edits `ux0:data/darkmeadow/config.txt`:

| section | settings |
|---|---|
| Controls | physical controls on/off, camera speed, invert camera X / Y, cursor speed, menu scroll speed |
| Display | MSAA (4x default / 2x / off), frame rate limit (60 / 30) |
| Movies | opening movie on/off |
| Diagnostics | engine log, log touches, text trace |
| Reset | forget recorded textures (deletes `texprefetch.bin`), all settings to defaults |

Up/down choose, left/right change (held: repeats), Cross toggles or runs an action, Triangle puts the setting under the
cursor back to its default, Start saves and starts the game, Circle quits (asking to save if something changed).
Changed values are shown in gold. Only the settings on this page are touched: a setting at its default is removed from
config.txt, every other line (comments, test switches, button positions...) is written back unchanged. Text is drawn
with vitaGL from an embedded DejaVu Sans Mono atlas (`configurator/font.h`, generated). Building with
`-DHOST_PREVIEW` on a PC renders the page into a .ppm instead, for checking the layout.

## r61: cutscene profile, steadier menus, cursor, package unpacked once, movie diagnostics

**Controls.** A fifth profile, *cutscene*: when no game screen at all has been drawn for 2 s (the game hides its HUD
in cutscenes) or a cutscene track (`cin_*`) is playing, the cursor is hidden and the sticks and buttons do nothing
except Start. Menus now count as open until no menu screen has been drawn for 1.5 s (250 ms flickered: menu screens are
not drawn every frame, so the profile kept switching between menu and exploration). The cursor is no longer moved by
profile changes (r60 put it back in the middle for every menu, e.g. the level/gold/kill-streak summary after a fight).
`[pad] longest gap between game screens in the last 10 s: N ms` shows how regularly screens are drawn, to tune those
two times.

**Battle starts.** r60's file lists showed only the enemy package opened during a battle start, two or three times at
once (`HOR_ai_light00_CT01.xxx` x3), yet 2.7 MB read from the card and 150 ms unpacking for a 1.5 MB package: a second
handle that got ahead of what the first had filled in unpacked the whole package again from the start. A package the
cache decides to keep is now unpacked completely when it is first opened, and every handle is served from memory.

**Movie.** r60 got the decoder's memory at the right alignment, but the player never became active (no frame, no audio).
r61 logs the player's own events (`[movie] player event 0x.. (source N, data ...)`), first tries a video-memory block
of its own for the decoder (as Sony's samples do), and adds two config.txt switches for testing:
- `DM_MOVIE_CDRAM_MB 20` leaves 20 MB of video memory outside vitaGL's pool, so that block can be had;
- `DM_TEST_MOVIE end_meadow` plays that movie ~10 s after the game starts drawing, without the engine knowing.

**Camera direction.** In exploration the right stick now moves its finger the way the stick is pushed (r60 moved it
the opposite way), so the camera turns exactly as it does for a finger dragged in that direction. Menu scrolling is
unchanged. `DM_LOOK_INVERT_X 1` / `DM_LOOK_INVERT_Y 1` still reverse it.

**Opening movie.** The engine never asks for `Startup` (on Android the Java side plays it while the engine starts),
so the loader plays it as soon as the engine draws its first frame, over whatever the engine draws while it loads;
skipping works as for any movie. The engine is not told about it (`JavaCallback_IsVideoPlaying` reports only movies
the engine started). Music and dialogue are silent while any movie plays. `DM_STARTUP_MOVIE <name> | off` in
config.txt.

**"Loading".** The engine asks for a movie called `Loading` at every level change. The game has no such movie (the
expansion file holds only Startup, Ending, end_meadow and end_psychWard), so on Android it is skipped too; here it
is logged and skipped.

## Physical controls (r59, r60)

`source/padinput.c` turns the sticks and buttons into made-up touches (their own pointer ids, so the touch screen keeps
working alongside), polled with the touch screen ~120 times a second, in four profiles chosen from the game's state:

| | exploration | sword fight | crossbow fight | menu |
|---|---|---|---|---|
| left stick | move the cursor | - | move the cursor (aim) | move the cursor |
| right stick | look around | flick = sword swipe | - | scroll (a drag from the cursor) |
| Cross | touch at the cursor (walk / use; held = drag) | block (held) | touch at the cursor: hold, steer, release to shoot | touch at the cursor (held = drag) |
| D-pad down | - | block (held) | - | - |
| D-pad left / right | - | dodge left / right | dodge left / right | - |
| Triangle | - | medkit | medkit | - |
| Circle | - | - | - | back button, top left (80, 38) |
| Square | - | bomb | bomb | - |
| Start | menu icon (480, 28) | menu icon | menu icon | back button (80, 38) |
| L / R | - | - | - | - |

**Choosing the profile (r60).** The game's screens are `UHorrorMenuScene` objects drawn every frame by
`RenderScene_Horror` (the base class and the HUD, Pickup, LibraryEntry and CombatSummary overrides are hooked in
`reimpl/ue3_trace.c`). Scenes whose class name contains `HUD` (the fight screen), `PauseButton` (the exploration
screen), `PopupText`, `WitchText`, `Notification` or `Subtitle` are gameplay (`DM_PAD_GAMEPLAY_SCENES` adds more); any
other scene drawn in the last 250 ms (pause menu, save files, inventory, stats, store, dialogs, library, combat summary,
game over, title screens) means a menu. Otherwise the player's combat pawn decides: `UObject::GotoState` is hooked, and
`HorrorPlayerCombatPawn` in a `Melee...` or `Block...` state is a sword fight, in a `Ranged...` state a crossbow fight,
anything else exploration; `HorrorPC` going to `PostCombatTransition` or `Dead` ends a fight. Logged as
`[ui] fight: sword|crossbow|none (player <state>)`. r59 counted the exploration screen's pause button and the floating
pickup texts as menus, so the profile kept flipping to the menu one (and Start pressed the menu's back button instead
of the menu icon). Select no longer switches profiles by hand. Fight music decides only if the combat state was never
seen. Each new scene is logged once as `[ui] scene <class> <path> (gameplay|menu)`, each profile change as
`[pad] <profile> controls`, each (class, state) once as `[ui] <class> goes to state <state>`.

**Look:** a finger dragged from the middle of the screen, faster the further the stick is pushed (on a curve). It goes
down only after 14 px of movement, so a nudge is never a tap. It is lifted only once it is 40 px from where it went down
and 350 ms after (it is moved out to 40 px and held still if needed): r59 lifted short looks after ~15 px and a fraction
of a second, which the game took for taps and walked. It is put back in the middle 90 px before the screen edge. **Swipe:** a flick past 85% is one swipe through the middle of the screen in the stick's direction (any angle);
the stick has to come back under 35% before the next. **Cursor:** a ring drawn over the frame at every swap
(`source/overlay.c`, the same save-and-restore drawing as the movie player), in exploration, menus and crossbow fights (not in sword fights); it
starts in the middle each time a menu or a crossbow fight begins, and stays where it was in exploration. Buttons hold a finger for as long as they are held (at least 60 ms). Every made-up finger is lifted when the
profile changes, and the buttons are left to the movie player while a movie plays.

**Settings (config.txt):** `DM_<EXPLORE|FIGHT|CROSSBOW|MENU|BUTTON>_<CROSS|CIRCLE|SQUARE|TRIANGLE|UP|DOWN|LEFT|RIGHT|L|R|START>
<x> <y>` (960x544 screen), `… cursor` or `… off` (`BUTTON` = all profiles); `DM_LOOK_SPEED` (px/s at full stick,
900), `DM_LOOK_INVERT_X 1` / `_Y 1`, `DM_CURSOR_SPEED` (650), `DM_SCROLL_SPEED` (700), `DM_SWIPE_LENGTH` (320 px),
`DM_SWIPE_MS` (90), `DM_SWIPE_CENTER <x> <y>`, `DM_PAD_CONTROLS 0` (all off). `DM_LOG_TOUCHES 1` logs
`[touch] finger down at (x, y)` for every real touch, to read positions off. Fight screen points (from a screenshot): block
shield (480, 492), dodge arrows (50, 512) and (912, 512), medkit (55, 130), bomb (893, 128). Square throws the bomb in both fights.

## Battle starts after r57, the trickster's last line, the ending movie (r58)

r57's breakdown (Profile, 27 battle starts, median 679 ms) showed texture reads down to 0-40 ms (mostly from the
texture-data cache), and the time going to the enemy package itself: ~240 ms of card reads (2.7-3.2 MB for a 1.3 MB
package) and ~150 ms unpacking, i.e. read and unpacked twice, plus ~280 ms of engine work. The package cache should have
caught the second read, but after floor 1 it kept nothing any more ("package cache 0 KB", no `[pcache]` line for any
climb battle).

**Package cache: handles that have read everything let go.** A zip handle that has been given its whole entry no
longer holds the entry or its slot (`done_with` in `reimpl/pkg_cache.c`): the entry can be dropped to make room, the
slot can be reused (oldest first), and further reads return 0. Handle slots 48 -> 256. When a package cannot be kept,
it is logged now: `[pcache] X not kept: cache full (P KB preloaded, B KB in N packages the engine still has open, of
L KB)` or `… all 256 handle slots are in use`.

**More memory for it.** vitaGL's RAM pool (whatever is left after the heap) never went below 80 MB free in any run, so
the heap goes from 200 to 240 MB (`DM_HEAP_MB`) and the package cache from 48 to 80 MB (`DM_PACKAGE_CACHE_MB`). That
leaves vitaGL ~40 MB of RAM pool and, at r57's peak, ~70 MB of heap free.

**The trickster's last line.** The choice scene asked for its combat music after 28.4 s of
`cin_defeat_trickster_choice_flmx`, whose last line runs to 30.0 s (then 3.7 s of silence), and stopped the music
0.7 s later. A cutscene track (`cin_*`) now keeps playing after a request for music or ambience, or a stop, until its
output has been quiet (peak below 600) for 200 ms, at most 2.5 s; then the request applies. Logged as
`PlaySong("…") waits for the end of the line in "cin_…"` / `StopSong waits for …` and `"cin_…" went quiet N ms after
the engine asked for "…"`. A dialogue line or another cutscene still cuts in at once. `DM_SONG_ANDROID_CUTS 1` turns
this off with the other dialogue protections.

**The ending movie.** r59 also stops the movie's audio thread before closing the player: closing first crashed the audio thread (data abort in `dm_movie_audio`, still reading from the closed player). `end_meadow.mp4` was found and opened, but no frame came out. The decoder's frame memory came
from `vglAlloc`, which aligns to 16 bytes and ignores the alignment the decoder asks for; it now comes from vitaGL's
aligned allocator (`vgl_memalign`) at the requested alignment (video memory, then the RAM pool). The log shows each
block's size, alignment, pool and address, and if no frame arrives, whether the decoder is active and how many audio
frames it produced.

## Battle-start loading: breakdown, no reopening, folder scans, texture prefetch (r57)

r56's floor-2 battle starts took 757-806 ms, and packages already unpacked in RAM still took 570-620 ms, so the package
file is no longer what costs the time. Four changes:

**Breakdown in the `[hitch]` lines.** Every `LoadPackage` / `FlushAsyncLoading` over 30 ms now ends with where its time
went (counters are global, so they cover the async loader thread while the game thread waits; a load nested in another
is not broken down twice):
`| card reads N ms (K KB) | unpacking N ms (zip) + N ms (engine chunks) | package cache K KB | texture reads N ms (R reads,
K KB, C KB from the texture-data cache) | .obb opens N (M from the card) | folder scans N (M from the card, T ms) |
allocations N (~T ms) | render thread uploads T ms | other (engine work) ~T ms`.
r60 adds `| files: name N KB (cache), ...`: the zip entries opened during the load, and whether the package cache
had them. Texture reads' card time is part of "card reads". Allocation time is estimated by timing every 64th call
(`malloc`/`calloc`/`realloc`/`memalign`/`free` all go through `reimpl/mem_valve.c` now). "Other" is what is left: the
engine building its objects.

**No reopening of the .obb per texture** (`DM_OBB_KEEP_OPEN`, default 1). The engine opened the patch .obb for every
texture (326 times in a floor-2 run) and closed it after a few reads. `open()` of an .obb for reading now returns a
virtual descriptor (0x4000 + slot) that only keeps a position. Reads go to the texture-data cache, the read-ahead
blocks or one positional read on the descriptor `obb_stream.c` keeps open all session. `lseek`, `read`, `fstat`,
`dup`, `fdopen` and `close` know these descriptors (`reimpl/io_stats.c`, `reimpl/io.c`). The texture-data cache's key
for an .obb is now matched by file name.

**Folder scans from memory** (`DM_DIR_CACHE`, default 1). Before each texture read the engine also called
`opendir("../..")` and closed it unread (475 times). Once a folder has opened successfully, later `opendir()` calls on
the same path get a placeholder; the real folder is opened only if the engine reads entries from it.

**Texture prefetch** (`DM_TEXTURE_PREFETCH`, default 1; `reimpl/tex_prefetch.c`). Every texture read is remembered with
the floor it happened on (from the last `HOR_fl<N>_persistent` package loaded; the front end is floor 0) and saved to
`ux0:data/darkmeadow/texprefetch.bin` at each floor change and every minute while new reads come in (16 bytes per
read). When a floor is entered, a low-priority thread waits until loading has been quiet for a second, then reads that
floor's recorded data into the texture-data cache, sorted and merged into reads of up to 512 KB and pausing whenever the
engine loads. It only fills free room in the cache and never pushes out what the engine is using; prefetched entries
the engine never asks for are the first to go. So the first visit to a floor records, and later visits (also in later
sessions) start with that floor's textures in memory. The cache's default went from 24 to 32 MB for this: video memory
stayed at 37 MB free at its lowest in r56, so this leaves about 28 MB. The file is checked against the .obb sizes;
deleting it starts over. Logged as:
- `[texpf] N recorded texture reads loaded from … (by floor: …)`;
- `[texpf] floor N: R texture reads put in the cache (K KB; …) from K KB read in P pieces, T ms on the card …`;
- `[texpf] N texture reads saved to …`.

## Texture-cache re-reads (r53, measurement only)

To decide whether a RAM cache for texture data would shorten battle starts, `read_counted` (`reimpl/io_stats.c`)
keeps one bit per 128-byte chunk of the obb files read through the engine's own `open()`/`read()`. The `[io]` line's
texture part now reads `texture-cache reads N ms for K KB (R KB of it read before; U KB different data so far)`:
- R = bytes in that window that had already been read earlier in the session. That is what a cache would have saved.
- U = the total amount of distinct texture data read so far, which is roughly the RAM such a cache would need.

The error is at most 128 bytes per read, where consecutive reads share a chunk. Behaviour is unchanged.

## The dark toy truck (r55-r56)

`DM_TEST_FALSE_COLOUR` showed the truck as an ordinary light-mapped material, darker than everything around it.
False colour mixes in the base texture unlit, so its base texture reaches the GPU nearly black.
`DM_TEST_TEX_TINT` + `DM_TEST_BASE_ONLY` gave the truck, the toy blocks next to it and the toy on the cabinet the
same colour (144, 108, 108), which is texture number 220. Number 220 received three different images during that run
(dumps 176, 212 and 244). The one uploaded when the pediatric room loaded is a mostly dark atlas, which is what the toys
then show. Either the engine uploads a dark or wrong image for them, or the number is reused for other content while the
toys still draw with it. `DM_TEST_TRACE_TEXTURE <n>` (r56) follows one texture number through the session:
- `[trace] texture N created/deleted`;
- every upload with level, size, format, byte count and a checksum;
- a full-size TGA of each level 0 as `texdump/trace<n>_<w>x<h>_<format>_<k>.tga` (first 40).

Texture numbers are handed out in load order, so the number has to be read from a tint run on the same route.

## The magenta plane on the stairs to floor 17 (r51)

It is not a missing texture: none of the textures uploaded in that area is magenta (r50 dump, PVRTC and RGBA). The
colour comes from the shader. `DefaultPixelShader.msf` has
`#elif USE_FALLBACK_STREAM_COLOR` -> `gl_FragColor = vec4(1,0,1,0);`. The engine compiles a material that way when the
mesh lacks the vertex colour stream the material reads; the vertex shader then substitutes white
(`vec4 VertexColor = vec4(1, 1, 1, 1)`). The magenta is UE3's marker for that, and the program is compiled from source
(no preprocessed copy of it is in `AllShaders.bin`). Since r51 `gldiag_shader_source` turns that `#elif` into
`#elif 0` in pixel shaders, so the material is drawn normally with white vertex colour. It is logged as
`[gl] shader N: material for a mesh missing its vertex colours, drawn normally instead of magenta`.
`DM_FALLBACK_STREAM_FIX 0` in config.txt brings the magenta back.

r51 edited every source-compiled pixel shader that contained that `#elif`. vitaGL's shader cache is keyed by the
exact source text, so 18 programs missed the cache and were compiled again during play at 1.1-1.3 s each: a 7.7 s
freeze in the intro and 1-2.5 s ones elsewhere. That was a one-off, since the new results are now in the cache. Since
r52 only shaders that actually say `#define USE_FALLBACK_STREAM_COLOR 1` are edited, so all others keep their cached
programs.

## The engine's own log (r50)

The library is a shipping build: `appOutputDebugString` is an empty function, so nothing the engine logs reaches
`__android_log_print`, and `[ALOG]` never appears. `FOutputDevice::Logf` (both overloads) is hooked in
`reimpl/ue3_trace.c`. Each message is formatted with UE3's TCHAR rules (4-byte `wchar_t`, `%s` takes a TCHAR string),
written to log.txt as `[UE3] ...` (or `[UE3] (type) ...` for the `EName` overload) and handed on, already formatted, to
the original. A message repeated straight away is counted (`previous message repeated N more times`), and after 600
messages the rest are dropped. `DM_UE3_LOG 0` in config.txt turns it off. Warnings such as a texture or package that
could not be found (the magenta "missing texture" on the floor 17 stairs) should show up here, if the shipping build
still emits them.

## The engine carries its own C stdio

`libUnrealEngine3.so` defines 43 libc stdio symbols itself (`fseek`, `fread`, `fgets`, `ftell`, `fclose`, its own
`__sF` for stdin/stdout/stderr, ...) and only imports `fopen`, `_fileno` and `setvbuf` from the system libc. On
Android that is consistent. Here `fopen` returns a newlib `FILE`, so the engine's own `_fseeko` crashed following bionic's
`FILE` internals (first crash: data abort at address `0xe` inside `fseek` called from `unzOpen2`, the bundled minizip
that opens the `.obb`). `source/reimpl/ue3_stdio.c` hooks those engine functions at startup and forwards them to the
Vita's stdio. If another libc function is found to be defined inside the engine and to touch `FILE` objects, add it there.

## Why `USE_SCELIBC_IO` is OFF

The engine calls `fopen()`, then `_fileno()` on the result, then reads with plain `read()`/`lseek()`/`fstat()`. With the
boilerplate's default (SceLibc IO), `fopen`/`_fileno` come from SceLibc while `read`/`lseek` come from newlib, and the two
number file descriptors differently. Keeping everything in newlib makes the descriptors consistent. This option is
cached by CMake, so delete `build/` after changing it.

## How it works

`main.c` -> `soloader_init_all()` loads and relocates the library, then `JNI_OnLoad` runs. The engine registers its
40 Java->native entry points with `RegisterNatives` (FalsoJNI ignores that, so `engine.c` captures them) and caches
66 native->Java callbacks. `engine_start()` then calls the entry points in the order `BaseUE3JavaApp` does:

| Java (APK) | Here |
|---|---|
| `onStart` | `InitModel(manufacturer, model, product)`, `InitZipVFS(true)` |
| `surfaceCreated` | `InitEGLCallback()` -> engine calls back `JavaCallback_initEGL` -> `gl_init()` |
| `surfaceChanged` | `PostInitUpdate(960, 544)` |
| `PostStartup` | `LanguageSet("en")`, `Initialize(960, 544, 5.0, multitouch)` |

After `Initialize` the engine runs its own game and render threads and calls `JavaCallback_swapBuffers` every frame.
Touch input goes in through `NativeCallback_InputEvent(type, x, y, id, time)` (type = Android MotionEvent action).

## Where to change things

| What | File |
|---|---|
| Language, device identity, screen size, heap size, reported RAM/cores, file names | `source/dm_config.h` |
| Callbacks that do real work (EGL, paths, local values, movie stub) | `source/java_impl.c` |
| Logging (log.txt) | `source/utils/logger.c` |
| Startup sequence and native capture | `source/engine.c` |
| Missing libc functions (sysconf, sysinfo, uname, ...) | `source/reimpl/ue3_compat.c` |
| Symbol table | `source/dynlib.c` |

`java_tables.c`, `java_callbacks.h`, `engine_natives.[ch]` are **generated** from the library:
`python3 tools/gen_java.py` (tables extracted by `tools/extract_jni_tables.py`, needs `pyelftools`).
To implement a callback, define `jcb_<JavaMethodName>` in `java_impl.c`; it overrides the generated weak stub.

## Not implemented yet

* **Movies** (`.m4v` inside the .obb): `StartVideo` logs and reports the movie finished at once.
* The Java-side sound effects (`LoadSoundFile`/`PlaySound`, unused by this game: it uses OpenSL ES).
* **Physical buttons and sticks** (`controls_handler_key/analog` are empty): touch only.
* **On-screen keyboard** callbacks, and everything online (Tapjoy, Flurry, billing, ads): logged no-ops.
* `SetFixedSizeScale` is logged and ignored (rendering stays 960x544).

## Notes for debugging

* Shaders are plain-text GLSL assembled by the engine (`ES2ShaderSourceFromKeys`); vitaGL caches the compiled ones in
  `ux0:data/darkmeadow/shader_cache/` (see "Shader cache").
  Character skinning (computed bone-matrix index) is the likeliest thing to need attention.
* Textures are PVRTC (`CookedAndroid_PVRTC`); the other cooked folders in the .obb are empty.
* The engine reads an optional `UE3CommandLine.txt` from `ux0:data/darkmeadow/` for engine switches.
* `dlsym_soloader` in the boilerplate returned the address of the table slot; it now returns the symbol itself
  (needed for the engine's OpenSL ES lookups).
* `sysconf` was a stub returning 0; it now reports page size and core count.
