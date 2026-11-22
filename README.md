# GT Racing: Motor Academy HD · Android Port

<p align="center">
  <a href="https://github.com/Adriansyah101/GTRacingMotorAcademyRecomp/releases/latest"><img src="https://img.shields.io/github/v/release/Adriansyah101/GTRacingMotorAcademyRecomp" alt="Latest release"></a>
  <a href="#license"><img src="https://img.shields.io/badge/license-MIT%20OR%20GPL--3.0-blue" alt="License: MIT OR GPL-3.0-only"></a>
  <img src="https://img.shields.io/badge/platform-Android-green" alt="Platform: Android">
  <img src="https://img.shields.io/badge/ABI-armeabi--v7a-green" alt="ABI: armeabi-v7a (32-bit ARM)">
  <a href="https://github.com/Adriansyah101/GTRacingMotorAcademyRecomp/releases"><img src="https://img.shields.io/github/downloads/Adriansyah101/GTRacingMotorAcademyRecomp/total" alt="Downloads"></a>
</p>

<p align="center">
  <a href="#features">Features</a> •
  <a href="#system-requirements">Requirements</a> •
  <a href="#setup-instructions-for-players">How to install</a> •
  <a href="#controls">Controls</a> •
  <a href="#settings">Settings</a> •
  <a href="#troubleshooting">Troubleshooting</a> •
  <a href="#known-issues">Known Issues</a> •
  <a href="#privacy">Privacy</a> •
  <a href="#build-instructions-for-developers">How to compile</a> •
  <a href="#contributing">Contributing</a> •
  <a href="#credits">Credits</a> •
  <a href="#license">License</a>
</p>

GT Racing: Motor Academy is a racing game by Gameloft for the iPhone and iPad, built around driving licences: tests
and events earn the licences, cars and money that open the next races.

This repository is an Android port of **the iPad game GT Racing: Motor Academy Free+ HD, version 1.3.4b**, made by
static recompilation. The game's 32-bit ARMv7 code is translated ahead of time into C, compiled for Android
(`armeabi-v7a`), and linked with a host that stands in for the iOS libraries the game uses: UIKit, OpenGL ES,
OpenAL, GameKit, Foundation and the rest. Nothing is emulated while the game runs.

The port runs the game with the fixes it needs on Android and adds features of its own, which are on by default.
Each one has a `GT_*` key in `gtracing.ini`; setting it to `0` gives the game's own behaviour.

## Features

| Feature | Where to change it | Default |
|---|---|---|
| Any resolution, wide screens (16:9, 19.5:9, 20:9) with the menus and HUD kept in shape | Options > Display*, `GT_RESOLUTION` `GT_WIDE` | the phone's |
| MSAA anti-aliasing and anisotropic filtering | Options > Display*, `GT_MSAA` `GT_ANISOTROPIC` | 4x, 16x |
| PostFX: bloom in the style of the paid 2010 iPad version, or the game's unused bloom pass, fixed | Options > Display*, `GT_POSTFX` | Enhanced |
| The iPad 2's full motion blur in the iPad 3 mode | Options > Display*, `GT_FULL_BLUR` | auto |
| The 3D car scene behind the main menu | `GT_MENU_3D` | on |
| The game's own touch controls, and gamepads (Xbox and PlayStation layouts) | Options > Controls | on |
| Look back and a 360-degree look around the car | `GT_RACE_LOOK` | on |
| Local and Bluetooth play over Wi-Fi or a VPN; Online play through room codes | `GT_LAN_MULTIPLAYER` | on |
| The Red Bull leaderboard, kept locally and shared online | `GT_RED_BULL_BOARD` `GT_CLOUD_BOARD` | on |
| A coin-for-cash shop, full prizes for repeated results (3x), a car service paid in coins | `GT_COIN_SHOP` `GT_REPEAT_PRIZE` `GT_PRIZE_MULT` `GT_SERVICE_PRICE` | on |
| An offline Top 10 Cars list | `GT_TOP_CARS` | on |
| A Name tile on the multiplayer page | Name* | on |
| Add-on cars and the paid edition's licence tests and audio, made from other editions you own | the `add-on/` folder, `GT_ADDONS` | on when the files are there |

