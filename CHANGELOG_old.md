# Changelog

## PC Port V1.0.2 (2026-11-09)

### Changed

- **`RADIO_SONGS`:** the rock station also plays the Java version's two rock songs, made from your own copy of
  Gangstar: West Coast Hustle and brought to the Mac's format and loudness. They come after the iPhone's songs in
  the same `add-on\radio.pak`. Either set may be missing from the pack. Same switch, and still off under
  `ORIGINAL_MAC_EXPERIENCE`. As before, no song is included.
- **`MSAA`:** its description, `FIXES.txt` and the README now say to keep 4 on graphics built into the processor.
  On an AMD one, 8 with every enhancement on sometimes kept the GPU busy for about 5 s on one frame. Windows then
  reset the GPU and the game stopped. 4 ran clean. The code is unchanged.

## PC Port V1.0.1 (2026-11-06)

### Fixed

- **`MISSION_SAVE_FIX`** (on by default, and under `ORIGINAL_MAC_EXPERIENCE` too): the original game keeps its mission
  flags in 8 groups, but its save writes only 7, so Reginald's five missions read as not done after every load. The
  save now keeps all 8, and the file still loads in the original game. It stays on under the preset so that a save
  made there keeps them, and MISSION REPLAY still offers them once the preset is off. Found on 2026-11-06, when the
  user asked to have those missions marked done in their save. MISSION REPLAY's SIDE list now includes them.

### Changed

- **`VEHICLE_REFLECTIONS`:** where the screen-space ray finds nothing, as on roofs and trunks seen from above, the
  screen itself is used as a sphere map, looked up by the reflected direction, so those parts no longer look bare.
- **Source:** the finished dev-only surveys (`BLOOM_SURVEY`, `FOG_SURVEY`, `VEHICLE_SURVEY`, `MISSION_SURVEY`,
  `SPAWN_SURVEY`) and the `VBO_STREAM` experiment are gone, and unused includes were removed. Nothing changes in
  play. An old ini's lines for them are noted in the log as unknown and ignored. Dev builds gained `TEST_SPAWN_CAR`,
  which puts the player at the wheel of a car of the named type, for looking at the reflections up close.

## PC Port V1.0.0 (2026-11-05)

The Windows Port became the PC Port: the same source now builds for Windows and Linux.

### Added

- **Linux:** a 32-bit Linux build from the same source, with the same settings, saves and patch table. It needs the
  system's 32-bit SDL3 and OpenGL; `ffmpeg` plays the intro and the custom radio's m4a, aac and wma. So far it has run
  in WSL2 (Debian 13) only.
- **`FAST_LOAD`** (on by default and under `ORIGINAL_MAC_EXPERIENCE`): the start-up "Loading" screen takes as
  many of its steps a frame as fit, instead of one every 33 ms. From the warning to the title took 2.1 s and now
  takes 0.7 s. Also on the GAMEPATCH tab (MENU: FAST START-UP LOADING).
- **Crash dump:** after a crash the port also leaves `cache\GangstarMV.dmp` for the bug report. `LOG=0` turns it off.

### Changed

