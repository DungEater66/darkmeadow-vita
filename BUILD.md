# Building the VPK

This builds `DarkMeadow.vpk`, which contains the loader (`eboot.bin`), the settings app (`configurator.bin`) and the
LiveArea. The game itself is not included: you need your own copy of *Dark Meadow: The Pact* for Android
(v1.4.6.4, version code 69).

## 1. Set up VitaSDK (soft-float)

The engine library is soft-float ARM code, so the loader must be built with **VitaSDK-softfp**, not the usual
hard-float VitaSDK.

1. Install VitaSDK-softfp and set the environment variables (add these to `~/.bashrc` or your shell profile):
   ```sh
   export VITASDK=/usr/local/vitasdk-softfp      # wherever you installed it
   export PATH="$VITASDK/bin:$PATH"
   ```
2. Make sure the SDK has the libraries the loader links (the same set the soloader boilerplate uses):
   OpenSLES, sndfile, mp3lame, mpg123, opus, vorbis, ogg, vorbisenc, vorbisfile, FLAC, vitashark, SceShaccCgExt,
   mathneon. Check one with `ls "$VITASDK/arm-vita-eabi/lib/libmpg123.a"`; install missing ones with VitaSDK's
   package manager (`vdpm`). If the link step later says `cannot find -lXYZ`, install whatever provides `libXYZ.a`.
3. Install an up-to-date **kubridge** into the SDK (the loader needs `kuKernelMemProtect`):
   ```sh
   git clone https://github.com/bythos14/kubridge
   cd kubridge && cmake -B build && cmake --build build && cmake --install build
   ```
   Check: `grep -c kuKernelMemProtect "$VITASDK/arm-vita-eabi/include/kubridge.h"` must print 1 or more.

You also need `cmake` (3.16 or newer), `make` and `python3`. On Windows use WSL.

## 2. Build

From the project folder (the one with `CMakeLists.txt`):

```sh
cmake -S . -B build-profile -DCMAKE_BUILD_TYPE=Profile
cmake --build build-profile -j4
```

The result is `build-profile/DarkMeadow.vpk`.

| Build type | Use |
|---|---|
| `Profile` | **Recommended.** Optimised (`-O3`), keeps the useful log (`log.txt`): performance, memory, loads. |
| `Release` | Optimised, errors only in the log. |
| `Debug` | Slow; full logging and vitaGL checks. For tracking down crashes. |

Use a separate build folder per build type (`build-profile`, `build-release`, ...). Delete the build folder and
configure again after changing `VITASDK` or any `-D` option.

Common errors:

| Message | Fix |
|---|---|
| `Please define VITASDK env var` | `VITASDK` is not set in this terminal |
| `cannot find -lOpenSLES` (or another `-l...`) | install that library (step 1.2) |
| `uses VFP register arguments` | you are using the hard-float SDK; use VitaSDK-softfp |
| `kuKernelMemProtect` undeclared / undefined | kubridge in the SDK is too old (step 1.3) |
| `undefined reference to shark_*` | update vitaShaRK in your SDK |
| `fatal: not a git repository` while building vitaGL | harmless |

## 3. Install on the Vita

1. **kubridge plugin:** install `kubridge.skprx` from the same kubridge project into `ur0:tai/` and add it under
   `*KERNEL` in `ur0:tai/config.txt`, then reboot. An old kubridge plugin makes the app fail to start.
2. **The VPK:** copy `DarkMeadow.vpk` to the Vita and install it with VitaShell.
3. **Game files**, all into `ux0:data/darkmeadow/`:
   * `libUnrealEngine3.so`, from the APK: `lib/armeabi-v7a/libUnrealEngine3.so` (the APK is a zip)
   * `main.69.com.phosphorgames.DarkMeadow.obb`
   * `patch.69.com.phosphorgames.DarkMeadow.obb`

   Leave the `.obb` files as they are.
4. **Movies (optional).** The game's movies are in a format the Vita cannot decode, so convert them once on a PC
   (needs `ffmpeg` with libx264):
   ```sh
   mkdir m4v && cd m4v
   unzip -j ../main.69.com.phosphorgames.DarkMeadow.obb 'UDKGame/Movies/*.m4v'
   cd .. && tools/convert_movies.sh m4v movies
   ```
   Copy `movies/*.mp4` to `ux0:data/darkmeadow/movies/`. Without them the game runs normally and skips its movies.

## 4. Settings

Open the app's LiveArea and tap **Settings**, or edit `ux0:data/darkmeadow/config.txt`. The settings app covers the
controls (button remapping, right stick camera), MSAA, frame-rate limit, the opening movie and the logs.
`log.txt` in the same folder is the place to look when something goes wrong.

## Third-party code in `lib/`

These are copies, not git submodules, because some of them are modified for this port (vitaGL logs through the
loader's logger). `lib/SUBMODULES.txt` lists the upstream projects and the commits they came from:
FalsoJNI, so_util, FalsoNDK, vitaGL (LGPL-3.0, see `lib/vitagl/COPYING.LESSER`), plus small pieces from the
soloader boilerplate (MIT, see `LICENSE`).
