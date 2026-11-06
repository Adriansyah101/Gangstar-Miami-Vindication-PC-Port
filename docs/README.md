# GangstarMV PC Port

Runs the macOS i386 build of **Gangstar: Miami Vindication** (Gameloft, 2011) on Windows. There is no
CPU emulation, because the game is already x86. A custom Mach-O loader runs the game at its own
addresses, and host shims supply the macOS libraries it imports. It is a compatibility project in
the spirit of Wine or Darling, for playing a copy you own on your own machine.

The game is **not included**. You need your own copy of the Mac `GangstarMV.app`. The port has been
run with one build of it only: `gmv_patch.bin` is tied to that binary's size and `__TEXT` checksum, and
the loader refuses any other.

## Status

Version **V1.0.1** (2026-11-06). Tested on Windows 11 with an NVIDIA RTX 3050 Ti.
[`CHANGELOG.md`](../CHANGELOG.md) lists everything the port does, by part: the loader, the macOS wrapper and the game layer.

| Area | State |
|---|---|
| Loading | 4360 low-range patches applied and verified, 451 static initializers run, 2660-file asset pack loads |
| Imports | 477 of 528 implemented. An audit found no way to reach the other 51 in this build ([details](NOTES.md#the-51-unfilled-imports)) |
| Game | Menus, on foot, driving, flying, cutscenes and races; played to the end on V1.0.0 |
| Graphics | OpenGL with the Mac's 4x MSAA in an SDL3 window, window or full screen, any resolution the monitor lists |
| Input | Keyboard (with the game's own keys on AZERTY), mouse, Xbox, PlayStation and other pads, all through SDL3 |
| Sound | The game's own mixer through SDL3, which follows the default output device; the intro movie decoded by Media Foundation |
| Saving and quitting | The game's own saves, kept next to the exe; clean exit |
| Online | Offline only. Gameloft's services are long gone, so their menu buttons report no connection |

### Not verified yet

- **Other hardware:** nothing has been run on an AMD or Intel GPU. The enhancements use their own
  shaders, so those are the most likely to differ.
- **Linux on a real GPU:** the Linux build has run in WSL2 only, with Mesa's software renderer.

Confirmed on 2026-11-05: the game played to the end on V1.0.0, the engine sound, the unsmoothed glow of
`SUN_MOON=2`, the custom station's HUD logo, and that logo being cleared when `ORIGINAL_MAC_EXPERIENCE` is
switched on.

## Getting started

1. Build it (below) or take a release folder: `GangstarMV.exe` plus `gmv_patch.bin` and `SDL3.dll`, which
   must stay next to the exe.
2. Put your copy of the game in one of these places, searched in this order:
   - next to the exe, so that `GangstarMV.app\Contents\MacOS\GangstarMV` exists there;
   - one folder up, so a build in `build\` finds the project's copy;
   - anywhere, with `GangstarMV.exe --app "D:\path\to\GangstarMV.app"`, which `build\release` and
     `build\dev` need unless the game is next to the exe.
3. Double-click the exe.

These folders are created next to the exe on the first run:

| Folder | Holds |
|---|---|
| `savegame\` | Your save (`Gangstar2.sav`), the game's settings and the port's `GangstarMV.ini` |
| `cache\` | The game's scratch files, `GangstarMV.log`, the file to send with a bug report, and `GangstarMV.dmp` after a crash |

### Privacy

- The port has no network code and sends nothing anywhere: its exe links no network library. The
  game's online buttons only report that there is no connection.
- `cache\GangstarMV.log` stays on your computer until you choose to send it. `LOG=0` turns it off and
  deletes the old one.
- The log writes your Windows user folder, the exe's folder and the game's folder as `%USERPROFILE%`,
  `%EXE%` and `%APP%`, and a release log does not name your songs. It does record your settings, your graphics
  card and driver, your display modes and the names of your game controllers, because a bug report needs
  them.
- After a crash, `cache\GangstarMV.dmp` holds what the port's threads were doing: registers, stacks and the
  list of loaded modules. It stays on your computer until you choose to send it, and the next start deletes
  it. The three folders above and the values of your environment variables are blanked in it, but it is raw
  memory you cannot read through first, so it can hold other scraps of what the game was doing, such as the
  file name of a song. `LOG=0` turns it off too.
- A release exe holds no path or name from the machine that built it.

Keys added by the port: **Alt+Enter** switches between full screen and a window, and **Alt+F4** quits
like closing the Mac window, so progress since the last save is lost.

## Settings

Everything lives in `savegame\GangstarMV.ini`, which is created with the defaults and a description
of each key. Deleting it restores the defaults. The defaults turn the enhancements on; a key
left blank, or missing from the file, takes its built-in value, which for the enhancements is off, as on
the Mac.

- **`GAMEPATCH_TAB`** (on by default) adds a GAMEPATCH tab to the game's own options menu, where most
  settings can be changed in game. `GAMEPATCH_TAB=0` removes it.
- **`ORIGINAL_MAC_EXPERIENCE=1`** plays the game as the Mac did, whatever the other keys say. It
  leaves the fixes the port needs and the loading speed-ups on, and turns everything else off, the fixes of
  the original game's own bugs included, as the table shows. The GAMEPATCH
  tab stays, holding only that switch. If you have added the Crusher or radio add-on, its files are still
  read at the start, unused, so that it can come back without a restart. Also left in, with no key: the
  speed-ups that give the same results (collision, physics step, HUD bars, billboards), and the hidden
  MISSION REPLAY tab's hooks in the SERVICES menu, which is built once at the start.

| Group | Keys | Default | Mac preset |
|---|---|---|---|
| Port fixes | `TEXFIX` `SWAP_COPY` `NO_FLICKER` `VTXFIX` `CRASH58_GUARD` `STOPLOCK_FIX` `FAST_FTELL` `FAST_FSEEK` `FAST_MEMMOVE` `STATIC_VBO` `DPI_AWARE` | on | on |
| The Mac's anti-aliasing | `MSAA` | 4 | 4 |
| Loading speed-ups (same picture and rules, fewer stalls) | `LOAD_SLEEP_SKIP` `LOAD_BUDGET` (200) `FAST_LOAD` | on | on |
| Original-game fixes | `MENUFLASH_FIX` `FADE_EDGE_FIX` `MENU_FADE_CLICKS` `RADIO_LOGO_FIX` `SWITCH_SAVE_FIX` | on | off |
| Frame rate | `FRAME_MS` | 60 fps (`8.3333`) | 30 fps, as on the Mac |
| Vsync | `SWAP_INTERVAL` | off | off |
| One physics step per tick | `PHYSICS_SYNC` | on | off |
| High frame rates, no effect at 60 ticks/s | `CAMERA_SYNC` `WALK_SYNC` `AI_SYNC` | on | off |
| Graphics | `ANISOTROPIC_FILTERING` (16) `AMBIENT_OCCLUSION` `BLOOM` (2, with `BLOOM_THRESHOLD` 0.5 and `BLOOM_WINDOWS` 2) `WATER_REFLECTIONS` `VEHICLE_REFLECTIONS` `DYNAMIC_SHADOWS` `SUN_MOON` (1, enhanced) `SPAWN_FADE` `FOG_MODE` (2, hybrid) | on | off |
| Draw distance and crowds | `DRAW_DISTANCE` (2) `TRAFFIC_DENSITY` `PEDESTRIAN_DENSITY` `PARKED_CAR_DENSITY` (3 each) | as given | 1 |
| Gameplay | `ALLOW_WEAPON_SPRINT` `MINIMAP_SHOPS` `MISSION_REPLAY` `PLAYERSWITCH_INVENTORY` `LC_SWITCH` | on | off |
| Add-ons (each does nothing until its files are there) | `CUSTOM_RADIO` (your own music, at `CUSTOM_RADIO_VOLUME` 0.53) `UC_CRUSHER` (made from your own copy of Urban Crime) `RADIO_SONGS` (made from your own copy of the iPhone version) | on | off |
| Controls | `KEY_MOVE` `PAD_LAYOUT` `PAD_MENU` `PAD_HINTS` `PAD_ICONS` | on | off |
| Mouse | `SHOW_CURSOR` | off | off |
| Menus and cutscenes | `SHORT_WARNING` `CUTSCENE_KEYS` `TUTORIAL_KEYS` `HIDE_MORE_GAMES` `HIDE_GAMELOFT_LIVE` | on | off |
| Intro movie | `NO_INTRO` | off (the intro plays) | off |

[`FIXES.txt`](../FIXES.txt) explains most of these in plain words, and the ini describes every key.

### How far each option has been tested

| Record | Options |
|---|---|
| Confirmed in game | `ANISOTROPIC_FILTERING` `AMBIENT_OCCLUSION` `BLOOM` `FOG_MODE` `DYNAMIC_SHADOWS` `SUN_MOON` `SPAWN_FADE` `DRAW_DISTANCE` the three density keys `ALLOW_WEAPON_SPRINT` `MINIMAP_SHOPS` `MISSION_REPLAY` `SWITCH_SAVE_FIX` `PLAYERSWITCH_INVENTORY` `LC_SWITCH` `CUSTOM_RADIO` `UC_CRUSHER` `RADIO_SONGS` `KEY_MOVE` `PAD_LAYOUT` `PAD_MENU` `SHORT_WARNING` `CUTSCENE_KEYS` `TUTORIAL_KEYS` `HIDE_MORE_GAMES` `HIDE_GAMELOFT_LIVE` |
| Seen working in a log | `FAST_LOAD` `WATER_REFLECTIONS` `VEHICLE_REFLECTIONS` `PAD_HINTS` `PAD_ICONS` (with a DualSense) |

## Building

You need MSYS2's 32-bit toolchain at `C:\msys64\mingw32\bin` with CMake and Ninja
(`pacman -S mingw-w64-i686-gcc mingw-w64-i686-cmake mingw-w64-i686-ninja`) and Python with `capstone`. The
WindowsApps `python` is a stub, so give the full path. The build is `host\CMakeLists.txt`; `host\build.ps1`
calls it with the arguments below (`docs\NOTES.md` has the CMake presets and the Linux build). From the
project root:

```powershell
# SDL3, once: the official 32-bit build, one pinned version, into host\third_party
tools\get_sdl3.ps1
# the low-range patch table, once per binary
& "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe" -I tools\genlow.py `
    "GangstarMV.app\Contents\MacOS\GangstarMV" build\gmv_patch.bin
host\build.ps1            # build\GangstarMV.exe
```

| Build | Output | Use |
|---|---|---|
| `host\build.ps1` | `build\` | Daily work: console, every diagnostic, `-g`. It holds local paths, so never share it |
| `host\build.ps1 -Dev` | `build\dev\` | For development: console and every diagnostic, symbols stripped. Its log is much longer and names your songs |
| `host\build.ps1 -Release` | `build\release\` | Public build: see below |
| `-NoIcon` | | Leaves out `host\games\gangstar\app.ico` even if it is there. The icon is the game's art, so it is not in the source |

A release build has no debug mode. The code of the traces, surveys, captures, profilers and test aids
is not compiled in, so no setting can bring it back: there is no console, no `[Diagnostics]` or `[Testing]`
key, no staged command-line flag, no debug hotkey and no `GMV_*` environment override. Its log holds only
the version, the settings, what was found about the hardware, what a feature applied, skipped or failed
at, and the details of a crash. Debugging needs a `-Dev` or plain build made from the source.

## Project layout

```
Gangstar-Miami-Vindication-PC-Port/
  GangstarMV.app/     the game (your copy): input only, never modified, never committed
  host/core/          the loader, which knows no game
  host/shims/         the macOS libraries, which know no game
  host/games/gangstar/    layout.h, the patches and the enhancements of this game
  host/games/template/    a game with no patches, for starting another
  host/vendor/        the custom radio's decoders (dr_libs, stb_vorbis), single files kept as they are
  host/third_party/   SDL3, fetched by tools\get_sdl3.ps1: local only
  tools/              genlow.py (the patch table), Mach-O and ObjC analysis, add-on packers
  docs/NOTES.md       the technical notes
  docs/folder-notes/  the notes put in a build's add-on and custom radio folders
  FIXES.txt           every fix and option in plain words
  CHANGELOG.md        everything the port does, by part
  CHANGELOG_old.md    the older record, by version
  analysis/           reports derived from the game: local only
  screenshots/        milestone captures of the game's art: local only
  extracted_audio/    samples from the game's packs: local only
  build/              build output, savegame\, cache\: local only
```

`.gitignore` keeps out the game, anything taken from it and the build output. SDL3 is under the zlib licence, in
`host\third_party\SDL3\LICENSE.txt` once fetched. dr_mp3, dr_flac, dr_wav and stb_vorbis in `host\vendor` are public
domain or MIT-0/MIT, each with its licence at its end.

## Technical notes

[`docs/NOTES.md`](NOTES.md) is the long form:

- how the loader, Objective-C runtime and shims work;
- every graphics setting in depth;
- the source layout, how to add another game, and the ABI notes;
- the numbered log of each fix and enhancement, with its evidence;
- the diagnostics;
- how the game was brought up.
