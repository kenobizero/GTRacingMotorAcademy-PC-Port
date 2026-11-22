# GT Racing: Motor Academy HD · Android port

The same static recompilation as the PC port, built for Android. The game's ARMv7 code is translated to C as
it is for Windows, and the host (`host/`) runs on Android's own libraries:

| | Windows | Android |
|---|---|---|
| Process | 32-bit x86 | 32-bit ARM (`armeabi-v7a`) |
| System calls | Win32 | POSIX (`host/core/posixwin.h`: the Win32 calls the host makes, on bionic) |
| The game's address range | the exe's first section (`gt.ld`, `reserve.S`) | `mmap` with `MAP_FIXED_NOREPLACE` as `libmain.so` loads (`host/core/fault_android.c`) |
| Graphics | desktop OpenGL 2.1 under the game's GLES calls | OpenGL ES 3.0: the game's ES 2 calls passed straight through (`gles_android.c` has the contexts and the picture) |
| Faults, crash report | vectored handler, minidump | signal handler, the log and a message box |
| Leaderboard requests | WinHTTP | Java's `HttpURLConnection` (`host/core/android_jni.c`) |
| Input | keyboard, mouse, pads | the touch screen (the game's own touch controls), the accelerometer for tilt steering, pads, Back as Esc |

Nothing is emulated while the game runs, on Android either.

## Requirements

- **A phone that runs 32-bit apps.** The host keeps the game's 32-bit pointers (guest and host addresses are the
  same words), so it is an `armeabi-v7a` app. Most phones made until about 2022 run them (for example the
  Snapdragon 8xx chips up to the 888 and 870). Some later chips run them on part of their cores only, and the
  newest have no 32-bit mode at all (Snapdragon 8 Gen 3 and later, Pixel 7 and later): the app does not
  install there.
- Android 10 or later, OpenGL ES 3.0.
- About 1.5 GB of storage for the game.
- Your own copy of the iPad game `GTRacingFreemiumHD.app`, version 1.3.4b (the same build as the PC port:
  SHA-1 of `GTRacingFreemiumHD` `eaeca1915d8b0a250583afed587f61ecb85a728a`).

## Building

On any PC (Windows, Linux or macOS):