- **Defaults:** a new `GangstarMV.ini` turns the enhancements on: 60 fps, 16x texture filtering, ambient occlusion,
  bloom HIGH with the lit windows at twice the glow, the hybrid fog, water and car reflections, the enhanced sun and moon, dynamic shadows, traffic fading in,
  draw distance 2, crowds at 3, every gameplay, control and menu change, and the three add-ons (custom radio at 0.53
  of the music's volume). The intro still plays. An existing ini keeps its values; a key it lacks stays as on the Mac.
  `ORIGINAL_MAC_EXPERIENCE=1` is unchanged.
- **`ORIGINAL_MAC_EXPERIENCE`:** it now turns every fix of the original game's own bugs off, `MENUFLASH_FIX` and
  `MENU_FADE_CLICKS` included, and keeps the loading speed-ups on (`LOAD_SLEEP_SKIP`, `LOAD_BUDGET` at 200,
  `FAST_LOAD`). They change no picture or rule, and without them the preset's 30 fps felt choppy in the main menu.
- **Source layout:** the two largest files are split by feature, and the loader and shims are apart from this game's
  patches, in `host\core`, `host\shims` and `host\games\gangstar`. Nothing changes in play.

- **Sound:** the game and the intro movie now play through SDL3 instead of `waveOut`, with the same queue. A build
  now has `SDL3.dll` next to the exe. If the default output device changes, the sound should follow it.
- **Pads:** every pad is now read through SDL3 instead of XInput and winmm. The buttons, sticks and dead zones stay
  as they were for Xbox and PlayStation pads. Over USB, a PlayStation pad's light may change colour while the game
  runs, as SDL sets it. An unusual pad that SDL has a mapping for now gets the standard layout, so check its buttons on the
  calibration page if you calibrated it before.
- **Window, keyboard and mouse:** these now go through SDL3 instead of Windows directly. Play should feel the same:
  the camera turns as far for the same mouse move, and the menus keep the system's pointer speed. The resolutions in
  the options now run from smallest to largest, after the desktop's.
- **Custom radio:** mp3, flac and wav songs are now decoded by the port itself instead of Windows, and ogg songs play
  too. m4a, aac and wma still go through Windows, as does a file the port's decoder cannot open.

### Removed

- **`NO_MOUSELOOK` and `NO_RMB`:** the mouse always turns the camera and the right button always locks on, as on the
  Mac. Their lines in an older `GangstarMV.ini` are skipped without a message.

### Fixed

- **`FADE_EDGE_FIX`** (on by default, off under `ORIGINAL_MAC_EXPERIENCE`): the black fades, such as the one after CONTINUE,
  now cover the screen's top row and left column too. With anti-aliasing on, the original game left a thin line of
  picture there. Also on the GAMEPATCH tab (FIXES: FADE EDGE FIX).
- **Resizing the window:** the picture now fills a window you resize or maximize, with its shape kept and black bars
  where the shape differs. Before, it stayed at its old size in a corner of a larger window. The game still draws at the
  resolution you chose, so a sharper picture in a large window comes from choosing a higher one. While you drag the
  border the game itself waits, and the window shows its last frame at the new size instead of black.
- **`DPI_AWARE`:** with Windows display scaling above 100%, the picture is no longer blurred by Windows stretching
  it. A window keeps the size you chose in pixels, so it looks smaller than before. `DPI_AWARE=0` brings the old
  stretch back.
- **AZERTY keyboards:** the game's own Z Q S D keys now work on a French or Belgian keyboard, as they did on the Mac.
  They come with a new `savegame\Gangstar2.preferences`, so delete that file if you have played on another layout.
- **Stuck keys:** a key or Alt held while switching away with Alt+Tab no longer stays pressed in the game.
- **`PAD_MENU`:** the pad's menu pointer could jump back to where the mouse rests, when Windows reported a mouse move
  though the mouse had not moved.
- **Error box after a crash:** the crash report's own checks could turn a crash into a wrong "data this port does not
  have yet" message.

## Windows Port v.1.0.0 (2026-11-03)

The first public release, for Windows. It includes everything from the earlier folders (v.0.5.0 and v.0.9.0),
listed here in full.

### The loader

- **Runs the Mac game as it is.** There is no CPU emulation, since the game is already x86 code. The game's i386 Mach-O
  binary is loaded at its own addresses, which the exe reserves before anything else can take them.
- **The low range.** The game's first code page overlaps the exe's own header, so that code runs from a copy. Every jump
  into and out of it is patched (4360 patches), and each patch is checked byte for byte at load. The patch table comes from
  `tools/genlow.py` and works only with the binary it was made from.
- **Every import bound.** Each of the game's 4243 import slots (528 distinct imports) is filled. A slot goes to a shim
  where one exists. A missing function becomes a stub that reports its name, and missing data becomes a stand-in that
  faults on use, so a gap shows up in the log instead of corrupting memory.
- **Start-up.** The game's 451 static initializers run one by one, followed by `main()`. If one of them faults, the log
  names it.
- **Error box.** When the game or the port cannot go on, a message says why and points to the log. Before, the game just
  vanished.
- **Settings file.** On the first run, `savegame\GangstarMV.ini` is written with every key and a description of each.
  Deleting it restores the defaults.
  - In a new ini, `LOG` sits in a `[Log]` section. An older ini keeps working.
  - `FIXEDSTEP` and `FIXED_MS`, an old experiment that normal play did not use, are gone. In an older ini they are
    ignored and reported in the log, as is any dev-only key in a release build.
- **Portable data.** Saves and settings go in `savegame\` next to the exe, and the game's scratch files and the log go
  in `cache\`.
- **Finding the game.** The loader looks next to the exe, then one folder up. `--app <path>` points it anywhere else.
- **The log** (`LOG`, on by default). It records the version, the settings, the graphics card and driver, the display
  modes, the controllers, and the details of a crash. `LOG=0` turns it off and deletes the old file.

### The macOS wrapper

- **Libraries.**
  - An Objective-C 1.0 runtime: message sending, the game's own eight classes, and the Cocoa class hierarchy.
  - Foundation and AppKit: `NSApplicationMain` with its run loop and timers, strings, bundles, user defaults, files,
    notifications, threads, windows, views, screens, cursors and alerts.
  - libSystem: the C library, pthreads, `mmap`, clocks, stdio, wide characters and `setjmp`/`longjmp`. `rand` keeps the
    Mac's range, so particles spread and random chances work as on the Mac.
  - The C++ runtime, with the Mac's `std::string` layout and `std::ostringstream`. 2D batches get the names the game
    gives them. Without that, a white square showed under the pause menu's pointer.
  - CoreGraphics, CoreFoundation and IOKit: the monitor's real display modes, and harmless stand-ins for the rest.
  - The network classes do nothing, so the game's online buttons report that there is no connection. The port has no
    network code.
  - 477 of the 528 imports are implemented. An audit found no way for this build of the game to reach the other 51.
- **Window and graphics.** The window is a Windows window, and OpenGL comes from WGL.
  - Window or full screen (`FULLSCREEN`, `SCREEN_WIDTH`, `SCREEN_HEIGHT`) at any resolution the monitor lists. Full
    screen draws at the chosen resolution and is scaled to the monitor, with black bars where the shapes differ.
  - Alt+Enter switches between full screen and a window. Alt+F4 quits, as closing the Mac window did.
  - `MSAA`: the Mac's 4x anti-aliasing, drawn into a screen framebuffer. 0, 2 and 8 are also accepted.
  - `SWAP_COPY` keeps the last frame after each present, as the Mac does, which the game's fades rely on. `NO_FLICKER`
    presents only the frames the game actually drew.
  - `TEXFIX`: fade rectangles drawn with a texture still on come out black, as the Mac's driver drew them. This restores
    the fade to black when you die and in the map menu.
  - Frame pacing (`FRAME_MS`, `SWAP_INTERVAL`): a steady 60 ticks a second, as on a 60 Hz Mac. Other rates and uncapped
    are available. The game's sleeps use a 1 ms timer.
- **Sound.** The game's own mixer plays through `waveOut`. `NO_AUDIO` turns all sound off, and `AUDIO_FRAMES` and
  `AUDIO_NB` size the queue.
- **The intro movie.** `intro.mov` is decoded by Media Foundation, with its sound. `NO_INTRO` skips it, and
  `INTRO_FLIP` turns it right way up if a decoder shows it upside down.
- **Keyboard and mouse.** Keys go to the game as the Mac's SDL events.
  - Camera look uses raw mouse movement, and the pointer stays inside the window while the game has focus.
  - Menu clicks, drags and hovers go to the game's view as Mac mouse events.
  - `NO_MOUSELOOK`, `NO_RMB` and `SHOW_CURSOR` change how the mouse behaves.
- **Controllers.** Xbox pads through XInput, and PlayStation and other pads through winmm, presented to the game in the
  layout it expects. Dead zones are radial and set by the kind of pad. The kind you used last is noted for the button
  pictures and hints.
- **File speed** (port fixes):
  - `FAST_FTELL` keeps the file position in memory instead of asking Windows about 580,000 times per load. The worst
    freeze on the way to the menu went from 1765 ms to 258 ms.
  - `FAST_FSEEK` serves seeks inside a 64 KB read buffer directly, as the Mac's C library does.
  - `FAST_MEMMOVE` does the physics' overlapping memory moves with SSE2, 3.5 to 8 times faster.

### Options menu (in the game's own menus)

- **`GAMEPATCH_TAB`** (on by default; `GAMEPATCH_TAB=0` removes it). A GAMEPATCH tab below VIDEO in the game's options
  menu. Most settings can be changed there in game, by category. A change is saved at once, and most take effect at once.
- **`ORIGINAL_MAC_EXPERIENCE`.** Plays the game as the Mac did:
  - It keeps the port's fixes and the Mac's anti-aliasing on, and turns every enhancement off.
  - It switches in game with no restart, in both directions. The enhancements' hooks go in and out with it, and the
    collision tables go back to the Mac's as the city next loads.
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
  the real clock.
- **`STATIC_VBO`.** Models that stay unchanged for a few frames are kept in the graphics card's memory, instead of
  being sent again on every draw. Draw time fell from 185 to 77 ms a second.
- **From bringing the port up** (not switchable): the black window at start, the saved resolution read as 0x0, asset
  files that failed to open, the resolution picker crash, and the intro logo that never finished.

### Original-game bugs fixed

- **`MENUFLASH_FIX`.** No flash of raw 3D background between the title and the main menu.
- **`MENU_FADE_CLICKS`.** The main menu takes clicks during its background fades.
- **`RADIO_LOGO_FIX`.** The HUD radio logo shows the car's station from the moment you start getting in.
- **`LOAD_SLEEP_SKIP`.** No 16 ms sleep every tick for a few seconds after loads.
- **`LOAD_BUDGET`.** The main menu comes in smoothly while the city's collision is built: 200 items a tick instead of
  1500.
- **`SWITCH_SAVE_FIX`.** A save made while playing as Betty no longer gives her things to Johnny.
- **Origin collision.** Buildings near the map's centre have collision as soon as a level starts. This fix applies only
  with `DRAW_DISTANCE` above 1.

### High frame rates

- **`PHYSICS_SYNC`** (on by default, off under `ORIGINAL_MAC_EXPERIENCE`). Car and object physics step once per tick, so
  driving stays smooth at any tick rate. An ini that already sets it keeps its value.
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

### Graphics enhancements (off by default)

- **`ANISOTROPIC_FILTERING`** (2 to 16). Roads and walls stay sharp at a distance.
- **`AMBIENT_OCCLUSION`.** Soft contact shadows in corners and under cars, drawn under the HUD. Tuned by `AO_STRENGTH`
  and `AO_RADIUS`.
- **`BLOOM`.** A glow around bright parts of the world. `BLOOM=2` also makes the light sprites and lit windows glow.
  Tuned by `BLOOM_INTENSITY`, `BLOOM_THRESHOLD`, `BLOOM_LIGHTS` and `BLOOM_WINDOWS`.
- **`WATER_REFLECTIONS`.** The sea reflects the city and the sky, from what is on screen. Tuned by
  `REFLECTION_STRENGTH`.
- **`VEHICLE_REFLECTIONS`.** Shiny cars reflect the street, the buildings and the other cars, while matte cars stay
  matte. Tuned by `VEHICLE_REFLECTION_STRENGTH`.
- **`DYNAMIC_SHADOWS`.** Vehicles and people cast shadows that follow the sun and the moon. At dusk and dawn a soft
  shadow stays under them.
- **`SUN_MOON`.** The iPhone version's sun and moon, with their glow, restored from the code and textures still in the
  Mac game. Three modes: MAC (off), IPHONE (`2`, the glow as the iPhone drew it) and ENHANCED (`1`, the glow hidden
  behind buildings with a soft edge).
- **`SPAWN_FADE`.** Cars and pedestrians, and their lights, fade in and out instead of popping.
- **`DRAW_DISTANCE`** (1 to 4). Draws the city up to four times as far as the Mac. The collision tables grow with it.
- **`FOG_MODE`.**
  - `0`: the game's own fog.
  - `1`: linear fog over the whole city in the sky's colour.
  - `2`: a hybrid fog, by the true distance at every pixel and thinner higher up (`FOG_HEIGHT`).
  - With 1 or 2, the skyline fades in with no seam, and the sea carries on past the map's edge.

### Gameplay (off by default)

- **`ALLOW_WEAPON_SPRINT`.** Your weapon stays in hand while you sprint, as on the iPhone. Two-handed weapons too.
- **`MINIMAP_SHOPS`.** Weapon shops, car rentals and repair shops show on the minimap, with the pause map's icons.
- **`MISSION_REPLAY`.** A tab in the SERVICES menu lists the missions you have completed, with their giver, location,
  type and reward.
  - Arrows switch between story, side missions and races.
  - START plays a mission again. You keep the cash you earn, and everything else returns to how it was when the replay
    ends.
- **`TRAFFIC_DENSITY`, `PEDESTRIAN_DENSITY` and `PARKED_CAR_DENSITY`** (0 to 4). Up to four times the Mac's cars and
  people. Raising one in the menu adds its cars and people at once, with no new level needed.
- **`PLAYERSWITCH_INVENTORY`.** In free roam, Betty shares Johnny's cash and weapons.
- **`LC_SWITCH`.** After the last mission, a marker at L.C.'s hotel lets you play as L.C. and back, as the game's own
  marker does for Betty. L.C. drives cars and motorbikes, and missions can be replayed as him.

### Controls (off by default)

- **`KEY_MOVE`.** Modern PC movement: W/A/S/D diagonals, smooth turns, and Alt to walk. Two held keys do not swing the
  body from side to side while the mouse turns.
- **`PAD_LAYOUT`.** A gamepad layout like GTA V's, on foot, in cars, on motorbikes (stick wheelie), flying and with the
  sniper. In a car, holding A locks on for drive-by shooting. The CONTROLS page shows the layout.
- **`PAD_MENU`.** The pad works the menus: the stick or D-pad moves the pointer, A clicks and B goes back. A pad button
  also skips the intro.
- **`PAD_HINTS`.** Tutorial boxes name the pad's buttons while the pad is what you last used.
- **`PAD_ICONS`.** The options menu's pad pictures show Xbox, DualShock 4 or DualSense buttons, for the pad you last
  used.

### Menus and cutscenes (off by default)

- **`HIDE_MORE_GAMES` and `HIDE_GAMELOFT_LIVE`.** Remove the main menu's links to Gameloft's long-gone services.
- **`SHORT_WARNING`.** The start-up language warning lasts 3 s instead of 15, and any key skips it.
- **`CUTSCENE_KEYS`.** Cutscenes without on-screen buttons: Esc or Enter skips, and Space shows the next line.
- **`TUTORIAL_KEYS`.** Esc, Enter or Space answers tutorial and question popups: Enter or Space is YES, Esc is NO.

### Sound and add-ons (off by default)

- **`CUSTOM_RADIO`.** A sixth radio station that plays your own songs from the `custom radio` folder, shuffled.
  - Songs can be mp3, m4a, aac, wma, wav or flac, decoded by Windows.
  - It follows the game's MUSIC volume, scaled by `CUSTOM_RADIO_VOLUME`.
  - The HUD shows the game's PLAY LIST / MY MUSIC logo for it.
- **`RADIO_SONGS`.** The nine extra songs of the iPhone version on the electro, hip-hop and rock stations, raised to the
  Mac's loudness. They need `add-on\radio.pak`, made from your own copy of the iPhone version. The songs are not
  included.
- **`UC_CRUSHER`.** Urban Crime's Crusher monster truck, parked by L.C.'s hotel after ON A LION HUNT. It is drivable,
  with its own engine sound and exhaust smoke, and its cab uses the Mac's interior texture. It needs
  `add-on\crusher.pak`, made from your own copy of Urban Crime.
- **Live switching.** The add-ons switch on and off in game with no restart.

### Release build and privacy

- `host\build.ps1` makes three kinds of build: a plain build for daily work, `-Dev` and `-Release`.
- A release build has no debug mode. The traces, surveys, captures, profilers, test aids, console, staged
  command-line flags, debug hotkey and `GMV_*` environment overrides are not compiled in. An environment variable
  therefore does not change a setting in a release build.
- The log writes your user folder, the exe's folder and the game's folder as `%USERPROFILE%`, `%EXE%` and `%APP%`,
  and a release log does not name your songs and holds far fewer lines.
- A separate dev folder is for people working on the port: console, every diagnostic and `GMV_*` overrides. Its log
  is much longer and names your songs.
- The release folders hold an `add-on` and a `custom radio` folder, each with a note on what goes there. No add-on
  or song is included.

### Tools

- **`tools/genlow.py`** makes the low-range patch table for a binary.
- **`tools/uc_crusher_pack.py`, `tools/uc_crusher_tex.py` and `tools/radio_gain.py`** build the Crusher and radio
  add-ons from your own copies of the games.
- **Analysis tools** for the Mach-O and Objective-C metadata: `machoinfo`, `objc1dump`, `initreach`, `disasm`,
  `lowcode`, and others.

### Known limits

See [Not verified yet](docs/README.md#not-verified-yet) in the README.
