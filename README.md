# GT Racing: Motor Academy HD · PC Port

<p align="center">
  <a href="https://github.com/Adriansyah101/GTRacingMotorAcademyRecomp/releases/latest"><img src="https://img.shields.io/github/v/release/Adriansyah101/GTRacingMotorAcademyRecomp" alt="Latest release"></a>
  <a href="#license"><img src="https://img.shields.io/badge/license-MIT%20OR%20GPL--3.0-blue" alt="License: MIT OR GPL-3.0-only"></a>
  <img src="https://img.shields.io/badge/platform-Windows-blue" alt="Platform: Windows">
  <img src="https://img.shields.io/badge/CPU-x86%20(Intel%20%2F%20AMD)-blue" alt="CPU: x86 (Intel or AMD)">
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

This repository is a Windows port of **the iPad game GT Racing: Motor Academy Free+ HD, version 1.3.4b**, made by
static recompilation. The game's 32-bit ARMv7 code is translated ahead of time into C, compiled for x86, and linked
with a host that stands in for the iOS libraries the game uses: UIKit, OpenGL ES, OpenAL, GameKit, Foundation and
the rest. Nothing is emulated while the game runs.

The port runs the game with the fixes it needs on a PC and adds PC features, which are on by default. Each one has a
`GT_*` key in `gtracing.ini`; setting it to `0` gives the game's own behaviour.

## Features

| Feature | Where to change it | Default |
|---|---|---|
| Any resolution, wide screens (16:10, 16:9, 21:9) with the menus and HUD kept in shape | Options > Display*, `GT_RESOLUTION` `GT_WIDE` | the monitor's |
| Full screen or a window | Alt+Enter, Options > Display*, `GT_FULLSCREEN` | full screen |
| MSAA anti-aliasing and anisotropic filtering | Options > Display*, `GT_MSAA` `GT_ANISOTROPIC` | 4x, 16x |
| PostFX: bloom in the style of the paid 2010 iPad version, or the game's unused bloom pass, fixed | Options > Display*, `GT_POSTFX` | Enhanced |
| The iPad 2's full motion blur in the iPad 3 mode | Options > Display*, `GT_FULL_BLUR` | auto |
| The 3D car scene behind the main menu | `GT_MENU_3D` | on |
| Keyboard, mouse and gamepads (Xbox and PlayStation layouts), remappable keys | Options > Controls*, `GT_PC_CONTROLS` | on, Touch Screen Off |
| Menus driven by the keys or a pad, with a pointer that snaps to the buttons | `GT_PAD_MENU` | on |
| Look back and a 360-degree look around the car | `GT_RACE_LOOK` | on |
| Replay zoom with the mouse wheel, and replay control from the pad | `GT_REPLAY_ZOOM` `GT_REPLAY_PAD` | on |
| Local and Bluetooth play over a LAN or a VPN; Online play through room codes | `GT_LAN_MULTIPLAYER` | on |
| The Red Bull leaderboard, kept locally and shared online | `GT_RED_BULL_BOARD` `GT_CLOUD_BOARD` | on |
| A coin-for-cash shop, full prizes for repeated results (3x), a car service paid in coins | `GT_COIN_SHOP` `GT_REPEAT_PRIZE` `GT_PRIZE_MULT` `GT_SERVICE_PRICE` | on |
| An offline Top 10 Cars list | `GT_TOP_CARS` | on |
| An Exit tile on the main menu and a Name tile on the multiplayer page | `GT_EXIT_BUTTON`, Name* | on |
| Add-on cars and the paid edition's licence tests and audio, made from other editions you own | the `add-on\` folder, `GT_ADDONS` | on when the files are there |