1. Install [Android Studio](https://developer.android.com/studio) (or the command-line SDK tools) with the
   **NDK** (r26 or later) and **CMake 3.22.1** from the SDK Manager, plus **Python 3.12** with `capstone` 5
   (`pip install "capstone>=5,<6"`) and **git**.
2. Put your `GTRacingFreemiumHD.app` in the repository's folder, as for the PC build.
3. Make the generated code and fetch SDL 3:

   ```
   python -I android/prepare.py
   ```

   This runs stage 1 and stage 2 of the recompiler (the first time only; `--gen` runs stage 2 again), writes the
   import bridges, makes `build/gen_android` (the generated code with GNU ld's `--wrap` rule kept under LLVM's
   linker: see the script), clones SDL 3 into `android/SDL`, and patches the menu pages into
   `build/android/patch`.
4. Build the APK:

   ```
   cd android
   ./gradlew assembleRelease        (Windows: gradlew.bat assembleRelease)
   ```

   or open the `android` folder in Android Studio and build there. The APK is
   `android/app/build/outputs/apk/release/app-release.apk`. Compiling the generated code takes a while (about
   1.8 million instructions of C), and the library is large.

   `assembleDebug` builds the development host (`GT_DEV=1`: the diagnostic keys and traces).

### Building on GitHub (no PC needed)

`.github/workflows/android.yml` builds the APK on GitHub's machines. Only the game's program is needed for that
(the file `GTRacingFreemiumHD` inside `GTRacingFreemiumHD.app`, a few MB), never its other files:

1. Upload that one file somewhere that gives a direct-download link (Google Drive:
   `https://drive.google.com/uc?export=download&id=<file id>`, Dropbox: the share link with `?dl=1`).
2. In your fork on GitHub: Settings > Secrets and variables > Actions > New repository secret, name
   `GAME_EXE_URL`, value: that link. Secrets are never shown in the logs or to other people.
3. Actions > Android APK > Run workflow (or push to `android-port`). The build takes about an hour; the APK is
   the run's artifact `GTRacing-android-apk` (a zip; kept 7 days).

## Installing and playing

1. Install the APK.
2. Start it once. On Android 11 and later it asks for **All files access** (Settings opens): allow it, so the
   game's folder can be on the shared storage. It makes the folder `GTRacing` there
   (`/storage/emulated/0/GTRacing`).
3. Copy your game into it, so that `GTRacing/GTRacingFreemiumHD.app/GTRacingFreemiumHD` is there (a USB cable
   and the PC's file manager, or any file manager on the phone).
4. Optional: copy `build/android/patch` to `GTRacing/patch` (the port's Options > Display page, the Touch Screen
   switch with pads, the Exit and Name tiles), and add-ons to `GTRacing/add-on`, as on the PC.
5. Start the game again.

Without All files access the port uses the app's own folder instead,
`Android/data/org.gtracing.port/files` (newer file managers cannot open that folder; a PC over USB can).

The save is `GTRacing/home`, the settings `GTRacing/gtracing.ini` (the keys of the PC port's README; Android's
defaults are below), the log `GTRacing/gtracing.log`.

## Android's defaults

| Key | Android | Why |
|---|---|---|
| `GT_TOUCH_SCREEN` | `1` | the game's own touch controls and HUD |
| `GT_RESOLUTION`, `GT_WIDE` | `monitor` | the screen's own size and shape (a 20:9 phone gets the wide menus and HUD) |
| `GT_DEVICE` | `ipad3` | the Retina art; `ipad2` uses less memory |
| `GT_MSAA` | `0` | a resolve pass is expensive on a phone's tiled GPU; `2` or `4` on a fast one |
| `GT_POSTFX` | `4` (as on the PC) | Enhanced; every mode is there (its passes are drawn with OpenGL ES). `0` or `1` for more frames on a slow phone |
| `GT_FPS` | `60` | the frame rate: `30`, `60`, `0` uncapped (vsync on: as fast as the screen refreshes, 120 on a 120 Hz phone). Options > Display > Frame Rate (the Full Screen row on the PC) |
| `GT_FXAA` | `0` | FXAA over the picture as it is drawn on the screen, alone or over MSAA. Options > Display > Anti-aliasing: Off, FXAA, 2x, 4x, 4x + FXAA, Auto |
| `GT_VSYNC` | `0` | the frame cap paces the frames (a capped rate only; uncapped always waits for the screen) |
| `GT_BIG_CORES`, `GT_PERF_HINT` | `1`, `1` | the game's threads on the fast cores; the frame thread's clocks raised to keep the frame rate (Android 13+) |
| `GT_GPU_PROFILE` | `igpu` | the lighter motion blur; `full` for the iPad 2's full one |
| `GT_TILT_INVERT` | `0` | `1` turns the tilt steering's direction, for a phone whose sensor axes differ |

Display page changes that need a restart close the app; open it again.

## Known gaps

- **Not tested on a phone yet.** The host compiles for `armeabi-v7a` against Android's own headers, and the
  Windows build is unchanged, but the first runs will find things to fix. Reports with `GTRacing/gtracing.log`
  are welcome.
- No crash dump: the log has the fault's lines (`[FAULT]`), and Android's own tombstone the rest. The log's
  `[main] libmain.so at ...` line turns a host address into a function: `llvm-addr2line` (in the NDK) on the
  unstripped `libmain.so` under `android/app/build/intermediates/cxx/`, with the address minus that base.
- Online play opens its port with UPnP / NAT-PMP as on the PC; the router's address is guessed (`.1` of the
  phone's network), as Android lets an app read no routing table.
