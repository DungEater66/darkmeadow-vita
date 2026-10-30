# Dark Meadow: The Pact - PS Vita

A loader that runs the Android version of *Dark Meadow: The Pact* on the PS Vita. It loads the game's own engine
library and provides what it expects from Android: graphics through vitaGL, sound, music and movies, touch, and
physical controls.

**The game is not included.** You need your own copy of the Android version, 1.4.6.4 (version code 69).

## Required files

On the Vita:

* **HENkaku/Ensō** with **kubridge** (a recent version: the loader needs `kuKernelMemProtect`). Install
  `kubridge.skprx` into `ur0:tai/`, add it under `*KERNEL` in `ur0:tai/config.txt` and reboot.

From the game, all into `ux0:data/darkmeadow/`:

| File | Where it comes from |
|---|---|
| `libUnrealEngine3.so` | the APK (a zip): `lib/armeabi-v7a/libUnrealEngine3.so` |
| `main.69.com.phosphorgames.DarkMeadow.obb` | the game's expansion file |
| `patch.69.com.phosphorgames.DarkMeadow.obb` | the game's expansion file |
| `movies/*.mp4` | optional, see [Movies](#movies) |

Leave the `.obb` files as they are.

## Building

You need **VitaSDK-softfp** (the soft-float VitaSDK; the game's library is soft-float code), `cmake` 3.16 or newer
and `make`. On Windows use WSL.

1. Set up the SDK:
   ```sh
   export VITASDK=/usr/local/vitasdk-softfp      # where you installed it
   export PATH="$VITASDK/bin:$PATH"
   ```
2. Make sure it has these libraries (install missing ones with `vdpm`): OpenSLES, sndfile, mp3lame, mpg123, opus,
   vorbis, ogg, vorbisenc, vorbisfile, FLAC, vitashark, SceShaccCgExt, mathneon.
3. Install a current kubridge into the SDK:
   ```sh
   git clone https://github.com/bythos14/kubridge
   cd kubridge && cmake -B build && cmake --build build && cmake --install build
   ```
4. Build:
   ```sh
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Profile
   cmake --build build -j4
   ```
   The result is `build/DarkMeadow.vpk`. Install it with VitaShell.

Build types: `Profile` (recommended: optimised, writes a useful log), `Release` (optimised, logs errors only), `Debug`
(slow, full logging). Delete the build folder after changing `VITASDK` or the build type.

| Error | Fix |
|---|---|
| `Please define VITASDK env var` | set `VITASDK` (step 1) |
| `cannot find -lXYZ` | install the library that provides `libXYZ.a` (step 2) |
| `uses VFP register arguments` | you are using the hard-float SDK; use VitaSDK-softfp |
| `kuKernelMemProtect` undeclared | kubridge in the SDK is too old (step 3) |

## Movies

The game's movies are in a format the Vita cannot decode. Convert them once on a PC with `ffmpeg` (with libx264):

```sh
mkdir m4v && cd m4v
unzip -j ../main.69.com.phosphorgames.DarkMeadow.obb 'UDKGame/Movies/*.m4v'
cd .. && tools/convert_movies.sh m4v movies
```

Copy `movies/*.mp4` to `ux0:data/darkmeadow/movies/`. Without them the game runs normally and skips its movies.

## Settings and logs

Tap **Settings** on the game's LiveArea to open the settings app. It covers the controls (speeds, button
remapping, right stick camera), MSAA, the frame-rate limit, the opening movie and some performance options.
Start saves and launches the game; Circle quits.

The loader writes **`ux0:data/darkmeadow/log.txt`** (with a `Profile` or `Debug` build; `Release` logs errors only).
In the settings app, under **Diagnostics**:

* **Engine log**: also copy the engine's own messages into `log.txt` (on by default).
* **Log touches**: write every touch position, to help place buttons.

`log.txt` is what to attach when reporting a problem.

## Controls

The touch screen works as on Android everywhere. The buttons switch automatically with what is on screen:

| | Exploring | Sword fight | Crossbow fight | Menus |
|---|---|---|---|---|
| Left stick | cursor | | aim (cursor) | cursor |
| Right stick | camera (if "Right stick camera" is on; otherwise drag the touch screen) | sword strike in that direction | | scroll |
| Cross | tap at the cursor | block | hold to draw, release to shoot | tap at the cursor |
| L / R | tap at the cursor | | hold to draw, release to shoot | tap at the cursor |
| D-pad down | | block | | |
| D-pad left / right | | dodge | dodge | |
| Triangle | | medkit | medkit | |
| Square | | bomb | bomb | |
| Circle | | | | back |
| Start | game menu | game menu | game menu | back |
| Select | physical controls off / on | same | same | same |

In cutscenes only Start works. Any button or a tap skips a movie. Every action can be moved to another button in the
settings app (select it, press Cross, then press the new button).

## Licenses

* The loader (`source/`, `configurator/`, `tools/`) is under the MIT license, see [LICENSE](LICENSE). It is based on
  the so-loader boilerplate by Andy Nguyen, Rinnegatamante and Volodymyr Atamanenko.
* Third-party code in `lib/`:
  * [vitaGL](https://github.com/Rinnegatamante/vitaGL), LGPL-3.0 (`lib/vitagl/COPYING.LESSER`); modified here so
    its messages go to `log.txt`
  * [FalsoJNI](https://github.com/v-atamanenko/FalsoJNI), MIT (`lib/falso_jni/LICENSE`)
  * [so_util](https://github.com/Rinnegatamante/so_util), MIT (`lib/so_util/LICENSE`)
  * [FalsoNDK](https://github.com/elliencode/FalsoNDK), Apache-2.0 (`lib/falso_ndk/LICENSE`)
  * fios and libc_bridge from the boilerplate (Andy Nguyen), and SHA-1 by Brad Conte (public domain)
* *Dark Meadow: The Pact* is © Phosphor Games. None of its files are part of this repository.

The detailed history of the port is in [DEVLOG.md](DEVLOG.md).
