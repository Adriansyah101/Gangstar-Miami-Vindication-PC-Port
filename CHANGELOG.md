# Changelog

Everything the port does, by the part of it that does it. The game itself is not included and is never changed on disk:
every item below happens in memory, at run time.

The port has three parts:

- **The loader** (`host/core`) loads the Mac game and runs it. It knows nothing about any particular game.
- **The macOS wrapper** (`host/shims`) stands in for the macOS libraries the game expects. It knows nothing about any
  particular game either.
- **The game layer** (`host/games/gangstar`) holds the fixes, enhancements and add-ons for Gangstar: Miami Vindication.

Around them are the build and the tools. Every setting named here is a key in `savegame\GangstarMV.ini`.
[`FIXES.txt`](FIXES.txt) explains each one in plain words, and [`docs/NOTES.md`](docs/NOTES.md) has the technical
detail. The older record, by version, is in [`CHANGELOG_old.md`](CHANGELOG_old.md).

## The loader

- **Runs the Mac game as it is.** There is no CPU emulation, since the game is already x86 code. The game's i386 Mach-O
  binary is loaded at its own addresses, which the program reserves before anything else can take them.
- **The low range.** The game's first code page overlaps the exe's own header, so that code runs from a copy. Every jump
  into and out of it is patched (4360 patches), and each patch is checked byte for byte at load. The patch table comes from
  `tools/genlow.py` and works only with the binary it was made from.
- **Every import bound.** Each of the game's 4243 import slots (528 distinct imports) is filled. A slot goes to a shim
  where one exists. A missing function becomes a stub that reports its name, and missing data becomes a stand-in that
  faults on use, so a gap shows up in the log instead of corrupting memory.
- **Start-up.** The game's 451 static initializers run one by one, followed by `main()`. If one of them faults, the log
  names it.
- **Fault handling.** Every run is guarded, so a crash is reported instead of the game silently disappearing:
  - When the game or the port cannot go on, a message box says why and points to the log. This covers a window or
    OpenGL that cannot be started.
  - After a crash, `cache\GangstarMV.dmp` records the state of the port's threads: a minidump on Windows, a core file
    on Linux. The log's hidden folders and environment values are blanked in it.
- **Settings file.** On the first run, `savegame\GangstarMV.ini` is written with every key and a description of each.
  Deleting it restores the defaults. A key that older versions used, or a dev-only key in a release build, is ignored
  and noted in the log.
  - The defaults turn the enhancements on: 60 fps, every graphics effect, draw distance 2, crowds at 3, the gameplay,
    control and menu changes, and the add-ons, which do nothing until their files are there.
  - A key left blank, or missing from the file, takes its built-in value, which for the enhancements is the Mac's.