\* The PC menus (Options > Display, the keys page in Options > Controls, the Exit and Name tiles) are the game's own
menu files with changes, so they are not in the download: they go in the optional `patch\` folder (see
[Setup](#setup-instructions-for-players)). Without them the game has its own menus, and `gtracing.ini` holds the
same settings.

More on each part is in [Features in detail](#features-in-detail).

## Disclaimer

**GT Racing: Motor Academy** is a trademark of Gameloft, and the game and its assets are the property of their
respective owners, as are the car makers' names and marks. This is an unofficial, non-commercial fan project. It is
not affiliated with Gameloft, and Gameloft has not made, approved or supported it.

The authors of this work do not promote or condone piracy in any way. The game is not included, and to use this
software you need your own legally obtained copy of the iPad game (`GTRacingFreemiumHD.app`). This repository has
no textures, models, sound, menus, data, pictures or executable code of the game.

> [!NOTE]
> The release program's icon is made from the game's artwork when it is built. This repository holds no art of the
> game, so a program built from the source gets its icon from your copy of the game.

## System Requirements

| | Windows |
|---|---|
| Processor | x86 (Intel or AMD). The program is 32-bit; tested on 64-bit Windows |
| System | Windows 10 or 11. Tested on Windows 11 |
| Libraries | Included (`SDL3.dll`) |
| Graphics | OpenGL 2.1 or newer. Tested on an NVIDIA RTX 3050 Ti and on AMD Radeon graphics built into the processor |
| Disk | About 1.5 GB for the game, 21 MB for the port |

**Not supported:** ARM processors (Windows on ARM runs x86 programs through its own emulation, but the port has not
been tried there), macOS and Linux.

## Setup Instructions (For Players)

1. Get your own copy of the iPad game, `GTRacingFreemiumHD.app`, version 1.3.4b.

   > [!IMPORTANT]
   > The port is made from **one build** of the game, and other builds are not supported. SHA-1 of
   > `GTRacingFreemiumHD.app\GTRacingFreemiumHD` **must** be `eaeca1915d8b0a250583afed587f61ecb85a728a`.
   >
   > To check yours: `certutil -hashfile "GTRacingFreemiumHD.app\GTRacingFreemiumHD" SHA1`

2. Go to [**Releases**](https://github.com/Adriansyah101/GTRacingMotorAcademyRecomp/releases/latest) and download
   `GTRacing-PC-Port-1.0.0-Windows-x86.zip`.

   To check that the download is intact, compare `certutil -hashfile <file> SHA256` with the SHA-256 in the release
   notes. `SHA256SUMS.txt` in the zip lists each file inside it.

3. Extract it.
4. Copy the contents of your `GTRacingFreemiumHD.app` into the empty `GTRacingFreemiumHD.app` folder inside it, so
   that `GTRacingFreemiumHD.app\GTRacingFreemiumHD` is there.
5. Double-click `gtracing.exe`. Keep `SDL3.dll` next to it.

The first start makes `home\` next to the program (your save), and each start writes `gtracing.log` next to the
program.

**Settings:** the picture settings are on the game's Options > Display page (with the PC menus), and all settings
are in `gtracing.ini` next to the program. See [Settings](#settings).

**Optional folders:** `patch\` is for the PC menus, and `add-on\` for extra content, one folder per add-on: more cars
(Dodge, Ferrari, Hyundai), and the paid edition's extra licence tests and its 44 kHz audio. Neither is in the
download, and the game runs without them. The note in each folder and `IMPORTANT.TXT` say where to find out more.
An add-on is turned off by deleting (or moving out) its folder. A save that holds an add-on car still loads without
that add-on: the car is hidden until the folder is back.

### Updating, backing up and uninstalling

- **Your progress** is in the `home` folder: `home\Documents` holds the game's save, and `home\Library\Preferences`
  the port's own files (controls, keys, player name, the local leaderboard). `gtracing.ini` holds the settings. To
  back up, copy `home\` and `gtracing.ini`.
- **To update,** extract the new version to a new folder, put the game in it as before, and copy the old `home\`
  folder into it. Copy `gtracing.ini`, `patch\` and `add-on\` over too, if you changed or filled them.
- **To uninstall,** delete the folder. The port keeps its files in its own folder.
- **`gtracing.log` and `gtracing.dmp` are safe to delete** at any time.

### Command line

| Option | What it does |
|---|---|
| `--app "<path to GTRacingFreemiumHD.app>"` | Use a copy of the game kept somewhere else. Without it, the port looks next to itself, then up to three folders up |
| `--selftest` | Run the port's self-checks instead of the game |

## Controls

| | Keyboard | Gamepad (Xbox / PlayStation) |
|---|---|---|
| Steer | A / D, Left / Right | left stick |
| Gas / brake | W / S, Up / Down | RT / LT (R2 / L2) |
| Gear up / down | Shift / Ctrl | A / X (Cross / Square) |
| Change camera | C | RB (R1) |
| Look back | V (held) | LB (L1) held |
| Look around (chase views) | numpad 1-9 | right stick |
| Pause | Esc | Menu (Options) |
| Menus | W A S D or the arrows, Enter, Esc | D-pad or left stick, A (Cross), B (Circle) |
| Full screen / window | Alt+Enter | |

The mouse works everywhere, as the iPad's touch screen: click to tap, drag to swipe, wheel to pinch in the replay.

Options > Controls has a **Touch Screen** switch, Off by default on a PC. With it Off you get keyboard and pad options
(camera tilt, manual or automatic gears, auto gas), the keys page (Options > Controls > Keyboard / Gamepad, with the
PC menus), a race HUD without touch icons, and texts that say "press" or "click" instead of "touch". On brings back
the game's own touch controls and settings.

## Settings

`gtracing.ini` next to the program holds `KEY=VALUE` lines; `#` starts a comment. Deleting a line gives that
setting its default. An environment variable of the same name wins over the file, so a script or a test run can
override any key.

- A missing or blank key takes its default; so does a number that is not one (`GT_FPS=abc`). A number outside a
  setting's range is taken as the nearest end of it (`GT_CURSOR_SIZE=9` is 6).
- A UTF-8 byte order mark (as older Notepad writes) is skipped. A line over 510 characters and a key that is not a
  name (letters, digits and `_`) are skipped, with a line in the log.
- `GT_INI=<file>` reads another file.
- The **Display** page in the game writes its settings back to the file. Full screen, anti-aliasing, anisotropic
  filtering, motion blur and PostFX apply at once; resolution and aspect ratio need a restart (the page's Restart to
  Apply button).

### Player settings

These are the keys in the release `gtracing.ini` (`tools/release/gtracing.ini`).