\* The port's menus (Options > Display and the Name tile) are the game's own menu files with changes, so they are
not in the download: they go in the optional `patch/` folder (see [Setup](#setup-instructions-for-players)). Without
them the game has its own menus, and `gtracing.ini` holds the same settings.

More on each part is in [Features in detail](#features-in-detail).

## Disclaimer

**GT Racing: Motor Academy** is a trademark of Gameloft, and the game and its assets are the property of their
respective owners, as are the car makers' names and marks. This is an unofficial, non-commercial fan project. It is
not affiliated with Gameloft, and Gameloft has not made, approved or supported it.

The authors of this work do not promote or condone piracy in any way. The game is not included, and to use this
software you need your own legally obtained copy of the iPad game (`GTRacingFreemiumHD.app`). This repository has
no textures, models, sound, menus, data, pictures or executable code of the game.

> [!NOTE]
> The app's icon is made from the game's artwork when it is built. This repository holds no art of the game, so an
> app built from the source gets its icon from your copy of the game.

## System Requirements

| | Android |
|---|---|
| Processor | ARM, with support for 32-bit (`armeabi-v7a`) apps |
| System | Android 7.0 or newer |
| Graphics | OpenGL ES 2.0 or newer |
| Storage | About 1.5 GB for the game, plus the app |

> [!IMPORTANT]
> The game's code is 32-bit ARM, so the app is 32-bit only. Many newer phones can no longer run 32-bit apps, and
> the app will not install or start on them.

**Not supported:** phones and tablets that run 64-bit apps only, and x86 Android devices.

## Setup Instructions (For Players)

1. Get your own copy of the iPad game, `GTRacingFreemiumHD.app`, version 1.3.4b.

   > [!IMPORTANT]
   > The port is made from **one build** of the game, and other builds are not supported. SHA-1 of
   > `GTRacingFreemiumHD.app/GTRacingFreemiumHD` **must** be `eaeca1915d8b0a250583afed587f61ecb85a728a`.
   >
   > To check yours (for example in Termux): `sha1sum GTRacingFreemiumHD.app/GTRacingFreemiumHD`

2. Go to [**Releases**](https://github.com/Adriansyah101/GTRacingMotorAcademyRecomp/releases/latest) and download
   the APK.

   To check that the download is intact, compare `sha256sum <file>` with the SHA-256 in the release notes.

3. Install the APK. Android asks you to allow installs from that source the first time.
4. Copy the contents of your `GTRacingFreemiumHD.app` to the phone, into the folder named in
   [android/README.md](android/README.md), so that `GTRacingFreemiumHD.app/GTRacingFreemiumHD` is there.
5. Start the game from its icon.

The first start makes `home/` in the app's folder (your save), and each start writes `gtracing.log` there.

**Settings:** the picture settings are on the game's Options > Display page (with the port's menus), and all
settings are in `gtracing.ini`. See [Settings](#settings).

**Optional folders:** `patch/` is for the port's menus, and `add-on/` for extra content, one folder per add-on: more
cars (Dodge, Ferrari, Hyundai), and the paid edition's extra licence tests and its 44 kHz audio. Neither is in the
download, and the game runs without them. An add-on is turned off by deleting (or moving out) its folder. A save
that holds an add-on car still loads without that add-on: the car is hidden until the folder is back.

### Updating, backing up and uninstalling

- **Your progress** is in the `home` folder: `home/Documents` holds the game's save, and `home/Library/Preferences`
  the port's own files (controls, player name, the local leaderboard). `gtracing.ini` holds the settings. To back
  up, copy `home/` and `gtracing.ini`.
- **To update,** install the new APK over the old one. Your save, settings, `patch/` and `add-on/` stay.
- **To uninstall,** uninstall the app. Back up `home/` first if you want to keep your save.
- **`gtracing.log` is safe to delete** at any time.

## Controls

The game plays with its own touch controls, as on the iPad: tilt or touch steering, the on-screen pedals, and the
pinch and swipe gestures in the replay.

A gamepad (Xbox or PlayStation layout, over Bluetooth or USB) also works:

| | Gamepad (Xbox / PlayStation) |
|---|---|
| Steer | left stick |
| Gas / brake | RT / LT (R2 / L2) |
| Gear up / down | A / X (Cross / Square) |
| Change camera | RB (R1) |
| Look back | LB (L1) held |
| Look around (chase views) | right stick |
| Pause | Menu (Options) |
| Menus | D-pad or left stick, A (Cross), B (Circle) |

## Settings

`gtracing.ini` in the app's folder holds `KEY=VALUE` lines; `#` starts a comment. Deleting a line gives that setting
its default.

- A missing or blank key takes its default; so does a number that is not one (`GT_FPS=abc`). A number outside a
  setting's range is taken as the nearest end of it.
- A UTF-8 byte order mark is skipped. A line over 510 characters and a key that is not a name (letters, digits and
  `_`) are skipped, with a line in the log.
- The **Display** page in the game writes its settings back to the file. Anti-aliasing, anisotropic filtering,
  motion blur and PostFX apply at once; resolution and aspect ratio need a restart (the page's Restart to Apply
  button).

### Player settings

| Key | Default | Meaning |
|---|---|---|
| `GT_DEVICE` | `ipad3` | `ipad3`: the iPad 3's Retina art; `ipad2`: the iPad 2's |
| `GT_RESOLUTION` | `monitor` | picture height in pixels: `monitor` (the phone's screen), a number such as `1080`, or `0` for the device's own |
| `GT_WIDE` | `monitor` | screen shape: `monitor` (the phone's screen), or the width in points: `1024` (4:3), `1366` (16:9), `1792` (21:9) |
| `GT_MSAA` | `auto` | anti-aliasing samples: `0`, `2`, `4`, `8`, or `auto` (4) |
| `GT_ANISOTROPIC` | `16` | anisotropic filtering: `1` (off), `2`, `4`, `8`, `16` |
| `GT_FULL_BLUR` | `auto` | motion blur: `1` full, `0` light, `auto`; also Options > Display > Motion Blur, applied at once |
| `GT_PAD_GLIDE` | `140` | ms for the pad pointer to glide to the next button; `0` jumps |

These keys have defaults that suit almost every player, and a line added to the file still changes them:

| Key | Default | Meaning |
|---|---|---|
| `GT_POSTFX` | `4` | also Options > Display > PostFX, applied at once: `0` Off (none, as shipped); `2` Classic, a bloom drawn the way the paid 2010 iPad version draws it, with each track's own glow and colour settings; `3` Classic Natural, the same glow with the world's own colours kept; `4` Enhanced, the game's own unused bloom pass, fixed, with a glow drawn only from the brightest parts of the picture; `1` the game's own unused bloom pass as shipped (ini only) |
| `GT_SOFT_REFLECTION` | `3` | the cars' live reflection, softened (mipmap level `1` to `6`) on the tracks below; `0`: the original sharp, streaky one |
| `GT_SOFT_REFLECTION_TRACKS` | blank | the tracks `GT_SOFT_REFLECTION` softens, by file name, comma-separated (blank: `snowrally,snowrallyalt`, the two Scandinavia tracks), or `all` |
| `GT_LAN_MULTIPLAYER` | `1` | Local, Bluetooth and Online multiplayer |
| `GT_LAN_PEERS` | blank | extra addresses Local should reach, comma-separated |
| `GT_P2P_PORT` | `28200` | the UDP port an Online room opens on the router |
| `GT_CLOUD_BOARD` | `1` | share the Red Bull leaderboard through `GT_CLOUD_BOARD_URL` |
| `GT_CLOUD_BOARD_URL` | `https://gt-redbull-board.gtracing-pc.workers.dev` | the leaderboard server (blank: this default); `GT_CLOUD_BOARD=0` keeps the board offline |

### Feature switches

Every port addition is on by default (except where noted). Set its key to `0` to get the original behaviour.

| Key | Feature |
|---|---|
| `GT_DISPLAY_MENU` | the Display page in place of Customer Care |
| `GT_WIDE_UI`, `GT_WIDE_HUD` | menus and HUD kept in shape on wide screens; HUD at the screen's edges |
| `GT_EVENT_SIDES`, `GT_UPGRADE_SIDES` | the neighbouring carousel cards shown on wide screens |
| `GT_MENU_3D` | the 3D scene behind the main menu |
| `GT_UPGRADE_3D` | the 3D scene behind the car upgrade menu (off by default; `1` turns it on) |
| `GT_MENU_CAR_RANDOM` | a varied car in the main menu |
| `GT_PAD_MENU` | the pad pointer that snaps to menu buttons |
| `GT_RACE_LOOK` | the 360-degree look (`GT_RACE_LOOK_SMOOTH`, ms, default 60) |
| `GT_TILT_ROLL`, `GT_TILT_SMOOTH` | camera tilt strength (percent, default 25) and smoothing (ms, default 400) |
| `GT_REPLAY_UNDIM`, `GT_REPLAY_PAD`, `GT_REPLAY_HINT_PAD` | the replay fixes and pad controls |
| `GT_COIN_SHOP`, `GT_DYNAMIC_PRICE` | the coin-for-cash shop and its changing rate (`GT_COIN_RATE`, default 1100) |
| `GT_PRIZE_MULT` | prize multiplier (default 3; `1`, or a value outside 1 to 100, is the original) |
| `GT_REPEAT_PRIZE` | full prizes for repeated results |
| `GT_SERVICE_PRICE` | the car service price in coins (default 250; `0` is the original service) |
| `GT_TOP_CARS` | the offline Top 10 Cars list |
| `GT_RED_BULL_BOARD` | the local Red Bull leaderboard |
| `GT_FIRST_NAME` | the name asked on the first start |
| `GT_ADDONS` | the add-on folders |
| `GT_MISSING_CARS` | safe loading of a save whose add-on cars are missing |
| `GT_GPU_PROFILE` | `auto`; or `igpu` / `full` to skip the GPU detection |
| `GT_UTF16_CSTRING` | C strings asked for in UTF-16 made in UTF-16 (the fix of a new profile's name) |
| `GT_FPS`, `GT_VSYNC` | the frame cap (default 60, `0` none) and vsync |
| `GT_LANG` | the game's language (e.g. `fr`) |

Optimisations can be turned off one by one, for comparison or if a driver misbehaves:

- `GT_FAST_SWF_FIND`, `GT_SWF_MISS_CACHE`;
- `GT_GL_STATE_CACHE`, `GT_GL_ARRAY_CACHE`, `GT_GL_QUERY_FREE`, `GT_STREAM_VBO`;
- `GT_FIXED_FAST`, `GT_INDEX_FAST`, `GT_PVRTC_FAST`;
- `GT_LIVE_BLOCKS`, `GT_WIDE_CACHE`.

`GT_HOME` (the save folder) and `GT_LOG` (the log file; `0`: no log) are paths.

## Performance Tips

The game is capped at 60 fps (`GT_FPS`). If it runs slowly or the phone gets hot, in this order:

1. Set `GT_MSAA` to 2 or 0.
2. Set PostFX (Options > Display, or `GT_POSTFX`) to Classic Natural or Off, and Motion Blur to light
   (`GT_FULL_BLUR=0`). Each draws extra passes over the picture.
3. Lower the resolution (`GT_RESOLUTION`, for example `720`).
4. Cap the frame rate lower (`GT_FPS=30`) to save battery and heat.

## Troubleshooting

| Problem | What to do |
|---|---|
| The app does not install ("App not compatible") | The phone cannot run 32-bit apps. See [System Requirements](#system-requirements) |
| The game says it cannot find `GTRacingFreemiumHD.app` | Copy the game into the folder named in [android/README.md](android/README.md), so that `GTRacingFreemiumHD.app/GTRacingFreemiumHD` is there |
| "loading the game: not ARM ..." or another loading error | The file is not the iPad game's program. Check its SHA-1 (step 1 above): only that one build works |
| No Display page or Name tile | The port's menus are not installed: `patch/` is empty. The same settings are in `gtracing.ini` |
| A new resolution or aspect ratio does not show | They apply after a restart: use the Display page's Restart to Apply button, or start the game again |
| Other players are not found in Local or Bluetooth | Both phones must be on the same Wi-Fi network or the same VPN. `GT_LAN_PEERS` can name the other phone's address |
| An Online room cannot be joined | The host's router must open the room's UDP port (`GT_P2P_PORT`, 28200) through UPnP or NAT-PMP. Without them, forward that port by hand or play Local over a VPN. Mobile data networks usually do not allow hosting |
| The Red Bull leaderboard shows only local times | The board's server is not reached: check the internet connection, and that `GT_CLOUD_BOARD=0` is not set |
| A black screen, a crash or an error | Report it (see below) with `gtracing.log` |

## Known Issues

1. **Few phones tested.** Reports from other phones and GPUs (Adreno, Mali, PowerVR) are welcome.
2. **Multiplayer** (Local, Bluetooth and Online) has not been tested between two phones. Opening the Online port on
   a real router (UPnP, NAT-PMP) has not been tested.
3. **Resolution and aspect ratio need a restart**, because the game sizes its screen, its render targets and its 3D
   view's shape once as it starts.
4. **The Display and Controls pages open about 40 frames late:** the game's own camera flight runs first, as on
   the iPad.
5. **Restart is missing from the pause menu** when the car's service points are 0. This is the original game's
   rule.
6. **Gameloft's servers are gone.** What needed them, such as the ads and Customer Care, does not work (the Display
   page takes Customer Care's place). The online lobby is replaced by the port's peer-to-peer Online play, and the
   online Red Bull leaderboard by the port's own.

### Reporting a bug

Open an issue on the [**Issues**](https://github.com/Adriansyah101/GTRacingMotorAcademyRecomp/issues) page with:

- your phone's model, its Android version and its chip (for example Snapdragon 870);
- the port's version, and which download you use (release or built from source);
- what happened, and the steps that lead to it;
- `gtracing.log`, attached as a file.

## Privacy

`gtracing.log` stays on your phone until you choose to send it. The names of your folders and your public internet
address are written in it as tags (`%HOME%`, `%APP%`, `%IP%` ...) or blanked. `GT_LOG=0` turns the log off: no file
is made and nothing is written to the system log.

The network is used only for multiplayer and for the Red Bull leaderboard server. The server receives a random
player number, the name you go by, the car and the lap time; never your account names, your phone's name or your
folders. `GT_CLOUD_BOARD=0` keeps the board offline.

In detail:

- **The app**: every compile maps the project folder to `.` (`-ffile-prefix-map`), so no build-machine path is in
  the code, the string tables or the debug info. The release build has no diagnostic keys or trace strings.
- **The log** (`host/core/scrub.c`): before a line is written, the save's folder, the game's folder and the app's
  folder are replaced by tags, longest first; public IPv4 addresses become `%IP%`. The formatting buffer is zeroed
  after each line. The game's own console output (`printf`, `puts`, `NSLog` ...) goes through the same path.
- **What the game sees**: no environment variables (`getenv` returns nothing), the host name `iPad`, a fixed device
  identifier, and a fixed GameKit name.
- **The network**: used only for multiplayer (local network; Online asks the local router through UPnP or
  NAT-PMP, with no outside server) and the Red Bull leaderboard server (`GT_CLOUD_BOARD_URL`, or off with
  `GT_CLOUD_BOARD=0`). The server receives a random player number, the chosen player name, the car and the lap
  time.

## Features in detail

### Picture

- Any resolution up to the phone's own, with the iPad 3's Retina art (or the iPad 2's).
- Wide screens: the 3D view fills the screen. The menus and the race HUD keep their shape, their backgrounds reach
  across the screen, and the HUD sits at the screen's edges as it does on the iPad.
- MSAA anti-aliasing (2x, 4x, 8x), anisotropic filtering (up to 16x) and the iPad 2's full motion blur in the
  iPad 3 mode.
- A GPU profile: picture settings left on "auto" pick lighter values on weaker GPUs.
- PostFX: the free iPad game shipped without any bloom. Options > Display > PostFX can switch on a bloom drawn the
  way the paid 2010 iPad version draws it, with each track's own glow and colour settings (Classic), or the same
  glow with the world's own colours kept (Classic Natural), in races and behind the menus' 3D scene. Enhanced adds
  the game's own unused specular-highlight bloom pass, with its shaders' colour and brightness faults evened out,
  under a Classic Natural glow blended so bright areas never clip. Enhanced is the default; Off (`GT_POSTFX=0`)
  draws the picture as the game shipped. Every PostFX choice applies at once.
- An in-game **Display** page (Options > Display) for all of the above. It replaces the Customer Care tile, which
  needed Gameloft's servers.
- The 3D scene with a car behind the main menu, as in older versions of the game.

### Controls

- The game's own touch controls.
- Gamepads (Xbox and PlayStation layouts through SDL 3).
- Menus driven by the pad: the pointer snaps to the menu's buttons and glides between them.
- Look back, a 360-degree look around the car (right stick), and a smoother, smaller camera tilt.
- In the replay: pad control of the camera, zoom and time slider.

### Game

- An offline coin-for-cash shop on the game's own shop screen, with a rate that changes now and then.
- Repeated race and event results pay their full prize, and prizes are multiplied (3x by default).
- A car service paid in coins only and done at once.
- An offline Top 10 Cars list: Buy and Rent open the Car Dealer at that car.
- The Red Bull leaderboard kept locally, merged with friends met in multiplayer, and optionally shared through a
  small server ([below](#online-leaderboard-server)).
- A player name, editable on the multiplayer page and asked for on the first start.
- Optional add-on cars and the paid edition's licence tests and audio, built from other editions of the game you own
  ([Add-ons](#add-ons)).

### Multiplayer

- **Local** and **Bluetooth** play over Wi-Fi or a VPN (Tailscale and the like).
- **Online** play through room codes (`GT-XXXXX-XXXXX`): one player hosts, and the port is opened on the
  router with UPnP or NAT-PMP. Gameloft's lobby server is gone, so this runs peer to peer.

### Fixes

- **Original-game faults** fixed:
  - the dimmed replay picture;
  - a medal counter that counted one place too many;
  - the off-centre placement of the menus' 3D pictures, which shows on wide screens.
- **Port issues** fixed: stale GL bindings across threads, a new profile's name (made as 8-bit text where the game
  asks for UTF-16), and others. The comments at each fix explain it.
- **Performance work**: native gameswf search and a miss cache, GL state shadowing and streaming vertex buffers,
  fast fixed-point conversion and index scans, threaded PVRTC decoding, and a frame cap.
- **Settings**: every number in `gtracing.ini` is checked when read (see [Settings](#settings)).

## Build Instructions (For Developers)

You need:

- **the game**: put `GTRacingFreemiumHD.app` (iPad, version 1.3.4b as its `Version.txt` says; its Info.plist's
  `CFBundleVersion` reads 1.3.3) in the repository's root folder. It is read, never modified;
- **Python** 3.12, with `capstone` 5 (and `unicorn` 2.1.3 for the tests);
- **the Android SDK and NDK**, for the `armeabi-v7a` build.

Clone this repository and put your `GTRacingFreemiumHD.app` in its folder. Then:

```
cd GTRacingMotorAcademyRecomp

# stage 1, once per binary: every function of the game, into analysis/stage1
python -I recomp/discover.py GTRacingFreemiumHD.app/GTRacingFreemiumHD analysis/stage1
```

The generated C, the app build, signing the APK and installing it on the phone are described in
[android/README.md](android/README.md). The host's platform calls have POSIX meanings on Android
(`host/core/plat.h`), and the game's OpenGL ES calls go to the phone's own OpenGL ES.

The build also:

- patches the game's menu SWFs (the port's menus; copy them into the `patch/` folder for your own use);
- makes the app icon from the game's artwork.

Every compile (host C, assembly, `runtime/` and the generated code) uses `-ffile-prefix-map=<project folder>=.`, so
no build-machine path is embedded in the app or its debug info.

### How it works

The port is built in five stages.

1. **Function discovery** (`recomp/discover.py`). Every function, block, jump table and literal pool in the
   Mach-O's `__text` is found, using `LC_FUNCTION_STARTS`, call targets and a small register-value tracker for
   SjLj exception resume labels. The result is `analysis/stage1/functions.json`. It covers 23,873 functions
   (14,348 ARM, 9,525 Thumb) and 1.8 million instructions, with no unresolved indirect jump.
2. **C code generation** (`recomp/armc.py`, `recomp/codegen.py`):
   - Each guest function becomes one C function, `F_<addr>(CPU *c)`.
   - Integer registers and flags are C locals, branches are `goto`, and jump tables are `switch`.
   - Calls follow the iOS ARM ABI. VFP short vectors and NEON are expanded per element.
   - NaN results are given ARM's bit pattern.
   - Every instruction form is tested bit for bit against Unicorn (Cortex-A8).
3. **Runtime** (`host/core/`, `runtime/`):
   - The game image is loaded at its own addresses.
   - The loader maps the segments and binds every import. Calls into imports go through ARM-ABI bridges
     generated from `imports.sig` (`recomp/bridgegen.py`).
   - Indirect calls go through a direct-mapped dispatch table.
4. **The iOS libraries** (`host/shims/`):
   - Objective-C 2, libc, libstdc++, pthreads and unwinding.
   - Foundation and CoreFoundation, property lists, CoreGraphics, UIKit, OpenAL and GameKit.
   - The network.
   - OpenGL ES 2, passed to the phone's own OpenGL ES (`host/shims/gl/`).
   - SDL 3 provides the window, input and audio output.
5. **The port's additions** (`host/games/gtracing/`): the features and fixes above.
   - Most hook the game by wrapping functions at link time (`--wrap`), or by replacing single instructions at code
     generation time (`HOST_INSNS` in `recomp/codegen.py`).
   - Menu additions are made by patching the game's own SWF menu files at build time (`tools/swfpatch_*.py`).
     The patched copies stay in the build folder.

### Repository layout

| Path | What |
|---|---|
| `android/` | the Android app project and its [README](android/README.md) |
| `recomp/macho.py` | Mach-O reader: segments, sections, symbols, function starts, stubs |
| `recomp/discover.py` | stage 1: function discovery |
| `recomp/disasm.py` | disassembly around an address, in the right ARM/Thumb mode |
| `recomp/census.py` | instruction counts by kind |
| `recomp/armc.py`, `recomp/codegen.py` | stage 2: the ARM/Thumb/VFP/NEON-to-C translator and the generator for `build/gen/` |
| `recomp/bridgegen.py` | the import bridges, from `host/core/imports.sig` |
| `runtime/` | `arm_rt.h` (CPU state, memory macros and helpers for the generated C), `rt_jump.c` and `rt_selftest.c` (guest-style functions for the self-checks) |
| `tests/difftest.py`, `tests/functest.py`, `tests/tmem.c` | instruction and whole-function tests against Unicorn |
| `host/core/` | the loader, the runtime (threads, calls between host and game, traps, log), `gtracing.ini` and the settings (`settings.c`), the self-checks, `imports.sig`, `plat.h` |
| `host/shims/` | the iOS libraries the game imports; `gl/` holds OpenGL ES and EAGL. They know nothing of the game: every call they make into it is declared in `game_hooks.h`, and shader patches are registered through `gl.h` |
| `host/games/gtracing/` | `layout.h` (the address-space layout); `game.h` (the game's globals, functions, structure offsets and vtable slots that several features use); `features/` (port additions and fixes); `render/` (picture additions) |
| `host/third_party/SDL3/` | SDL 3 |
| `tools/swfpatch_*.py` | build-time patches of the game's menu SWFs |
| `tools/*_addon.py` | add-on builders ([below](#add-ons)) |
| `tools/make_icon.py` | the app's icon, made from the game's `iTunesArtwork` at build time |
| `tools/addr2sym.py`, `tools/fdis.py` | debugging helpers: host addresses to symbols, guest function disassembly |
| `tools/layer_check.py` | the layer boundary in the built objects: the shims reach the game only through `game_hooks.h` |
| `cloud/redbull-board/` | the optional online leaderboard server (Cloudflare Worker + D1) |

`.gitignore` keeps out the game, anything taken from it (other editions, analysis dumps, screenshots) and the build
output.

### Tests

- `tests/difftest.py` checks every instruction form the translator emits, on random states, against Unicorn.
- `tests/functest.py` checks whole leaf functions against Unicorn.

```
python -I tests/difftest.py GTRacingFreemiumHD.app/GTRacingFreemiumHD analysis/stage1/functions.json build/difftest
python -I tests/functest.py GTRacingFreemiumHD.app/GTRacingFreemiumHD analysis/stage1/functions.json build/functest
```

### Add-ons

Add-ons are folders in `add-on/`. Their files are read as if they were in the game's bundle. Each one is built by a
tool from a game edition you own, and none is part of this repository.

| Tool | Builds |
|---|---|
| `tools/hyundai_addon.py` | the Hyundai Genesis Coupe and i-oniq, from GT Racing: Hyundai Edition (Android OBB) |
| `tools/android_free_addon.py` | the Dodge Charger 2012 and Ferrari 288 GTO, from Android GT Racing Free+ 1.4.3 |
| `tools/premium_addon.py` | the paid iPad edition's three extra licence tests and its 44 kHz audio |

Run each with `python -I tools/<tool>.py`; its docstring lists the arguments.

### Online leaderboard server

`cloud/redbull-board/` is a small Cloudflare Worker with one D1 table, which shares the Red Bull Time Trial's
best laps between players. The game uses the project's server by default; `GT_CLOUD_BOARD_URL` picks another,
and with `GT_CLOUD_BOARD=0` the board stays on the phone and with the friends met in multiplayer.

- **What it receives**: a random player number, the name the player goes by, the car and the lap time.
- **Validation**: names and lap times are checked.
- **Player ids**: they are never shown; the board shows a hash alias instead.

See its [README](cloud/redbull-board/README.md) for deployment.

## Contributing

Help is welcome, and the most useful help right now needs no code at all:

- **Play it on phones that have not been tried:** other chips and GPUs (Adreno, Mali, PowerVR), older phones.
- **Play multiplayer between two phones**, on one Wi-Fi network or over a VPN, and Online through room codes.

Report what you find on the [Issues](https://github.com/Adriansyah101/GTRacingMotorAcademyRecomp/issues) page,
with the log, even when everything works.

For code:

- **Comments**: write them in plain, technical English. Explain what the code does and why: the original
  game's behaviour (with addresses), whether a fix is for an original-game fault or a port fault, and how it
  was verified.
- **What comments leave out**: conversation transcripts, quoted requests, personal names and dates of work
  sessions.
- **New features**: they are on by default, with a `GT_*` key to switch them off.
- **Before a pull request**: build with no warnings and run `tools/layer_check.py`.
- **Game files**: never commit any, including extracted, patched or converted ones.

## Credits

- **[Adriansyah101](https://github.com/Adriansyah101)**, for the original port.
- **Gameloft**, for the game.
- **[SDL](https://www.libsdl.org/)**, for the window, input, sound and gamepads.
- **[Android NDK](https://developer.android.com/ndk)**, for the toolchain.
- **[Capstone](https://www.capstone-engine.org/)**, for the disassembler behind function discovery, and
  **[Unicorn](https://www.unicorn-engine.org/)**, for the instruction tests.
- **[Cloudflare Workers](https://workers.cloudflare.com/)** and D1, for the leaderboard server.

## License

The code in this repository is dual-licensed: you may use it under the terms of either the
[MIT License](LICENSE-MIT) or the [GNU General Public License, version 3](LICENSE-GPL-3.0), at your choice
(SPDX: `MIT OR GPL-3.0-only`).

Third-party code keeps its own license: SDL 3 in `host/third_party/SDL3` is under the zlib license
([LICENSE.txt](host/third_party/SDL3/LICENSE.txt)). The licenses cover this project's code only, not the game,
its files or its trademarks.