- **Portable data.** Saves and settings go in `savegame\` next to the exe, and the game's scratch files and the log go
  in `cache\`.
- **Finding the game.** The loader looks next to the exe, then one folder up. `--app <path>` points it anywhere else.
- **The log** (`LOG`, on by default):
  - It records the version, the settings, the graphics card and driver, the display modes, the controllers, and the
    details of a crash.
  - Your user folder, the exe's folder and the game's folder are written as `%USERPROFILE%`, `%EXE%` and `%APP%`.
  - A release log does not name your songs.
  - `LOG=0` turns the log and the crash dump off and deletes the old files.
- **Windows and Linux.** The loader asks the system for only a few things, all through `core/platform.h`. One version
  of that file answers on Windows and another on Linux, and the address layout and patch table are the same on both.
- **Any game.** The loader and the wrapper reach a game only through `core/game.h`. `games/template` is a game with no
  patches, for starting another. A second Mac game's binary has been loaded with it and verified, but not run.
- **Profilers** (dev builds only). `LOAD_PROFILE` and `PERF_PROFILE` show where the time goes in a long tick, on both
  systems.

## The macOS wrapper

### Libraries

- **Objective-C 1.0 runtime.** It covers message sending (`objc_msgSend`, struct returns and `super` calls), the game's
  own eight classes, and the Cocoa class hierarchy that messages are resolved against.
- **Foundation and AppKit.** `NSApplicationMain` with its run loop, timers and delayed calls, plus:
  - strings, bundles and user defaults;
  - files, notifications and threads;
  - dates, locales and arrays;
  - windows, views, screens, colours, cursors and alerts.
- **libSystem.** The C library, pthreads, `mmap`, clocks and time, stdio, wide characters, `setjmp`/`longjmp`, the
  rune locale and the keyboard layout calls. `rand` keeps the Mac's range, so particles spread and random chances work
  as on the Mac.
- **The C++ runtime.** `std::string` with the Mac's copy-on-write layout, `std::ostringstream`, red-black trees and lists.
  2D batches get the names the game gives them. Without that, a white square showed under the pause menu's pointer.
- **CoreGraphics, CoreFoundation and IOKit.** The monitor's real display modes, the mouse movement for the camera, and
  harmless stand-ins for the rest.
- **Offline.** The network classes do nothing, so the game's online buttons report that there is no connection. The port
  has no network code.
- **Coverage.** 477 of the 528 imports are implemented. An audit found no way for this build of the game to reach the
  other 51.

### Window and graphics (SDL3 and OpenGL)

- **Window or full screen** (`FULLSCREEN`, `SCREEN_WIDTH`, `SCREEN_HEIGHT`). Any resolution the monitor lists is
  available, ordered from smallest to largest after the desktop's. Full screen draws at the chosen resolution and is
  scaled to the monitor, with black bars where the shapes differ.
- **Keys.** Alt+Enter switches between full screen and a window. Alt+F4 quits, as closing the Mac window did.
- **Resizing.** A window you resize or maximize is filled, keeping the picture's shape, with black bars where it
  differs. While you drag the border, the window shows the last frame at the new size.
- **`DPI_AWARE`.** With Windows display scaling above 100%, the picture is drawn at the screen's real pixels instead of
  being stretched and blurred.
- **`MSAA`.** The Mac's 4x anti-aliasing, drawn into a screen framebuffer. 0, 2 and 8 are also accepted.
- **Kept frames.** `SWAP_COPY` keeps the last frame after each present, as the Mac does, which the game's fades rely
  on. `NO_FLICKER` presents only the frames the game actually drew.
- **`TEXFIX`.** Fade rectangles drawn with a texture still on now come out black, as the Mac's driver drew them. This
  restores the fade to black when you die and in the map menu.
- **Frame pacing** (`FRAME_MS`, `SWAP_INTERVAL`). A steady 60 ticks a second, as on a 60 Hz Mac. Other rates and
  uncapped are available. The game's sleeps use a 1 ms timer.
- **OpenGL.** The game's GL calls are bound on first use. Every GL function comes from one loader, built on
  `SDL_GL_GetProcAddress`, on both systems. Bad answers from a driver are rejected, and on Windows the GL 1.1 functions
  fall back to `opengl32.dll`.

### Sound

- **The game's own mixer, through SDL3.** The game's audio callback is pulled in small slices and queued, about 93 ms
  ahead. Sound follows the default output device when it changes.
- **Settings.** `NO_AUDIO` turns all sound off. `AUDIO_FRAMES` and `AUDIO_NB` size the queue.
- **The intro movie.** `intro.mov` is decoded by Media Foundation on Windows and by the system's `ffmpeg` on Linux, with
  its sound. `NO_INTRO` skips it, and `INTRO_FLIP` turns it right way up if a decoder shows it upside down.
- **Song decoders** for the custom radio. The port decodes mp3, ogg, flac and wav itself (dr_mp3, dr_flac, dr_wav and
  stb_vorbis), so songs sound the same on both systems. m4a, aac and wma use Media Foundation on Windows and `ffmpeg` on
  Linux.

### Input

- **Keyboard.** Keys go to the game as the Mac's SDL events.
  - On a French or Belgian keyboard the game uses its own Z Q S D keys, as on the Mac.
  - A key held while you Alt+Tab away does not stay pressed.
- **Mouse.**
  - Camera look uses SDL's relative mouse movement, and the pointer stays inside the picture while the game has focus.
  - Menu clicks, drags and hovers go to the game's view as Mac mouse events, at the system's pointer speed.
  - `NO_MOUSELOOK`, `NO_RMB` and `SHOW_CURSOR` change how the mouse behaves.
- **Controllers.** Xbox, PlayStation and other pads go through SDL3 and are presented to the game in the layout it expects.
  - Radial dead zones are set by the kind of pad.
  - A pad SDL knows gets the standard layout.
  - The kind of pad you used last is noted for the button pictures and hints.

### File speed (port fixes)

- **`FAST_FTELL`.** The file position is kept in memory instead of asking Windows about 580,000 times per load. The
  worst freeze on the way to the menu went from 1765 ms to 258 ms.
- **`FAST_FSEEK`.** Seeks inside a 64 KB read buffer are served directly, as the Mac's C library does.
- **`FAST_MEMMOVE`.** The physics' overlapping memory moves are done with SSE2, 3.5 to 8 times faster.
- **Linux.** `FAST_FTELL` and `FAST_FSEEK` are off there, since glibc already behaves this way.

### Linux

- **One source.** The same source builds a 32-bit Linux program.
  - Windows names map onto pthreads, `clock_gettime`, `stat` and `mprotect`.
  - The Mac's own calls have Linux versions beside the Windows ones.
  - The game's condition variables and the stack alignment get the extra care Linux needs.
- **What has run.** On Linux (WSL2, Debian 13) the game ran to the city, on foot and in cars, with every enhancement and
  add-on.
- **What is needed.** 32-bit SDL3 and GL. `ffmpeg` is used for the intro and the custom radio's m4a, aac and wma, and
  FreeType for the pad pictures' letters. When the icon file is present, the window gets the game's icon.

## The game layer

### Options menu (in the game's own menus)

- **`GAMEPATCH_TAB`** (on by default). A GAMEPATCH tab below VIDEO in the game's options menu. Most settings can be
  changed there in game, by category. A change is saved at once, and most take effect at once.
- **`ORIGINAL_MAC_EXPERIENCE`.** Plays the game as the Mac did:
  - It keeps the port's fixes, the loading speed-ups and the Mac's anti-aliasing on. It turns every enhancement off,
    and the fixes of the original game's own bugs too.
  - It switches in game with no restart, in both directions. The enhancements' hooks go in and out with it, and the
    collision tables and traffic pool return to the Mac's sizes.
  - Under it, the tab holds only this switch.
- **The SOUND page** gets two rows under Music for the custom radio: a volume slider with a mute button, and an
  ON / OFF selector.
- **The SERVICES menu** gets a MISSION REPLAY tab (see Gameplay).

### Port fixes (problems from running the Mac game on Windows)

- **`VTXFIX`.** Fixes a crash within a few steps of moving. The game's own safety check is added to the one path of its
  vertex converter that lacked it.
- **`CRASH58_GUARD`.** Turns a crash about 13 minutes into play, an empty helper pointer on a streamed object, into a
  logged no-op.
- **`STOPLOCK_FIX`.** A car's gas and brake no longer die after a full stop. The game's 200 ms settle lock now runs on
  the real clock. The bug is in the original game: the lock reads the frame time instead of a clock, so it lets go
  only on a frame shorter than the one at the stop. The Mac's uneven frame times gave one within a few frames, and so
  did the early port builds that let the graphics driver set the pace. The even 60 Hz pacing added on 2026-10-25
  never gives one, so without this fix the car can stay dead. That is why it stays on under
  `ORIGINAL_MAC_EXPERIENCE`.
- **`STATIC_VBO`.** Models that stay unchanged for a few frames are kept in the graphics card's memory, instead of
  being sent again on every draw. Draw time fell from 185 to 77 ms a second.
- **From bringing the port up** (not switchable): the black window at start, the saved resolution read as 0x0, asset
  files that failed to open, the resolution picker crash, and the intro logo that never finished.

### Original-game bugs fixed (on by default, off under `ORIGINAL_MAC_EXPERIENCE`)

- **`MENUFLASH_FIX`.** No flash of raw 3D background between the title and the main menu.
- **`MENU_FADE_CLICKS`.** The main menu takes clicks during its background fades.
- **`FADE_EDGE_FIX`.** Black fades cover the screen's top row and left column too.
- **`RADIO_LOGO_FIX`.** The HUD radio logo shows the car's station from the moment you start getting in.
- **`SWITCH_SAVE_FIX`.** A save made while playing as Betty no longer gives her things to Johnny.
- **Origin collision.** Buildings near the map's centre have collision as soon as a level starts. This fix applies only
  with `DRAW_DISTANCE` above 1.
- **`MISSION_SAVE_FIX`** (2026-11-06; unlike the others, it stays on under `ORIGINAL_MAC_EXPERIENCE`). The original
  save writes only 7 of the 8 groups of mission flags, so Reginald's five missions read as not done after every load.
  The save now keeps all 8, and the file still loads in the original game. It stays on under the preset so that a save
  made there keeps them, and MISSION REPLAY still offers them once the preset is off. MISSION REPLAY's SIDE list now
  includes Reginald's missions.

### Loading (on by default and under `ORIGINAL_MAC_EXPERIENCE`)

- **`LOAD_SLEEP_SKIP`.** No 16 ms sleep every tick for a few seconds after loads.
- **`LOAD_BUDGET`.** The main menu comes in smoothly while the city's collision is built: 200 items a tick instead of
  1500.
- **`FAST_LOAD`.** The start-up "Loading" screen takes as many of its steps a frame as fit, instead of one every
  33 ms, so the title comes about 1.4 s sooner. What it loads is the same.
- **Why they stay on under the preset.** They change no picture or rule, only when the loading work is done. Without
  them, each scene change in the main menu stalled for 116-219 ms, followed by a few seconds of uneven frames, which
  made the preset's 30 fps feel choppy. With them the stalls are 31-83 ms. How long a stall lasts depends on the
  machine, not on the game.

### High frame rates

- **`FRAME_MS`.** The frame rate, 25 to 400 fps; 60 by default, 30 as on the Mac.
- **`PHYSICS_SYNC`.** Car and object physics step once per tick, so driving stays smooth at any tick rate.
- **`CAMERA_SYNC`, `WALK_SYNC` and `AI_SYNC`.** Above 60 ticks a second, these keep the running camera sway as at 60,
  stop walking characters from flicking their facing, and stop runners from being taken as stuck. They do nothing at
  60.
- **Pad sticks** turn the camera at the same speed at any frame rate.

### Speed (always on, same results)

- **Collision and physics.** A faster Box2D broadphase query, faster proxy insert and remove, and a physics step that no
  longer walks every body.
- **HUD and billboards.** The HUD's health and armour bars without a search of the whole UI on every update, and the
  billboard turn with its range tests first.
- **Result.** In a busy scene with draw distance and crowds at 2, these took the test laptop from about 70 to about 113
  frames a second.

### Graphics enhancements (on by default, off under `ORIGINAL_MAC_EXPERIENCE`)

- **`ANISOTROPIC_FILTERING`** (2 to 16). Roads and walls stay sharp at a distance.
- **`AMBIENT_OCCLUSION`.** Soft contact shadows in corners and under cars, drawn under the HUD. Tuned by `AO_STRENGTH`
  and `AO_RADIUS`.
- **`BLOOM`.** A glow around bright parts of the world. `BLOOM=2` also makes the light sprites and lit windows glow.
  Tuned by `BLOOM_INTENSITY`, `BLOOM_THRESHOLD`, `BLOOM_LIGHTS` and `BLOOM_WINDOWS`.
- **`WATER_REFLECTIONS`.** The sea reflects the city and the sky, from what is on screen. Tuned by
  `REFLECTION_STRENGTH`.
- **`VEHICLE_REFLECTIONS`.** Shiny cars reflect the street, the buildings and the other cars, while matte cars stay
  matte. Tuned by `VEHICLE_REFLECTION_STRENGTH`. Where the screen-space ray finds nothing, as on roofs and trunks, the
  screen itself is used as a sphere map, looked up by the reflected direction (2026-11-06).
- **`DYNAMIC_SHADOWS`.** Vehicles and people cast shadows that follow the sun and the moon. At dusk and dawn a soft
  shadow stays under them.
- **`SUN_MOON`.** The iPhone version's sun and moon, with their glow, restored from the code and textures still in the
  Mac game.
  - `2` (IPHONE) draws the glow as the iPhone did.
  - `1` (ENHANCED) hides the glow behind buildings with a soft edge.
- **`SPAWN_FADE`.** Cars and pedestrians fade in and out instead of popping.
- **`DRAW_DISTANCE`** (1 to 4). Draws the city up to four times as far as the Mac. The collision tables grow with it.
- **`FOG_MODE`.**
  - `0`: the game's own fog.
  - `1`: linear fog over the whole city in the sky's colour.
  - `2`: a hybrid fog, by the true distance at every pixel and thinner higher up (`FOG_HEIGHT`).
  - With 1 or 2, the skyline fades in with no seam, and the sea carries on past the map's edge.

### Gameplay (on by default, off under `ORIGINAL_MAC_EXPERIENCE`)

- **`ALLOW_WEAPON_SPRINT`.** Your weapon stays in hand while you sprint, as on the iPhone.
- **`MINIMAP_SHOPS`.** Weapon shops, car rentals and repair shops show on the minimap, with the pause map's icons.
- **`MISSION_REPLAY`.** A tab in the SERVICES menu lists the missions you have completed, with their giver, location,
  type and reward.
  - Arrows switch between story, side missions and races.
  - START plays a mission again. You keep the cash you earn, and everything else returns to how it was when the replay
    ends.
- **`TRAFFIC_DENSITY`, `PEDESTRIAN_DENSITY` and `PARKED_CAR_DENSITY`** (0 to 4). Up to four times the Mac's cars and
  people, applied at once from the menu.
- **`PLAYERSWITCH_INVENTORY`.** In free roam, Betty shares Johnny's cash and weapons.
- **`LC_SWITCH`.** After the last mission, a marker at L.C.'s hotel lets you play as L.C. and back, as the game's own
  marker does for Betty.

### Controls (on by default, off under `ORIGINAL_MAC_EXPERIENCE`)

- **`KEY_MOVE`.** Modern PC movement: W/A/S/D diagonals, smooth turns, and Alt to walk.
- **`PAD_LAYOUT`.** A gamepad layout like GTA V's, on foot, in cars, flying and with the sniper. The CONTROLS page shows
  it.
- **`PAD_MENU`.** The pad works the menus: the stick or D-pad moves the pointer, A clicks and B goes back. A pad button
  also skips the intro.
- **`PAD_HINTS`.** Tutorial boxes name the pad's buttons while the pad is what you last used.
- **`PAD_ICONS`.** The options menu's pad pictures show Xbox, DualShock 4 or DualSense buttons, for the pad you last
  used.

### Menus and cutscenes (on by default, off under `ORIGINAL_MAC_EXPERIENCE`)

- **`HIDE_MORE_GAMES` and `HIDE_GAMELOFT_LIVE`.** Remove the main menu's links to Gameloft's long-gone services.
- **`SHORT_WARNING`.** The start-up language warning lasts 3 s instead of 15, and any key skips it.
- **`CUTSCENE_KEYS`.** Cutscenes without on-screen buttons: Esc or Enter skips, and Space shows the next line.
- **`TUTORIAL_KEYS`.** Esc, Enter or Space answers tutorial and question popups.

### Sound and add-ons (on by default, off under `ORIGINAL_MAC_EXPERIENCE`)

- **`CUSTOM_RADIO`.** A sixth radio station that plays your own songs from the `custom radio` folder, shuffled.
  - It follows the game's MUSIC volume, scaled by `CUSTOM_RADIO_VOLUME`.
  - The HUD shows the game's PLAY LIST / MY MUSIC logo for it.
- **`RADIO_SONGS`.** The nine extra songs of the iPhone version on the electro, hip-hop and rock stations, raised to the
  Mac's loudness. They need `add-on\radio.pak`, made from your own copy of the iPhone version.
- **`UC_CRUSHER`.** Urban Crime's Crusher monster truck, parked by L.C.'s hotel after ON A LION HUNT. It is drivable,
  with its own engine sound and exhaust smoke. It needs `add-on\crusher.pak`, made from your own copy of Urban Crime.
- **Live switching.** The add-ons switch on and off in game with no restart. No add-on or song is included.

## Builds

- **CMake** (`host/CMakeLists.txt`) is the only build, with presets for Windows (MSYS2's 32-bit gcc) and Linux (gcc
  `-m32`). `host\build.ps1` and `host/linux/build.sh` call it.
- **Three kinds of build:**
  - a plain build for daily work;
  - `-Dev`, with the console and every diagnostic;
  - `-Release`, the public build.
- **Release builds** have no debug mode. The traces, surveys, test aids, console and `GMV_*` environment overrides are
  not compiled in, and the exe holds no path or name from the machine that built it.
- **Release folders** include `add-on` and `custom radio` folders, each with a note on what goes there.
- **A CI workflow** builds the Windows and Linux variants with no game files, and treats any warning as a failure.

## Tools

- **`tools/genlow.py`** makes the low-range patch table for a binary.
- **`tools/get_sdl3.ps1`** fetches the official 32-bit SDL3, pinned and checked by hash.
- **`tools/get_linux_sysroot.py` and `tools/linux_check.ps1`** compile and link the Linux build from Windows, with
  clang.
- **`tools/playtest/playtest.ps1`** runs scripted tests of the exe, with real input and screenshots, and reports PASS or
  FAIL for each.
  - The scenarios cover start-up, the city, the camera, every enhancement, full screen, resizing, pads, the Mac preset,
    the custom radio, crashes and quitting.
  - Each run gets its own folder, so your save and ini are never touched.
- **`tools/uc_crusher_pack.py`, `tools/uc_crusher_tex.py` and `tools/radio_gain.py`** build the Crusher and radio
  add-ons from your own copies of the games.
- **Analysis tools** for the Mach-O and Objective-C metadata: `machoinfo`, `objc1dump`, `initreach`, `disasm`,
  `lowcode`, and others.

## Known limits

See [Not verified yet](docs/README.md#not-verified-yet) in the README.