| Key | Default | Meaning |
|---|---|---|
| `GT_DEVICE` | `ipad3` | `ipad3`: the iPad 3's Retina art; `ipad2`: the iPad 2's |
| `GT_RESOLUTION` | `monitor` | picture height in pixels: `monitor`, a number such as `1080`, or `0` for the device's own |
| `GT_WIDE` | `monitor` | screen shape: `monitor`, or the width in points: `1024` (4:3), `1229` (16:10), `1366` (16:9), `1792` (21:9) |
| `GT_FULLSCREEN` | `1` | `1` full screen, `0` a window |
| `GT_MSAA` | `auto` | anti-aliasing samples: `0`, `2`, `4`, `8`, or `auto` (4) |
| `GT_ANISOTROPIC` | `16` | anisotropic filtering: `1` (off), `2`, `4`, `8`, `16` |
| `GT_FULL_BLUR` | `auto` | motion blur: `1` full, `0` light, `auto` (light on integrated GPUs); also Options > Display > Motion Blur, applied at once |
| `GT_BACKGROUND_THROTTLE` | `1` | 30 fps behind other windows, 10 fps and a paused race when minimized (never in multiplayer) |
| `GT_CURSOR_SIZE` | `3` | mouse pointer size, a multiple of 32 px (1 to 6) |
| `GT_PAD_GLIDE` | `140` | ms for the pad pointer to glide to the next button; `0` jumps |

These keys are not in the release `gtracing.ini`. Their defaults suit almost every player, and a line added
to the file still changes them:

| Key | Default | Meaning |
|---|---|---|
| `GT_POSTFX` | `4` | also Options > Display > PostFX, applied at once: `0` Off (none, as shipped); `2` Classic, a bloom drawn the way the paid 2010 iPad version draws it, with each track's own glow and colour settings; `3` Classic Natural, the same glow with the world's own colours kept; `4` Enhanced, the game's own unused bloom pass (white highlights, no doubled brightness, stronger car highlights, a softer sheen on sunlit track surfaces) with a glow over it drawn only from the brightest parts of the picture, in their own colours, and slightly richer colours, so shadows stay as deep as with Off and bright areas do not turn flat white; its own settings behind the menus' 3D scene; `1` the game's own unused bloom pass as shipped (ini only) |
| `GT_SOFT_REFLECTION` | `3` | the cars' live reflection, softened (mipmap level `1` to `6`) on the tracks below; `0`: the original sharp, streaky one |
| `GT_SOFT_REFLECTION_TRACKS` | blank | the tracks `GT_SOFT_REFLECTION` softens, by file name, comma-separated (blank: `snowrally,snowrallyalt`, the two Scandinavia tracks), or `all`; the names are listed in `host/games/gtracing/render/softrefl.c` |
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
| `GT_PC_CONTROLS` | the Touch Screen switch and keyboard/pad rows in Options > Controls |
| `GT_TOUCH_SCREEN` | `1`: start with Touch Screen On when no choice is saved yet (default: Off) |
| `GT_CAMERA_TILT_DEFAULT` | `1`: Camera Tilt On with the keyboard / gamepad when no choice is saved yet (default: Off) |
| `GT_HIDE_TOUCH_HUD` | touch icons hidden in a race with Touch Screen Off |
| `GT_NO_TOUCH_TEXT` | "touch" texts reworded with Touch Screen Off |
| `GT_PAD_MENU` | the pad pointer that snaps to menu buttons |
| `GT_CURSOR` | the gold pointer (`0`: the Windows arrow) |
| `GT_RACE_LOOK` | the 360-degree look (`GT_RACE_LOOK_SMOOTH`, ms, default 60) |
| `GT_TILT_ROLL`, `GT_TILT_SMOOTH` | camera tilt strength (percent, default 25) and smoothing (ms, default 400) |
| `GT_REPLAY_UNDIM`, `GT_REPLAY_ZOOM`, `GT_REPLAY_PAD`, `GT_REPLAY_TOUCH_ICONS`, `GT_REPLAY_HINT_PAD` | the replay fixes and controls |
| `GT_EXIT_BUTTON` | the Exit button |
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
| `GT_CRASH_DUMP` | the crash minidump |
| `GT_UTF16_CSTRING` | C strings asked for in UTF-16 made in UTF-16 (the fix of a new profile's name) |
| `GT_SWAP_COPY` | the pixel format that keeps Windows' pop-ups visible in full screen |
| `GT_FPS`, `GT_VSYNC` | the frame cap (default 60, `0` none) and vsync |
| `GT_LANG` | the game's language (e.g. `fr`) |

Optimisations can be turned off one by one, for comparison or if a driver misbehaves:

- `GT_FAST_SWF_FIND`, `GT_SWF_MISS_CACHE`;
- `GT_GL_STATE_CACHE`, `GT_GL_ARRAY_CACHE`, `GT_GL_QUERY_FREE`, `GT_STREAM_VBO`;
- `GT_FIXED_FAST`, `GT_INDEX_FAST`, `GT_PVRTC_FAST`;
- `GT_LIVE_BLOCKS`, `GT_PRECISE_WAIT`, `GT_SCHED_YIELD`, `GT_WIDE_CACHE`.

`GT_HOME` (the save folder), `GT_LOG` (the log file; `0`: no log) and `GT_INI` (the settings file) are paths.

## Performance Tips

The game is light for a current PC: on the GPUs above a race runs at over 300 fps with the frame cap off. It is
capped at 60 fps (`GT_FPS`). On a slower PC, in this order:

1. Set `GT_MSAA` to 2 or 0.
2. Set PostFX (Options > Display, or `GT_POSTFX`) to Classic Natural or Off, and Motion Blur to light
   (`GT_FULL_BLUR=0`). Each draws extra passes over the picture.
3. Lower the resolution (`GT_RESOLUTION`, for example `720`).

## Troubleshooting

| Problem | What to do |
|---|---|
| "no GTRacingFreemiumHD.app next to the program (or give --app &lt;folder&gt;)" | Put the `GTRacingFreemiumHD.app` folder next to `gtracing.exe` (or up to three folders up), or name it with `--app` |
| "The GTRacingFreemiumHD.app folder next to gtracing.exe has no game in it" | Copy the files of your game's `GTRacingFreemiumHD.app` into it, so that `GTRacingFreemiumHD.app\GTRacingFreemiumHD` is there |
| "loading the game: not ARM ..." or another loading error | The file is not the iPad game's program. Check its SHA-1 (step 1 above): only that one build works |
| "SDL3.dll was not found" | Keep `SDL3.dll` in the same folder as `gtracing.exe` |
| "Windows protected your PC" (SmartScreen) or an antivirus warning | The exe is not digitally signed, and a new program with few downloads gets this warning. Click **More info**, then **Run anyway**. The source is all here, so you can build the exe yourself, and the SHA-256 lets you check the download |
| No Display page, keys page, Exit or Name tile | The PC menus are not installed: `patch\` is empty. The same settings are in `gtracing.ini` |
| A new resolution or aspect ratio does not show | They apply after a restart: use the Display page's Restart to Apply button, or start the game again |
| Other players are not found in Local or Bluetooth | Both PCs must be on the same network or the same VPN. If Windows Firewall asks, allow `gtracing.exe` on private networks. `GT_LAN_PEERS` can name the other PC's address |
| An Online room cannot be joined | The host's router must open the room's UDP port (`GT_P2P_PORT`, 28200) through UPnP or NAT-PMP. Without them, forward that port by hand or play Local over a VPN |
| The Red Bull leaderboard shows only local times | The board's server is not reached: check the internet connection, and that `GT_CLOUD_BOARD=0` is not set |
| A black screen, a crash or an error box | Report it (see below) with `gtracing.log` |

## Known Issues

1. **Few GPUs tested:** an NVIDIA RTX 3050 Ti and the AMD Radeon graphics built into a laptop's processor, on
   Windows 11. Nothing has been run on Intel graphics or on an AMD graphics card.
2. **Multiplayer** (Local, Bluetooth and Online) has been tested with two copies of the game on one PC. Opening the
   Online port on a real router (UPnP, NAT-PMP) has not been tested. Reports from play between two PCs are welcome.
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

- your Windows version, and your graphics card and driver;
- the port's version, and which download you use (release or built from source);
- what happened, and the steps that lead to it;
- `gtracing.log`, attached as a file.

After a crash there is also `gtracing.dmp`. Attach it only if you are happy with what the [Privacy](#privacy)
section says about it.

## Privacy

`gtracing.log` and `gtracing.dmp` stay on your computer until you choose to send them. The names of your folders,
your Windows user name, your PC's name and your public internet address are written in them as tags (`%USERPROFILE%`,
`%EXE%`, `%USER%`, `%PC%`, `%IP%` ...) or blanked. `GT_LOG=0` turns the log off: no file is made and nothing is
written to the console. A crash still writes the dump (`GT_CRASH_DUMP=0` turns that off).

The network is used only for multiplayer and for the Red Bull leaderboard server. The server receives a random
player number, the name you go by, the car and the lap time; never your Windows user name, your PC's name or your
folders. `GT_CLOUD_BOARD=0` keeps the board offline.

In detail:

- **The exe**: every compile maps the project folder to `.` (`-ffile-prefix-map`), so no build-machine path is in
  the code, the string tables or the debug info. The release build has no diagnostic keys or trace strings.
- **The log** (`host/core/scrub.c`): before a line is written, the save's folder, the game's folder, the exe's
  folder and `%USERPROFILE%` are replaced by tags (`%HOME%`, `%APP%`, `%EXE%`, `%USERPROFILE%`), longest first, in
  the ANSI code page and UTF-8, either slash and any case; the Windows user name and PC name become `%USER%` and
  `%PC%` as whole words, and public IPv4 addresses `%IP%`. The formatting buffer is zeroed after each line. The
  game's own console output (`printf`, `puts`, `fwrite` and `write` to the console, `NSLog` ...) goes through the
  same path. `GT_LOG=0` (in the environment or in `gtracing.ini`, which is read before the log is opened) turns the
  log off: nothing is formatted, no file is made, nothing reaches the console.
- **The crash dump** (`host/core/crashdump.c`): `MiniDumpNormal | MiniDumpFilterModulePaths` (thread stacks and
  registers, module list without paths; no heap). Before the file is kept, every copy of those folders and of every
  environment value (8-bit and UTF-16LE, longest first) is overwritten with `_` of the same length, so the dump's
  offsets stay valid; path fragments in stack memory are blanked too. A dump that cannot be cleaned is deleted.
- **What the game sees**: no environment variables (`getenv` returns nothing), the host name `iPad`, a fixed device
  identifier, and `GT Racing PC` as its GameKit name.
- **The network**: used only for multiplayer (local network; Online asks the local router through UPnP or
  NAT-PMP, with no outside server) and the Red Bull leaderboard server (`GT_CLOUD_BOARD_URL`, or off with
  `GT_CLOUD_BOARD=0`). The server receives a random player number, the chosen player name, the car and the lap
  time, with the user agent `GTRacingPC-board/1`.
- **Checks**: `tools\privacy_scan.py` searches files (8-bit and UTF-16LE, any case; a zip's files too) for the
  Windows user name, the PC name and host name, the name and e-mail address Git commits with, the user folder,
  `C:\Users` and the project folder (read when it runs, never written down). `tools\playtest.ps1` runs it on both
  exes (unstripped), on a minidump and log from a crash forced in a test run, and with `--release` on what a release
  is made of (`tools\release\`: `README.txt`, `IMPORTANT.TXT`, the `gtracing.ini` template; `build\release\`
  without its symbols and private folders). `tools\package_release.py` runs it on every release file and on the
  log of the release exe's check run.

## Features in detail

### Picture

- Any resolution up to the monitor's own, with the iPad 3's Retina art (or the iPad 2's).
- Wide screens (16:10, 16:9, 21:9): the 3D view fills the screen. The menus and the race HUD keep their shape,
  their backgrounds reach across the screen, and the HUD sits at the screen's edges as it does on the iPad.
- Full screen or a window (Alt+Enter); the choice is saved.
- MSAA anti-aliasing (2x, 4x, 8x), anisotropic filtering (up to 16x) and the iPad 2's full motion blur in the
  iPad 3 mode.
- A GPU profile: picture settings left on "auto" pick lighter values on integrated GPUs.
- PostFX: the free iPad game shipped without any bloom. Options > Display > PostFX can switch
  on a bloom drawn the way the paid 2010 iPad version draws it, with each track's own glow and colour settings
  (Classic), or the same glow with the world's own colours kept (Classic Natural), in races and behind the
  menus' 3D scene. Enhanced adds the game's own unused specular-highlight bloom pass, with its shaders' colour and
  brightness faults evened out, the cars' highlights a little stronger and sunlit asphalt and grass kept from
  turning flat white, under a Classic Natural glow blended so bright areas never clip; the menus' 3D scene gets
  its own settings so the cars there look as they do on the track. Enhanced is the default; Off (`GT_POSTFX=0`)
  draws the picture as the game shipped. Every PostFX choice applies at once. The game's own pass as shipped stays
  reachable from `gtracing.ini`.
- An in-game **Display** page (Options > Display) for all of the above. It replaces the Customer Care tile, which
  needed Gameloft's servers.
- The 3D scene with a car behind the main menu, as in older versions of the game.

### Controls

- Keyboard, mouse and gamepads (Xbox and PlayStation layouts through SDL 3).
- The Touch Screen switch in Options > Controls ([above](#controls)).
- Menus driven by the keyboard (W A S D or the arrows, Enter, Esc) or the pad: the pointer snaps to the menu's
  buttons and glides between them.
- A gold mouse pointer in the game's style, sized for the display.
- Look back, a 360-degree look around the car (right stick or numpad), and a smoother, smaller camera tilt.
- In the replay: mouse-wheel zoom, and pad and numpad control of the camera, zoom and time slider. With Touch
  Screen On, the pad's pointer also stops on the pinch and swipe hints at the top: Cross zooms in a step (and back
  out after the closest) or turns the camera by 90 degrees.

### Game

- An Exit button on the main menu.
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

- **Local** and **Bluetooth** play over the local network or a VPN (Tailscale, Radmin VPN and the like).
- **Online** play through room codes (`GT-XXXXX-XXXXX`): one player hosts, and the port is opened on the
  router with UPnP or NAT-PMP. Gameloft's lobby server is gone, so this runs peer to peer.

### Fixes

- **Original-game faults** fixed:
  - the dimmed replay picture;
  - a medal counter that counted one place too many;
  - the off-centre placement of the menus' 3D pictures, which shows on wide screens.
- **Port issues** fixed: stale GL bindings across threads, the run-loop wait, `sched_yield`, the screenshot
  button, Windows' volume pop-ups hidden in full screen, a new profile's name (made as 8-bit text where the game
  asks for UTF-16), and others. The comments at each fix explain it.
- **Performance work**: native gameswf search and a miss cache, GL state shadowing and streaming vertex buffers,
  SSE2 fixed-point conversion and index scans, threaded PVRTC decoding, a precise frame timer, and throttling while
  the window is in the background. Measured results are in [Performance notes](#performance-notes).
- **Crash handling**: a minidump and a message box. The log and the dump are scrubbed of personal paths and names.
- **Settings**: every number in `gtracing.ini` is checked when read (see [Settings](#settings)).

## Build Instructions (For Developers)

Windows only (the host is a 32-bit Windows program). You need:

- **the game**: put `GTRacingFreemiumHD.app` (iPad, version 1.3.4b as its `Version.txt` says; its Info.plist's
  `CFBundleVersion` reads 1.3.3) in the repository's root folder. It is read, never modified;
- **Python** 3.12, with `capstone` 5 (and `unicorn` 2.1.3 for the tests);
- **clang** from [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) on `PATH`, for the generated game code;
- **MSYS2's mingw32 gcc** in `C:\msys64\mingw32`, for the host and the link.

Clone this repository and put your `GTRacingFreemiumHD.app` in its folder. Then:

```
cd GTRacingMotorAcademyRecomp

# stage 1, once per binary: every function of the game, into analysis\stage1
python -I recomp\discover.py GTRacingFreemiumHD.app\GTRacingFreemiumHD analysis\stage1

# the generated C, into build\gen, compiled with clang (later builds can skip this with --no-gencc)
python -I host\build.py --gen

# build\host\release\gtracing.exe, with SDL3.dll next to it
python -I host\build.py --release --no-gencc

build\host\release\gtracing.exe --app "%CD%\GTRacingFreemiumHD.app"
```

There are two builds of the host, from the same sources:

| Build | Command | Output | `GT_DEV` |
|---|---|---|---|
| Development | `python -I host\build.py` | `build\host\gtracing.exe` | `1`: the test switches, traces and diagnostic logs below are compiled in |
| Release | `python -I host\build.py --release` | `build\host\release\gtracing.exe` | `0`: diagnostic keys are not read (`dev_getenv` is a constant `NULL`) and diagnostic log lines compile to `if (0)`, so neither their names nor their format strings are in the exe |

`tools\playtest.ps1` runs the development build; `tools\package_release.py` packages the release build.

The build also:

- patches the game's menu SWFs into `build\host\patch\` (the PC menus; copy them into a release's `patch\` folder
  for your own use);
- makes the icon from the game's artwork;
- copies `SDL3.dll` next to `build\host\gtracing.exe`.

A clean build of either kind has no compiler warnings (`-Wall -Wextra`). Every compile (host C, assembly,
`runtime/` and the generated code) uses `-ffile-prefix-map=<project folder>=.`, so no build-machine path is
embedded in the exe or its debug info.

### Android

An Android build (`armeabi-v7a`, a phone that runs 32-bit apps) is made from the same sources: the Win32 calls
have POSIX meanings there (`host/core/plat.h`), and the game's OpenGL ES calls go to the phone's own OpenGL ES.
How to build it, install it and copy the game to the phone: [android/README.md](android/README.md).

### Running a build

```
build\host\gtracing.exe [--app <path to GTRacingFreemiumHD.app>] [--trace] [--selftest]
```

The game folder is found next to the exe, or in the folders above it. `--app` names it explicitly. A
`GTRacingFreemiumHD.app` counts only when the game's program (`GTRacingFreemiumHD`) is in it, so the release's
empty placeholder folder is skipped. A copy one level too deep (`GTRacingFreemiumHD.app\GTRacingFreemiumHD.app`)
is found as well.

- **Save**: `home\` next to the exe (`GT_HOME` changes it).
- **Log**: `gtracing.log` next to the exe (`GT_LOG=<file>` changes it; `GT_LOG=0` writes no log at all, neither
  the file nor the console).
- **After a crash**: `gtracing.dmp`, next to the log.
- **Other options**:
  - `--selftest` runs the host's self-checks instead of the game;
  - `--trace` (or `GT_TRACE=1`, development build only) logs every Objective-C message and static initializer.

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
   - The exe is based at 0x10000, and its first section reserves the game's address range (`gt.ld`,
     `reserve.S`), so the game image is loaded at its own addresses.
   - The loader maps the segments and binds every import. Calls into imports go through ARM-ABI bridges
     generated from `imports.sig` (`recomp/bridgegen.py`).
   - Indirect calls go through a direct-mapped dispatch table.
4. **The iOS libraries** (`host/shims/`):
   - Objective-C 2, libc, libstdc++, pthreads and unwinding.
   - Foundation and CoreFoundation, property lists, CoreGraphics, UIKit, OpenAL and GameKit.
   - The network.
   - OpenGL ES 2 on desktop OpenGL (`host/shims/gl/`).
   - SDL 3 provides the window, input and audio output.
5. **The PC port** (`host/games/gtracing/`): the features and fixes above.
   - Most hook the game by wrapping functions at link time (GNU ld `--wrap`), or by replacing single
     instructions at code generation time (`HOST_INSNS` in `recomp/codegen.py`).
   - Menu additions are made by patching the game's own SWF menu files at build time (`tools/swfpatch_*.py`).
     The patched copies stay in the build folder.

### Repository layout

| Path | What |
|---|---|
| `recomp/macho.py` | Mach-O reader: segments, sections, symbols, function starts, stubs |
| `recomp/discover.py` | stage 1: function discovery |
| `recomp/disasm.py` | disassembly around an address, in the right ARM/Thumb mode |
| `recomp/census.py` | instruction counts by kind |
| `recomp/armc.py`, `recomp/codegen.py` | stage 2: the ARM/Thumb/VFP/NEON-to-C translator and the generator for `build/gen/` |
| `recomp/compile_all.py` | compiles `build/gen/` in parallel (x86_64 or i686) |
| `recomp/bridgegen.py` | the import bridges, from `host/core/imports.sig` |
| `runtime/` | `arm_rt.h` (CPU state, memory macros and helpers for the generated C), `rt_jump.c` and `rt_selftest.c` (guest-style functions for `--selftest`) |
| `tests/difftest.py`, `tests/functest.py`, `tests/tmem.c` | instruction and whole-function tests against Unicorn |
| `host/build.py` | builds `build/host/gtracing.exe` (development) or `build/host/release/gtracing.exe` (`--release`) |
| `host/core/` | the loader, the runtime (threads, calls between host and game, traps, log), `gtracing.ini` and the settings (`settings.c`), crash dumps, `--selftest`, `gt.ld`, `imports.sig` |
| `host/shims/` | the iOS libraries the game imports; `gl/` holds OpenGL ES and EAGL. They know nothing of the game: every call they make into it is declared in `game_hooks.h`, and shader patches are registered through `gl.h` |
| `host/games/gtracing/` | `layout.h` (the address-space layout); `game.h` (the game's globals, functions, structure offsets and vtable slots that several features use, and `gt_hook_slot`, the checked vtable hook in `features/hook.c`); `features/` (port additions and fixes, with `features.h` and `pcinput.c`, the PC's keyboard, mouse and gamepad input); `render/` (picture additions) |
| `host/third_party/SDL3/` | SDL 3 for i686 |
| `tools/swfpatch_*.py` | build-time patches of the game's menu SWFs |
| `tools/*_addon.py` | add-on builders ([below](#add-ons)) |
| `tools/make_icon.py` | the exe's icon, made from the game's `iTunesArtwork` at build time |
| `tools/package_release.py`, `tools/release/` | the release folder, with its `gtracing.ini` and `README.txt` templates |
| `tools/addr2sym.py`, `tools/fdis.py` | debugging helpers: host addresses to symbols (`--map` for a release build's map), guest function disassembly |
| `tools/playtest.ps1` | the regression run: `--selftest`, scripted game runs on a copy of a save with their logs checked, and the privacy check |
| `tools/privacy_scan.py` | the privacy scan of exes, logs, dumps and release folders |
| `tools/layer_check.py` | the layer boundary in the built objects: the shims reach the game only through `game_hooks.h` (`--unused`: external symbols no other object uses) |
| `cloud/redbull-board/` | the optional online leaderboard server (Cloudflare Worker + D1) |

Files include each other by bare name. `host/core/` and `host/shims/` have only their own folders and
`host/games/gtracing/` (for `layout.h`) on the include path, so a shim cannot include a game feature's header; the
game layer sees every folder. Two folders must not hold the same file name, because object files are named after
their source; `build.py` stops if they do.

`.gitignore` keeps out the game, anything taken from it (other editions, analysis dumps, screenshots) and the build
output.

### Testing and diagnostics keys

These keys are meant for development, and they are documented next to the code that reads them. Only the
development build reads them; the release build ignores them. They include:

- **Scripted input**: `GT_KEYNAV`, `GT_PADNAV`, `GT_TAPS`, `GT_PEDALS`, `GT_PAUSE`, `GT_CAM`, `GT_WHEEL`,
  `GT_TEST_MENU`, `GT_TEST_MINIMIZE`, `GT_TEST_AUTOPILOT` (the player's car driven by its own AI driver),
  `GT_TEST_DISPLAY` (PostFX and Motion Blur changes at given frames).
- **Run control**: `GT_QUIT_AT`, `GT_SHOTS` (up to 64 frames).
- **PostFX tuning**: `GT_POSTFX_BALANCE_TUNE`, `GT_POSTFX_ENHANCED_GLOW`, `GT_POSTFX_ENHANCED_TONE`, `GT_POSTFX_ENHANCED_MENU`.
- **Measurement and logs**: `GT_BENCH`, `GT_SPIKE_MS` (logs each frame slower than that many ms), `GT_GL_STATS`,
  `GT_SWF_FIND_STATS`, the `*_LOG` keys, `GT_TRACE` and `GT_GLDEBUG`.
- **Crash test**: `GT_CRASH_TEST=<frame>` (a host write to an unmapped address on that frame).

### Tests

- `gtracing.exe --selftest` checks the host without starting the game. The checks cover:
  - Foundation and the runtime;
  - the fast paths against their plain versions, byte for byte;
  - the leaderboard's merge and names;
  - the parsing of numeric settings (malformed, out of range, NaN);
  - the input code paths;
  - the guest `setjmp` / `longjmp` and C++ / Objective-C exceptions (throw, catch by base class, `catch (...)`, a
    cleanup on the way), which the menus and a race never reach.
- `tools\playtest.ps1` runs `--selftest` and `tools\layer_check.py`, then the main menu, Options > Display, a
  Simple Race on the U.S. Speed Ring with PostFX and motion blur switched while it runs (its blur shader patch
  included), the Garage with the add-on cars, a Simple Race left through the pause menu,
  and a Test Center race with its replay. Every input comes from the
  game's test switches, on a copy of the save in `%TEMP%` (removed after), with the online leaderboard off. A run
  fails on a fault, a crash, a GL shader error, a private name in the log or a missing log line. The results
  are in `build\playtest\`. `-List` names the scenarios and `-Scenario` picks some. The run ends with the privacy
  check described [above](#privacy) (`-NoPrivacy` skips it).
- `tests/difftest.py` checks every instruction form the translator emits, on random states, against Unicorn.
- `tests/functest.py` checks whole leaf functions against Unicorn.

```
python -I tests\difftest.py GTRacingFreemiumHD.app\GTRacingFreemiumHD analysis\stage1\functions.json build\difftest
python -I tests\functest.py GTRacingFreemiumHD.app\GTRacingFreemiumHD analysis\stage1\functions.json build\functest
```

### Release package

`python -I tools\package_release.py --version <V> --zip` makes `build\release\GTRacing-PC-Port-<V>-Windows-x86\`
and its zip from the release build (`host\build.py --release`). The script:

- strips the exe and checks that its loaded sections still match the build;
- copies only the exe, `SDL3.dll`, the release `gtracing.ini`, `README.txt` and `IMPORTANT.TXT`;
- adds the folders `GTRacingFreemiumHD.app\`, `patch\` and `add-on\`, empty but for a note on what goes in
  each (`tools\release\folders`);
- scans every file for personal names and paths (`tools\privacy_scan.py`);
- runs `--selftest` on the stripped exe;
- with `--zip`, writes the zip and compares it with the folder.

The release holds no game files. Players add their own `GTRacingFreemiumHD.app`.

### Performance notes

Each entry: the symptom, its cause, the fix, and the measurement (uncapped frame rates unless noted, two runs
each on a scratch save copy). The full analysis is in the comment above each fix.

| Fix (key) | Symptom and cause | Fix | Measured |
|---|---|---|---|
| `PRECISE_RUNLOOP`, `PRECISE_WAIT` | The render thread's `CFRunLoopRunInMode(..., 0.001)` became `Sleep(whole ms + 1)`, 2 to 2.5 ms on Windows' 1 ms tick; the frame cap spun the rest of each wait | a high-resolution waitable timer | 234 / 235 fps (menus / race) to 317 / 315 fps; the ~11 % of the main thread spent spinning at 60 fps is gone |
| `SCHED_YIELD` | `sched_yield` returned at once, so the game's wait-for-thread loops spun (96,000-115,000 calls a frame at start) | `SwitchToThread`, as iOS yields the time slice | about 30 times fewer spins; race loading time unchanged (11.02 / 11.06 s) |
| `FAST_SWF_FIND`, `SWF_MISS_CACHE` | gameswf's find-by-name walk (0x50a654) was 20-30 % of the main thread in the menus; missed paths walked the whole tree each frame | the walk in host code, plus a cache of misses | main menu 403 / 406 to 462 / 463 fps; characters visited per frame ~38,000 to ~1,700 |
| Optimisation pass 4 (`GL_QUERY_FREE`, `READABLE_STABLE`, ...) | state queries forced the threaded driver to sync (0.04-0.17 ms a frame in present, 0.20 ms in the side bands) | state kept per context instead of queried | main menu 332 / 369 to 403 / 392 fps |
| Optimisation pass 6 (`LIVE_BLOCKS`, `FIXED_FAST`, `INDEX_FAST`, `GL_ARRAY_CACHE`, `PVRTC_FAST`) | per-draw `VirtualQuery`, scalar fixed-point and index loops, single-threaded PVRTC decoding | heap block table, SSE2 loops, decoding in bands on a thread pool (byte-exact, checked by `--selftest`) | race 334 / 332 to 362 / 367 fps (AMD), 303 / 300 to 330 / 318 fps (NVIDIA) |

### Add-ons

Add-ons are folders in `add-on\` next to the exe. Their files are read as if they were in the game's bundle. Each
one is built by a tool from a game edition you own, and none is part of this repository.

| Tool | Builds |
|---|---|
| `tools/hyundai_addon.py` | the Hyundai Genesis Coupe and i-oniq, from GT Racing: Hyundai Edition (Android OBB) |
| `tools/android_free_addon.py` | the Dodge Charger 2012 and Ferrari 288 GTO, from Android GT Racing Free+ 1.4.3 |
| `tools/premium_addon.py` | the paid iPad edition's three extra licence tests and its 44 kHz audio |

Run each with `python -I tools\<tool>.py`; its docstring lists the arguments.

### Online leaderboard server

`cloud/redbull-board/` is a small Cloudflare Worker with one D1 table, which shares the Red Bull Time Trial's
best laps between players. The game uses the project's server by default; `GT_CLOUD_BOARD_URL` picks another,
and with `GT_CLOUD_BOARD=0` the board stays on the PC and with the friends met in multiplayer.

- **What it receives**: a random player number, the name the player goes by, the car and the lap time.
- **Validation**: names and lap times are checked.
- **Player ids**: they are never shown; the board shows a hash alias instead.

See its [README](cloud/redbull-board/README.md) for deployment.

## Contributing

Help is welcome, and the most useful help right now needs no code at all:

- **Play it on hardware that has not been tried:** Intel graphics, an AMD graphics card, an older PC.
- **Play multiplayer between two PCs**, on one network or over a VPN, and Online through room codes.

Report what you find on the [Issues](https://github.com/Adriansyah101/GTRacingMotorAcademyRecomp/issues) page,
with the log, even when everything works.

For code:

- **Comments**: write them in plain, technical English. Explain what the code does and why: the original
  game's behaviour (with addresses), whether a fix is for an original-game fault or a port fault, and how it
  was verified.
- **What comments leave out**: conversation transcripts, quoted requests, personal names and dates of work
  sessions.
- **New features**: they are on by default, with a `GT_*` key to switch them off.
- **Before a pull request**: build with no warnings, run `--selftest` and `tools\layer_check.py`.
- **Game files**: never commit any, including extracted, patched or converted ones.

## Credits

- **[Adriansyah101](https://github.com/Adriansyah101)**, for the port.
- **Gameloft**, for the game.
- **[SDL](https://www.libsdl.org/)**, for the window, input, sound and gamepads.
- **[llvm-mingw](https://github.com/mstorsjo/llvm-mingw)**, for the clang that compiles the generated game code.
- **[MSYS2](https://www.msys2.org/)** and **[mingw-w64](https://www.mingw-w64.org/)**, for the Windows toolchain.
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
