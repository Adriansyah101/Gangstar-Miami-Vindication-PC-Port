# GangstarMV PC Port: technical notes

The long form behind the [README](README.md), which has the current status, setup and settings.

- [How it works](#how-it-works)
- [Running and settings](#running-and-settings)
- [Source layout](#source-layout)
- [Fix and enhancement log](#fix-and-enhancement-log)
- [Diagnostics](#diagnostics)
- [Bring-up history](#bring-up-history)

## How it works

The game is a non-PIE i386 Mach-O linked at `0x1000` with no rebase info, so it must run at its
own addresses. The host exe is based at `0x10000` (the lowest address Windows allows) and its
first section reserves the whole game range so nothing else can be mapped there.

- **Image layout** is fixed in [`host/games/gangstar/layout.h`](../host/games/gangstar/layout.h) and enforced by the linker
  script [`host/core/gmv.ld`](../host/core/gmv.ld) + the reservation in [`host/core/reserve.S`](../host/core/reserve.S).
- Only the code below `0x11000` (the Mach-O header + early CRT) cannot sit at its own address,
  because the PE header page owns `0x10000..0x11000`. That range runs from a **copy at
  `0xC70000`** with every branch into/out of it patched exactly; a stray jump into the original
  range faults and a vectored exception handler ([`host/core/veh.c`](../host/core/veh.c)) redirects it.
- The exact low-range patches (4360) come from [`tools/genlow.py`](../tools/genlow.py) →
  `build/gmv_patch.bin` (CRC-locked to the binary, not to its location). Applied and
  byte-verified at load.
- Every Mach-O bind slot (4243 slots, 528 distinct imports) is written: to a host shim if
  implemented, else to a name-reporting thunk (code) or a no-access stand-in (data) so a missing
  import announces itself instead of corrupting silently.

### Running game code

A run loads and verifies the image and then runs the game, always under the VEH + page protections
so a fault is contained and reported, never silent. The plain and `-Dev` builds can stop earlier
(`--verify-only`, `--run-init`).

- **Static initializers** ([`host/core/runinit.c`](../host/core/runinit.c)): the 451 `__mod_init_func`
  entries are called one at a time; the VEH is told which one is live so a fault names it.
- **`main()`** (`--run-main`): runs all initializers, initializes the ObjC runtime and the NS
  string constants, then calls `main` with a minimal `argc/argv/envp/apple`.

### The Objective-C runtime ([`host/shims/shimsobjc.c`](../host/shims/shimsobjc.c) + [`.S`](../host/shims/shimsobjc.S))

The game ships 8 of its own ObjC classes in `__OBJC` (`GameDelegate`, `GameApplication`,
`GameWindow`, `EAGLView`, `MovieView`, `igphtmlmacViewController`,
`GLLivehtmlmacViewController`, `sendInfo`). No dyld ObjC runtime ever ran, so the
class-refs/message-refs still hold **pointers to the class-name / selector-name C-strings**, and
the metadata sits at its own addresses. On this basis:

- `gmv_objc_init()` parses `__OBJC` (`__module_info → symtab → class defs`) into a class
  registry. (Struct layout is in [`tools/objc1dump.py`](../tools/objc1dump.py).)
- `objc_msgSend` ([`.S`](../host/shims/shimsobjc.S)) reads `self`/`_cmd`, asks `gmv_objc_lookup` for the
  IMP, and **tail-jumps** to it — a game method (real code in `__text`), a Foundation stub, or
  `nil` for a no-op. `objc_msgSend_stret` (struct returns) and `objc_msgSendSuper` have their own
  asm entries + lookups (`gmv_objc_stret_lookup`, `gmv_objc_super_lookup`); the legacy
  `objc_exception_*` are stubbed (the real `@try` uses our `setjmp`/`longjmp`).
- A game instance's `isa` is the game's `objc_class`; a Cocoa instance we create gets a "fake
  class" descriptor tagged with its name. `+alloc` / `gmv_objc_new_instance` allocates
  `instance_size` bytes and sets `isa`. Compile-time `CFConstantString` literals are recognized
  as `NSString` instances and routed accordingly.
- Lookup for an instance: game methods (up the game superclass chain) → NSObject defaults →
  Foundation/AppKit stubs, **walking the superclass chain by name** — and through a hardcoded
  Cocoa hierarchy (`cocoa_super`: `QTMovieView→NSView→NSResponder→NSObject`, `NSWindow→…`) so a
  message to `GameWindow` resolves against the `NSWindow` stub, an `EAGLView`/`MovieView` against
  `NSView`, etc.

### Foundation / AppKit / Cocoa stubs ([`host/shims/shimsfnd.c`](../host/shims/shimsfnd.c), [`host/shims/shimsappkit.c`](../host/shims/shimsappkit.c))

Cocoa methods are host IMPs in `{class, selector, imp}` tables (class, instance, struct-return).
NSString objects are `gmv_objc_new_instance("NSString")` with the C-string pointer at offset 4;
`CFConstantString` literals keep theirs at offset 8, and `nsstring_cstr` handles both. Coverage:

- **NSString**: `stringWithFormat:`/`initWithFormat:` (shared printf-style `nsformat`, `%@`
  aware), `stringWith/initWithCString/UTF8String`, `substringToIndex:`, `hasPrefix:`,
  `stringByAppendingPathComponent:` / `AppendingString:`, `replacingOccurrences…`,
  `AddingPercentEscapes…`, `writeToFile:…`, `isEqual(ToString):`, `length`, `UTF8String`.
- **NSBundle** (real `.app` path, `resourcePath`, `pathForResource:ofType:`, `infoDictionary`),
  **NSUserDefaults** (the resolution and full screen live in `GangstarMV.ini`), **NSFileManager** (real Win32 file ops),
  **NSNotificationCenter** (real: stores observers, delivers on post), **NSWorkspace**,
  **NSThread** (`detachNewThreadSelector:` spawns a real thread), **NSURL/NSURLRequest/
  NSURLConnection/NSData** (inert — the IGP/network feature fails silently), **NSLocale**,
  **NSDate/NSDateFormatter**, **NSArray**, **NSWindow/NSView/NSScreen/NSColor/
  NSCursor/NSOpenGL\***, **QTMovie/QTMovieView** (a stand-in object; see below for the movie).
- `NSApplicationMain` ([`shimsappkit.c`](../host/shims/shimsappkit.c)) creates `GameApplication` as
  `NSApp`, creates `GameDelegate`, sends `-applicationDidFinishLaunching:`, then runs the loop: a
  Win32 message pump, the deferred `performSelector:…afterDelay:` queue, the scheduled `NSTimer`,
  `-[GameDelegate update]`. At frame 3 it plays `intro.mov` ([`shimsvideo.c`](../host/shims/shimsvideo.c),
  Media Foundation) and posts `QTMovieDidEndNotification`. The game's `startGame` then enters its own
  render loop and does not come back. Also AudioUnit (the render callback is captured for
  [`shimsaudio.c`](../host/shims/shimsaudio.c)), CoreFoundation and IOKit (fail-soft),
  `Gestalt`, `NSSearchPathForDirectoriesInDomains` (returns a writable dir **next to the exe**;
  logged as `[dirs] NSSearchPath dir=N -> …`. The game asks only for `dir=5` (Library) and funnels
  everything there — save, settings, and its `home/`,`tmp/` sandbox — so the dir alone can't split
  them. That dir maps to `cache\`, and the `fopen` shim then **redirects by basename**:
  `*.sav`/`*.preferences` → `savegame\`, leaving the scratch sandbox in `cache\`. Confirmed from the
  `[fopen]` trace — the save is opened `rb` on load then `a+b`/`r+b` as the game autosaves), NS
  string constants.

### C++ runtime / libstdc++ ([`host/shims/shimscxx.c`](../host/shims/shimscxx.c), [`host/shims/shimsstr.c`](../host/shims/shimsstr.c), [`host/shims/shimsstream.c`](../host/shims/shimsstream.c))

`__cxa_*`, RB-tree insert/erase/increment, `_List_node_base` ops, `std::allocator<char>` (ported
from libstdc++ 4.2); `std::string` (COW `_Rep` layout). `std::ostringstream` is **not** a real
stream reimplementation — [`shimsstream.c`](../host/shims/shimsstream.c) keeps a per-stream **accumulator
keyed by address range**: `std::ios_base::ios_base()` (always out-of-line) registers a buffer in
a window around the object, `operator<<` appends to whichever buffer contains the ostream
pointer, `str()` returns a COW `std::string`, and the `__ZTV*`/type_info vtable data symbols are
inert (never dispatched — all stream ops are out-of-line to our shims).

### Rendering ([`host/shims/gl/`](../host/shims/gl/), [`host/games/gangstar/render/`](../host/games/gangstar/render/))

`ensure_gl()` creates an SDL3 window (the chosen resolution, or SDL's borderless full screen over the
monitor), a 32/24/8 double-buffered pixel format and a GL context; verified live on OpenGL 4.6 (NVIDIA RTX 3050 Ti).
`NSOpenGLContext` `makeCurrentContext`/`flushBuffer` map to `SDL_GL_MakeCurrent`/`SDL_GL_SwapWindow`. On Windows the
format comes from a hidden helper window, because SDL can only ask for one by its attributes and `SWAP_COPY` needs
`PFD_SWAP_COPY`: with the format SDL chooses, the driver took over a full-screen window, and screen captures froze
(run 2026-11-04, seen on NVIDIA 617.14). A window the player resizes keeps the screen FBO at the size the game asked
for and `msaa_present` scales it with black bars, as in full screen: the game sets its main viewport only a few times a
run, so it could never follow the client area (measured 2026-11-05, on the Win32 window too: a larger window showed the
old size in its corner). `gl_window_fit` moves a window only for a new size, so the game asking the same one again
leaves the player's size alone; a new resolution, or coming back from full screen, still sizes it. During a drag Windows
keeps the main thread inside `SDL_PumpEvents`, so the game stops; SDL still sends expose events from that loop, and an
event watch (`redraw_watch`, shimsinput.c) presents the FBO's last frame again (`gmv_gl_redraw`), only during the pump, as
mid-frame calls such as `SDL_SyncWindow` send them too and the FBO holds a whole frame only at the pump. **All GL imports are
bound**: a `_gl*` import resolves (lazily, on first call, once a context exists) via
`gl_real` (below), wrapped in a per-function thunk
(`mov eax,fn; jmp gmv_gl_forward`). `gmv_gl_forward` is a generic cdecl→stdcall adapter: it
copies a fixed 64 bytes of args into a fresh frame, calls the stdcall GL function (which pops its
own args), and restores `esp` from `ebp` so the arg count need not be known.

**One GL loader** (since 2026-11-05). Every GL function the host looks up, the game's imports and the enhancements' own,
comes from `gl_real(name)` in [`shims/gl/gl_bind.c`](../host/shims/gl/gl_bind.c), declared in `shims.h`, on both systems.
It is `SDL_GL_GetProcAddress`, which asks the driver first (`wglGetProcAddress`, `glXGetProcAddress` or
`eglGetProcAddress`) and then the GL library. No loader library such as GLAD is used: the host needs names, not
generated prototypes, and SDL already does the lookup. Two additions:

- Some Windows drivers answer 1, 2, 3 or -1 for a name they do not hand out, and SDL 3.4 passes that on as a function
  (it tests only for NULL). `gl_real` counts those as no answer.
- On Windows, a name with no answer is then taken from `opengl32.dll`, where GL 1.1 lives.

Before, Windows asked `opengl32.dll` first and the driver second. Compared on the RTX 3050 Ti over 7 scenarios, every
name got the same function except 13 GL 1.1 ones (`glBindTexture`, `glDrawArrays`, `glDrawElements`, `glGenTextures`,
`glDeleteTextures`, `glTexSubImage2D`, `glPolygonOffset`, the four array pointers and the client-state pair). NVIDIA's
driver hands those out itself, so they now go straight to it instead of through opengl32's jump to it. No name was
left unresolved and no driver gave 1, 2, 3 or -1. On Linux nothing changed: it already went straight to SDL.

### Note on the abandoned "slide" approach

An earlier idea slid the whole image (`tools/genlink.py` → `build/gmv_link*.bin`) but needed
~64k heuristic relocations (1713 unfilled weak-bind slots, 184 unslid jump tables) with
silent-corruption risk. It was dropped in favour of running the image at its own addresses.
`genlink.py` and `gmv_link*.bin` remain only as a fallback and are not used by the current host.

## Running and settings

**Just double-click `GangstarMV.exe`** (it carries the game's icon) — with no arguments it launches
the game directly. The dev build also opens a console window for the stage log (closing the game
closes both); the release build (`host\build.ps1 -Release`) has no console and only writes `cache\GangstarMV.log`.

The staged flags work only in the plain and `-Dev` builds. A release build takes only `--app`.

```powershell
.\GangstarMV.exe                 # no args (same as a double-click): load, patch, bind, then run the game
.\GangstarMV.exe --verify-only   # stop after load+verify, no game code (the old default; --no-run is an alias)
.\GangstarMV.exe --run-init      # run all 451 static initializers (or `--run-init N` for the first N)
.\GangstarMV.exe --run-main      # explicit: run all initializers, then main() → the game runs
.\GangstarMV.exe --app "<project root>\GangstarMV.app"   # if the .app isn't found automatically
```

All runtime files are located relative to the **exe's own folder** (not the working directory), so a
double-click works from anywhere: `gmv_patch.bin` next to the exe, the log in `cache\`, the save and
`GangstarMV.ini` in `savegame\`, and the bundle via `find_app` (next to the exe, then `..\GangstarMV.app`,
the project root). Everything is logged to `cache\GangstarMV.log`. Reports: `[MISSING] ... import <name>`,
`[objc] unhandled message [cls sel]`, `[fault] ...` + `while running ...` — each stops cleanly
with the exact cause.

**Game data (portable).** The game's writable data lives next to the exe, created on demand:
`build\savegame\` holds the save game (`Gangstar2.sav`), the settings (`Gangstar2.preferences`),
and `GangstarMV.ini` (an older copy next to the exe is moved there automatically), and `build\cache\` holds the game's own
sandbox (`home\`, `tmp\`) and the log (`GangstarMV.log`). To wipe progress/settings, delete those
folders.

**Settings: `GangstarMV.ini` (in `savegame\`).** Every host switch, including the bug fixes
(`TEXFIX`, `SWAP_COPY`, `NO_FLICKER`, `VTXFIX`, `CRASH58_GUARD`, `STOPLOCK_FIX`), frame pacing,
audio, mouse, intro and all the diagnostics, is a key in this file. The file is created with
the built-in defaults and a one-line description of each key on first run, so the game behaves
the same with or without it; delete it to restore the defaults. Keys are case-insensitive, the
`GMV_` prefix is optional, `[sections]` only group them, and a blank value means "default".
Unknown keys and malformed lines are reported as `[config]` lines in `GangstarMV.log`, together
with every non-default value in effect. In a plain or `-Dev` build an environment variable
`GMV_<key>` overrides the file for one run, so every `GMV_*` name in these notes is also an ini key. A
release build ignores the environment and leaves out the `[Testing]` and `[Diagnostics]` keys, along
with the code behind them; its `LOG` key sits in a `[Log]` section (item 30). Lookup: `gmv_getenv` in [`host/core/config.c`](../host/core/config.c), which also
holds the key list and the template text. The resolution and full screen are the `[Display]` keys
`SCREEN_WIDTH`, `SCREEN_HEIGHT` and `FULLSCREEN`; the game's own Video options write them there
the moment they change (shimsfnd.c's NSUserDefaults shim). An older build's `gmv_prefs.txt` is
moved into the ini on the first run and deleted. In full screen the game draws at the chosen
resolution, as on the Mac (its `setBackingSize::` sets the GL surface to it and `fitWindowSize`
switches the display to that mode): the screen FBO has that size and each present scales it into
the borderless window with its shape kept and black bars (`gmv_gl_render_size`,
`gmv_gl_view_rect` and `msaa_present` in `shimsgl.c`; the mouse maps through the same rectangle).
The screen FBO is therefore made at any `MSAA` (without samples at 0).

`ORIGINAL_MAC_EXPERIENCE=1` (section `[Preset]`) plays the game as on the Mac without editing the
other keys. The table is `mac_preset` in [`host/games/gangstar/settings.c`](../host/games/gangstar/settings.c), and it goes ahead of the
file and, in a dev build, of any `GMV_*` environment variable:

- **Stay on:** `TEXFIX`, `SWAP_COPY`, `NO_FLICKER`, `VTXFIX`, `CRASH58_GUARD`, `STOPLOCK_FIX`,
  `FAST_FTELL`, `FAST_FSEEK`, `FAST_MEMMOVE`, `STATIC_VBO`, `DPI_AWARE`, and since 2026-11-05 the loading
  speed-ups `LOAD_SLEEP_SKIP`, `LOAD_BUDGET` (200) and `FAST_LOAD`.
- **Set to the Mac's:** `MSAA` 4, `DRAW_DISTANCE` 1, `FOG_MODE` 0, and `TRAFFIC_DENSITY`,
  `PEDESTRIAN_DENSITY` and `PARKED_CAR_DENSITY` 1.
- **Set to their defaults:** `FRAME_MS` and `SWAP_INTERVAL` (60 ticks/s, vsync off), `FOG_HEIGHT`.
- **Forced off:** `PHYSICS_SYNC`, `CAMERA_SYNC`, `WALK_SYNC`, `AI_SYNC`, every fix of an original-game bug
  (`MENUFLASH_FIX`, `FADE_EDGE_FIX` and `MENU_FADE_CLICKS` since 2026-11-05, `RADIO_LOGO_FIX`, `SWITCH_SAVE_FIX`), `SOLO_CAR`,
  `ANISOTROPIC_FILTERING`, `AMBIENT_OCCLUSION`, `BLOOM`, `WATER_REFLECTIONS`, `VEHICLE_REFLECTIONS`,
  `DYNAMIC_SHADOWS`, `SUN_MOON`, `SPAWN_FADE`, `HIDE_MORE_GAMES`, `HIDE_GAMELOFT_LIVE`, `SHORT_WARNING`, `CUTSCENE_KEYS`,
  `TUTORIAL_KEYS`, `ALLOW_WEAPON_SPRINT`, `MINIMAP_SHOPS`, `MISSION_REPLAY`, `PAD_ICONS`, `PAD_LAYOUT`,
  `PAD_MENU`, `PAD_HINTS`, `KEY_MOVE`, `PLAYERSWITCH_INVENTORY`, `LC_SWITCH`, `UC_CRUSHER`,
  `CUSTOM_RADIO`, `RADIO_SONGS`, `NO_INTRO`, `SHOW_CURSOR`.

Each value it overrides is logged as a `[config] ... overridden` line. `FULLSCREEN`, the screen size,
`INTRO_FLIP`, `NO_AUDIO`, the audio-buffer, diagnostic and testing keys are left as set.

`GAMEPATCH_TAB` is on by default, and blank or a missing key counts as on (`gmv_config_tab` in
[`host/core/config.c`](../host/core/config.c)). Under `ORIGINAL_MAC_EXPERIENCE=1`, where the tab holds only that
switch, the hooks of the enhancements are not in at all, and the collision tables and spawn pool are the
Mac's. The switch needs no new start (`mac_hooks_in`, `mac_hooks_out`, `mac_features_in` and
`mac_features_out` in [`host/games/gangstar/patch/`](../host/games/gangstar/patch/)):

- Going off, the hooks go in, the running game's collision tables grow (`gmv_bp_grow` in
  [`host/games/gangstar/features/broadphase.c`](../host/games/gangstar/features/broadphase.c)), the pool moves and gets its extra copies, which can take
  a moment.
- Going on, the bytes the game shipped with come back at once, with these exceptions: `DRAW_DISTANCE`
  follows after the next cell update, a copy still out in the city is let go when it returns (item 27), and during
  a mission replay the other hooks stay until it ends. The collision tables of the world in use shrink to
  the Mac's sizes after that cell update, as soon as what is in use fits them with room to spare
  (`gmv_bp_shrink`).

What stays in under the preset: the fixes it keeps on, and what the switch itself needs, since these are
built once at the start: the options tab, the SERVICES menu's replay tab (hidden), and the menu file's
additions. Those are the tab's lines and the Custom Radio rows (below the stage, the game's rows where the
file has them).

`UC_CRUSHER` and `RADIO_SONGS` follow the switch and their menu lines at once, because with the add-on
there, their files are read at every start, whatever the keys say and under the preset too. (The user's
choice, 2026-11-03, so that neither ever needs a new start. A game started under the preset then behaves as
the Mac's, with no truck, hooks or new songs, but with the add-on there it has read the longer sound, station
and type lists and the merged animation bank. Without the add-on it reads the Mac's files.) Going on, the
truck leaves the city once no menu is up, and a player at the wheel carries on in a D340 (the Mac's `Pickup`)
at the same place and speed.

`GAMEPATCH_TAB=0` leaves all of it out.

`[Graphics]` holds the anti-aliasing the Mac game used and optional enhancements beyond it.

`MSAA=4` (the default) restores the Mac's multisample anti-aliasing. `-[EAGLView
initWithFrame:shareContext:]` (0x199bd0) asks `NSOpenGLPixelFormat` for Multisample,
SampleBuffers 1, Samples 4 (the list at 0x5f6de0) unless `-[GameDelegate isOldDevice]` picks the
list without them (0x5f6e40). That flag is set at 0x19e06e only for eight early Intel-graphics
models in `hw.model` (Macmini1,1 to iMac10,1), so the Mac version ran with 4x MSAA on any other Mac,
and so does the port, whose `sysctl` reports no model. The port's window format was
single-sampled, so the anti-aliasing was lost until this setting. The game's screen is now a
multisampled FBO of the window's size (`msaa_*` in [`host/shims/gl/`](../host/shims/gl/)), with RGB8
colour like the window format and depth 24/stencil 8. Binding framebuffer 0 binds that FBO, and
the binding queries still answer 0. Each present resolves the FBO and copies it to the back buffer,
and the FBO keeps its contents between presents as the Mac back buffer did. The game's
`glReadPixels` and `glCopyTexSubImage2D` of the screen read the resolved copy. `MSAA=0` turns it
off, and 2 or 8 pick another count, capped at `GL_MAX_SAMPLES`. `[msaa]` lines log the count and
screen size.

**MSAA 8 on built-in graphics (2026-11-09).** The user's laptop runs the game on an AMD GPU built
into the processor. With `MSAA=8` and all enhancements on, the game sometimes stopped. The log
showed one frame of about 5 s. Then the driver dropped the game's GPU memory, and the game hung,
or a NULL `glGetString` crashed `strstr`. That matches Windows resetting a GPU that took too long
(TDR). VRAM was not full: the 8x screen with its depth and glow layer is about 200 MB. Eleven
scripted drives (`zz-msaa-*` scenarios, deleted after the hunt) narrowed it down:
- Hangs and crashes came only with ambient occlusion on, in 3 of 6 runs. The AO pass reads the
  depth of every pixel many times, and at 8x each pixel has 8 samples.
- AO off: 4 of 4 runs passed, but with the glow layer on, some screenshots were black except
  for the glow, HUD included.
- `MSAA=4` with everything on: 20 minutes clean, no black frame, longest frame 0.25 s.

Nothing was changed in the code. This is neither an original nor a port bug: the chip is too slow
for that much work a frame. The ini description, FIXES and README now say to keep 4 on built-in
graphics. The preset is not affected (it uses 4).

`ANISOTROPIC_FILTERING=N` (2-16) sharpens textures seen at a shallow angle, such as roads and walls
in the distance. The engine already sets `GL_TEXTURE_MAX_ANISOTROPY_EXT` per texture to
min(the texture's level, the GPU maximum) in its texture-state code (0x501010), and that stays the
default. The setting wraps `glTexParameterf`/`glTexParameteri` (`af_*` in
[`host/shims/gl/`](../host/shims/gl/)) to raise every level the game sets, and every texture given a
mipmapped minifying filter, to at least N, capped at the GPU maximum. `[aniso]` lines log the level
in effect and the levels the game itself asks for.

`AMBIENT_OCCLUSION=1` adds screen-space ambient occlusion, which the Mac version never had. It is
applied between the 3D world and the 2D layer, so the HUD, map and text are not darkened (`ao_*` in
[`host/shims/gl/`](../host/shims/gl/)). The engine is fixed-function and loads its projections with
`glLoadMatrixf`. The only `glOrtho` call, at 0x19a6fd, belongs to the EAGLView. So the switch is
read from the projection matrix: a perspective matrix has w depending on z. On the screen, after
enough depth-tested perspective draws, the AO goes in before the first draw under a
non-perspective projection. If the frame has no 2D layer, it goes in before a depth clear or the
present instead. It runs at most once per presented frame, so the frames the game keeps on screen
between draws, as in fades, are not darkened twice. The pass works in four steps:

1. Copy the depth buffer to a texture.
2. Compute AO at half resolution from view-space positions, using the inverse of the scene's
   projection. The engine can draw parts of a frame under different projections and depth ranges
   (`setDepthRange` calls `glDepthRange`), so one depth buffer can hold several layers. Each
   depth-writing perspective draw is counted under its projection and depth range. The AO uses the
   pair most draws had and leaves pixels from the other layers alone.
3. Blur it twice without crossing depth edges.
4. Multiply it into the screen with depth-aware upsampling.

The AO fades out with the game's fog. All the GL state the pass touches is saved and restored.
`AO_STRENGTH` (default 1.2) scales the darkening, and `AO_RADIUS` (default 60) sets the reach in
world units, which are about 1 cm (the scene's near plane is 50 and the fog starts at 45000). `[ao]` lines log the scene's near and far planes, the fog, and where the pass was
applied.

`BLOOM=1` (LOW in the options menu) adds a soft glow around the bright parts of the 3D world, such
as neon, street lights, headlights and the sun. `BLOOM=2` (HIGH) also makes the game's light sprites
and the lit windows on the buildings glow, at `BLOOM_LIGHTS` and `BLOOM_WINDOWS` (blank = 1, 0 = not
at all). The Mac version never had it. It runs at the same point as the AO, after
it, inside the same state save (`bl_*` in [`host/shims/gl/`](../host/shims/gl/)):

1. Resolve the screen's colour and depth to textures.
2. Run a bright pass at half resolution, with a soft knee above `BLOOM_THRESHOLD` (default 0.75,
   0-0.95). Each tap is weighted down by its brightness, so single bright pixels do not flicker.
   On the world the brightness is the brightest channel, so saturated neon glows as much as white.
   On the sky it is the dimmest channel, so the sun and its glow bloom but a sunset or a blue sky
   does not wash out to white. A pixel counts as sky when it is at the far end of the depth range
   most draws used, or beyond it.
3. Spread the glow over six levels: dual-filter downsamples, then tent upsamples, each added onto
   the level above.
4. Screen-blend the sum into the frame (1 - (1 - frame)(1 - glow)) at `BLOOM_INTENSITY` (default 1.0),
   with a dither against banding.

The game draws in 8-bit colour, so the glow comes from how bright a pixel is on screen, not from
light values. With only the brightest channel, a sunset sky turned near-white, which is why the
sky is judged on its dimmest channel. `[bloom]` lines log the size, the number of levels and the
settings.

`WATER_REFLECTIONS=1` makes the sea reflect the city and the sky. Neither the Mac nor the iPhone
version has any reflection in its sea: it is a translucent textured sheet at height 0 that writes no
depth. The reflections are screen-space, and they run at the same point as the AO, before it and
the bloom (`sea_mark_*` and `ssr_*` in [`host/shims/gl/`](../host/shims/gl/)):

1. Each chunk of sea sets a stencil bit where it is drawn. A chunk is alpha-blended, writes no
   depth, lies flat at sea level and is at least 50 m across. The game's stencil state is saved
   around the draw.
2. On those pixels the water point is where the eye's ray meets height 0. Pixels where something
   drawn is nearer than that point are left alone. Four travelling waves ruffle the normal, fading
   out where a wave would be only a few pixels long.
3. The reflected ray is marched across the screen against the depth of the city's near pass,
   perspective-correct, with a binary search at the first crossing. A hit takes its colour from the
   frame as drawn so far.
4. A ray that hits nothing takes the screen where its direction meets the sky. Off screen it takes
   the sky's colour at eye level (with `FOG_MODE` 1 or 2), or fades out.
5. The reflection is mixed in by Fresnel's law for water: 2% looking straight down, rising towards
   grazing angles. It is scaled by `REFLECTION_STRENGTH` (default 1.0, 0-2) and thinned with the
   sea's own fog.

Only what is on screen can be reflected, so the tops of tall buildings above the screen and
anything behind the camera are missing. `[ssr]` lines log the passes.

`DYNAMIC_SHADOWS=1` draws shadows that follow the sun and the moon, in place of the game's own, a ready-made
shadow-mesh under each model that is the same at any hour (`ds_*` in [`host/shims/gl/`](../host/shims/gl/),
`gmv_ds_node` in [`host/games/gangstar/patch/`](../host/games/gangstar/patch/)):

1. **The game's own go.** The vehicle update (0x112bc4) and the character update (0x76090) set the shadow-node's
   visibility each tick while it is visible. Those two sites (0x112c3a, 0x775db) now hide it and keep it in a
   list. Once the new shadows stop, the nodes whose owners the world or a pool still holds are shown again.
2. **What casts.** Those same updates give the owner, so every scene node under it goes in a table (two tables,
   swapped every 30 frames). A draw is a caster if the node being drawn is in the table (`gmv_ds_caster`, through
   the `SPAWN_FADE` list hooks).
3. **The map.** Casters draw into a 2048x2048 depth map around the camera, 60 m each way.
4. **The pass.** It runs first in `ao_run`. Each pixel's position comes from the frame's depth, as for the AO, and
   is looked up in the map with 16 filtered taps. Casters set a stencil bit and take a larger bias, because the
   bias a car's own surface needs would slide its shadow away from the feet of whatever stands on the ground.
5. **The light.** The sun's strength is 0.45, the moon's 0.30. The sun is above the 3% line from about 5.05 h to
   16.9 h and the moon from about 18.1 h to 3.2 h, so for part of the day neither is up. A low light's direction is
   drawn towards straight up and its strength towards 0.40, so as it sets the shadow shortens to a soft one under
   the thing, with no second map, and the next light draws it out again (`ds_light`).

Buildings do not cast, so a car under a bridge still gets a sun shadow. A vehicle off screen casts nothing. People
without a shadow-mesh cast only through their shiny-draw test. Moon shadows
also show under street lights. `[shadow]` lines log the passes: casters, source, direction, hour and strength.

The frame's first caster gives the map its view. When that caster's view cannot be found (`ds_begin`), it stays out of
that frame's map and the next caster is asked, so it costs that one shadow for one frame, or every shadow if no caster
gives a view. In a run with about 14400 maps, 1123 casters in 838 frames gave no view. For 204 of them, in 198 frames,
the node was not in the table and the caster was within the map's 96 m; the nearest was 11 m. 13 frames had no shadows
at all, 2 of them with such a caster within 96 m. One of those frames had 69 casters, none with a missing node, and is taken
to be a camera cut. The user saw nothing odd in an earlier run of the same code path. Dev builds print a summary of
this at most once a minute, and when the Mac switch puts the game's own shadow back, the log gives the reason for each
node left out.

`SUN_MOON` brings back the sun and moon of the iPhone version (Gangstar2, the same engine): `2`
(IPHONE) draws the glow as the iPhone does and `1` (ENHANCED) adds the depth handling and the gain
described below. The Mac game has no sun or moon in the sky, but most of the iPhone code and all four textures
(`add-sun.tga`, `add-moon.tga`, `add-glow.tga`, `add-glow-moon.tga`) are still in it. The anchors
in `sun.bdae` cross the sky with the time of day. The time-of-day update (0x142892) still places
two glow billboards towards them, and the skybox render (0xbe468) still draws a sun disc and a moon
disc around the camera, hidden below the horizon. Two pieces were removed. The skybox constructor
never makes the discs, and the update hides both glows every tick, where the iPhone shows the sun
glow by day and the moon glow at night (time state 3). The setting puts both back (`apply_sun_moon`
in [`host/games/gangstar/patch/`](../host/games/gangstar/patch/)). Each disc is made as the iPhone makes it, a
1024x1024 billboard of the texture, and the game's own tweaks "draw skybox sun" and "draw skybox
moon" still switch each one. Then the glow for the time of day is shown. A `[sunmoon]` line logs
the discs made for each skybox.

The glow's material has no depth test, and it is drawn after the world in the world's depth range,
so as shipped it would shine through buildings. The discs come before the world, since the skybox
draws them, but the skybox draws them last among its layers: after `sky-mesh` and after
`farcity-mesh`, the distant skyline. They have no depth test either, so the moon would sit in front
of the skyline. The iPhone's skybox render (0x2bfef4) draws the discs before the skyline, so both
modes replace the skyline draw (0xbe9f1) with a call that draws the discs
first and the skyline over them, then skips the game's own disc draws. The skyline texture is a
cutout (sky fully transparent, buildings opaque), so the moon disappears only while it is behind a
building. Its arc peaks at about 27° above the horizon, so around moonrise and moonset it is
often behind the skyline.

The iPhone's glow uses the same material as the Mac's: material 8 of `DefaultEffects.bdae`, "Unlit
Textured Additive Blend No Depth No Culling", and the file's contents are identical in both versions. So
`SUN_MOON=2` keeps that material's own draw, glow through buildings included, with the Mac's texture as it
is. The Mac's glow textures are DXT5 with visible rings and the iPhone's PVRTC ones have none, so the rings
stay in this mode; the smoothing described below is `SUN_MOON=1`'s only. It has not been compared with a
real iPhone.

With `SUN_MOON=1`, the billboard render (vtable 0x676a70) is
wrapped so that a glow's draw can be told apart. Its fixed-function shading (texture times vertex
colour, added ONE/ONE) is replaced by a small shader that does the same and reads a copy of the
depth drawn so far. Where that is sky (the far end of the world's range or beyond), the glow is
drawn, at 1.6 times the material's brightness (`GLOW_GAIN`). Where it is the world, the glow leaks
a little way in past the silhouettes, like light wrapping around an edge, and fades out 3% of the
screen height inside them (`GLOW_WRAP`). Thin
things like poles and rotor blades get most of it. The glow textures are DXT5, whose 5- and 6-bit
colours turn the gradient into about a dozen visible rings. So the first time each glow is drawn, its
texture is read back, blurred in floating point by a Gaussian of about 1/60 of its width, and kept as
a 16-bit copy that the shader reads instead. The shader also dithers its output by less than one
8-bit step, so the screen's own 8 bits per channel cannot make new rings. If the shader cannot be used, a plain depth
test (LEQUAL, no depth writes, all fragments at the far end of the world's range) hides the glow
behind the world with a hard edge ([`host/shims/gl/`](../host/shims/gl/), `glow_begin`).

The glow counts the skyline as sky, since the skyline is drawn in the sky's depth range, so it
stays when the moon is behind a skyline building. At night those buildings are dark navy, and the
glow added over them looks like plain sky. The result is a glow with no moon in it.

`DRAW_DISTANCE=N` (more than 1, up to 4) draws the city N times as far as the Mac version. How far
the game draws comes from a device profile, `Structs::PhonePerformanceProfile`, read at start-up
(0x1a8c5e) from `_packMac/file002743.dat` into 0x949a00. The file has two profiles. The phones'
profile is 15000/30000, and the Mac takes the other one:

- `perfFarHighRes` 45000 (about 450 m) is the full-detail city. It sets the camera's far plane each
  frame, the start of the fog and the window of full-detail cells streamed around the player
  (1.5 times the distance).
- `perfFarLowRes` 90000 is the far end of the low-detail band drawn behind it and of the fog.

The setting scales both right after the read, before the city is built, so all of these move
together (`apply_draw_distance` in [`host/games/gangstar/patch/`](../host/games/gangstar/patch/)). The profile also sets
how far away cars and pedestrians are removed (1.5 times `perfFarHighRes`, at 0x76c59). That check
is pointed at a copy of the Mac's profile, so traffic comes and goes as on the Mac. A
`[gamepatch] draw distance` line logs the distances in effect. The world's share of the depth
buffer stays the same, so the further it reaches, the coarser the depth steps at the far end.

The buildings' collision consists of Box2D shapes. The city grid (`CCustomBatchGridSceneNode`)
creates them for the objects in the cells within `perfFarHighRes` of the camera, so the setting
affects collision in two ways.

The first is the starting rectangle. `updateCells` (0x13d5a4) adds a cell's objects when the cell
comes into range. It decides that by comparing with a rectangle around the last position (+0x1a4),
which a new or reset grid sets to (0,0,0). On the first update, every cell within that distance of
the world origin therefore counts as already added. At 3 times the distance this covers most of
the city, and those buildings get no collision; you walk through them. The origin collision fix
(item 13 of the fix log) mends that and is applied only with this setting. The Mac
game has the same starting rectangle, but it only covers about ±225 m around the origin.

The second is table size, since a window N times as wide loads N² times as many shapes. Right after
the profile read (0x1a8c79), the game sizes the Box2D broadphase from `perfFarHighRes`. Above 7500
it gets 16384 proxies (0x649680) and 131072 pairs (0x649684, also the size of the pair hash table).
The menu scenes alone come close to 16384 at 3 times, and neither table checks for being full: more
shapes would overwrite the heap. The setting therefore also:

- scales both sizes by N², rounding the pairs up to a power of two;
- guards the two `b2BroadPhase::CreateProxy` calls, so a full pool gives the shape no proxy (as
  Box2D does for a shape outside the world) and logs `[gamepatch] collision proxies full` instead
  of corrupting memory;
- logs `[gamepatch] collision in use` lines at 1024, 2048, 4096, ... proxies, showing how full the
  tables get.

The fog's distances follow `DRAW_DISTANCE` (an enhancement; the game's own under
`ORIGINAL_MAC_EXPERIENCE`): with `FOG_MODE` 1 or 2 from 100 m to 800 m at 1x, with `FOG_MODE` 0
from 450 m to 900 m at 1x, the end 50 m further for each 0.5x more. The game sets linear fog
every tick in its time-of-day update (0x142892):
- **Start:** `perfFarHighRes`, where the full detail ends.
- **End:** the low-detail far distance (0xb5a00) plus 2/3 of the low-detail band.
- **Values:** 450-1200 m on the Mac, 1350-3600 m at `DRAW_DISTANCE=3`. The fog is 60% dense where
  the city's geometry ends, which hides that edge.

The pair goes to the shader parameter setter 0x54cec0 through the call at 0x142d5a, which
`apply_fog` redirects to substitute ours, from the draw distance in force, so a change in the
options menu moves the fog at the next tick. `[gamepatch] fog` logs the distances and the game's own
at the first tick and after each change. Past the fog's end nothing can be seen.

`FOG_MODE` picks how the fog is drawn (an enhancement; `0` under `ORIGINAL_MAC_EXPERIENCE`):
- **0:** the game's own fog, on only some materials, as on the Mac, at the distances above. This is the default.
- **1:** linear fog over the whole city, as below.
- **2:** the hybrid fog (below), with `FOG_HEIGHT` for its thinning with height.

With `FOG_MODE` 1 or 2, the fog covers the whole city. In the game, fog is a per-material
switch: bits 12-13 of the render-state word, read at 0x4fc871, where 0 means `glDisable(GL_FOG)`.
Most of the city's materials have it off, on the Mac too: buildings, roads and the sea.
- **What gets the fog:** `fog_force_begin` in shimsgl.c draws every perspective, depth-tested draw
  to the screen that the game left unfogged with the game's linear fog.
- **What stays clear:** the sky dome, the sun and moon discs, the glows, the 2D layer and the game's
  own framebuffers. For the sky, gamepatch.c wraps the skybox render 0xbe468 (vtable entry 0x6522d8)
  to mark its draws.
- **Blended draws:** additive ones fade towards black and multiplying ones towards white, so they
  thin out rather than turn into fog-coloured shapes.
- **Fog colour:** the game's fog colour is a near-white that drifts away from the sky at dusk and
  at night, so the fogged city showed as a pale band against the sky. The fog now takes the sky's
  own colour at eye level instead, for both the forced fog and the game's own.
  - **Where it comes from:** the sky gradient is a wall kept in front of the camera. Its vertex
    colours change with height and are symmetric about eye level.
  - **How it is read:** `sky_read` reads those colours and the wall's position as the sky is drawn,
    every frame.
- **Where the world ends:** past `FOG_END` the city is only fog-coloured shapes, and they covered
  the distant skyline's foot. When the fog ends before the drawn city does, world draws to the
  screen are clipped at the fog's end, at the depth from the eye that the fog counts (a user clip
  plane in eye space, `GL_CLIP_PLANE5`). This also spares the GPU the hidden city.
  - **Log:** `[fog] the world ends at ... m`.
- **Skybox height:** the skybox render (0xbe468) places the sky gradient and the distant skyline
  around the camera at height 0, which is the sea, 600 m out.
  - **The problem:** from a helicopter the skyline sinks below where the sea fades out, about 14°
    under the horizon at 150 m up. The city drawn out past it then rises over the skyline with its
    square edge. Lifted to the camera's height, the skyline floats over a band of haze instead.
  - **The fix:** with the whole-world fog, the instruction at 0xbe5c6 that zeroes the height goes to
    a trampoline that stores the camera's height × (1 − 600 m / D). D is where the world is seen to
    end: the fog's end, or the city's far end if that comes first. The skyline's foot, 600 m out, is
    then seen at the same angle as the sea D away, so it stands where the fogged sea ends at any
    altitude. On the ground it is at sea level, as on the Mac.
  - **Log:** `[gamepatch] skybox raised with the camera`.
- **The sea past the map's edge:** the city's sea fills a box from x −1207 to 1196 m and y −1104 to
  1461 m, and stops there. Near the map's edges and from the air that edge was in view well inside
  the fog, a straight line with nothing past it.
  - **The fix:** after the first chunk of sea on the box's edge in each pass of a frame, the sea goes
    on past the box out to beyond the camera's far plane. It uses that chunk's state, the edge's deep
    blue, and the texture laid as the sea lays it (fitted to the chunk's own texture coordinates, so
    it scrolls with them). The fog then takes it into the sky's colour.
  - **Per pass:** the city is drawn in a near pass and a low-detail far pass, each with its own depth
    range. At first the sea was carried on only once per frame, and that first chunk could be the
    far pass's. Then the whole extension lay past the fog's clip and the box's edge showed again, a
    slanted line across the sea about 70% into the fog (`DRAW_DISTANCE=2`, `FOG_END=800`).
  - **Log:** `[fog] the sea goes on past the map's edge`. The code is `sea_beyond` in `render/fog.c`.
- **Below the horizon:** the sky gradient repeats the sky upside down below eye level, which showed
  as an orange or dark void wherever no world was drawn. Those colours are now the fog's colour, as
  fully fogged sea would be.
- **Distant skyline (farcity):** it has no real distance. The mesh is a half cylinder around the
  camera with Z up and its foot where the world is seen to end, and it lies past the end of the drawn
  city.
  - **At its foot:** it takes the fog of the city's far end, so the city's fogged edge runs into it
    without a seam. gamepatch.c works out where the city ends from the game's own fog end:
    far = (3·end + 2·start)/5.
  - **Higher up:** the fog thins to 60% of that (`FOG_FARCITY`) by about 8° above the horizon
    (`FOG_FARCITY_RISE`), so the tall buildings stand out of the haze. Its colour there turns to the
    sky's right behind each point, found by taking the point's direction into the sky's own space
    and following it to the wall. Below eye level that is the fog's colour.
  - **How it is drawn:** a small shader repeats the game's material (unit 0 texture × vertex colour;
    the alpha test stays fixed-function) and adds the fog. Without shaders, the skyline gets 60%
    throughout through a constant fog coordinate.
- **Log:** `[gamepatch] fog on the whole world` and `[fog] the skyline: ...` confirm it.

The hybrid fog (`FOG_MODE=2`) answers two faults of the linear fog:
- **It counts depth along the view:** the fog is the same at a screen edge as in the middle, so it
  shifts as the camera turns.
- **It stops dead at its end:** the end shows as a line where the fogged sea meets the skyline.

Each fogged material is drawn through a fragment shader instead (`fog_hybrid_begin` in shimsgl.c).
The fixed-function vertex stage keeps the game's lighting and texture matrices, and the alpha test
and blending stay as they were.
- **Distance:** the true distance from the eye at each pixel, from its depth (`gl_FragCoord` through
  the inverse projection, as the AO does). It is right across the sea's triangles, some of them
  almost 1 km across.
- **Curve:** an Exp2 curve fitted between `FOG_START` and `FOG_END`:
  clear = (e^(−a·t²) − e^(−a)) / (1 − e^(−a)) × (1 − t⁸). Here t runs from 0 at the start to 1 at
  the end, and a = 3 at sea level. It eases in at the start and reaches complete fog at the end
  without a kink.
- **Height:** a scales with the mean of e^(−height / `FOG_HEIGHT`) along the line of sight
  (default 60 m; 0 = the same at every height). The sea and the streets haze sooner than the tops of
  the towers, but every height is complete at `FOG_END`.
  - **Camera height:** the skybox trampoline also stores it (`g_cam_z`).
  - **Up direction:** taken from the sky's orientation (`sky.inv`).
- **End:** nothing is drawn past `FOG_END`. The clip plane counts depth, which reaches further at
  the screen's edges, so the world ends on the circle where the skyline's foot stands.
- **Materials:** three shaders cover every setup a survey of the city's draws found (`FOG_SURVEY`, a dev
  diagnostic, taken out 2026-11-06 once its work was done):
  - plain colour;
  - unit 0 `MODULATE`;
  - unit 0 `MODULATE` plus unit 1 `COMBINE`, which adds its colour and multiplies its alpha (lit
    windows and reflections).
- **Texture environment:** each unit's is tracked through the `glTexEnv*` calls, so picking a
  shader asks GL nothing.
- **Fallback:** any other setup keeps the linear fog, logged once as
  `[fog] hybrid fog: a setup without a shader ...`.
- **Log:** `[fog] hybrid fog, frame ...` counts the draws each way every 10 s.

## Source layout

`host/` has a part that knows no game and a part for each game:

```
host/
  CMakeLists.txt      the build, for Windows and Linux; CMakePresets.json names the variants, cmake/ the 32-bit
                      Linux toolchain
  build.ps1           calls CMake for one game: -Game <folder under games\>, gangstar by default
  linux/              build.sh and build_stage1.sh, which call CMake too; stage1_stubs.c
  core/               the loader: Mach-O, image, imports, exception handler, log, settings file
  shims/              the macOS libraries, for any game
  shims/gl/           the GL window, its present and the GL imports
  games/gangstar/     layout.h, game.c, settings.c
    patch/            what is patched into the game's code, a file for each feature
    render/           the graphics enhancements, a file for each effect
    features/         the larger speed-ups and add-ons
  games/template/     a game with no patches, the starting point of a new one
  third_party/SDL3/   from tools\get_sdl3.ps1, not in the repo: official 32-bit build, one pinned version, checked by hash
```

[`host/core/game.h`](../host/core/game.h) is everything `core/` and `shims/` ask of a game: the settings table, the
patches at the start, and hooks for files, sound, input, the frame and each GL draw. A hook is an ordinary function,
called directly. Nothing else crosses the line: `core/` and `shims/` hold no game address in their code, and
`build.ps1 -Game template` links them with no Gangstar source, which the build check repeats. Their comments still
cite Gangstar's addresses where they explain why a shim is as it is.

### Building with CMake

`host/CMakeLists.txt` is the only build. `build.ps1`, `linux/build.sh` and `linux/build_stage1.sh` keep their old
arguments and only call it (build folders `build/cmake/<game>-<kind>`, `linux-` in front on Linux); `BUILD=dev` or
`BUILD=release` before `linux/build.sh` gives Linux the dev and release builds, and `-DGMV_STAGE1=ON` is the loader
alone. CMake 3.25 or newer for the presets, and Ninja (`linux/*.sh` fall back to make without it). Directly:

```
cmake --preset windows            cmake --build --preset windows            (like build.ps1)
cmake --preset windows-dev        ...                                        (build.ps1 -Dev)
cmake --preset windows-release    ...                                        (build.ps1 -Release)
cmake --preset linux              cmake --build --preset linux              (like linux/build.sh)
```

`-DGMV_GAME=<folder>` picks the game (gangstar by default on both systems), `-DGMV_NOICON=ON` leaves the icon out and
`-DGMV_OUT_DIR=<folder>` sends the program elsewhere; the presets' build folders go under `build/cmake/`. On Windows
the presets and `build.ps1` put MSYS2's `mingw32\bin` first on the PATH (`pacman -S mingw-w64-i686-cmake
mingw-w64-i686-ninja`); on Linux `cmake/linux-i386.cmake` adds `-m32` (`apt install cmake ninja-build`).
`compile_commands.json` in the build folder serves editors. `tools/linux_check.ps1` stays a separate check: clang and
a Debian sysroot from Windows.

Checked when CMake took over (2026-11-05), against the scripts' own gcc commands: on Windows the release, dev and
template builds are the same bytes apart from the PE time stamp and checksum, and the plain build the same after
`strip` (the source order is the old `build.ps1`'s, case-insensitive). The old `linux/build.sh` took the game's folders
in `find`'s order, so on Linux the link order and a few bytes of padding differ from it; the symbol set is the same,
neither holds a local path, both reach the menu with the same log apart from addresses and timing, and two CMake builds
of one source are the same bytes but for the build id. The stage-1 loader verifies with the same log as before.

The CI workflow (`.github/workflows/build.yml`) builds through CMake in two jobs, with no game files and any warning a
failure: Windows (MSYS2 mingw32: gangstar and template, release and dev) and Linux (a `debian:trixie` container, as
Ubuntu's runner has no 32-bit SDL3 development files: the same four and the stage-1 loader, against the system's SDL3
3.2.10 headers since `third_party/` is not in a checkout).

### Adding a game

1. Copy `games/template` to `games/<yourgame>` and fill in its `layout.h`: the end of the binary's `__LINKEDIT`, the
   names, the Objective-C classes, `__text`.
2. Make the patch table: `python -I tools\genlow.py <binary> build\<yourgame>\gmv_patch.bin`.
3. `host\build.ps1 -Game <yourgame>`, then `<yourgame>.exe --verify-only` reports the load and lists the imports with
   no shim. Add those to `gmv_game_host_symbols` in the game's folder, or to `shims/` if any game could use them.

The game must be a non-PIE i386 Mach-O. This was tried with the binary of one other Mac game: it loads, 44961 low-range
patches apply and verify, and 407 of its 581 imports bind. Doing that led to two changes here: rebase info is ignored,
as nothing slides, and `genlow.py` can end the low range at a called function when the first named one is too far. That
game has not been run on this host. Only Gangstar has.

### Towards Linux

The aim is one source for Windows and Linux. Done so far: [`shimsaudio.c`](../host/shims/shimsaudio.c),
[`shimsjoy.c`](../host/shims/shimsjoy.c), [`shimsinput.c`](../host/shims/shimsinput.c) and
[`shimscg.c`](../host/shims/shimscg.c) use SDL3 only and include no Windows header, and the window and GL context are
SDL3's.

The loader builds for both. [`core/platform.h`](../host/core/platform.h) is all `core/` asks of the system, in UTF-8
paths, answered by `platform_win32.c` and `platform_linux.c`; [`fault.c`](../host/core/fault.c) says what a fault means,
and `veh.c` or `fault_linux.c` get it from the system. On Windows the game's range is a section of the exe, by
`gmv.ld`. On Linux the program is an ordinary non-PIE one linked at 0x08048000, and maps the range with
`MAP_FIXED_NOREPLACE` as it starts, so no link script is needed; the link address matters, since lld's default of
0x400000 lies inside the range. `vm.mmap_min_addr` is 0x10000 by default, the same limit as Windows, so the layout and
`gmv_patch.bin` are the same on both. The game cannot use `%gs` on its own, or it would not run on Windows, so glibc's
use of it is no conflict. `crashdump.c` has a Linux counterpart, `crashdump_linux.c`; `loadprof.c` has a part for each.

The shims build for both too. [`shims/compat.h`](../host/shims/compat.h) is `windows.h` on Windows, so that build is
unchanged, and on Linux the same Win32 names over pthreads, `clock_gettime`, `stat` and `dlsym`: an `SRWLOCK` is a
plain mutex there, and a thread `HANDLE` is joined by `WaitForSingleObject`. GL functions come from `gl_real` on both
systems (see "One GL loader"). Where Linux has the Mac's own call, `shimsw.c` has a Linux body beside the Windows one (mmap,
stat, opendir, nanosleep to the microsecond, gettimeofday, localtime, pread). Three things differ for real:

- glibc's `pthread_cond_t` is 48 bytes and the Mac's 28, so on Linux the game's condition holds a pointer to one on the
  heap; the mutex (24 + 4 bytes) still fits the Mac's 44;
- C code on Linux expects the stack 16-byte aligned at each call, so `shimsobjc.S`'s dispatchers make room before
  calling C (`ALIGN_CALL`), and `gmv_missing_import` realigns itself; GL is cdecl there, so a GL thunk jumps straight to
  the driver with the game's own arguments instead of going through `gmv_gl_forward`;
- the build needs `-freg-struct-return`, as the Mac returns small structs in registers (mingw does by default); glibc
  functions that return a struct (`div`) must not be called from the host.

Linux has no decoder that every system carries, so the intro movie and the custom radio's m4a, aac and wma songs come
from the system's `ffmpeg` program ([`ffpipe_linux.c`](../host/shims/ffpipe_linux.c)): the host starts it with `posix_spawnp` and reads
raw frames or samples from a pipe. It runs `ffmpeg` and `ffprobe` as programs rather than loading FFmpeg's libraries, so
a 64-bit `ffmpeg` of any version serves the 32-bit host. The intro takes the movie's size and frame rate from
`ffprobe`, then one `ffmpeg` sends BGRA frames at that rate and a second sends 48 kHz stereo; the queue, the pacing and
the drawing are the Media Foundation path's own (`show_front`, `intro_start`, `intro_end`, `intro_path`). A song goes
straight to 16-bit stereo at the output's rate. Without `ffmpeg` the intro is skipped and the custom station leaves out
those songs (mp3, ogg, flac and wav need no `ffmpeg`: see item 18); the log says why once.

On Linux FAST_FTELL and FAST_FSEEK are off, as glibc already keeps the offset and seeks inside its buffer (a
`LOAD_PROFILE` run with FAST_FTELL on loaded the menu's scene in the same time), and DPI_AWARE has no effect. An NSAlert is an SDL message box with the alert's own
button titles. The help texts in `settings.c` write paths with backslashes; `config.c` writes them with `/` into a
Linux `GangstarMV.ini`, and the words that differ (the program, the home folder as `$HOME` in the log) come from macros
there.

`host/linux/build.sh [game]` builds the host with gcc through CMake (default `template`), `host/linux/build_stage1.sh`
the loader alone with stubs, which stops after verifying with `--verify-only`. Both need `-D_FILE_OFFSET_BITS=64`: without it a
32-bit `stat` or `readdir` fails with EOVERFLOW on a file whose inode number is above 32 bits, which is every file on
WSL's `/mnt/c` and many on btrfs or xfs, and the game is "not found". `tools/get_linux_sysroot.py` (Debian trixie i386:
libc, GL, SDL3 3.2.10) + `tools/linux_check.ps1 [-Game g] [-Stage1]` compile and link the same from Windows with clang
and lld; the SDL3 functions the host uses are all in 3.2.10, or the link would fail.

**First run on Linux (2026-11-05, WSL2 Debian 13, gcc 14.2, Mesa 25.0.7):** WSL2 has to be set up once (`wsl --install
-d Debian` from an administrator, then a restart of Windows), and inside it `dpkg --add-architecture i386` and
`gcc-multilib libsdl3-dev:i386 libgl-dev:i386 libgl1-mesa-dri:i386`. The loader's `--verify-only` log is the Windows
one apart from the lines that are each system's own (the exe's mapping, DEP): the same segments, 4360 patches, 451
initializers and protections. The template host, given the Gangstar binary through a `Template.app` of links, runs the
game to the title and the main menu: its window, GL with 4x MSAA, SDL3 audio, keys, timers and threads all work, and the
fault handler passes its self-test; the imports it lacks are the Windows build's own 49. WSLg has no GPU driver for a
32-bit program (`/usr/lib/wsl/lib` is 64-bit only), so GL is Mesa's llvmpipe in software (about 27 frames a second at
1920x1080).

**`games/gangstar` on Linux (2026-11-05):** its 75 files that included `windows.h` include `compat.h`, and
[`compat_linux.c`](../host/shims/compat_linux.c) answers the few calls they make: `VirtualQuery` from
`/proc/self/maps`, `VirtualProtect` by `mprotect`. Paths it builds use `GMV_SEP`. A Windows path search for `\` had
crashed the Crusher's set-up, and the game's `./` in front of an absolute path in the bundle is now dropped
(`gmv_in_bundle`). The pad pictures draw their letters with FreeType and fontconfig, opened with `dlopen` (Liberation Sans
Bold where the Windows build uses GDI's Arial). Calls from assembly name C symbols through `GMV_ASM_SYM`. Packages:
those above, plus `libfreetype-dev:i386` to build and `ffmpeg` for the intro and the custom radio's m4a, aac and wma. Run in WSL: the
intro (220 frames, upright, its sound played by the log; not heard), the menu, the city on foot and in a car, the pause menu, the Crusher, the radio
songs, a mission replay, L.C., the DualSense pictures, a custom radio song from a file, and every graphics enhancement
(bloom, AO, shadows, the two reflections, the fogs: each logs its first pass as on Windows). Not tried yet: a fault in the
game's own code through the signal handler (`TEST_FATAL`'s faults in the host ran, see the crash dump), seeing the alert
box (the process waited on it after a crash), hearing the custom station in a car, and a real GPU (only a Linux on
the machine itself has one for a 32-bit program). Nothing ties the host to Windows any more: the crash dump and
`LOAD_PROFILE` were the last.

The window icon: when `games/<game>/app.ico` is present, `linux/build.sh` builds it into the program (`.incbin`, as
`app.rc` puts it in the exe; `NOICON=1` leaves it out) and `gl_context.c` hands its six 32-bit pictures to
`SDL_SetWindowIcon`, the 256 px one with the others as its smaller sizes. In WSL the taskbar showed the game's icon.

`LOAD_PROFILE` and `PERF_PROFILE` on Linux: no thread can read another's registers there, so the sampler thread sends
the main thread a real-time signal (`SIGRTMAX - 3`) about once a millisecond, and the handler takes the sample from
the registers it interrupted, with the same stack scan (its one guarded read is `gmv_readable`, a system call). Code is
named by `dl_iterate_phdr` and `dladdr` (`libc.so.6!fn+off`, `linux-gate.so.1` for the vDSO, `host 0x...` for the
program) instead of the export tables. The shims' sleeps loop on EINTR and the handler is installed with
`SA_RESTART`, so the game's calls carry on. The thread id is kept per thread: `gettid` is a system call on Linux,
and calling it for each of a scene's 2.3 million file calls had cost 0.5 s of a 2.3 s tick. In WSL the menu
scene's load is one tick of about 1.8 s (446 ms on Windows on the same machine), mostly the game's small reads of
the game folder on `/mnt/c` (`__syscall_cancel_arch` under `fread`), which crosses to the Windows drive. The
playtest's `profile` scenario checks both profilers' lines on Windows.

On Wayland the cursor does not move under relative mode, so `shimsinput.c` switches modes by the game's own pointer
rather than reading the system's cursor beside raw motion, which is what the Win32 version did.

### Where each thing is

| File | Role |
|---|---|
| [`host/core/main.c`](../host/core/main.c) | `gmv_main`: address-space checks, load, patch, bind, verify; **runs the game by default** (no args = double-click), `--verify-only`/`--run-init`/`--run-main` for staged modes; `find_app`; passes the `.app` path to the bundle stub |
| [`host/core/platform.h`](../host/core/platform.h), [`platform_win32.c`](../host/core/platform_win32.c), [`platform_linux.c`](../host/core/platform_linux.c) | What the loader asks of the system, and the entry point (`wmain`, `main`) |
| [`host/core/config.c`](../host/core/config.c) | The settings file: loaded first thing in `gmv_main` (template written if missing); `gmv_getenv` = environment, then ini, then built-in default. The keys are the game's ([`settings.c`](../host/games/gangstar/settings.c)) |
| [`host/core/macho.c`](../host/core/macho.c) | Read-only Mach-O parser + bind-opcode interpreter |
| [`host/core/image.c`](../host/core/image.c) | Maps segments, runs the low range from its copy, applies & verifies patches |
| [`host/core/imports.c`](../host/core/imports.c) | Collects imports, binds every slot (shim / thunk / data stand-in) |
| [`host/core/fault.c`](../host/core/fault.c), [`veh.c`](../host/core/veh.c), [`fault_linux.c`](../host/core/fault_linux.c) | Low-range redirect, the fault report, init/main fault context; the vectored handler and the signal handlers that feed them |
| [`host/core/runinit.c`](../host/core/runinit.c) | Runs the static initializers (`run_initializers`) and `main()` (`run_main`) |
| [`host/core/log.c`](../host/core/log.c) | The log: lines are buffered until the file opens, and the user's folder is written as `%USERPROFILE%` |
| [`host/core/crashdump.c`](../host/core/crashdump.c), [`crashdump_linux.c`](../host/core/crashdump_linux.c) | The dump `gmv_fatal` writes after a crash, with the log's hidden folders blanked: a minidump on Windows, an ELF core file on Linux |
| [`host/core/loadprof.c`](../host/core/loadprof.c) | `LOAD_PROFILE` and `PERF_PROFILE`: where the time goes in long ticks (Windows and Linux). Left out of a release build |
| [`host/core/thunks.S`](../host/core/thunks.S), [`host/core/reserve.S`](../host/core/reserve.S), [`host/core/gmv.ld`](../host/core/gmv.ld) | The missing-import thunks' common tail; the game's address range as a section of the exe; the linker script |
| [`host/shims/shims.c`](../host/shims/shims.c) | libc shims (mingw CRT); `gmv_shim_lookup` searches **all** tables, the game's too, then `gmv_gl_resolve` |
| [`host/shims/shimsw.c`](../host/shims/shimsw.c) | Win32-backed: pthread, mmap, clocks, time, stdio, `sysctl`, `chdir`/`mkdir`, wchar, `setjmp`/`longjmp`, `__DefaultRuneLocale`, TIS/keyboard |
| [`host/shims/shimscxx.c`](../host/shims/shimscxx.c) | C++ runtime / libstdc++ (`__cxa_*`, RB-tree, `_List_node_base`, `std::allocator<char>`) |
| [`host/shims/shimsstr.c`](../host/shims/shimsstr.c) | `std::string` (COW, libstdc++ 4.2 layout) + `_Rep::_M_destroy` + `gmv_std_string_new` |
| [`host/shims/shimsstream.c`](../host/shims/shimsstream.c) | `std::ostringstream` accumulator (`oss_str_impl`), streambuf get/put accessors, inert vtables |
| [`host/shims/shimssdl.c`](../host/shims/shimssdl.c) | SDL: init + video + `SDL_GL_SwapBuffers`→present; `SDL_PeepEvents` hands over the keyboard events from `shimsinput.c` |
| [`host/shims/shimsobjc.c`](../host/shims/shimsobjc.c) + [`.S`](../host/shims/shimsobjc.S) | Objective-C runtime; also the `gmv_gl_forward`/`gmv_gl_lazy` + `setjmp`/`longjmp` asm |
| [`host/shims/shimsfnd.c`](../host/shims/shimsfnd.c) | Foundation/AppKit/Cocoa method tables (instance / class / struct-return) |
| [`host/shims/shimsappkit.c`](../host/shims/shimsappkit.c) | AppKit C ABI + run loop; AudioUnit, CoreFoundation, IOKit, Gestalt stubs; NS string constants |
| [`host/shims/shimscg.c`](../host/shims/shimscg.c) | CoreGraphics: real display-mode list (`EnumDisplaySettings`), `CFArray`/`CFStringCompare` over it; `CGGetLastMouseDelta` (camera look) + cursor-visibility tracking |
| [`host/shims/shimsinput.c`](../host/shims/shimsinput.c) | Input: SDL3's events. Keyboard → the game's SDL events; menu/touch pointer → fake `NSEvent` → `EAGLView` `-mouseDown:`/`-mouseUp:`/`-mouseDragged:`/`-mouseMoved:` (hover forwarded); **camera look** from SDL's relative mouse while the game shows no pointer (`gmv_game_pointer_shown`), fed to `CGGetLastMouseDelta`, cursor confined to the picture while focused; pumps SDL from the game's `SDL_PumpEvents`; diagnostics (`GMV_CLICK_S3`, `GMV_WATCH`, `GMV_INPUT_TRACE`, `GMV_NO_MOUSELOOK`) |
| [`host/shims/shimsjoy.c`](../host/shims/shimsjoy.c) | Controllers: the game's SDL 1.3 joystick API (`SDL_NumJoysticks`/`Open`/`Update`/`GetAxis`/`GetButton`/`GetHat`) over SDL3: a pad SDL has a mapping for is presented as the DirectInput layout the game expects (buttons X A B Y LB RB LT RT Back Start LS RS, then PS and touchpad on Sony's, sticks on axes 0-3, d-pad as hat), any other device passed through; radial stick dead zone, by the kind of pad; notes the kind of pad last used. Dev builds: `PAD_TEST`, a virtual pad for the playtest |
| [`host/shims/shimsaudio.c`](../host/shims/shimsaudio.c) | Audio backend: a playback thread that pulls PCM from the game's captured CoreAudio render callback (`gmv_au_render_cb`) in 256-frame slices and queues it on an SDL3 stream (about 93 ms, as with `waveOut` before); also the intro movie's stream; level/underrun/lowest-queue diagnostics (`GMV_NO_AUDIO`) |
| [`host/shims/shimsvideo.c`](../host/shims/shimsvideo.c) | The intro movie: `intro.mov` decoded with Media Foundation on Windows, by the system's `ffmpeg` on Linux |
| [`host/shims/songdec.c`](../host/shims/songdec.c), [`songdec_impl.c`](../host/shims/songdec_impl.c) | The custom radio's own decoders for mp3, flac, wav and ogg (`gmv_song_*`), over the single files in [`host/vendor/`](../host/vendor/) |
| [`host/shims/ffpipe_linux.c`](../host/shims/ffpipe_linux.c) | Linux: runs `ffmpeg` / `ffprobe` and reads their output through a pipe (`gmv_ff_start`, `gmv_ff_read`, `gmv_ff_stop`, `gmv_ff_output`) |
| [`host/shims/compat.h`](../host/shims/compat.h), [`compat_linux.c`](../host/shims/compat_linux.c) | `windows.h` on Windows; on Linux the Win32 names the host uses, over pthreads, `clock_gettime`, `stat`, `mprotect` and `/proc/self/maps` |
| [`host/shims/gl/`](../host/shims/gl/) | `gl_window.c` the pacer's settings; `gl_msaa.c` the screen framebuffer; `gl_context.c` the SDL window, GL context, pixel format, `NSOpenGLContext`; `gl_present.c` the frame present (`gmv_gl_swap`) and tick pacer; `gl_draw.c` the draw wrappers and `TEXFIX`; `gl_trace.c`, `gl_diag.c` diagnostics; `gl_bind.c` CGL stubs, the GL binding (`gmv_gl_resolve`) and the one GL loader (`gl_real`); `glshim.h` what they share with a game's render code |
| [`host/games/gangstar/layout.h`](../host/games/gangstar/layout.h) | The fixed address-space layout, the names, the classes |
| [`host/games/gangstar/game.c`](../host/games/gangstar/game.c), [`settings.c`](../host/games/gangstar/settings.c) | Gangstar's side of `game.h`; every key of `GangstarMV.ini` and the `ORIGINAL_MAC_EXPERIENCE` preset |
| [`host/games/gangstar/patch/`](../host/games/gangstar/patch/) | Surgical patches to the game's own code and the hooks behind the fixes and enhancements, a file for each: `fix_*` (`fix_vertex.c` the movement crash, `fix_crash58.c` the null-delegate guard), `sync_*` the high tick rates, `pad_*`, `key_move.c`, `draw_distance.c`, `density.c`, `spawn_fade.c`, `mission_replay.c`, `lc_switch.c`, `tab*.c` the GAMEPATCH tab, `game_read.c` the game's memory, strings and script commands read without faulting, `diag_*` and `test_*` the diagnostics and test aids. `patch_common.c` writes to the game's code and `patch_apply.c` holds `gmv_apply_gamepatch`. Each patch no-ops unless the bytes match. `patch.h` is what they share |
| [`host/games/gangstar/render/`](../host/games/gangstar/render/) | The effects, a file for each: `ao.c`, `bloom.c`, `bloom_glow.c`, `fog.c`, `fog_hybrid.c`, `aniso.c`, `water_reflections.c`, `vehicle_reflections.c`, `dynamic_shadows.c`, `sun_moon_glow.c`. `draw_hooks.c` says what happens around each draw, `present_hold.c` is `MENUFLASH_FIX`, `render.h` what they share |
| [`host/games/gangstar/features/padicons.c`](../host/games/gangstar/features/padicons.c) | `PAD_ICONS`: recognises the menu's gamepad atlas (`splash_gamepad`, 4096×2048 DXT5) at its upload, keeps the blocks under its pictures, draws Xbox / PlayStation versions over them (distance-field shapes, GDI Arial letters on Windows, FreeType's on Linux, its own DXT5 encoder, mip levels 1-6 box-filtered) and swaps them in with `glCompressedTexSubImage2D` after a present when the pad kind changes |
| [`host/games/gangstar/features/customradio.c`](../host/games/gangstar/features/customradio.c) | `CUSTOM_RADIO`: the sixth station's songs, decoding (`songdec.c` first; Media Foundation on Windows, `ffmpeg` on Linux for the rest) and mixing (item 18) |
| [`host/games/gangstar/features/ucveh.c`](../host/games/gangstar/features/ucveh.c) | `UC_CRUSHER`: the Crusher monster truck, from the player's own add-on folder |
| [`host/games/gangstar/features/radiosongs.c`](../host/games/gangstar/features/radiosongs.c) | `RADIO_SONGS`: the iPhone version's extra radio songs, from the player's own add-on folder (item 26) |
| [`host/games/gangstar/features/vbostatic.c`](../host/games/gangstar/features/vbostatic.c) | `STATIC_VBO`: unchanged models kept in GL buffer objects (item 12) |
| [`host/games/gangstar/features/broadphase.c`](../host/games/gangstar/features/broadphase.c) | A faster `b2BroadPhase::Query` with identical results |
| [`host/games/gangstar/features/b2solve.c`](../host/games/gangstar/features/b2solve.c) | `b2World::Solve` without walking every body, with identical results |
| [`host/games/gangstar/features/hudbars.c`](../host/games/gangstar/features/hudbars.c) | The HUD's health and armour bars without a full UI search per update |
| [`host/games/gangstar/features/bbfast.c`](../host/games/gangstar/features/bbfast.c) | The billboard turn with its range tests first, same results |
| [`host/games/gangstar/features/perf_probes.c`](../host/games/gangstar/features/perf_probes.c) | `PERF_PROFILE`: the game's own routines, timed. Left out of a release build |
| `tools/` | Offline analysis/generators (genlow, machoinfo, disasm, initreach, objc1dump, ...) |
| `analysis/` | Parsed reports: imports, static-init reachability, ObjC classes, SDL |

Older items of the log below name `gamepatch.c` and `shimsgl.c`. Those two files were split up: what `gamepatch.c` held
is in `games/gangstar/patch/`, and what `shimsgl.c` held is in `shims/gl/` and `games/gangstar/render/`, under the name
of the feature. The functions kept their names, so a search finds them.

### Shim coverage (477 of 528 imports, log of 2026-11-01)

The other 51 are 49 functions, whose thunk reports the name if called, and 2 data symbols, which get
a no-access stand-in. By library: libcrypto 20, libstdc++ 9, libSystem 8, Security 6, CoreVideo 3,
OpenGL 3, CoreFoundation 1, libobjc 1.

### The 51 unfilled imports

Audited 2026-11-02 by following each import's callers up through the binary (direct calls and jumps,
pointers in data, PIC references, Objective-C message references). None can be reached in this build:

| Imports | Count | Used by | Why it does not run |
|---|---|---|---|
| libcrypto, Security, `std::ifstream`, `std::istream` | 20 + 6 + 7 | One start-up routine of the game and its helper | The code that uses them does not run in the build the port was made with |
| `CGLCreateContext`, `CGLGetCurrentContext`, `CGLGetPixelFormat` | 3 | 0x595f30, at 0x596278 onwards | Needs the config's +0x10 set, and its direct caller 0x37d4 clears that byte first. Its only other reference is a base-class vtable slot (0x68293c). Start-up code, so a change would show at every launch |
| `CVDisplayLink*` | 3 | `-[EAGLView setupDisplayLink]` | No message reference to the selector in the binary |
| `getifaddrs`, `freeifaddrs`, `inet_ntoa` | 3 | `-[GameDelegate getIPAddress]`, sent only by `-testConnection` | `testConnection` sends it only if the reachability flags say reachable, and the shim always answers 0 (`sc_reach_get_flags`). **A shim that reports a network would reach these** |
| `CC_MD5` | 1 | `-md5HexDigest:`, sent only by `webView:didFinishLoadForFrame:` | The host has no WebKit, so that delegate never fires |
| `objc_enumerationMutation` | 1 | `-findButtonInView:` in the two web view controllers | Sent only by itself, and called only if a collection changes while being walked |
| `std::ios_base::Init::~Init` | 1 | 204 static destructors | They are registered through `__cxa_atexit`, which the shim ignores |
| `clock` | 1 | 0x1b8a30, a one-line wrapper | No reference to the wrapper |
| `system` | 1 | 0x595a30, which builds an `rm -rf` line | No reference to the function |
| `uname` | 1 | The constructor 0x527d40 (vtable 0x681028) | Its two callers, 0x527f90 and 0x527fa0, have no reference |
| `dyld_stub_binder` | 1 | `__stub_helper` | The loader writes every lazy pointer up front, so the helpers never run |
| `std::nothrow` (data) | 1 | One site, 0x53f188 | Its address is passed on and never read |
| `__CFConstantStringClassReference` (data) | 1 | The `isa` of 189 string literals | The Objective-C shims recognise literals by address and never read it |

This is static analysis, so a pointer computed at run time cannot be ruled out, though nothing suggests
one. None of the 24 game logs on hand records a call.
A call would log `[MISSING] game called unimplemented import <name>` and end the process with exit code
3 (`gmv_missing_import` in [`host/core/imports.c`](../host/core/imports.c)), after the error box (see Diagnostics).
Any other build of the game is refused by the patch table first, so this audit covers only the
build the port was made with.

| Area | State | Where |
|---|---|---|
| libSystem (libc, pthread, mmap, time, stdio, wchar, setjmp, rune locale, keyboard, `stat`) | nearly complete | `shims.c`, `shimsw.c` |
| libstdc++ / C++ runtime | init set + `std::string` (COW + `_Rep`) + RB-tree/list + `std::ostringstream` (accumulator + by-value `str()`) | `shimscxx.c`, `shimsstr.c`, `shimsstream.c` |
| libobjc | `objc_msgSend` / `_stret` / `Super` / exceptions | `shimsobjc.c/.S` |
| AppKit / Foundation / Cocoa | launch path + lots of value classes; more as walls appear | `shimsappkit.c`, `shimsfnd.c` |
| CoreGraphics / CGL | real display-mode list (`EnumDisplaySettings`), `CFArray`/`CFStringCompare` over it; `CGGetLastMouseDelta` camera look + cursor tracking | `shimscg.c`, `shimsgl.c` |
| OpenGL (~112 imports) | **bound** (lazy cdecl→stdcall to opengl32); drawing live | `shimsgl.c` + `shimsobjc.S` |
| SDL (events, video, GL swap) | init + swap wired (`SDL_GL_SwapBuffers` → SDL3's `SDL_GL_SwapWindow`); **keyboard events wired** (`SDL_PeepEvents`); **joystick API** over SDL3 | `shimssdl.c`, `shimsgl.c`, `shimsinput.c`, `shimsjoy.c` |
| AudioUnit | **wired to an SDL3 backend** — the game's render callback is pulled for PCM and played | `shimsappkit.c`, `shimsaudio.c` |
| CoreFoundation, IOKit | fail-soft stubs | `shimsappkit.c` |

Per-library counts are in the import dump in `GangstarMV.log`. A Mach-O symbol is the C name with
one leading underscore; triple-underscore names are C identifiers that themselves begin with `__`.

### ABI notes (i386, macOS → Win32)

- Mostly identical cdecl; `int`/`long`/`size_t`/pointer/`float`/`double` are the same width.
- **16-byte stack alignment.** The game assumes `esp` is 16-byte aligned at every call (SSE
  `movaps`/`movapd` on stack locals). Every host function that calls into game code —
  `run_initializers`, `run_main`, `shim_NSApplicationMain`, `gmv_notif_post`, the `NSThread`/
  pthread trampolines, the input dispatch — **must** be marked
  `__attribute__((force_align_arg_pointer))`. Without it the fault shows up as a Windows "access
  violation read 0xffffffff" at a `movaps`. A force-aligned helper that gets *inlined* loses its
  realignment, so also mark it `noinline` (or align the caller instead).
- **Struct returns:** macOS i386 returns a struct through a hidden pointer the callee pops
  (`ret $4`); a shim returning a large struct needs `__attribute__((callee_pop_aggregate_return(1)))`
  (used for `CGDisplayBounds` and the `NSRect` stret stubs). **A C++ class with a non-trivial copy
  ctor is also returned this way even when it is one word** — e.g. `std::string` (a single `char*`
  with a COW copy ctor) is returned via the hidden pointer, *not* in `eax`. mingw disagrees: it
  returns a ≤8-byte aggregate in `eax` with no hidden pointer and no pop, so the attribute does
  nothing. Such returns are therefore written in asm — see `gmv_oss_str` (`ostringstream::str()`)
  in [`host/shims/shimsobjc.S`](../host/shims/shimsobjc.S): it builds the result, stores it through the hidden
  pointer, and does `ret $4`.
- `wchar_t` is 4 bytes on macOS (our wchar shims operate on `uint32_t`); `jmp_buf`, `time_t`
  (4 vs 8), `off_t` (64-bit) differ and are handled explicitly in `shimsw.c`.
- No C++ exceptions in the game (`__unwind_info` is only 0x48 bytes); ObjC 1.0; SDL 1.3.

## Fix and enhancement log

The numbered items are the record of each fix and enhancement, with the evidence behind it. The
numbers stay as they are, because `FIXES.txt` and the source cite them. Each item was written as the
work went on, so an early one can read as unfinished (the crash in item 1, the engine pitch in item 2)
until a later passage settles it. The current status is in the [README](README.md).

Some settings have no item here yet: `MISSION_REPLAY`, `SPAWN_FADE`, `PAD_MENU`,
`PAD_HINTS`, `PAD_ICONS`, `UC_CRUSHER`, `DRAW_DISTANCE`, the three density keys, `VEHICLE_REFLECTIONS` and
`GAMEPATCH_TAB`. The table below gives their history. For how they work, see the ini's description of
the key and the source file's header comment.

### Every key's history

One row per key, so that each change from the Mac game says why it is there. The columns:

- **Kind:** what the change is.
  - *port fix*: a fault of running the Mac game on this system.
  - *original bug*: a fault of the game itself.
  - *speed-up*: the same results, sooner.
  - *enhancement*: something the Mac game never had.
- **First worked on:** the first day the key appears in the work record, local time (UTC+7). Work from
  before 2026-10-25 has no such record, so "by 10-22" means it is in the status snapshot of that day.
- **Under the preset:** what `ORIGINAL_MAC_EXPERIENCE=1` does with the key, and why.
- **Item:** where the evidence is.

| Key | Kind | First worked on | Why it exists / what brought it up | Under the preset | Item |
|---|---|---|---|---|---|
| `NO_FLICKER` | port fix | by 10-22 | The game draws every other tick, but the port presented every tick, so blank frames flickered between the drawn ones. | on: without it the port flickers | snapshot of 10-22 |
| `VTXFIX` | port fix (cause not proven) | by 10-22 | Crash within a few steps of walking: one path of the vertex converter lacks the game's own `0xFF` check. Not seen on the Mac. A shader-key collision inside the game is suspected, but not proven. | on: without it the port crashes | 1 |
| `CRASH58_GUARD` | port fix (cause not proven) | by 10-22 (fired on 10-16) | Crash about 13 min into play: an on-demand animation reader with no file. Not seen since 10-16. Which of two causes it was is unknown. | on: without it the port can crash | snapshot of 10-22 |
| `FRAME_MS`, `SWAP_INTERVAL` | port fix (the pacer) + enhancement (other rates) | 10-25 | The driver's vsync ran the game at the panel's rate (dt 0 ↔ 14 ms on a 144 Hz laptop), which broke the engine sound and box climbing. The Mac ticked at 60 by its 60 Hz vblank. | 16.667 ms (60 ticks/s, 30 drawn fps) and vsync off, as on the Mac | 4 |
| `STOPLOCK_FIX` | original bug, exposed by the port | 10-25 | The 200 ms lock after a full stop reads the frame time, not a clock. The Mac's uneven frames, and the early driver-paced builds, hid it. The pacer of 10-25 exposed it that same day. | on: with the pacer, a car would stay dead | 5 |
| `SWAP_COPY` | port fix | 10-25 | After a swap the Mac's back buffer still holds the last frame. Windows' default format holds the frame before last, so cutscene fades, which draw over the last frame, landed on a stale picture. | on | 7 |
| `TEXFIX` | port fix | 10-25 | The death and map-menu fades never darkened the screen: the fade quad samples a texture with no coordinates, and this driver returns a transparent texel where the Mac's returned an opaque one. | on: the Mac showed these fades | 8 |
| `PHYSICS_SYNC` | enhancement (high frame rates) | 10-25 | At other tick rates the game's fixed 1/60 s physics steps fall unevenly on the drawn frames (judder). At 60 ticks/s the game's own step is used either way. | off: the game's own step | `sync_physics.c` header |
| `ORIGINAL_MAC_EXPERIENCE` | preset | 10-26 | One switch back to the Mac's behaviour without editing each key. Audits on 11-02, 11-03 and 11-05: the last found 36 places in the game's code still changed, all from the keys left on and the keyless rows below; 34 places and 153 bytes after the preset's changes later on 11-05 (three original-bug fixes off, three loading speed-ups on), the same whether the preset is on from the start or switched on in game. | — | "Running and settings" |
| `MENUFLASH_FIX` | original bug | 10-26 | Two half-ready frames of raw 3D flash between the title and the main menu. | off since 11-05 (on from 10-26 by the user's choice; the user then wanted every original-bug fix off under the preset) | 9 |
| `MENU_FADE_CLICKS` | original bug | 10-26 | The main menu ignores clicks while its background fades. | off since 11-05 (on 11-03 to 11-05 by the user's choice, off before) | 10 |
| `RADIO_LOGO_FIX` | original bug | 10-26 | The HUD shows the last station played, not the car's. | off (the user's choice) | 11 |
| `FAST_FTELL`, `FAST_FSEEK`, `FAST_MEMMOVE` | port fix (speed) | 10-26 | Windows' C library asks the system on every `ftell`/`fseek` and copies backwards slowly. The Mac's library does neither, so loads froze for about 2 s and driving stuttered. Linux's library behaves as the Mac's. | on | 12 |
| `LOAD_SLEEP_SKIP` | speed-up of the game's own wait | 10-26 | While streamed objects load, the game sleeps 16 ms every tick for 2-4 s. | on since 11-05 (off before, by the user's choice): under the preset each scene change of the main menu stalled, frames of 34-50 ms for seconds; same picture and rules | 12 |
| `MSAA` | port fix (Mac's look restored) | 10-26 | The Mac game asks for 4x multisampling. The port's window format had none. | 4, the Mac's | "Running and settings" |
| `ANISOTROPIC_FILTERING` | enhancement | 10-26 | Sharper roads and walls at a distance. | 0: only what the game asks for | "Running and settings" |
| `AMBIENT_OCCLUSION` | enhancement | 10-26 | Soft contact shadows. | off | "Running and settings" |
| `SUN_MOON` | enhancement (iPhone feature brought back) | 10-26 | The Mac build half-removed the iPhone version's sun and moon. | off | "Running and settings" |
| `DRAW_DISTANCE` | enhancement | 10-26 | The city drawn further than the Mac's 450/900 m. Above 1 it also gives the buildings near the map's centre their collision at once (item 13). | 1 | 13, `draw_distance.c` header |
| `CAMERA_SYNC` | enhancement (high frame rates) | 10-26 | Above 60 ticks/s the running camera lost the Mac's sway. Installed only below 16.667 ms. | off | 4 |
| `WALK_SYNC`, `AI_SYNC` | enhancement (high frame rates) | 10-27 | Above 60 ticks/s walkers flicked their facing and runners were taken as stuck. Installed only below 16.667 ms. | off | 4 |
| `BLOOM` (+ `BLOOM_LIGHTS`, `BLOOM_WINDOWS`) | enhancement | 10-27 | Glow around lights, neon and lit windows. | off | "Running and settings" |
| `WATER_REFLECTIONS` | enhancement | 10-27 | Neither the Mac's sea nor the iPhone's reflects anything. | off | "Running and settings" |
| `FOG_MODE`, `FOG_HEIGHT` | enhancement | 10-27 | The game fogs only some materials; buildings, roads and the sea get none, on the Mac too. Fog over the whole city is new. | 0: the game's own fog | "Running and settings" |
| `HIDE_MORE_GAMES`, `HIDE_GAMELOFT_LIVE` | enhancement | 10-27 | Their pages and servers are long gone. | off: the Mac shows both | 14, 15 |
| `ALLOW_WEAPON_SPRINT` | enhancement (as on the iPhone) | 10-27 | The Mac puts the weapon away during a sprint. | off | 16 |
| `TRAFFIC_DENSITY`, `PEDESTRIAN_DENSITY`, `PARKED_CAR_DENSITY` | enhancement | 10-27 | More cars and people than the Mac's caps of 18 / 14 / 10. | 1, and the Mac's spawn pool | `density.c` header |
| `GAMEPATCH_TAB` | enhancement (native menu, no overlay) | 10-27 | Settings changed inside the game's own options menu. | stays, with only the preset's switch, which is the way back | "Running and settings" |
| `VEHICLE_REFLECTIONS` | enhancement | 10-28; 11-06 | Shiny cars reflect the street, after the water reflections. Since 11-06 the ray's misses (roofs, trunks) take the screen as a sphere map instead of staying bare, the user's choice after comparing three looks on a Laufer. | off | `vehicle_reflections.c` header |
| `MISSION_REPLAY` | enhancement | 10-28 | Replay completed missions from the SERVICES menu. | off; its tab's hooks stay in, hidden, because the menu is built once at the start | `mission_replay.c` header |
| `PAD_ICONS` | enhancement | 10-28 | The game's pad pictures show only where a button is, not its Xbox or PlayStation name. | off | `padicons.c` header |
| `UC_CRUSHER` | enhancement (add-on from the user's own Urban Crime) | 10-29 | The Crusher truck parked by L.C.'s hotel after ON A LION HUNT. | off; with the add-on there its files are still read at the start, unused (the user's choice, 11-03) | `ucveh.c` header |
| `SPAWN_FADE` | enhancement | 10-29 | Cars and people pop in and out on the Mac. | off | `spawn_fade.c` header |
| `STATIC_VBO` | speed-up | 10-30 | The Mac build sends every model to the graphics card on every draw. | on: same picture, about 10% of each frame back | 12 |
| `MINIMAP_SHOPS` | enhancement | 10-30 | The game keeps the shops in the minimap's list but never draws them. | off | 17 |
| `CUSTOM_RADIO` | enhancement | 10-30 | A sixth station with the player's own music. | off | 18 |
| `SHORT_WARNING` | enhancement | 10-30 | The start-up warning holds for 15 s and takes no input. | off | 19 |
| `CUTSCENE_KEYS`, `TUTORIAL_KEYS` | enhancement | 10-30 | Cutscenes and popups need the mouse on the Mac. | off | 20, 21 |
| `PAD_LAYOUT` | enhancement | 10-30 | A GTA V-like pad layout. | off: the Mac's buttons | 22 |
| `PAD_MENU`, `PAD_HINTS` | enhancement | 10-30 | The Mac's menus are mouse-only, and its tutorials name only keys. | off | `pad_menu.c`, `pad_hints.c` headers |
| `KEY_MOVE` | enhancement | 10-30 | The Mac's keys move one direction at a time and snap the body round. | off | 28 |
| `SWITCH_SAVE_FIX` | original bug | 10-30 | A save made as Betty gave her things to Johnny. | off (the user's choice) | 23 |
| `PLAYERSWITCH_INVENTORY` | enhancement | 10-30 | Betty shares Johnny's cash and weapons in free roam. | off | 24 |
| `LC_SWITCH` | enhancement | 10-31 | Play as L.C. once the story is done. | off | 25 |
| `RADIO_SONGS` | enhancement (iPhone and Java songs, add-on from the user's own copies) | 11-03; 11-09 | The Mac lacks nine of the iPhone's songs; since 11-09 also two rock songs of the Java version. | off; files read at the start, unused (the user's choice) | 26 |
| `DYNAMIC_SHADOWS` | enhancement | 11-03 | Shadows that follow the sun and the moon. | off: the game's fixed shadow | 27 |
| `LOAD_BUDGET` | speed-up of the game's own wait | 11-03 | The game builds 1500 collision items a tick on the way to the main menu, hitching for 43-131 ms; `DRAW_DISTANCE` makes it heavier. | 200 since 11-05 (the game's 1500 before, by the user's choice): measured that day in the main menu at 30 fps, its scene changes stalled 116-219 ms with 1500 and 31-89 ms with 200; how long a stall lasts depends on the machine, not on the game | 29 |
| `DPI_AWARE` | port fix (Windows only) | 11-04 | Windows display scaling above 100% blurred a window the system stretched. | on: no change to the game | the ini's description |
| `FAST_LOAD` | speed-up | 11-05 | The start-up Loading screen takes one step every 33 ms. | on since 11-05 (off before, by the user's choice): same loading steps, sooner | 31 |
| `FADE_EDGE_FIX` | original bug | 11-05 | With 4x MSAA a black fade leaves the top row and left column showing. The Mac used 4x as well, so it most likely showed the same line. | off (on earlier on 11-05 by the user's choice, then off with the other original-bug fixes) | 33 |
| `NO_INTRO`, `SHOW_CURSOR`, `INTRO_FLIP` | convenience | by 10-25 | Skip the intro, keep the arrow shown, and flip the intro for a decoder that shows it upside down. | `NO_INTRO` and `SHOW_CURSOR` off (leaks found in the 11-02 audit); `INTRO_FLIP` left as set | — |
| `LOG` | port | 10-26 | The port's log for bug reports. | left as set | 30 |
| no key: collision query, proxy lists, Box2D body list, HUD bars, billboard turn | speed-up | 10-29 / 10-30 | Same results, from about 70 to 113 fps in a busy scene at draw distance and crowds 2. | always on: identical results | `broadphase.c`, `b2solve.c`, `hudbars.c`, `bbfast.c` headers |

**Input is wired** (mouse/touch): SDL3's events feed the game's `EAGLView` `-mouseDown:`/
`-mouseUp:`/`-mouseDragged:`/`-mouseMoved:` through a fake `NSEvent`, and the key-window gate the game
checks before accepting mouse events is driven from the window's focus. The numbered notes below were written
for the Win32 `WndProc` that came before SDL3 (2026-11-04); what they found still holds. Plain hover moves are now
forwarded too (not just drags) — the menu has an on-screen pointer that highlights the button under
it, so hover is required; this is why hover highlights now light up. A **state-triggered tap**
(`GMV_CLICK_S3=1`) auto-dismisses the rating splash for testing (a normal click does it too).

1. **Keyboard input — wired; plumbing verified.** `gmv_input_wndproc` now handles
   `WM_KEYDOWN`/`WM_KEYUP` (+ `WM_SYSKEYDOWN`/`UP`, which fall through so Alt+F4 reaches
   DefWindowProc). **Alt+F4 / the close button** (2026-10-31): `WM_CLOSE` used to post a `WM_QUIT`
   nobody read once the game ran, so the window never closed; it now ends the process like the Mac's
   own close (`-windowWillClose:` 0x19af50 → `terminate:`, no save). **Alt+Enter** switches full
   screen ↔ window through the game's own switch, as the options' FULL SCREEN line does
   (`gmv_toggle_full_screen`, run from `gmv_input_pump` after the queue; the Enter is kept from the
   game). The game's own toggle (0x19bcd0) does nothing within 500 ms of its last switch (clock
   0x45d000, time kept at 0x68a6c0), so a quick second press was dropped; presses now wait while a
   switch is pending, and a dropped request is made again once the 500 ms are over
   (`gmv_full_screen_tick`). Both tested OK (2026-10-31). Both are Windows conventions for the port, always on;
   translates the Win32 virtual-key to an SDL 1.3 keycode 1:1 (`gmv_vk_to_sdl` in `shimsinput.c`:
   letters/digits → ASCII keycode, navigation/modifier/function keys → `scancode|(1<<30)`), and
   queues it; `SDL_PeepEvents` (`shimssdl.c` → `gmv_input_get_sdl_event`) hands one back per
   `SDL_GETEVENT` call. The game's per-frame event drainer (`0x18a9e`) reads **type@0** and
   **keysym.sym@0x10** and sets its key-state table, so a faithful `keysym.sym` is what matters.
   **Verified end to end** (`GMV_INPUT_TRACE=1`): `W` → WndProc capture → SDL sym `0x77` sc `26`
   delivered, 10/10 drained by the game with no loss, every frame regardless of state. **Movement
   confirmed in-world:** the ARROW keys move the player character. **Known crash being chased**
   (root mechanism found): within ~3 movement inputs the game faults with a write to `0x006222ad`
   at eip `0x00564e5a`, inside the engine's **vertex-format converter / skinning transform** (fn
   `0x563df0`–`0x564e5a`, called from `0x51f5ce`). The converter walks a semantic→attribute map:
   per semantic it reads a **destination** attribute index `al=[arg8+sem]` and a **source** index
   `cl=[arg18+sem]`, where `0xFF` means "attribute absent". It guards `if(al==0xFF) skip` but **not**
   `cl`, so when the source map marks an attribute absent (`cl=0xFF`) it still computes
   `srcDesc = descArr + 0x14 + 255*16`, far past a 1-attribute descriptor array → reads garbage
   floats → a wild stride (`0x40af`) → writes the converted vertex to `0x6222ad` (which happens to
   land in `__cstring`) → fault. Confirmed from a `veh.c` dump: attribute index computed = `255`
   while the descriptor-array header says `1` attribute. So the real question is **why the source
   attribute map (`arg18`) and the destination format (`arg8`) are inconsistent** in our port for
   the character's (animated) mesh — a mismatch that never happens on macOS. Front-end key presses
   are safe; mouse-only play is stable for minutes — only moving triggers it (the walk-animation
   skinning path). **Fix applied** (`host/games/gangstar/patch/`, `gmv_apply_gamepatch` from `run_main`): the
   converter's entry is `0x563c90` (a vtable method, no direct callers); its dest-absent path
   (`0x5640f0`) already guards `cl==0xFF`, but the dest-present path (`0x563e53`) does not. The host
   now overwrites the 8-byte `cmp al,0xff; je 0x5640f0` at `0x563e29` with a jump to a small
   trampoline that performs both the original `al` test and the missing `cl` test, routing
   `cl==0xFF` to `0x564070` (the loop-continue the engine itself uses) — i.e. it applies the
   engine's **own** guard to the one path that lacks it (faithful correctness fix, not an addition;
   byte-sanity-checked; `GMV_VTXFIX=0` disables it for A/B). **Confirmed working: movement no longer
   crashes and play has reached several missions.** `veh.c` still dumps the descriptor array + computed index on that
   fault should it recur. (The menu "hit-test" scare was a *test-harness* bug: a script
   PostMessaged to `hwnd 0` because it read `MainWindowHandle` before the GL window existed — real
   mouse input works and `-transformMousePoint:`@`0x19a4d0` maps points correctly.) Tools:
   `GMV_STATE_TRACE=1` (state/front-end probe, `shimsinput.c`); `GMV_GL_TRACE=1` (per-frame draw
   counts); `GMV_CLICK_S3=1` (auto-dismiss splash); `GMV_INPUT_TRACE=1` (tap/key/WndProc/thread
   trace); `GMV_WATCH=1`+`GMV_WATCH_ADDR=` (hardware write-watchpoint, `veh.c`);
   `GMV_VTXFIX=0` (disable the vertex-converter `cl` guard, to reproduce the original crash);
   `scratchpad/disva.py <hexVA>` (Mach-O VA→file disassembler, file = VA−0x1000 in `__TEXT`),
   `scratchpad/callers.py <hexVA>` (`call rel32` xref scanner).
2. **Audio — wired, playing, faithful.** The game drives a CoreAudio `AudioUnit`; the host captures
   its output ASBD + render callback (`shimsappkit.c`) and a `waveOut` playback thread
   ([`host/shims/shimsaudio.c`](../host/shims/shimsaudio.c)) pulls that callback for PCM and plays it (44.1 kHz
   stereo s16). The backend is healthy (full-scale peaks, zero underruns). **All vehicle audio
   loads**: the `.wav` names probe a (non-existent) loose file then resolve to the numbered
   `_packMac/fileNNNNNN.dat` packs — verified every engine/SFX sample loads this way (the `.wav`
   "FAIL" lines in a trace are the harmless loose-override probe, same as `.tga`/`.bdae`). The
   **car "engine rev" was investigated in depth** (env-gated hooks in `gamepatch.c` under
   `GMV_CARSND_TRACE`: the mixer was reversed — `0x5ea3a0`→`0x5ea400`→`0x5bfc70`, per-voice render
   `0x5c2dc0`): each car has per-type **idle/onidle/offidle** loop samples that the game **crossfades
   by volume** with throttle/RPM — and all three voices are created and their volumes do modulate.
   This build's software mixer has **no runtime pitch-sweep knob** (a voice's rate is only its base
   sample-rate × doppler), so the engine "rev" is a loop crossfade, not a pitch ramp. Our port
   reproduces this faithfully; a continuous-pitch engine would be an *enhancement* (out of the pure port's
   scope), not a fix. `GMV_NO_AUDIO=1` silences the backend.
3. **Mouse camera-look — wired.** The in-game camera reads *relative* mouse motion via
   `CGGetLastMouseDelta` (engine poll `0x3f4a`). The host feeds it from SDL's **relative mouse**
   (`shimsinput.c`) while the game shows no pointer ([`pad_menu.c`](../host/games/gangstar/patch/pad_menu.c)
   `gmv_pointer_up`, CursorGame's +4 or +5), so the camera follows the mouse hands-free during gameplay; while the
   pointer shows, the mouse is a normal one with the system's speed. SDL keeps a position in relative mode, so a click
   has a place in both. The cursor is confined to the picture while focused. (`NO_MOUSELOOK` and `NO_RMB`, which turned the camera
   feed and the right button off, were taken out on 2026-11-05: never needed.) Compared with the raw `WM_INPUT` before it (2026-11-04): the same 300-count move turned the camera 296 px and
   295 px in a 960 px shot.

4. **Frame pacing — fixed (2026-10-25).** The game is dt-driven but was tuned on Macs with a
   60 Hz display: `SDL_GL_SwapBuffers` waited one vblank per tick, so it ran 60 ticks/s and
   rendered every other tick (30 fps). The host used to leave the driver's vsync in charge, so on
   a faster panel the game ticked at the panel rate. Measured on the RTX 3050 Ti laptop: **~150
   ticks/s, with the game's whole-ms dt alternating 0 ↔ 14 ms**. That is the root of the unstable
   engine rev, and of box-climbing breaking at high rates. Now the default paces every tick to
   **60 Hz** with a fixed-deadline pacer and vsync off (`shimsgl.c`, `pace_frame_ms`). Measured
   afterwards: 60.0 ticks/s, dt 16.7 ms, jitter 0.0–0.2 ms, 30 render fps, the same as the Mac.
   The pacing itself was validated earlier with `pace_60fps.bat`; it is now simply the default.
   Related fixes in the same pass:
   - **1 ms timer for the whole process** (`shimsw.c`, `timer_resolution_init`):
     `timeBeginPeriod(1)`, plus an opt-out from Windows 11 dropping the requested resolution when
     the window is hidden. Previously only the opt-in pacer requested 1 ms, so every game
     `usleep`/`nanosleep` (VoxThread audio streamer, the 1 ms loader waits) slept ~15.6 ms.
     `usleep` also truncated (`usleep(900)` → `Sleep(0)`); sleeps now round to the nearest ms,
     and sub-ms requests just yield.
   - **The fixed-timestep hook never installed.** Its call site `0x6029` lies in the low range,
     which runs only from its copy at `+GMV_LOW_DELTA`; nothing is mapped at `0x6029`, so the old
     patch's `VirtualProtect` failed and `pace_fixedstep.bat` silently ran without it. It now
     patches the copy (`0xc75029`) and verifies the decoded call target. It stayed opt-in
     and unneeded for normal play, and v.1.0.0 removed it (item 30).
   - **Intermittent "address-space layout is wrong" at startup.** The check required
     `0x11000..0xD00000` to be *one* region, but a protection change on any page splits it while
     it still belongs to the exe. Seen once on 2026-10-25 and not reproducible since. The check now
     walks every region and logs the offending one if it ever fails for a real reason.
   - `GMV_LOOP_TRACE`'s stack scan now reports low-copy return addresses (the tick) translated
     back to their original VAs.
   - **Running camera sway at high tick rates (`CAMERA_SYNC`, 2026-10-26).** On foot, the camera
     manager (`0x44640`) either snaps the scene camera to the on-foot camera's position or glides
     there with a 0.1 s follow. `0x43da4`/`0x43c48` pick the glide when the scene camera is more
     than √3000 ≈ 55 units away (`0x5f1308`). Snapped, that distance is one frame's movement, so at
     30 drawn frames/s a running character trips it. Gliding, it is speed × 0.1 s and running keeps
     it on. The glide trails ~75 units and pulses with the stride (19–43 units per frame), which is
     the "shake" you see while running. At 240 ticks/s a frame's movement is a quarter as long, the
     glide never starts, and the camera stays rigid. `GMV_CAM_TRACE` measured a trail of 70–82 units
     at 60 ticks/s and 0 at 240 (`log_cam60b.log`, `log_cam240b.log`). The glide isn't frame-rate
     free either: moving dt/0.1 s per frame, it trails speed × (0.1 s − dt), which is 75 units at
     30 frames/s and 105 at 120.

     The fix is `apply_cam_follow` in `gamepatch.c`. It hooks the entries of `0x43da4` and `0x43c48`
     and does three things:
     - **Follow time.** It sets the follow time to 0.0667 s + dt, which gives the Mac's trail.
     - **Latch test.** It decides the latch on the gap a Mac frame would see: the trail before this
       frame plus the wanted position's move over one Mac frame. A Mac frame is 33.3 ms plus
       however late it was drawn. At 60 ticks/s the drawn frames come in pairs like 42/24 ms, with
       stalls of 60+ ms, and a late frame at a stride peak starts most running glides. So the
       window adds the lateness of recent frames, counting only spikes above both 2 ticks and the
       recent average frame. The position at the start of the window is interpolated between
       frames, because a window of whole frames runs long. With an exact 33.3 ms window, only
       turns started a glide (`log_cam240d.log`). On a straight run the original also starts only
       from a stutter or a turn, so how soon it starts depends on how much the game stutters
       (`log_cam240e.log`: about 25 ms stutters, right at the trigger line). In `log_cam240f.log`,
       with stutters of 27–42 ms, 9 running glides started (windows of 39–66 ms). All but one
       ended when the character stopped. The other ended when the wanted eye stopped moving (a
       2.7 unit move) while the character still ran. That matched the 60 ticks/s feel by eye, and
       `log_cam60f.log` (same route, 60 ticks/s) agrees: 8 glide starts, most just after a frame
       drawn 10 ms late (43/24 ms pairs, about one per 2 s at both rates). 5 of them held until a
       stop. The other 3 dropped on the next frame, a one-frame blip that the hold here turns into
       a held glide. `GMV_CAM_TRACE` counts glide
       starts, ends and late frames, and logs the test behind each start and end.
     - **Hold.** After a start it holds the glide until that gap first passes 55, while a Mac glide
       would settle above 55. A glide here starts one short frame behind, not one 33 ms frame behind.

     The first version scaled one frame's step instead. The uneven 4–25 unit steps then tripped it
     at random, and each glide dropped within a frame or two (`log_cam240c.log`). The decision
     reaches the game through the two compares, which point at host floats set to 0 or FLT_MAX.
     `camsim.py` and `camsim2.py` (in the session scratchpad) simulated the design against the
     original at 30 frames/s, for turns and for late frames. Glide starts, glide ends and the trail
     all matched.

     The fix only installs when `FRAME_MS` is below 16.667. `ORIGINAL_MAC_EXPERIENCE=1` forces it
     off.
   - **Walkers flicking their facing at high tick rates (`WALK_SYNC`, 2026-10-27).** In the main
     menu's 4th (shotgun) and 5th (ambulance) scenes, walking characters snapped 50–75° to one
     side and back on every drawn frame at 120 and 240 ticks/s, but not at 60. A script walk
     command updates its character through `0xc897a`. Each update, `0x657c8` picks the point to
     head for, then `0x6341e` turns the character straight to it (there is no turn rate).
     `0x657c8` casts a ray (`0x2472c`, called at `0x659c8`) toward the destination (`+0x514`):
     - **Ray clear:** the character heads for the destination.
     - **Ray blocked:** the character heads for a side-step point the call returns.

     Either way the result goes to `+0x520`. The ray starts at the node at `+0x2e8`, a bone about
     one unit off the body's centre, so it swings with the facing. When the ray only just grazes
     an obstacle, facing the side-step point swings the bone clear, and facing the destination
     blocks it again. The choice then flips on every update while the walker crosses that narrow
     band. At 30 updates/s he crosses it in an update or two. At 120 updates/s each update moves
     him a quarter as far, so the flipping lasted up to 20 frames. A per-call recorder on
     `0x6341e` measured this, with the target point, position and heading at each call: 3.8
     flips/s at 240 ticks/s against 0.5 at 60 (runs of up to 20 updates against 1–2).

     The fix is `apply_walk_sync` in `gamepatch.c`. It redirects the call at `0x659c8`, and per
     character makes the real call at most once per 33.3 ms (the Mac's average rate). In between
     it returns the last answer, clear or blocked with its side-step point. A new destination asks
     again at once. The walker steers as at 30 updates/s and still turns and moves at the full
     rate. Over three runs each, facing jumps came to 0.35–0.70 per walker-second with the fix at
     240, against 0.42–0.69 at 60. Flips less than 33 ms apart dropped from 26–60 per run to the
     one-off turns at scene starts.

     Only the script walk command uses `0x657c8`. Pedestrian AI casts the same ray from other code,
     which this fix leaves alone. The fix only installs when `FRAME_MS` is below 16.667.
   - **Running characters taken as stuck at high tick rates (`AI_SYNC`, 2026-10-27).** In the main
     menu's shotgun scene the walker sometimes turns back a second time after his shots. At 240
     ticks/s his run then got stuck repeating its first steps, for up to 4 s; at 60 it played
     through. A temporary probe on the animation request (`0x11b03c`) showed his body animation
     switching between 212 (stand) and 213 (run) every 42 ms, 84–143 times in that run back. 212
     came from `ActorStop` and 213 from `ActorRun`, each a new blend.

     The characters' AI is a tree of behaviour states (`0xf267c`). The states share one vtable
     at `0x6525c8`, and enter, update and exit each run a small script (`0xc653a`, `0xc64ca`,
     `0xc645e`). Transitions test bytecode expressions (`0xf3ee0`) over the character's
     properties (vtable `+0x34`, `0x782e8` for NPCs). His run state's enter script is
     `ActorRun` and its exit script is `ActorStop`. Its way out is `p45 || p44 || !p84`, and the
     state it leads to (`p25 && !p159`) hands straight back.

     Property `0x54` (84) is a stuck check that counts calls, not time. Every 5th call
     (`0x6425ac`) it compares how far the character moved since the last check with 20 units
     (`0x6425b0` = 400, squared), and answers 0 below that. The expression is tested every game
     update, so the 5 calls span 167 ms at the Mac's 30 updates/s. At 240 ticks/s (120 updates/s)
     they span 42 ms, and a character running at about 3 m/s moves 12 units in that time.

     The fix is `apply_ai_sync` in `gamepatch.c`. It points the two NPC vtable slots and the
     Player's two forwarders (`0xf6673`, `0xf6694`) at a wrapper. For property `0x54` the wrapper
     lets a call through at most once per 33.3 ms per character and answers 1 in between, as the
     original's uncounted calls do. Over three 125 s runs (30 loops) at 240 ticks/s the walker's
     stand/run switches came to 3–9 per loop, against 1–7 at 60 (21 loops) and up to 149 without
     the fix. The same check runs for every AI character in play, so it also stops chasing
     enemies stuttering at high tick rates. The fix only installs when `FRAME_MS` is below 16.667.
     `ORIGINAL_MAC_EXPERIENCE=1` forces it off.

     The script walk command is `ActorGoTo`; `0xc897a` is the setup slot of its vtable (`0x6527e8`),
     which the script runner calls on every update. Besides the scene scripts, the menu's
     ambient pedestrians walk through it as well, from a shared behaviour script (kind 1,
     index 87), so the fix covers them too.

     **Test aid: the player at the wheel of a car (`TEST_SPAWN_CAR`, 2026-11-06).**
     `patch/test_spawncar.c`, dev builds only, asked for by the user to compare car reflections up
     close. `TEST_SPAWN_CAR=Laufer` (a type's own name in `metatypes.gmap`; Laufer is type 141):
     once the save is in the city, no menu shows, no mission is under way and the player has stood
     3 s on foot, a car of that type is made where they stand, as the Crusher is (`uc_make`), and
     they are seated at once through Player vt+0x68, as the Crusher's swap does. The function needs
     `force_align_arg_pointer`: the seating's `movaps` on the stack (`0x6e262`) faulted without it.
     The playtest scenario `car-reflections` uses it.

     **Test aid: one menu scene on a loop, camera behind its walker (`MENU_LOOP_SCENE`,
     `MENU_LOOP_CAM`, 2026-10-27).** `apply_menu_loop` in `gamepatch.c`, for watching the walkers
     up close:
     - **The menu's scene sequence.** The menu background is script 81 (kind 0, started by
       `0x1b5b52`, kept at `0x949ad8`). It starts script 92, which starts the first scene. Each
       scene is a script of its own. It sets its two `SceneCamera_MML_*` cameras with `CamSet`, runs
       about 10 s, and ends by starting the next scene with `ScriptUnlock` (setup `0xd080e`; `0x3244c`
       sets a script running). The order is fixed: COCONUT 83, SWAMP 99, PORT 94, COUNTRYSIDE 84
       (the shotgun walk), DOWNTOWN 86 (the ambulance) and 87, AIRPORT 82, LILHAVANA 90 and 89,
       POORSUBURB 93, RICHRESID 97, 95 and 96, SOUTHBEACH 98, NORTHBEACH 91, DESIGND 85.
     - **The loop.** `MENU_LOOP_SCENE=N` replaces every scene start on the main menu with the Nth
       scene. When that scene ends, it is set running again on the next tick.
     - **The camera.** `MENU_LOOP_CAM=1` collects the characters `ActorGoTo` moves. After 1 s it
       takes the one on foot nearest the scene camera's aim, within 30 m. F9 steps to the next.
       The camera manager's update (`0x44640`, called only at `0x1a2316`) runs first. The camera
       node (manager `+0`) is then set to a chase view, through the setPosition / setTarget that
       `0x44b88` snaps with (vtable `+0xa4` / `+0x104`). The view follows his smoothed walking
       direction, so it does not turn with his facing.
     - **Things that went wrong on the way.**
       - The camera manager's on-foot mode (mode 0, via `0x44b88`) does not move on the menu.
       - During the scene cameras' 11 s blends, the manager skips the eye/target setters
         `0x43da4` / `0x43c48` entirely.
       - A walker is only read while his walk command moved him in the last 100 ms and his scene
         node (`+0x10`, read by the position getter `0x23d16`) exists. Reading a pedestrian
         without one crashed.
     - **The chase camera changes how the scene plays.** In COUNTRYSIDE with `MENU_LOOP_CAM=0`
       the shotgun walker does the same thing on every loop, at 60 and at 240 ticks/s alike. He
       walks to a point, turns about 150° to chase a target moving some 120 m away, drops out of
       his walk command for about 3 s, then comes back about 10 m away and snaps 70° to a new
       heading. With the chase camera he just walks steadily toward a target that stays put.
       Leaving the hooks and position reads in but the camera alone plays it as `MENU_LOOP_CAM=0`
       does, so moving the camera is what changes it. The engine evidently simulates the scene
       differently depending on where the camera is; the check responsible was not found. The
       chase view is good for watching a walker's facing and legs, but `MENU_LOOP_CAM=0` is the
       true scene. Measured with a temporary ray probe (2026-10-27): facing jumps over 20° were
       0.27–0.31/s for him in every case, and his walk ray started from the same bone offset.

   The static evidence came from capstone sweeps of the Mach-O. REA's Ghidra provider only
   accepts PE files on Windows, so it can't open this binary here. The tick at `0x5f30` calls the
   glf sleep `0x598100` with the tweak `gMainThreadSleepEveryFrame` (`0x685420`, zero-initialised
   `__common`, registered by name at `0xff10`), so the game has no sleep-based limiter of its own.
   It relied on the vblank. Controls: `GMV_FRAME_MS=<ms>` (period; `0` = old uncapped behaviour),
   `GMV_SWAP_INTERVAL=<n>` (overrides the vsync-off default), `GMV_PACE_LOG=1` (`[framepace]` dt
   stats every 2 s; also on whenever either of the other two is set explicitly).

5. **Car stuck after a full stop — fixed (2026-10-25).** Once a car stopped completely, gas and
   brake could die for good (steering still worked; leaving and re-entering didn't help). The
   vehicle update (`0x112bc4`) starts a 200 ms "settle" lock when a car goes from moving forward to
   stopped with all wheels down: `flags(+0x180) |= 0x60; this->[0x184] = stamp`, released when
   `(unsigned)(stamp - this->[0x184]) > 199`. While `0x20` is set, Car's force routine (`0xfef68`)
   skips the whole drive-force block; `0x40` makes brake/reverse (`0x10d26e`) return early. The
   stamp is read from `app->[0x17ad8]`, which is the **frame dt** (the tick copies `*[0x63e018]` =
   `0x949ae0`, written by `0x1b8aa0`), not a clock. So the lock releases only on a frame with a
   *smaller* dt than the one recorded at the stop. Mac vsync jitter supplies one within a few ticks;
   this host's steady dt (16/17/17 ms) can stay above a recorded 16 forever. `gamepatch.c` (`apply_stoplock_fix`, permanent, `GMV_STOPLOCK_FIX=0` disables) makes the
   stamp the tick's own `now_ms` (`0x949adc`), giving the intended 200 ms.
   Why the early builds (early October 2026) never showed it: before item 4's pacer the driver's
   vsync set the pace, and dt went 0 ↔ 14 ms. Physics only stepped on the 14 ms ticks, so a stop was
   always stamped 14 and the next 0 ms tick let go (inferred from the code, not measured then). The
   even pacer of 2026-10-25 (16/17/17 ms, jitter under 0.2 ms) can stamp a stop at 16 and then never
   gives a shorter tick, so the bug showed up the day the pacer came in, and this fix came the same
   day. It stays on under `ORIGINAL_MAC_EXPERIENCE`, which keeps the pacer. One difference from the
   Mac remains: there the lock most likely let go at the first shorter frame, a few ticks after the stop; here it
   lasts the 200 ms the code asks for. Found through a Ghidra
   12.1.4 headless decompile of the Mach-O (REA itself can't open a Mach-O on Windows). It also
   corrects the earlier "Box2D sleep" suspicion: the accelerate handler (`0x109614`) wakes the body
   itself (`m_flags &= ~e_sleepFlag; m_sleepTime = 0`).
   Correction to item 2: the engine *does* have a runtime pitch. Each voice's pitch factor (`+0x64`,
   `0x4000` = 1.0×, clamped at 2.0×) scales the sample step, and `AudioSource::update` (`0x5b1980`)
   ramps it linearly from `+0x9c` to `+0xa0` over `+0xa8` (elapsed `+0xa4 += dt`), pushed through
   `setPitch` (vtbl `+0x30`). Engine pitch is still being compared with the Mac reference;
   `build\trace_engine.bat` captures the speed, voice and ramp traces plus the mixed PCM in one pass.

6. **Solo-car listening mode (test aid, 2026-10-25).** `GMV_SOLO_CAR=1` mixes only the player's
   car and mutes everything else (traffic, peds, music, radio, other SFX). `trace_engine.bat` turns
   it on. The player's car sounds are the ones the car-sound module (`0xa2fc2..0xa3f61`, driven by
   `Car/Moto::UpdateCarSounds`, vtbl `+0xd8`) plays through its 5-entry SoundManager function table
   at `0x689dd0`, installed by `0x10aba8` from the slots at `0x63e478`. `UpdateCarSounds` only runs
   for the vehicle the player drives (`0x113a46` sets `+0x1e4 = 1` only when `+0x280` is the
   player). The host wraps `isPlaying` / `play` there and resolves each (sample, handle) to its
   render voice: SoundManager table → emitter handle at `+0x24` → `AudioSource` via `0x5b0440`
   under the device lock → `+0x134`. Every voice of every class is summed by one call in the device
   mixer (`0x5bfddb`, `voice->vtbl[0x60](voice, accum, frames)`), which is re-routed so that any
   voice not stamped in the last second mixes into a discarded scratch buffer. Muted voices still
   render, so their playback state advances as normal. This mode changes what you hear, so it is
   off by default.

   With `GMV_CARSND_TRACE=1` as well, the car-sound `setPitch` (`0x61036`) is wrapped too and
   `[carpitch]` logs each requested engine pitch (rpm / layer reference rpm) next to the
   `AudioSource` pitch (`+0xa0` target set by `0x5b2200`, `+0x98` pushed by `0x5b1980`) and the
   render voice's `+0x60` target / `+0x64` current / `+0x68` per-buffer step (`0x5c0120`, consumed
   by `0x5c2dc0`). The voice step is `(DAT_0064d5f8 << 14) / voice[+8] * (target - cur) >> 14`
   when `DAT_0064d5f8 < voice[+8]`, else the whole difference; the file default is `0x7fffffff`
   (instant), and the trace logs its live value.

   **Engine-pitch freeze fixed (2026-10-25).** The second capture logged
   `DAT_0064d5f8 = 0` and every voice step 0: `cur` stuck at the pitch each layer started with
   while `target` followed the rpm. The game sets that limit once at audio start-up (`0x5ea9f7`):
   `AudioUnitGetProperty(kAudioUnitProperty_MaximumFramesPerSlice)` into an uninitialised local,
   then `0x5bf320(frames / 44100.0)`, which stores `(int)(seconds × 16384)`. The limit is one
   render slice in 2.14 seconds and `voice[+8]` (~540, about 33 ms) is the time between pitch
   updates, so each slice moves the voice `slice / interval` of the way and a pitch change glides
   in over one game frame. The `AudioUnitGetProperty` shim used to return noErr without writing
   anything, so the game read 0. It now answers property 14 with the frames the backend pulls per
   render call (`gmv_audio_frames()`, 256 by default or `GMV_AUDIO_FRAMES`), giving a limit of 95.
   Any other property returns `kAudioUnitErr_InvalidProperty`; the game makes no other queries.

   Third capture (after the fix): the user confirmed the pitch now follows the revs; the
   remaining "odd at high rev" character was checked against the host path and found to be
   game-side. Limit 95 logged, `[engrev]` shows one step per render (v15 1.236 → 1.400), the
   game presented ~60 fps (7740 frames in 131 s) with sound updates every ~33 ms, the mix peaked
   at 3816/32767 with 0 underruns, and a click scan of the dump found only 2 events in 98 s of
   audio (voice starts on render boundaries, not periodic). The voice resampler `0x5c26c0` is
   integer linear interpolation with no filter, and the fetch `0x5c0590` carries the 2.14
   fractional phase across render calls, so the slice size adds no discontinuities. The engine
   samples are 32 kHz (base step 11888), and the top layer plays at up to ~1.74×. Only one host
   choice can still change the sound: if Mac AUHAL reported its default 1156-frame
   MaximumFramesPerSlice while rendering 512-frame slices, the Mac ramp reached each target
   about 2.3× faster (~15 ms instead of ~33 ms). That has not been measured on a Mac.

   First capture (2026-10-25, solo mode): the player's engine tone stays flat for 10+ s at a time
   (53.5 / 55.1 / 65.6 Hz) with jumps between levels, and `[engrev]` shows each layer's volume
   crossfading while its pitch stays at the value it started with. `GMV_AUDIO_DUMP=1` now writes
   next to the exe (the game changes the working directory) and refreshes the WAV sizes about once
   per second, because the game quits through `ExitProcess` without closing the file.

7. **Cutscene fade-to-black — fixed, confirmed in game (2026-10-25).** The fade before a cutscene did not render
   properly. Mac `flushBuffer` keeps the back buffer's contents after a present, and the game relies
   on it: it renders every other tick, and its fades draw over the retained frame rather than
   redrawing it. `ChoosePixelFormat` gave the window a `PFD_SWAP_EXCHANGE` format (`flags=0x8225`),
   whose back buffer holds the frame before last after a swap, so each draw landed on a stale
   picture. A standalone probe on the RTX 3050 Ti confirmed this: the default format returned the frame
   before last, while the driver's matching `PFD_SWAP_COPY` formats kept the last frame, windowed
   and in borderless fullscreen. `ensure_gl` (`shimsgl.c`) now takes the `PFD_SWAP_COPY` twin of the
   chosen format (same colour, alpha, depth and stencil), so the log shows
   `[gl] pixelformat #19 ... swap=copy`. If no twin exists it keeps the original format, and
   `GMV_SWAP_COPY=0` restores the old behaviour. The present-skip below stays as a fallback
   for drivers without copy formats.

8. **Death → hospital fade — FIXED, verified in-game (2026-10-25):** the screen goes black on
   death and the map-menu fade works too. Cause and fix:
   see "Eighth trace" below.
   **Corrected symptom (user, after the fourth trace):** when the player dies, the screen never
   turns black at any point, even though `[fade]` shows the death fade (caller 0x1b5094, alpha 57
   → 227) being drawn. So the real question is why that black never reaches the screen. The
   "fades too fast" analysis below was a misreading of the report; it stays as a record of how the
   cutscene fader works. Next diagnostic: `trace_fade.bat` now arms `gmv_gl_fade_capture_arm` at
   the first death-fade draw. For about 4 s it saves pre-swap back buffers (`gmv_fadecap_*.bmp`,
   at most 960 px wide) and logs one `[fadecap]` line per tick: clears, draws, mean brightness, and
   whether the anti-flicker present-skip held the frame back. That shows whether the black is
   drawn but not presented, presented but overdrawn, or never drawn.

   **First capture (fifth trace):** the generic fade state 0x1b5094 is not death-only. The first
   one in the run opened the pause map/services menu, so the capture filmed that instead of the
   death, which came later (alpha 58 → 228). It still showed the key fact. At the tick where that
   fade was at alpha 244, the pre-swap back buffer was the plain gameplay frame, with mean
   brightness 97, the same as before the fade. So the rectangle never darkens the back buffer.
   Every drawn tick was presented (`present=yes`), so the present-skip is not the cause. The
   cutscene fader (0x34918) draws through the same 0x4c5ce0 with the same arguments and does
   show, so the difference must be where in the frame the fade is drawn, or the GL state it is
   drawn with. Next diagnostic:
   - `gmv_gl_fade_mark` gets every fade call and re-arms the capture on each new 0x1b5094 fade,
     up to 4 sessions (`gmv_fadecap_s<session>_<nn>.bmp`, written to the game's cwd, the repo root).
   - Each `[fadecap]` line now carries `seq=` with the tick's order of clears (C),
     framebuffer binds (B<fb>), fade calls (F, or f for other callers) and draw runs (D<n>).
   - `[fadestate]` logs the GL state at the first draw after a fade call: FBO, viewport, depth,
     blend, scissor, cull, alpha test, stencil, texture and colour mask. It does this for both
     0x1b5094 and the cutscene fader, so the two can be compared.

   **Sixth trace:** no death this time. Both captures were the map menu opening, which uses the
   same 0x1b5094 fade, and again the black never showed. That makes the map a quicker repro than
   dying.
   - The GL state at the first draw after the fade call matches the working cutscene fader:
     fb 0, depth off, blend SRC_ALPHA/ONE_MINUS_SRC_ALPHA, no scissor, colour mask on. So the
     state at that draw is not the problem.
   - That first draw is a 180-index batch, where the cutscene fader draws a lone 6-index quad. The
     fade rectangle therefore goes through the 2D batch, either inside that batch or as the draw
     right after it.
   - The tick sequence is `C D41 F D2 C D1`. After the fade call come two draws, then a second
     `glClear`, then a single draw before the swap. Normal gameplay ticks are `C D3 C D31`.
   - Next diagnostic: `[postfade]` lines. From a 0x1b5094 fade call until the swap, each clear
     (with its mask and game return address) and each draw is followed by a read-back of the
     centre and quarter pixels. This shows which call darkens the frame and which one undoes it.
   - **Capture trigger changed to the death (at the user's request).** The captures were starting
     too late, so `apply_cine_trace` now also hooks `Player::applyDamage` (0xf8282, reached
     through the this-adjusting thunk 0xf9050 in the Player vtable), but only to learn the player
     pointer. Health is at `player+4+0x578`: Character::getHealth reads `+0x578` of the Character
     part, which starts at `player+4`. `gmv_death_poll` runs once per present and arms a 12 s
     capture (`gmv_gl_death_capture_arm`) on the tick the health goes from above 0 to 0 or
     below, whatever lowered it. The menu-fade auto-arm is gone. It saves every 4th presented
     frame, plus every presented frame within 300 ms of a fade call. Images are at most 640 px
     wide, named `gmv_fadecap_s<death>_<nnn>_f<frame>_t<GetTickCount>_+<ms since health 0>ms.bmp`,
     so `t` lines up with the `[fade]` / `[fadecap]` stamps. Up to 3 deaths per run.

   **Seventh trace (first death-triggered capture).** The trigger works: `[death] player object …
   health 300`, then `health 300 -> 0`, then a 12 s capture with 114 images.
   - Timeline: "YOU BEEN MURKED" plays for about 5 s. At +5125 ms the state-machine fade
     0x1a5cea starts at **alpha 255** and ramps down. That frame should be solid black, but the
     capture shows the street with the player already removed, at mean brightness 63–75, so the
     respawn swap is visible. No 0x1b5094 fade happened in this death; it was used later by the
     map menu.
   - Every failing fade (0x1a5cea and 0x1b5094) has the same tick shape, `f D2 C`: after the
     fade call there is a batch flush (a 180-index draw), then the fade quad (6 indices, texture 2
     bound, TEXTURE_2D on, MODULATE, blend SRC_ALPHA/ONE_MINUS_SRC_ALPHA, colour array on), then a
     depth-only clear (`mask 256`, harmless).
   - `[postfade]` read-backs show the centre and quarter pixels are identical after every one of
     those calls, even at alpha 169 or 255. **The fade quad is drawn but changes nothing.**
   - The cutscene fader, which does show, draws its quad as the first draw after the call, with
     no batch flush before it.
   - Next diagnostic: `[fadequad]` dumps the first 6-index draw after each fade call, up to 4
     per caller class (0x1b5094, cutscene 0x034aa1, 0x1a5cea, others), so the working and failing
     quads can be compared:
     - client-array formats (size, type, stride, pointer), and each vertex's position, colour and
       texcoord;
     - enabled state, bound texture and env mode on texture units 0–3;
     - the bound texture's size, internal format and min filter, plus the texel under vertex 0's
       texcoord;
     - any pending GL error.

   **Eighth trace: cause found.** Every fade quad (0x1a5cea, 0x1b5094 and the cutscene fader
   alike) is drawn with **GL_TEXTURE_2D still enabled on unit 0** and no texcoord array. The
   bound texture is whatever the previous 2D batch used. For the failing fades that is texture 2,
   the 1024×1024 RGBA HUD atlas, whose texel (0,0) is `136,136,136,0`, fully transparent. The
   quad's own vertices are correct: full screen, colour `0,0,0,a` with `a` up to 255.
   - Why the texture stays on: glitch's `setTexture(stage, NULL)` (0x503af0) only records the
     NULL and never unbinds, and the 2D rectangle material keeps unit 0 enabled. When it disables
     a texcoord array, the driver sets the current texcoord to `(0,0,0,0)` with `glMultiTexCoord4f`
     (0x4fa464, 0x4ff800, 0x4ffb52). So the fixed pipeline samples at s/q = 0/0, which is
     undefined, and with MODULATE the result is `vertex colour × texel`.
   - On this driver the sample is the transparent atlas texel, so the black ends up at alpha 0
     and the quad changes no pixels. The Mac driver evidently gave an opaque sample, so the black
     showed there. One cutscene-fade dump had texture 23 bound, whose sample is opaque (alpha
     211). That is why some fades showed and others didn't, depending on what was bound last.
   - **Fix (host, `shimsgl.c`, "texfix"):** the host now tracks `glActiveTexture`,
     `glClientActiveTexture`, `glEnable`/`glDisable(GL_TEXTURE_2D)` and
     `glEnable`/`glDisableClientState(GL_TEXTURE_COORD_ARRAY)` per unit. For each `glDrawArrays`
     / `glDrawElements` (except `GL_POINTS`, since point sprites generate their own coordinates),
     any unit that is enabled without a texcoord array is disabled for that one draw and
     re-enabled afterwards. A draw without texture coordinates cannot mean to show a texture, so
     it renders with plain vertex colour, which is the opaque-sample result the Mac gave.
   - The game's own fade code, timing and alpha ramps are untouched. `GMV_TEXFIX=0` turns the fix
     off for comparison. `[fadequad]` now also logs the current texcoord, the tracked bits and
     the number of patched draws.
   - This is a deliberate deviation only in that it removes undefined driver behaviour. It also
     applies to any other untextured rectangle that had the same problem.

   The user reports that after a death, every later fade gets too fast: cutscenes, the main menu,
   and the fade when going up a building. The user has decided this must be fixed even if
   the Mac build does the same (an exception to the faithful, no-enhancements rule).
   **Third trace (7:07 PM)** (menu → load → up a building → death → up a building → mission)
   captured it. The building fades are the cutscene fader driven by the script commands
   `ScriptCommand<Data::CineFadeIn/CineFadeOut>::update(int dt)` (0xcd8be / 0xcda6c, vtbl+8). They
   were 9 frames each way before the death (twice), then 6 frames, then 3 frames after it. Frame
   spacing in the log stays about 33 ms, and the old fades fit exactly `len = 300` with real-ms
   `dt` of 33/33/34. So `dt` is real milliseconds (frame dt from 0x1b8aa0), not ticks; the "ticks"
   note below is wrong. The post-death fades fit either `len` 200 then 100 with normal `dt`, or
   `len` 300 with `dt` about 1.5× then 3×. The 1 s fades around the mission start after the death
   still took 30 frames. `readData` (0xea494) reads `[cmd+0xc]` and `[cmd+0x10]` (the length)
   straight from the script stream. To settle which value changes, `GMV_FADE_TRACE=1` now also
   logs every update call as a `[cine]` line (`len`, `dt`, timer, alpha, and the caller's return
   address).
   **Fourth trace (7:31 PM), settled:** `dt` is a normal 30–37 ms on every step, before and after
   the death. Every update comes from the generic script runner 0x32706 (ret 0x327b7), which steps
   a script's command array (`+4` count, `+8` index, `+0x10` commands). Before the death the
   building used one command pair (`cmd` 0xaa178c/0xaa1828, `len` 300, the same objects both
   times). After the respawn a freshly loaded script (commands 0xa11ce8..0xa12db0, allocated in
   order) plays fades with `len` 500/200, 200/200 (plus a second fade-out that starts already black
   and so ends at once), 100/100, then 300/300 twice, then 500/400. The 200 and 100 ms fades came
   about 20 s and 24 s after the death fade in both the 7:07 and the 7:31 runs. The commands are
   built by table-driven factories (0xd47c8 in, 0xd4776 out, in `ScriptCommands.cpp`) and
   `readData` takes the length straight from the script file. So nothing carries over and nothing
   is rescaled: the short fades are the original script data, and the Mac build plays them the
   same way. All scripts live uncompressed in `_packMac/file002968.dat`, and each command is
   stored as its type id then its arguments (`0x5a` = 90 CineFadeIn, `0x5b` = 91 CineFadeOut, then
   the length). The whole post-death sequence matches file offsets 0x4201..0x4adf exactly,
   including the double fade-out (0x45a2, 0x460c). That is the intro cutscene of mission C1 M04
   (`CheckpointMission_Roscoe_House_00`, then `SceneCamera_C1_M_04_00..11` with Roscoe, Betty
   and a hearse procession, ending at `Checkpoint200M_C1M04_01`). Its camera cuts are authored
   with 200 ms and 100 ms fades. It starts about 7 s after the respawn (mission restart from its
   checkpoint). The 6:50 PM run played the same cutscene *before* any death with the same short
   fades (0.53 s, then about 0.16–0.3 s). So the death does not cause them; it only happens to
   start this cutscene. The death fade itself (generic fade state, ctor 0x1b452a, 150 ms) is a
   separate code path and never touches the cutscene fader's length. Status: waiting for the
   user to decide whether to lengthen authored short script fades. That would leave the death
   fade unchanged. Earlier runs: the first `trace_fade.bat` run quit about 5 s after respawning, so no
   cutscene after the death was captured. That run did show cutscene-fader (`[fade] caller=0x034aa1`) fades
   already come in two lengths before any death: about 0.27 s (9 steps) and about 1 s (18–30
   steps). Second trace (6:50 PM): one mission cutscene played twice before the death (at about
   14 s and 163 s). Its fade sequence was identical both times, and it matches the same cutscene
   in the first run, also played before any death: 0.53 s to black, then 172/156/172/203/266/
   297 ms fades. The run again quit about 4 s after the hospital fade-in, so a cutscene *after* a
   death is still untraced.
   How the cutscene fader works (static): script step 0xcda6c starts a fade from the command's own
   length field (`[cmd+0x10]`). Each frame it does `timer -= dt` and sets
   `alpha = 255 - timer*255/len`; timer is 0x6424b0 and alpha is 0x6424b4, both reached through
   `__nl_symbol_ptr` 0x63e6c8/0x63e6c4. The fade-in step 0xcd8be mirrors it (`alpha =
   timer*255/len`). The other writers are 0xcda10/0xd72c0/0xd731c, 0x1b5e22 (next to the respawn
   fade code), 0x37950, and the direct stores in 0x33d0a/0x33e78/0x34c00. They all leave the timer
   and alpha idle (-999) or full black (255), and none of them writes a length.
   Original analysis of the death fade itself: all screen fades are a
   black `draw2DRectangle` (0x4c5ce0). `GMV_FADE_TRACE=1` (`build\trace_fade.bat`) logs them as
   `[fade]` lines. A traced death showed the respawn uses the generic fade state (draw2d 0x1b4f00,
   update 0x1b4cfc). Its ctor (0x1b452a) hard-codes a 150 ms fade-out (`0x96`), so you get 2–4
   visible steps. Then phase 1 skips the scene (`app+0x17ae4` = 0, checked at 0x475f) and the
   frame clears to black while the hospital area streams in (~2.7 s). The cutscene fader
   (0x34918) then fades in from black over about 1 s. Every step was drawn; the fade-out is only
   short, which matches the original.

9. **Main-menu entry flash — original game, fixed at the user's request (2026-10-26).** Pressing a
   key on the title showed two frames of the raw 3D menu background (camera facing a building wall,
   no overlay) before the menu faded in from black. `GMV_FLICKER_CAP` caught it at the state switch
   from the title (`0x65a388`) to the main menu (`0x659f28`), identically at 60 and 240 ticks/s
   (brightness 89 → 29 → 110 → 11), so it is the game's own frames: ~66 ms at 60 ticks/s, as on the
   Mac. The menu background is a scripted camera flight whose fades run through the cutscene fader
   (alpha `0x6424b4`, timer `0x6424b0`; overlay drawn by `0x34918` unless the alpha is -999). On the
   first menu frame the fader is idle and the camera unplaced; on the second the script's opening
   1 ms CineFadeOut (`0xcda6c`) has started with alpha 0 (transparent, timer 1); only the third is
   black (alpha 255), then the fade-in follows. `menu_flash_hold` (`shimsgl.c`) skips presenting from
   that switch until the alpha reaches 255 (cap 300 ms), so the title stays up until the first black
   frame; `[menuflash]` logs the hold and its release. A first version released on "fader running"
   and still let the alpha-0 frame through (`log_menufix.log`).
   `GMV_MENUFLASH_FIX=0` shows the original flash. A deliberate deviation from the Mac build, so since
   2026-11-05 `ORIGINAL_MAC_EXPERIENCE=1` shows the flash too (the user's choice: every original-bug fix off under it).

10. **Main-menu clicks ignored during the background fades — original game, changed at the user's
    request (2026-10-26).** Right after "Press any key", menu clicks were often ignored for a second
    or so at a time, mostly while the 3D background scene was still loading. The background is a
    scripted camera flight cut together with the cutscene fader (item 9), and on every tick a fade
    runs, CineFadeIn (`0xcd8be`) and CineFadeOut (`0xcda6c`) call `0x283b0`. That sets the input-lock
    flag `[[app+0x17ac8]+0x18]` and calls `0x19d72`, which zeroes every key and button level of the
    input manager (`*0x685510`). A click only stores a level (`-[EAGLView sendMouseEvent:...]`
    0x19a3d0 → 0x1816e: `[im+0x1a5e]` left, `+0x1a5f` right), and the manager turns levels into
    press/release events once per tick (`0x19b60`), so a press made during a fade was wiped before
    it was seen. The Mac runs the same code. Found with a hardware write-watchpoint on the level
    bytes: apart from the click handler, only `0x19d72` called from `0x283ce` wrote them, every tick
    of each dead stretch. `apply_menu_fade_clicks` (`gamepatch.c`) aims the four fader calls at
    `gmv_cine_fade_lock`: while the main-menu state (`0x659f28`) is on top and the front end is not
    switching screens (`0x32de0`), it sets the lock flag but skips the reset; anywhere else it calls
    `0x283b0` as before. The script's explicit lock (`0xcd66c`) is untouched.
    `GMV_MENU_FADE_CLICKS=0` keeps the original; `ORIGINAL_MAC_EXPERIENCE=1` kept the fix on from
    2026-11-03 (the user's request) and keeps the original since 2026-11-05 (the user's choice). A first guess (press
    and release landing in one long loading frame) was wrong: the logs showed them in separate
    frames, so that change was reverted.

11. **HUD radio logo shows the wrong station after getting into a car — original game, changed at
    the user's request (2026-10-26).** The HUD showed one station (e.g. 104FM "Sparkles") while
    another played (99FM "Rock Tour"). The music manager keeps the current station in `[mm+0x21c]`,
    and the HUD logo is set only by `0x17357e` (widget `+0x14`), from `Radio::show` (`0x930bc`)
    and the radio refresh (`0x9323a`), both with `[mm+0x21c]`. Getting into a car switches the HUD
    first, so the refresh runs then; only afterwards does the car code (`0x10a57c`) start the
    radio with the car's own station (`0x60190`; `[car+0x120]` the first time, `[car+0xc8]` after
    that), and nothing refreshes the logo again. So the HUD keeps the station that played last
    (104FM before the radio has ever been on). `GMV_RADIO_TRACE=1` caught it: `HUD logo: station 0`,
    then `start(4)` picking `m_radio_rock_08.wav`. The station table itself is right (104FM 80s,
    303FM electro, 42FM hip-hop, 101.1FM latino, 99FM rock), and the Mac runs the same code.
    The refresh comes from the player's enter-vehicle step `0xf89fe` (through `0x28564` and again
    inline at `0xf8ab7`) as the player starts getting in. The radio only starts once they are
    seated, so the two are a whole get-in animation apart. A first version of the fix updated the
    logo only when the radio started, and the user still saw the old logo for about a second.
    `apply_radio_logo_fix` (`gamepatch.c`) now has two parts:
    - **Part 1.** The last call of `0xf89fe` (`0xf6032` at `0xf8aef`) goes to `gmv_radio_enter`.
      For the player only (`0x617b2`), and before making that call, it works out the station the
      car is about to start, the way `0x10a57c` and `0x60190` will:
      - It does nothing if the byte at `0x647ed0` is clear or `[mm+0x220]` has `0x11`.
      - Otherwise it reads the station from the car `[character+0x55c]`: `[car+0xc8]` if bit 0 of
        `[car+0x183]` is set, else `[car+0x120]`. A value of -1 means `[mm+0x21c]`.

      It then shows that logo the way `0x9323a` does: unless `[mm+0x220]` has `0x20`, and only once
      the HUD has its radio widget (`[app+0x218] > 0x34`), it calls `0x17357e`. This runs in the
      same game tick as the game's own refresh, so the old logo is never drawn.
    - **Part 2.** The two car-entry calls go to `gmv_radio_entry`. After the radio starts, it shows
      the logo of the station that actually started, and logs it if part 1 guessed differently.

    `[radiologo]` logs each change. `GMV_RADIO_LOGO_FIX=0` or `ORIGINAL_MAC_EXPERIENCE=1` keeps the
    original.

12. **Scene loads froze the game for ~2 s — port bug (2026-10-26).** Loading a scene (the main
    menu's background, for one) stalled the main thread for about 1.8 s in a single tick. The user
    asked for a GL texture/shader cache, so first `GMV_LOAD_PROFILE=1` (`loadprof.c`) measured
    where the time went:
    - The game uses fixed-function GL with no shaders, and texture uploads took 13-18 ms per load.
    - Loading is synchronous on the main thread; the loader threads did no file I/O in those ticks.
    - About 85% of the tick was the main thread in `NtQueryInformationFile` / `NtSetInformationFile`
      under msvcrt.

    The game's file stream (vtable `0x6839a8`) clamps every read to the bytes left: `Read`
    (`0x5a0320`) calls `GetSize` and `Tell` before each `fread`, and `Tell` (`0x5a0170`) is a tail
    call to `ftell`. One load does ~580,000 reads. macOS stdio keeps the offset in the FILE
    (`__SOFF`), so `ftell` never entered the kernel there. msvcrt's `ftell` makes two kernel calls
    every time, about 3.7 µs together. `FAST_FTELL` (`shimsw.c`) keeps the position of each file
    opened read-only: `fopen`/`fseek` set it, `fread` advances it, and `ftell` returns it. A short
    read, `SEEK_END` or an error drops it, and the next `ftell` asks msvcrt once. The game gets the
    same values either way.

    In the same pass, `shim_fopen` now opens `./C:\...` asset paths from the drive letter straight
    away, instead of failing on the original spelling first.

    Measured on the way to the main menu (`[framepace]`, no profiler): the worst tick went from
    1765 ms to 258 ms. A texture cache would not have helped. What is left is real reads, opens,
    msvcrt's `fseek` refilling its buffer, and driver work in `glCompressedTexImage2D`.
    `GMV_FAST_FTELL=0` restores the msvcrt `ftell`. It stays on under `ORIGINAL_MAC_EXPERIENCE=1`,
    since the Mac never had the stall.

    The first in-city run with the profiler (FRAME_MS 4.17) found no loading stall while driving.
    The 60-150 ms hitches came from the host's own console logging. `[dirs] NSSearchPath` printed
    ~20 lines per save/menu event and is now logged once per dir. The rest came from
    `RADIO_TRACE`/`INPUT_TRACE`/`CAM_TRACE`. The release exe has no console, so it pays only the
    log-file write. The 20-28 ms hitch every 2 s window while driving was the same trace output:
    with the traces off and `LOAD_PROFILE=15`, driving stays at a 7-14 ms worst frame. The VTXFIX
    `[vtxmiss]` dump (61 ms) now goes to the log file only.

    What is left is the game's own work. Spawning a set of physics bodies runs Box2D 2.0's
    sweep-and-prune broadphase (`CreateProxy` 0x1b9e50, `DestroyProxy` 0x1b9c00). It inserts each
    proxy into sorted bound arrays with `memmove` and renumbers the bounds after it. That is O(n)
    per proxy, and one tick of it took 190 ms. The original does exactly this. For 2-4 s after a load
    the main thread also sleeps 16 ms every tick. The profiler's `sleep <us> x<n> <- game ...` line
    names the caller: the `Update` (vtable slot 2, called from 0x327b4) of a game state at 0xd2164
    that waits for streaming to finish. Until 0x1adbc4 reports the loader idle (every queued object
    at state 1 and the main-thread queue empty), it raises the per-tick queue budget at 0x641114
    from 25 to 1500 items and calls the game's ms sleep (0x598100) with 16. Once the loader is idle,
    it restores the budget. This is the original's design. At the Mac's 60 Hz the sleep costs
    nothing visible, but here it caps those seconds at ~50-60 fps. At the user's request
    `LOAD_SLEEP_SKIP` (default on; `apply_load_sleep_skip` in [`host/games/gangstar/patch/`](../host/games/gangstar/patch/))
    replaces that call (0xd21d8) with a 5-byte NOP and leaves the budget alone; `GMV_LOAD_SLEEP_SKIP=0`
    keeps the sleep, and so did `ORIGINAL_MAC_EXPERIENCE=1` until 2026-11-05; since then it skips it too (the user's
    choice, see item 29). The 217 ms and 111 ms ticks
    in the same window are that 1500-item budget spawning bodies into the broadphase. The
    profiler's stack walk no longer mistakes the game's PIC base idiom (`call $+5; pop`) for a
    return address, which had blamed 0x1bf214, 0x14c7ee and 0xd2171.

    The start-up load still had two ticks of 766 ms and 1312 ms, about half of it in `NtCreateFile`
    under the file stream's `fopen` (0x5a0247). The 2660 pack files cost about 5 ms each on their
    first open and 0.05 ms after (Windows Defender's on-access scan). Once they had been scanned,
    the same ticks were 395 ms and 281 ms with almost no open time left. What remained was
    `NtSetInformationFile` under `fseek` (Seek, `0x5a0190`) and `NtReadFile` refilling the buffer
    afterwards. One tick made ~270,000 file calls for ~640 KB. FreeBSD/macOS stdio serves a seek
    inside the bytes it has already buffered by moving its pointer. msvcrt's `fseek` always drops
    the buffer and calls the kernel, twice for `SEEK_CUR`. `FAST_FSEEK` (`shimsw.c`) gives streams
    opened `"rb"` a 64 KB buffer (`setvbuf`). With `FAST_FTELL`'s kept position, a seek that lands
    inside msvcrt's buffer moves the FILE's `_ptr`/`_cnt`. Other seeks go to msvcrt as `SEEK_SET`.
    The buffer is trusted only while `_cnt > 0`: after a read too large for the buffer, `_base`
    holds older bytes. Against the real msvcrt.dll, 300,000 random reads, seeks and `ftell`s on a
    pack file matched the file byte for byte. Seek-heavy patterns went from 1.4-2.6 µs to
    50-70 ns per call, and plain sequential reads were unchanged. `GMV_FAST_FSEEK=0` restores
    msvcrt's `fseek` and 4 KB buffer. Like `FAST_FTELL`, it stays on under
    `ORIGINAL_MAC_EXPERIENCE=1`.

    With the profiler off, the worst start-up frame became 243 ms. That is GPU uploads (~73 MB
    of `glCompressedTexImage2D` from `CTexture::updateData`), ~85 MB of pack reads, and the game's
    own parsing, all in one frame by design. A ~145 ms hitch while driving is pure CPU in Box2D
    (callers around 0x1bf121/0x1bf2a5, no I/O or GL). Of its samples, 16% are `memmove` from
    0x1b9f29 and 0x1b9cd3, which shift the broadphase's sorted 12-byte bound arrays. The insert
    (`memmove(p + 24, p, n)`) overlaps with dst > src, so it must copy backwards. msvcrt does that
    with `std; rep movsd`, which gets no fast-string microcode. `FAST_MEMMOVE` (`shim_memmove` in
    [`host/shims/shims.c`](../host/shims/shims.c)) copies those backward moves with SSE2, as the Mac's libSystem
    memmove does. Every other memmove still goes to msvcrt, whose forward copy is fast. Against
    msvcrt.dll, 200,000 random overlapping moves matched a reference copy. Backward moves of
    48 B to 60 KB went from 36 ns-4.1 µs to 4.6 ns-1.25 µs, 3.5-8x faster. `GMV_FAST_MEMMOVE=0`
    restores msvcrt's memmove. It stays on under `ORIGINAL_MAC_EXPERIENCE=1`.

    The game's Collada loaders create every vertex and index buffer with glitch usage 4 (client
    memory), so the driver copies each model on every draw. `STATIC_VBO`
    ([`host/games/gangstar/features/vbostatic.c`](../host/games/gangstar/features/vbostatic.c)) watches those buffers. It checksums them between frames,
    and gives one unchanged for 4 checks usage 0 (`GL_STATIC_DRAW`), so glitch's next bind uploads it
    once. A converted buffer goes back to client memory before glitch's `map` (0x517b10) or `setData`
    (0x5177f0) touches it, or when its checksum changes. Its data is always kept (bind 0x504580 is
    asked not to drop it). Sites seen writing behind the engine's back, and CBatchMesh's per-draw
    index ranges, are never converted. The host's own vertex readers (the sky's colours, the sea)
    read a converted buffer through its kept data. Time inside the draws went from 185 to 77 ms a
    second (10.9 to 4.7 µs a draw). `GMV_STATIC_VBO=0` turns it off. It stays on under
    `ORIGINAL_MAC_EXPERIENCE=1`.

13. **Buildings near the map's centre have no collision when a level starts — original game, fixed
    at the user's request (2026-10-26).**
    - **Cause:** the city grid's `updateCells` (0x13d5a4) creates a building's collision when its
      cell comes within range. It compares with a rectangle around the last position, which starts at
      (0,0,0). On the first update, every cell within range of the world origin (about ±225 m on the
      Mac) counts as already added, so you can walk through those buildings until their cells leave
      range and come back.
    - **Fix:** `apply_origin_collision_fix` in [`host/games/gangstar/patch/`](../host/games/gangstar/patch/) wraps the
      only call (0x135a5a). It moves that starting position 100 km away, so the first update adds
      every cell in range.
    - **Only with the draw distance (2026-10-27):** the same rectangle covers most of the city at
      `DRAW_DISTANCE=3`, so the fix is applied whenever `DRAW_DISTANCE` is above 1. At 1 the user
      found a bug with it and chose the Mac's behaviour, so the game is left as it is there. The
      fix's own switch (`ORIGIN_COLLISION_FIX`) was removed; `ORIGINAL_MAC_EXPERIENCE=1` keeps the
      original because it forces `DRAW_DISTANCE=1`.
14. **"More Games!" corner on the main menu — enhancement at the user's request (2026-10-27),
    `HIDE_MORE_GAMES=1`, off by default.**
    - **What it was:** the page-curl corner at the bottom right is the button
      `widgetMainMenu.btnMore` of the menu's flash file. Pressing it calls the menu's listener with
      action 4 (0x1691b0), which opens Gameloft's in-game ads page (`ingameads.gameloft.com`).
      That service is long gone. In the port it shows "No Internet connection available".
    - **Change:** `apply_hide_more_games` in [`host/games/gangstar/patch/`](../host/games/gangstar/patch/) replaces two
      slots of the main-menu widget's vtable (0x659048): show (slot 1, 0x16e91c) and the text setup
      (slot 6, 0x171c00). Each wrapper calls the game's function and then hides the button with the
      game's own set-visible, 0x2fdf80(flash, "widgetMainMenu.btnMore", 0). The click handler
      (slot 5, 0x1729da) compares the button's name with `btnMore` on press and on release. Both
      matches are dropped (0x172bf1 `jne` to `jmp`, 0x172ace `je` to `nop`s), so the empty corner
      takes no clicks either.
    - **Checked in game:** the corner is gone from the menu's first frame and stays hidden through
      the background scenes. A click where it was does nothing. With the setting off, the corner and
      its popup are as before.
    - `ORIGINAL_MAC_EXPERIENCE=1` forces it off.
15. **Gameloft LIVE! badge on the main menu — enhancement at the user's request (2026-10-27),
    `HIDE_GAMELOFT_LIVE=1`, off by default.**
    - **What it was:** the badge at the top right is the button `widgetMainMenu.btnGLLive`. Pressing
      it calls the menu's listener with action 3, which opens the Gameloft LIVE! sign-in
      (`GLLivehtmlmacViewController`). That service is long gone too, and in the port it shows the
      same "No Internet connection available".
    - **Change:** the same hooks as item 14 (`apply_hide_more_games`): the two vtable wrappers also
      hide `widgetMainMenu.btnGLLive`, and the click handler's two `btnGLLive` matches are dropped
      (0x172bb5 and 0x172aaa, `jne` to `jmp`). The two settings are independent.
    - **Checked in game:** the badge is gone and a click where it was does nothing. With
      `HIDE_GAMELOFT_LIVE=0` and `HIDE_MORE_GAMES=1`, the badge and its popup are back while the corner
      stays hidden.
    - `ORIGINAL_MAC_EXPERIENCE=1` forces it off.
16. **Weapon kept in hand while sprinting — enhancement at the user's request (2026-10-27),
    `ALLOW_WEAPON_SPRINT=1`, off by default.**
    - **What it was:** on the Mac, running (W) keeps the weapon, but sprinting (Shift, the Sprint
      action 0x12) puts it away. The HUD shows fists until Sprint is released, and then the weapon
      comes back. The iPhone keeps the weapon out while sprinting. The Sprint key's handler
      (0x11e002) does this. On press it marks the character as sprinting (+0x666 = 1) and calls
      Player vt[0x78](1), which selects weapon slot 1, the fists. On release it clears the mark and
      selects the slot it saved before the sprint (handler +0x48). A trace of the player's weapon
      slot showed it going from 5 to 1 on the Shift press and back on release, with
      `fl__player__sprint` (animation 71) playing in between.
    - **Change:** `apply_weapon_sprint` in [`host/games/gangstar/patch/`](../host/games/gangstar/patch/) turns the start of
      the fists call into a jump past it (0x11e269 `eb 61` to 0x11e2cc). It also skips the restore
      on release (0x11e2ae `eb 1c`), which would now only undo a weapon change made during the
      sprint. The sprinting mark still goes on and off as before.
    - **Checked in game:** with the Uzi, the player sprints holding it, and the HUD keeps the Uzi
      icon. Weapon slot 8 stays selected through the press and the release, and the same sprint
      animation plays. The game has no armed sprint animation except a two-handed one, and it still
      plays the unarmed sprint pose with the gun in one hand. Two-handed weapons were checked in
      game on 2026-11-03, with no problem. With the setting off, the sprint shows fists as before.
    - `ORIGINAL_MAC_EXPERIENCE=1` forces it off.
17. **Shops on the minimap — enhancement at the user's request (2026-10-30), `MINIMAP_SHOPS=1`,
    off by default, also in the GAMEPATCH tab (SHOPS ON MINIMAP).**
    - **What it was:** the weapon shops, car rentals and repair shops show only on the pause map,
      whose legend names them, so finding one meant pausing. The minimap (MiniMap.cpp, world
      +0x9c) keeps a list at +0x5c of the objects it has icons for, each with its sprite made at
      the object's set-up (0x46eda; icon number from 0x239ce). The shop checkpoints (category
      0x15: CheckpointShop, ShopCarRental, ShopRepair, ShopWeapon) and the teleports (0x16) are in
      that list, but the draw loop of 0x4623a skips both categories (0x4679d: `cat - 0x15 < 2`
      jumps to the next entry).
    - **Change:** `apply_minimap_shops` / `minimap_shops_set` in
      [`host/games/gangstar/patch/`](../host/games/gangstar/patch/) rewrite those 17 bytes. Only category 0x16 is
      skipped now. Category 0x15 goes to 0x4681f, which sets the draw's "inside only" flag, as
      the game does for its own badges: a shop shows while it is inside the minimap and is not
      pinned to the edge when it is not. The options tab switches it in game.
    - **Checked in game (2026-10-30):** the user confirmed the shop icons show on the minimap near
      a shop and go away with distance. Switching it off and on again in the GAMEPATCH tab took
      effect at once, and the log was clean.
    - `ORIGINAL_MAC_EXPERIENCE=1` forces it off.
18. **Custom radio station — enhancement at the user's request (2026-10-30), `CUSTOM_RADIO=1`,
    off by default, also on the game's SOUND page (Custom Radio Volume and Custom Radio).**
    - **What it was:** the iPhone version could play the iPod's music instead of the game's
      (`MusicPlayer`, a playlist choice in the volume menu). The Mac build kept the class, but
      every method is an empty stub (0x180e08..0x180e7e; `MacOSXMusicPlayer` only calls stubs).
      The user wanted their own music as an extra station next to the game's five instead.
    - **Change:** [`host/games/gangstar/features/customradio.c`](../host/games/gangstar/features/customradio.c) plays the files of the `custom radio`
      folder next to the exe (mp3, ogg, flac, wav, m4a, aac, wma) in a shuffled order. They are
      decoded, resampled to the output rate, and kept about 3 s ahead in a ring.
    - **Decoders (2026-11-05):** mp3, ogg, flac and wav go through the host's own decoders,
      [`host/shims/songdec.c`](../host/shims/songdec.c) over dr_mp3, dr_flac, dr_wav and stb_vorbis
      (unchanged single files in [`host/vendor/`](../host/vendor/), pinned in its `README.txt`;
      public domain or MIT-0/MIT). Their code is built with `-msse2 -mfpmath=sse`, so a song
      decodes to the same samples on Windows and Linux: checked byte for byte with gcc 16
      (MSYS2) and gcc 14 (Debian), where x87 maths had differed by one step of rounding. m4a,
      aac and wma, and a file of the first four kinds that the decoders will not open, go to
      Media Foundation on Windows and `ffmpeg` on Linux. Linux without `ffmpeg` leaves only
      those out. Checks: against `ffmpeg`, flac, wav (16-bit, float, ADPCM) and 5.1 flac are
      identical, mp3 and ogg within 92-103 dB, with the same length; mono is doubled at full
      level as the Media Foundation path does (ffmpeg's own upmix is 3 dB lower). The
      `custom-radio` playtest proves each decoder, a non-ASCII name, the resample, an m4a by
      Media Foundation and a broken mp3 falling back; the same run on Linux with and without
      `ffmpeg` in WSL. ogg is new: Media Foundation does not read it. `gmv_cr_mix` in
      [`host/shims/shimsaudio.c`](../host/shims/shimsaudio.c)'s audio thread adds them after the game's render.
      `custom_radio_set` in [`host/games/gangstar/patch/`](../host/games/gangstar/patch/) makes the music manager count
      six stations (`[mm+0x270]`). Its two song-pick calls (0x601cd in the radio start, 0x605ba
      in the update) go to `gmv_cr_pick`, which answers the sixth station with -1. That is the
      manager's own "nothing playing" slot, so after the tuning noise the game plays nothing there.
    - **When it is heard:** while the radio is on, the sixth station is tuned in and its noise is
      over. Its volume is the game's MUSIC group (vox group 1, set by the options' music slider
      and muted by the pause menu) times `CUSTOM_RADIO_VOLUME` (0 to 1), with a 40 ms fade on each
      change. When it is not heard, the song waits where it was.
    - **SOUND page rows (the user asked for them there):** as the menu file loads,
      `cr_swf_patch` rebuilds `widgetVolumes` with five rows, 88 units apart and 80 higher than the
      game's three (the user found them too low):
      - Row 4, "Custom Radio Volume", is a copy of the Music row (`txtRadio`, `barRadio`).
      - Row 5, "Custom Radio", is a title plus the options' ◀ value ▶. It takes the same parts as
        the GAMEPATCH lines, from widgetActionKeyTitle (`crValueBar`, `btnCrLeft`, `btnCrRight`,
        `crTextValue`). The arrows' names must start with `btn`: a widget takes clicks only from
        the `btn...` parts it lists as it shows (0x16e9e8).
      - The titles are new, wider edittexts in the Music title's style, outline filter included.
        gameswf lays a text out from the box's left edge as if it were at the game's -2 px, so the
        boxes start there and their placements move left by the difference of the centres.
      - The titles' texts are set at runtime, as the game does for its own ("Music").
    - **How the rows work:** the volume widget's vtable (0x6593c8) gets our texts (slot 6) and
      events (slot 5) for these parts.
      - The slider is read the way the game reads its own (x / width + 0.22 + 0.05) and sets the
        volume.
      - The speaker mutes and unmutes it.
      - The arrows switch `CUSTOM_RADIO`.
      - A press, release or click saves both keys.
      - The rows are hidden under `ORIGINAL_MAC_EXPERIENCE`, and the in-game Mac switch turns the
        station off and back on.
    - **The credit:** with the options tab, `om_credit_patch` also puts the author's credit
      "PATCH BY ADRIANSYAH101" at the screen's top right, inside widgetActionKeyTitle (`txtPatchBy`). Only
      the GAMEPATCH page shows it. The VIDEO page, the CONTROLS page and the mission replay hide
      it.
    - **HUD logo:** "PLAY LIST" over "MY MUSIC", as the game's files have it.
      - The HUD radio shows one of five logo clips (`0x17357e`). A sixth clip, `play_list`, held the
        iPhone's iPod logo. The HUD hides it when it is built (`0x17b7c4`), and the Mac never shows it.
      - `0x17357e` now jumps to `gmv_cr_logo`. That does the same job and also shows `play_list`
        for the sixth station.
      - The game's own `0x17357e` does not know that clip, so it would stay up over the next station's
        logo. As `ORIGINAL_MAC_EXPERIENCE` goes on in game, `play_list` is hidden first.
    - **Limits:** a car left on the sixth station after the setting went off gets station 0.
    - `ORIGINAL_MAC_EXPERIENCE=1` forces it off.
19. **Start-up warning for 3 s, skippable — enhancement at the user's request (2026-10-30),
    `SHORT_WARNING=1`, off by default.**
    - **What it was:** the "This game contains coarse language..." screen is a state of its own
      (vtable 0x65a488). Its enter (0x1b814e) keeps the ms clock 0x1b8a60 at `[state+0xc]`. Its
      update (0x1b81a8) shows `Texts.txtDisclaimer` until 14999 ms (0x3a97) have gone by, then
      moves to the next state (0x1b5a30). It takes no input, so the player waits 15 s. A
      `LOAD_PROFILE` run showed that nothing loads meanwhile: every tick took about 4 ms.
    - **Change:** the update slot goes to `gmv_sw_update` (`gamepatch.c`). After 3 s, or when a
      key, mouse button or pad button goes down while the game's window is in front, it sets the
      start 15 s back, and the game's own update moves on. Modifiers and Tab do not count, so
      coming back with Alt+Tab does not skip it. Pads are read through `gmv_joy_any_button`
      (`shimsjoy.c`).
    - **In game:** the GAMEPATCH tab's MENU category has it as SHORT START-UP WARNING. The
      warning is over by the time the menu is up, so the change is saved and applies from the
      next start, like SKIP INTRO MOVIE.
    - `ORIGINAL_MAC_EXPERIENCE=1` forces it off.
20. **Cutscenes with PC keys — enhancement at the user's request (2026-10-30), `CUTSCENE_KEYS=1`,
    off by default, also on the GAMEPATCH tab (MENU: HIDE CUTSCENE KEYS, at once; tested OK 2026-10-30).**
    - **What it was:** during a cutscene the HUD shows `widgetSkipScene` (menu widget 43, vtable
      0x658e88). It holds the letterbox bars, the touch SKIP button `btnSkip` and, while a dialog
      line waits, the arrows: `Texts.dialogArrow` and `whiteArrows`, shown by the app's 0x15ebfa
      and hidden by 0x15eb48. The cutscene start also shows the game's own pointer (CursorGame
      +4 through 0x181d14 at 0x36a86).
      - A click on SKIP calls the cutscene's listener `[w+0xc]->vt[0x3c](l, w, 1)` (0x16fb70).
      - A dialog line (update 0xce02c) waits while `[w+0x18]` is set; a press on the arrows
        clears it.
    - **Change** (`gamepatch.c`):
      - The widget's texts slot (6, 0x16fbee) hides `btnSkip` after the game's texts.
      - The dialog line's arrow show (0xcde01) hides the arrows instead. The two dialog hides
        (0xce020, 0xce34c) mark the line as over.
      - The cutscene start's pointer show passes 0.
      - `shimsinput.c`'s WndProc hands Esc, Enter and Space to `gmv_cutscene_key` while the widget
        is shown, and keeps them (and their release) from the game. After the messages,
        `gmv_cutscene_pump` acts on the game's thread: Esc or Enter calls the listener as the
        SKIP click does; Space clears `[w+0x18]` while a line waits.
      - The menu manager is read from its static (0xa064a0, guard 0xa066d8), so a key before it
        exists does not build it.
    - `ORIGINAL_MAC_EXPERIENCE=1` forces it off.
21. **Tutorial popups with PC keys — enhancement at the user's request (2026-10-30), `TUTORIAL_KEYS=1`,
    off by default, also on the GAMEPATCH tab (MENU: POPUP KEYS, at once). Tutorial OK and question
    popups tested OK 2026-10-30.**
    - **What it was:** a tutorial popup ("Combat ... OK") is `widgetTutoBlock`, menu widget 25
      (Widgets::BlockingTutorial, vtable 0x658e28, built at 0x15f725). Show 0x16f748 sets
      `[w+0x10]`; the OK click (0x16f842) plays sound 0x57, drops the popup's input listeners and
      calls `[w+0xc]->vt[0x3c](l, w, 1)`. Its keyboard listener 0x17eaac does the same for a key
      event of kind 2 and code 0x1b.
    - **Change** (`gamepatch.c`): while the popup is shown, `gmv_cutscene_key` takes Esc, Enter
      and Space (and keeps them from the game). `gmv_cutscene_pump` then calls 0x17eaac with such
      an event, which does what the OK click does.
    - **Question popups** ("Repair? ... YES / NO", asked for the same day) are the dialog widgets:
      show 0x17d430 sets `[w+0x10]`; the click handler 0x17d494 stores the answer at `[w+0x18]`
      (btnYes 0xb, btnNo 0xd) and calls `[w+0xc]->vt[0x3c](l, w, 1)`. The kinds differ only in
      their texts slot: YES/NO 0x659a88 and widgetConfirm 0x659ac8 (price in blue); 0x659a48 has
      one button (btnYes labelled OK). A walk of the menu's widgets finds the one shown. Enter or
      Space answers YES and Esc answers NO; the one-button kind takes any of the three as OK.
    - `ORIGINAL_MAC_EXPERIENCE=1` forces it off.

22. **GTA V-like gamepad layout — enhancement at the user's request (2026-10-30), `PAD_LAYOUT=1`, off
    by default, also on the GAMEPATCH tab (FIXES: PAD LAYOUT, MAC / GTA-LIKE, at once). Tested with a pad 2026-10-30; the limit of
    the stick wheelie to motorbikes was retested on 2026-11-03, with no problem.**
    - **What it was:** the poller 0x191a0 sends pad buttons as events `{vtable, 0x1e, button,
      state}` (0 pressed, 1 held every frame, 2 then 3 released) and the D-pad as `{.., 0x1d,
      direction, state}`. Each control mode has an input handler whose onEvent (vtable slot 2)
      checks that the game is playable, maps the button to an action through its
      `std::map<int,int>` at handler+0x1c and calls its dispatcher: foot 0x656648 / 0x11ca7e /
      0x11edf4, car 0x656aa8 / 0x12bb8e / 0x12d496, flying 0x656ae8 / 0x12e99e / 0x12fab6, sniper
      0x650a88 / 0xa5e96 / 0xa6c14. The D-pad moves (actions 0x23-0x26).
    - **Actions:** 1 shoot, 3/5 get in or out, 4 shops, 9/10 next/previous weapon (sniper zoom),
      0xb pause, 0xc-0xe radio, 0xf leave the sniper view, 0x11 cover / car brake, 0x12 sprint /
      car gas, 0x14 look back, 0x15 reload, 0x16 CB mission, 0x17 handbrake, 0x18 lock-on toggle
      (player+0x6f8), 0x27-0x2a flying up/down/turn.
    - **Change** (`gamepatch.c`): our onEvent puts our action in the handler's map for the
      button, runs the game's onEvent (its checks and dispatch) and puts the game's value back. A
      button missing from the map is first added by a run with state 3, which dispatches action
      0 (a no-op). The D-pad becomes buttons 12-15. LT on foot locks on while held. In a car the
      left stick's up/down is dropped and the triggers are gas and brake (digital, as the Mac's
      own B/X).
    - **Stick camera speed (high-fps fix, on whatever PAD_LAYOUT says; user report 2026-10-30:
      the right stick was far too fast in the sniper view):** the pads are read in the drawn-frame
      phase 0x3ed8 (every other tick), and each read turns the camera by `(int)(v * K)` pixels
      (event `{vt, 0x1b, 1/3, y, x}`; K 16 on foot and in a car for the right stick, 50 in the
      sniper view for both sticks). So the turn speed grew with the frame rate (4x at 120 drawn
      frames/s). Now each read turns by `v * K * dt / 33.3 ms`, with the fractions carried over.
      With `PAD_LAYOUT=1` the foot and car camera turns 4x the Mac's speed (user report: the Mac's
      speed felt heavy; 4x is what 120 drawn frames/s gave before), and the sniper view eases small
      pushes (`v * |v|`) and turns slower as it zooms in (view angle at handler+0x38, 6-50, / 50).
      Off under ORIGINAL_MAC_EXPERIENCE.
    - **Stack alignment (port fix):** every call from the hook into game code (the original
      onEvent, the event's type method) goes through a `force_align_arg_pointer` helper. Before,
      getting into a car crashed: the get-in path resets input (0x19d72), and the SDL code there
      (0x599d2f, `movapd` on a stack local) faulted on the misaligned esp left by our helper frames.
    - `ORIGINAL_MAC_EXPERIENCE=1` forces it off.
23. **A save made as Betty gave her things to Johnny — original game, fixed at the user's request
    (2026-10-30), `SWITCH_SAVE_FIX=1` (default on, off under `ORIGINAL_MAC_EXPERIENCE`; GAMEPATCH
    FIXES: PLAYERSWITCH SAVE FIX, at once). Tested OK: Johnny kept his cash after a save as Betty, whose free
    roam always starts her at 10000, and his weapons came back right too (2026-10-31).**
    - **Shared with MISSION_REPLAY (fixed 2026-10-31):** the replay holds saves back through the same ten
      calls. With this fix put in first, the replay's check found them changed and turned START off
      ("THE REPLAY COULD NOT BE SET UP IN THIS GAME" for every mission). Each now accepts a call already
      wrapped by the other; the replay's wrapper passes on to this one, which writes as the game does
      when the fix is off or not in.
    - **What it was:** the script command PlayerSwitch (0xd00a8 → 0xf6b48) gives control to another
      Player. `*0x689ea8` is the player in control, and `*0x689ea4` is the main one, Johnny (0xf3518
      hands control back to him). Each Player keeps its own cash (+0x754), armour (+0x580), outfit
      (+0x75c) and weapons and ammo (+0x7f4, `Player::getWeaponInfos`). The save writer 0x98148
      writes those of the player in control, and the file does not record who that was, so the
      load gives them to Johnny. Going to the main menu or quitting (0x16458a) hands control back
      first only during a mission (state 4). In free roam as Betty, it saved hers.
    - **Change** (`gamepatch.c`): the writer's ten calls go to `gmv_ssf_write`. When the player in
      control is not the main one, it lends the main one's four fields to the player in control
      for the length of the write. The position is still where you saved (the user's choice).
      `[switchsave]` logs each such save.
24. **Betty shares Johnny's cash and weapons in free roam — enhancement at the user's request
    (2026-10-30), `PLAYERSWITCH_INVENTORY=1`, off by default and under `ORIGINAL_MAC_EXPERIENCE`;
    GAMEPATCH FIXES: PLAYERSWITCH INVENTORY (MAC / SHARED), at once. Tested OK (2026-10-31):
    Betty cycled through all of Johnny's weapons with their ammo.**
    - **What it was:** each Player is built (0xf9e94) with 10000 cash and its own WeaponInfos
      (0x8ae3c, at +0x7f4): the 16 weapon slots' objects at +0, the 11 ammo counts at +0x40, the
      slot in hand at +0x6c. A weapon object's +0xa1 is 1 while it is locked (0x8616a / 0x8612e),
      and +0x3c / +0x44 are what the save keeps of it. So after a switch Betty had 10000 and only
      her fists, which left free roam as her very limited.
    - **Change** (`gamepatch.c`, `gmv_playerswitch_inventory_tick`, once a present): while the
      player in control is not the main one and no mission is on, the first look copies Johnny's
      cash, ammo and weapons onto them; each later one copies theirs back onto Johnny. It is one
      wallet (the user's choice): what Betty spends or picks up is Johnny's too. During a mission
      sharing stops, so missions keep the game's own loadout (the user's choice). Money the same
      Betty made during it (a reward) is added to Johnny's when sharing resumes. The weapon in
      hand is never locked under its holder. `[switchinv]` logs each start and stop.
    - **The "armed" byte** (WeaponInfos +0x70): the first test shared the cash but not the
      weapons, though Betty's slots were unlocked like Johnny's. On foot the weapon keys (actions
      9/10 → 0x11e5ba, number keys → 0x11d222) ask 0xf4b44, which reads this byte through
      0x8a44a; the game sets it when a weapon is given (0x8b00c), and Betty's was 0. It is copied
      with the rest.
    - **Weapons go back onto Johnny one way:** the copy back can unlock a weapon or set the byte,
      but never lock or clear, so Johnny never loses a weapon through Betty.
25. **Play as L.C. after the story — enhancement at the user's request (2026-10-31), `LC_SWITCH=1`,
    off by default and under `ORIGINAL_MAC_EXPERIENCE`; GAMEPATCH FIXES: PLAY AS L.C., at once.
    Rebuilt on 2026-11-03 at the user's request to work like the game's own switch to Betty.
    Tested OK by the user on 2026-11-03 on foot, by car and by motorbike.**
    - **What the game has:** L.C. is the level object `LC_00`, a Player like `Johnny_00` and
      `Betty_00`, kept out of sight until a mission script places him. Scripts 62 and 63 are the
      switch to Betty and back. Each waits at the arrow marker
      `Checkpoint_Betty_Building_Bottom_00` (command 0x8f), then runs ObjectStop on the player,
      CineFadeOut 200, ObjectEnable of the one to play at `Marker_C5ME_GG_00`, ActorFollow Null,
      ActorSetAnim, ObjectMoveTo of the player to `Marker_C5ME_GG_01`, PlayerSwitch, ActorFollow of
      the one left after the new one, ObjectRelease of the one left, CineFadeIn 200, and
      ScriptUnlock of the other script.
    - **Change** (`gamepatch.c`, `lc_tick`, once a present): ours runs the same commands' own code
      on commands built by us, with no new script and nothing new in the save. The marker is
      `Checkpoint_LC_Hotel_00`, the same kind of object as Betty's (type 46, arrow). The level puts
      it at the kerb with radius 300, which reaches the road, so while it is ours it stands at
      71920 -24900 203 (the spot of `Marker_C3M06_GG_08`, on the hotel's walk) with radius 150,
      moved by its own vt+0x6c. It goes home before any mission, because C3M03 waits on it too.
      L.C. arrives at `Marker_C3M06_LC_00` and Johnny goes to `Marker_C3M06_GG_00`, the places
      mission C3M06 opens with. Whoever arrives takes the first marker, 360 units away, so no
      switch back fires at once. ActorSetAnim is left out, since it is the idle pose the switch
      0xf6b48 gives anyway. The fade is the fader's alpha, as in MISSION REPLAY.
    - **Camera:** in the first run the camera stayed on Johnny with L.C. under control. The target
      comes from the control handlers' restart (0x284f6, then 0x11ceb4, then 0x44b88), not from
      0xf6b48 itself, and 0x284f6 restarts them only from state 3 or in a cinematic. Later logs show
      a switch finding them in state 1, where it does nothing, or in state 4, where they are on
      their way. Why they are in state 1 has not been traced, and neither has why the first run's
      camera stayed. For 2 s after a switch `lc_camera` checks whom the camera follows (manager
      0x4547e, +8 the camera, its +4) and gives it through 0x44b88 if wrong, logging the handlers'
      state.
    - **Into the marker by car:** the user drove in, the camera went to L.C. and the keys did
      nothing. The log's "active" read 257 instead of 256, so byte +0x18 of the controls was set.
      That is the lock the input dispatch checks (0x28084, read at 0x19122 and others). The player
      in control sets it when leaving a vehicle (0xf95b5; 0xf8802 clears it again unless his +0x588
      has bit 0x20) and when taking a hit (0x73949). It is cleared when he is up again (0x7ba7e,
      0x76090), but only if he is still in control, so a lock taken by the one left stays for good.
      Which of them set it in that run has not been traced. `lc_swap` now clears it right after the
      switch unless a cinematic is on. Only the player in control can take the lock, so the one
      left cannot take it again. For the same 2 s, if the new player is on foot with the handlers
      steady on another handler, they restart on the on-foot one. That second case was not seen; it
      follows from 0x284f6 doing nothing outside state 3. A last log line gives their state after
      the 2 s. The user reported the car as fine with this fix.
    - **By motorbike:** the user reported the camera staying in one place from
      the moment of the switch, with no turning. A dev trace of the camera shows its target right and
      its own eye and look following the player, while byte +0x26 of the manager (blend running) stays
      1 until the next vehicle. The cause is a cut made during a blend. `lc_camera` gives the target
      through 0x44b88 with blend -1, a cut, which writes -1 to the blend length (+0x58, +0x8c) and
      leaves +0x26 as it finds it. The update 0x455a0 adds to the progress only for a length above 0,
      and 0x44640 ends a blend only at progress 1, so that blend never ends, and until it ends the
      scene's camera stays where the cut put it. The rider is off a frame after ObjectStop, so the
      on-foot handler's blend is running when the swap comes 200 ms later. In the log's one switch
      with the vehicle still set a frame after the stop (taken to be a car from its camera distance),
      the exit runs inside the swap and 0xf8afc there (0x44262) clears +0x26 first, so the cut was
      plain. `lc_camera` now clears +0x26 whenever it is set with a length below 0. That also covers
      a cut from the handlers' own enter (state 0, blend -1), which it would otherwise not see, as the
      target is right in that case. The log says "blend ended" when it does. The user reported the
      motorbike as fine with this fix. That log has five such lines, both ways, each with the lock
      cleared and the handlers steady on foot 2 s later, and a switch by the larger vehicle with
      neither. The marker's log line never names the vehicle, even reading it before ObjectStop, so
      which vehicle is which still comes from the camera distance; why it reads none is not traced.
    - **When it is up:** once every mission with a save bit is done and free roam has been quiet
      for 3 s, again after each mission, and not while playing Betty.
    - **When control leaves L.C. another way:** MissionStart (0xce840), a death (0x3706f) and
      Betty's marker switch by themselves. Then L.C. is released and follows no one, and Johnny
      follows no one and is no longer released. Turning the setting off while playing as L.C.
      brings Johnny back at the hotel and hides L.C. Cash and weapons follow
      `PLAYERSWITCH_INVENTORY`, which acts on any player in control other than Johnny. `[lcswitch]`
      logs each step.
    - **Tested:** the user reported the camera and the marker's new place as normal. The log of that
      run shows the switch to L.C. with the camera given, and a mission replay started as L.C.: control
      went to Johnny, the mission passed and the marker came back at its new place, with no fault.
      The first run, before the camera check, switched back through the marker twice. A later log
      shows a death as L.C.: control went to Johnny with the camera, and the marker worked again
      after it. The next log shows the car fix clearing the lock and three switches back to Johnny
      through the marker with no lock and the camera given, with no fault and no complaint from the
      user. C3M03 (GRILLED GATOR) was then replayed, started as L.C., and the user reported it as fine.
      The first try failed and the second passed, with the player 113 units from the marker's home and
      440 from our spot soon after, so the mission's wait used the home. That log quits within seconds
      of the pass, so it does not show the marker coming back after this mission. The failed try left
      Johnny where it failed: MISSION REPLAY's put-back skips a run in which another character was in
      control at START.
    - **Before (2026-10-31):** L.C. stood at `Marker_C3M07_LC_00` and G, or D-pad Down with
      `PAD_LAYOUT`, within 2.5 m of him switched. That key is gone.

26. **The iPhone version's extra radio songs — enhancement at the user's request (2026-11-03),
    `RADIO_SONGS=1`, off by default and under `ORIGINAL_MAC_EXPERIENCE`; GAMEPATCH FIXES: RESTORED
    RADIO SONGS. First run 2026-11-03: the pack, the ids and the station lists came up as designed, all
    four new hip-hop songs opened from the add-on, and the `ORIGINAL_MAC_EXPERIENCE` switch took them
    out and back with a clean exit, and the songs sound normal to the user. A second run opened songs of
    the electro and rock stations too and switched them off and on from the menu line.**
    - **Level:** the iPhone's nine songs average -25.1 LUFS (BS.1770) and the Mac's 18 songs -21.9, and the
      user noticed it. It comes from the files: the Mac's copies of the 18 songs both versions share are
      3.0 dB louder than the iPhone's (17 of them; `electro_05` by 5.8 dB), and the sound list holds only
      names, no per-sound level. At the user's request (2026-11-03) `tools\radio_gain.py` raises the add-on's
      wavs by 3 dB, to -22.1 on average, with no sample at full scale (the songs peak well below it). The
      songs are encoded again, which adds a little noise. A stereo block's data goes 4 bytes of the left, 4
      of the right, not nibble by nibble: a first tool that read it wrongly scrambled the songs and made
      them sound broken, and its measurements were wrong too. The raised songs sound normal to the user.
    - **What was missing:** the iPhone's stations play 26 songs, the Mac's 18 (electro 5 / 4, hip-hop
      6 / 2, rock 7 / 4). The iPhone has one more electro song in no station's list; the user asked
      for it too, so the electro station has 6 here. The songs are the same format as the Mac's (IMA
      ADPCM, stereo, 32 kHz). `tools\radio_gain.py` makes the folder from the player's own copy and
      `tools\uc_crusher_pack.py` packs it into `add-on\radio.pak`, which is never shipped.
    - **How:** no code of the game changes. The fopen shim sends the songs' names to the unpacked
      add-on and the sound list and station list to copies in `cache\radio`. The sound list gets the
      songs at the end, after the Crusher's sounds when those are in, so no existing id moves. Each
      station's new songs come after its own. The game sizes everything from these lists (0x60ac8,
      0x94ef0) and takes a name holding `m_` for music (0x5f58e), so the MUSIC volume applies.
    - **Switching:** the lists are read once, when the sound manager is made, so with the add-on there
      they hold the songs at every start, whatever the key says. Off sets each station's count back to
      the Mac's, which leaves exactly the Mac's list, and empties its bag. A new song that is playing
      gives way to the next, with the tuning noise. On restores the counts. `rs_tick` puts the key in
      force the first time the manager exists: the game sets its pointer only after the manager's
      constructor, which reads the stations, so a tick never sees half a list.
    - **The Java version's rock songs (2026-11-09, the user's request):** two more rock songs,
      `m_radio_rock_java_02.wav` and `m_radio_rock_java_03.wav`, which the user says the Java version's rock
      station plays. The user took them from their own copy of Gangstar: West Coast Hustle. They were IMA ADPCM
      too, but at 44.1 kHz with 2048-byte blocks and 4-6 dB louder than the Mac's rock songs (-16.2 and -18.2
      LUFS). ffmpeg brought them to the Mac's format (32 kHz, 1024-byte blocks) and level (-21.9 and -22.0 LUFS,
      peaks -12.3 and -10.4 dBFS); the originals stay outside the pack. They are the last two entries of the
      list, so every earlier song keeps its id, and the rock station has 4 / 9 songs with the whole pack. Same
      switch, same preset direction (off under `ORIGINAL_MAC_EXPERIENCE`). Like the rest, never shipped. First
      run 2026-11-09: 11 of 11 songs found, rock 4 / 9, and the user tuned to ROCK TOUR and heard both songs.

27. **Shadows from the sun and moon — enhancement at the user's request (2026-11-03), `DYNAMIC_SHADOWS=1`, off by
    default and under `ORIGINAL_MAC_EXPERIENCE`; GAMEPATCH GRAPHICS: DYNAMIC SHADOWS. Built in four steps (shiny cars,
    characters, every other vehicle, moon), each run in game. The user judged the look right, the twilight
    transition free of jumps and its 0.40 dark enough. How it works is under "Running and settings".**
    - **Fault after the switch, cause unproved:** in one run a replay of MIAMI BOUND! read `ch+0x514` in
      `gmv_walk_ray` on a page no longer committed, a few seconds after `ORIGINAL_MAC_EXPERIENCE` went on and off, so
      the walker's memory had probably been freed (inferred). No shadow code was on the stack. The same steps passed
      in the next run, and the fault has not been seen again.
    - **What changed because of it:** going on used to free our pool copies (vfunc 0x10). The Mac frees nothing of
      its pool before a load, so an object may keep a pointer to a copy that went home. They are now let go instead
      (`pd_let_go`): out of every slot and asleep, never touched again. The pool's slots are still exactly the
      Mac's. The price is one set of copies, about 174 objects, for each on-and-off of the switch until the game
      ends (not measured). With this, three replays followed the switch, with 16, 46 and 5 copies still out as it
      went off: two passed, and the third ran about 45 s until the game was closed. That does not prove the fix: the
      fault was seen only once, and the dev-only line that would prove it,
      `[pool] copy ... was let go but is walking`, never appeared. It remains in the dev build, with `[pool] walker`
      for a recurrence.
    - **Known gap:** a save load under the preset, while copies are still on their way home, can leave a stale address
      in `pd_extra`. A new Mac object at that address would lose its slot. Not seen in a run.

28. **Keyboard movement: two keys swung the body from side to side — our bug, fixed (2026-11-03), `KEY_MOVE=1`.** The
    setting itself is an enhancement at the user's request (2026-10-30), off by default and under
    `ORIGINAL_MAC_EXPERIENCE`; how it works is in the header comment of its block in
    [`host/games/gangstar/patch/`](../host/games/gangstar/patch/).
    - **What was seen:** on W/S with A/D held while the mouse moved, the body turned the wrong way and back with the
      run animation still playing, though the character kept walking diagonally. One key alone did not do it.
    - **Cause, measured with a trace of each move call:** a call (0x685d2) turns the body by half its error, from a
      facing renewed only when a frame is drawn, so the calls between two draws add up. The facing moved by 0.5, 1,
      1.5, 2 and 3 of the error for 1, 2, 3, 4 and 6 calls, and above 2 the error grows. Our handlers sent one call
      for each held key, so W+D reached 6 calls where W alone rarely did, and one stretch swung to about 100 degrees
      and more. The mouse most likely only supplies the first error (inferred: the trace ran only while the view
      turned).
    - **Fix:** only one held key sends the move, the game's own one-key rate. It is the first held key whose events
      still came in the last 100 ms, because a held flag can outlive its key. The direction still comes from all the
      held keys.
    - **Result:** calls per facing for W+D went from 2/4/6/8 (642/118/48/2 times, 11 calls once) to 1/2/3/4/5
      (337/136/56/2/1). The user confirmed the movement normal in game.

29. **Slow frames on the way to the main menu (2026-11-03).** A tester reported frames of 246.8 and 98.7 ms.
    A `GMV_LOAD_PROFILE=40` run split the way there into three causes. All numbers carry the profiler's
    overhead. Matching the tester's two frames to these ticks is inferred, as they came from another run.
    - **The game's own load, before the title (left alone):** ticks of 394.5 and 282.5 ms, mostly file reads and
      the game's own code. The Mac runs the same load.
    - **Our `glow_box` (`shimsgl.c`, fixed):** the iPhone sun/moon glow took a tick of 162.6 ms, about 70% of its
      samples in the blur: two 512x512 textures, one channel at a time, columns at an 8 KB stride. `glow_rows` and
      `glow_cols` now blur all four channels together with SSE2 and walk the columns row by row. Offline, a
      texture went from 53.8 to 1.3 ms on a synthetic glow, and the largest 16-bit difference was 1 of 65535, on
      0.25% of the values. In game the tick went to 57.6 ms, and the user sees the glow unchanged.
    - **Box2D at the menu entry (original game, made heavier by `DRAW_DISTANCE`):** six ticks of 43-131 ms, about
      440 ms, made 12339 proxies at 2x draw distance, the bounds an axis going from 9682 to 30564. `CreateProxy`
      (`0x1b9e50`) renumbers every bound above the one it inserts, so the cost of a proxy grows with the count.
      A tighter loop would gain about 2x at best (an estimate, not built). A trace of every present and proxy, since
      removed from the dev build, measured it: replaying 15662 of its records (bound counts match on every record; the insert positions are
      off by up to 69, taken to be bodies that moved, as moves are not recorded) gives 189 million bounds renumbered,
      and the frame gaps come to about 2.3-3.3 ns a bound.
    - **What was choppy "during the black":** it is not black. Copies of the screen from that trace show the menu's frame and logo
      brightening over the black from the first frames, the buttons sliding in by about 0.6 s, and the pointer,
      all while the proxies are made. Only the city behind is black, until the fade at about 2.2 s.
    - **Insertion order (tried offline, not worth building):** only the order of the renumber was changed in the
      replay. Sorting within each tick gains 1.01x and leaves the heaviest tick at about 49 million bounds; sorting
      the whole burst ascending gains at most 1.79x and needs all the proxies known in advance, while they come over
      8 ticks; descending costs 0.49x. A reorder would also change proxy ids and pair order.
    - **`LOAD_BUDGET` (`gamepatch.c`, default 200, also under `ORIGINAL_MAC_EXPERIENCE` since 2026-11-05, switched live):** the wait
      state `0xd2164` raises the items a tick from 25 to 1500 (`0xd21a4`, budget at `0x641114`), and this takes that
      to 200. Three runs at 2x draw distance, counting only presents that read no pixels:

      | | 1500, with the old fade hold | 1500 | 200 |
      |---|---|---|---|
      | Gaps over 33 ms in the first 1.1 s | 7 | 4 | 1 |
      | Longest gap after 234 ms | 113.7 ms | 101.1 ms | 32.4 ms |
      | Proxies end at | 517 ms | 575 ms | 1069 ms |
      | Fade timer starts at | 2216 ms | 2265 ms | 2118 ms |
      | Bounds an axis at the fade | 30580 | 30580 | 30580 |

      The first column has one sampled present fewer than the others, so its count is a little high against them.
      The wait ends when `0x1adbc4` finds the queue at `0x689fe0` empty, and that is the queue `0x134fc0` is given
      with the limit on (`0x135616`), so a small budget makes a wait longer and cannot start the fade early. The queue
      at `0x689f28` (`0x134d30`) also reads the budget and is not in that check; what is left of it would go on at 25
      a tick. Three gaps of 98, 45 and 110 ms stay in the first 234 ms (two of them read pixels), and are taken to be
      the queue at `0x689ee0`, which has no limit (`0x1355f8`). The user saw nothing odd in the city or the menu. How
      much longer a load into the city takes was not measured. A later run with the key as shipped gave 2 gaps over
      33 ms (44.61 and 33.01 ms), proxies ending at 1038 ms and the wait ending at about 2.09 s. Turning
      `ORIGINAL_MAC_EXPERIENCE` on in that run put 1500 back: `[macaudit]` lists no difference at `0xd21a4`. The way
      back to 200 writes no log line.
    - **`MENU_FADE_HOLD` (removed):** it gave `CineFadeIn`'s Update dt 0 on the main menu while proxies came in bursts.
      With it the fade waited about 190 ms, without it about 10 ms, and the longest gap in the fade was 17.95 and
      17.79 ms, with no proxy made: the burst ends about 1.7 s before the fade begins, and the choppy part is the menu's
      frame and buttons, which that timer does not move.
    - **Crash found on the way, `fake_class` (`shimsobjc.c`, our bug):** a run stopped at start-up with
      `[objc] unhandled message [NSURL locationInWindow]`. The list of fake classes had no lock, so the IGP
      thread's first `NSURL` and the main thread's first `NSEvent` (the first mouse move) could share a slot, and
      the event was named `NSURL`. A stress test of the logic gave the wrong name in 64115 of 200000 rounds with
      the old code and in 0 of 200000 with an `SRWLOCK`. That this race fired in that run is inferred from the
      message and the timing, as the log does not show the slot.
    - **Profiler crash (`loadprof.c`, dev builds only):** `after_call` read the bytes before a stack value equal
      to msvcrt's base, where the page before is free. It now checks with `ReadProcessMemory` when those bytes
      cross a page.
    - **The loading fixes go on under `ORIGINAL_MAC_EXPERIENCE` (2026-11-05, the user's choice).** The user found
      the preset choppy in an odd way, in the main menu too, while another game run on the same host looks smooth
      at the same 30 drawn fps. Between stalls both games are equally even: the pacer, the present before the wait and vsync
      off are the same code in both. The difference was the main menu's scene changes, which load a part of the
      city each. Two 90 s runs in the main menu at 60 ticks/s (`PACE_LOG`, `LOAD_PROFILE=30`, `PERF_PROFILE`):
      with the preset as it was, 16 ticks over 30 ms, the scene changes 116-219 ms (the game's 1500-item budget)
      followed by several ticks of 34-50 ms (the game's 16 ms sleep); without the preset, 8 ticks, 31-89 ms. With
      `LOAD_SLEEP_SKIP`, `LOAD_BUDGET` 200 and `FAST_LOAD` on under the preset: 7 ticks (3 of them the start-up load,
      the same 444 ms either way), the scene changes 31-83 ms. How long such a stall lasts depends on the machine,
      not on the game, so the user chose to keep these three on and, at the same time, every original-bug fix off
      under the preset (`MENUFLASH_FIX`, `FADE_EDGE_FIX`, `MENU_FADE_CLICKS`). `FAST_LOAD` moved out of the
      preset's hook group (`patch_apply.c`), or `[macaudit]` counted it as an enhancement left in. `mac-preset`,
      `mac-audit` and `mac-switch` pass; 34 places either way.

30. **A release build has no debug mode, and its log names nothing of the player's (v.1.0.0, 2026-11-03, at the
    user's request).** An audit of the strings in the release exe still found code from the debug work: the
    `[perf]` reports, the audio dump, the `[vbfill]` and `[vtxmiss]` memory dumps, the `VBO_STREAM` experiment,
    the bloom shader's `survey` uniform, the `GMV_*` environment overrides, the import list, and event lines
    with heap addresses, the game's paths and the names of the player's songs. All of it now depends on
    `GMV_DEV`, so the compiler leaves it out of a release build.

    - **Environment:** `gmv_envkey` in [`host/shims/shims.h`](../host/shims/shims.h) is `getenv` in a dev build and NULL in
      a release build, so a `GMV_*` variable has no effect there. The exe still imports `getenv`: the game's
      own calls use it through the import shim.
    - **Log lines:** `gmv_trace` in [`host/core/log.h`](../host/core/log.h) is `gmv_log` in a dev build and
      `if (0) gmv_log(...)` in a release build, which drops the text and the call but still checks the
      arguments. 166 event lines moved to it. Calls in their arguments were checked to only read; the one
      that assigned a variable used later (`shown`, in the replay's HUD check) was split out first.
    - **What stays in a release log:** the version, the settings in effect, the hardware found, what a feature
      applied, skipped or failed at, the guards' counts (`[crash58]`, `[vtxmiss]`), and the crash details (`[fault]`,
      registers, module, stack scan).
    - **Log privacy:** `log_hide_path` in [`host/core/log.c`](../host/core/log.c) writes the exe's folder as `%EXE%` and
      the game's as `%APP%`, as `%USERPROFILE%` already was, in UTF-8 and the ANSI code page. A folder shorter
      than its tag stays as it is, because the tag must fit in its place. The custom station's lines say "a
      song" instead of a file name. A release build no longer writes to stdout, so `LOG=0` leaves nothing
      behind, even with stdout redirected.
    - **Rendering and timing are meant to be unchanged.** With the experiment left out, `gmv_gls_draw` calls the
      real draw directly, which is the path a release build already took. The glow shader computes
      `t.rgb * k`, which is what `survey = 0` gave. The audio thread no longer measures each buffer's peak.
    - **Checked** by compiling every file both ways under `-Wall -Wextra` and scanning the staged folders for
      debug tags and private strings. The exe's import table lists no network library.
    - **Removed from every build:** `FIXEDSTEP` and `FIXED_MS`, the fixed-timestep experiment. They were off by
      default and normal play did not use them. An older ini's lines are reported as unknown settings and ignored.
    - **The dev folder** is built from the same source with `-Dev`, for people working on the port. It keeps the
      console, the diagnostics and the `GMV_*` overrides, and its log names the player's songs, so its readme says so.

31. **The start-up "Loading" screen, one step a frame — speed-up at the user's request (2026-11-05), `FAST_LOAD`,
    default on, and on under `ORIGINAL_MAC_EXPERIENCE` since later that day (off before; both the user's choice).** The user asked whether loading could be
    faster. A run with the log stamped from outside (`SHORT_WARNING=1`, `NO_INTRO=1`) gave: the loader done at
    0.25 s, the SDL gamepad start-up 0.28-0.64 s, the window and GL context 0.2 s, the first frame at 1.6 s, the
    warning until 4.0 s, the "Loading" screen until the title at 6.1 s, and about 1 s from Continue into the city.
    - **What the screen does:** the loading state (vtable `0x65a288`, update `0x1b589e`) keeps its step at
      `[state+8]` and its percent at `[state+0xc]`. Each update, every 33 ms, calls the step function `0x1abc92`
      once (at `0x1b5935`); at step 16 it goes on to the title (`0x65a388`). A temporary probe logged each step:
      step 3 (`0x1abe25`) walks a list built by `0x1a8d20` (about 17 items) twice, one item a call, so it stayed
      about 34 updates (1.1 s) at 20 % with ticks under 3 ms. Only steps 5 and 8 have much to do: 438 and 316 ms of
      small reads, texture uploads and a `glGetError` after each upload.
    - **Fix (`fix_load.c`):** the call at `0x1b5935` goes to `fl_step`, which calls the step function again with
      the step it returned until it reaches 16 or 10 ms have gone by in this frame. The update stores the last step
      and moves on to the title itself. What the steps call between them and repeat is harmless: `0x165c6` clears
      the touch flags and `0x294be` is called at every update anyway. Patched with `patch_undo_now` set, next to
      `SHORT_WARNING`, and read at start: the screen is over by the menu (GAMEPATCH > MENU: FAST START-UP LOADING,
      next start).
    - **Result:** from the end of the warning to the title, 2.08 s with `FAST_LOAD=0` and 0.73 s with it. The title
      looked as before, and the boot, features, world, gamepatch and mac-preset scenarios passed.
    - **Not done, and why:** the SDL gamepad start-up is about 165 ms warm, nearly all of it DirectInput (7 ms with
      `SDL_JOYSTICK_DIRECTINPUT=0`), but generic pads need it. The `glGetError` calls (16 sites, all around buffer
      and texture uploads, `0x4ffd41`-`0x50a907`) cost perhaps 50-150 ms in steps 5 and 8, and skipping them would
      hide real errors.

32. **The defaults: enhancements on in a new ini — the user's choice (2026-11-05).** The user asked that with
    `ORIGINAL_MAC_EXPERIENCE=0` the defaults be the settings of their own screenshots. This supersedes the "off by
    default" in the items above, which describe each feature as it was built.
    - **What changed:** only the defaults `settings.c` writes into a new `GangstarMV.ini`: `FRAME_MS=8.3333`
      (60 fps), `ANISOTROPIC_FILTERING=16`, `AMBIENT_OCCLUSION=1`, `BLOOM=2`, `BLOOM_THRESHOLD=0.5` (INTENSITY HIGH), `BLOOM_WINDOWS=2`,
      `FOG_MODE=2`, `WATER_REFLECTIONS`, `VEHICLE_REFLECTIONS`, `DYNAMIC_SHADOWS`, `SPAWN_FADE` and `SUN_MOON` 1,
      `DRAW_DISTANCE=2`, the three densities 3, every gameplay, control (`KEY_MOVE`, `PAD_*`) and menu
      (`SHORT_WARNING`, `CUTSCENE_KEYS`, `TUTORIAL_KEYS`, `HIDE_*`) key 1, `CUSTOM_RADIO=1` with
      `CUSTOM_RADIO_VOLUME=0.53`, `UC_CRUSHER` and `RADIO_SONGS` 1. `NO_INTRO` stays 0 and the resolution blank (the
      monitor's), as in the screenshots on the user's 1920x1080 screen.
    - **What did not:** the call sites' built-in values. A key left blank or missing from the file still means the
      Mac's, so an older ini keeps what it says, the GAMEPATCH tab's "blank" column stays true, and the playtest
      scenarios, which write only their own keys, run as before. The Mac preset already forced every one of these
      keys, so `ORIGINAL_MAC_EXPERIENCE=1` is unchanged.
    - **Without the add-ons or songs** (a friend's copy): the radio keeps its five stations, the Crusher and the
      iPhone songs are left out with a log line each, and nothing else changes.
    - **Test:** a new `first-start` scenario starts with no ini (`noini`, added to `playtest.ps1` with `inihas`),
      checks the file the game wrote and each effect's log line, and walks in the city. It passed, as did boot,
      gamepatch and mac-preset.

33. **A line of picture along the top and left of a black fade — original game, fixed at the user's request
    (2026-11-05), `FADE_EDGE_FIX`, on by default; off under `ORIGINAL_MAC_EXPERIENCE` (on there for a few hours on 2026-11-05, then
    off with the other original-bug fixes by the user's choice).** The user saw the top row and
    the left column of the screen keep showing the picture while the fade after CONTINUE held it black.
    - **Cause:** the game's 2D projection is moved by 0.375 of a pixel (`FADE_TRACE` now logs it: x scale 2/width,
      translate -1 + 0.75/width), so a rectangle from (0,0) starts 0.375 px inside the top-left corner. The
      cutscene fader's rectangle (caller `0x034aa1`) comes out at -0.375, on the corner; the state fades'
      (`0x1a5cea`, the one after CONTINUE, and the generic fade state's) at exactly 0. Without samples that still
      covers the centre of the first row and column. With 4x MSAA, the Mac's own setting, some of their samples
      lie outside it, so those pixels stay part picture. Read back right after the fade's draw: the corner and
      the middle of the top row and left column were not black with `MSAA=4` and were black with `MSAA=0`; the
      Mac preset showed it the same. Not the present: `msaa_present` copies the frame 1:1.
    - **Fix:** `fix_fade_edge.c`. draw2DRectangle (`0x4c5ce0`) hands the driver its destination rectangle at
      `0x4c5db8` (`mov eax,[ebp+0x10]` / `mov [esp+4],eax`). Those 7 bytes become a call to a stub that
      passes a copy starting at (-1,-1) when the rectangle starts at (0,0). The far corner is kept, and so is any
      rectangle that starts elsewhere. `ecx`/`edx` (the driver's vtable and the driver) are saved around the
      call. The function's entry, where `FADE_TRACE` hooks, is not touched. Live on the GAMEPATCH tab (FIXES:
      FADE EDGE FIX).
    - **Test:** a `fade` scenario continues the save with `MSAA=4`, shoots the black at the window's own size
      (`shot <name> full`) and checks it with a new `edge` step: the top row and left column may be at most 3 of
      255 brighter than the row and column two pixels in. With the fix 0.4; with `FADE_EDGE_FIX=0` 70.8 (fails, as
      it should). boot, gamepatch, mac-preset, first-start and world passed too.

34. **Reginald's missions forgotten by every save — original game, fixed at the user's request (2026-11-06),
    `MISSION_SAVE_FIX`, on by default and on under `ORIGINAL_MAC_EXPERIENCE` too (the user's choice: a save made
    under the preset must keep them, so `MISSION_REPLAY` still offers them once the preset is off).** The user
    asked to have Reginald's missions and every wheel and surfboard marked done in their save, and wondered
    whether their own earlier edits of the save had broken it. They had not: every save has the same gap,
    the new-game one and an untouched one from 2026-10-18 too.
    - **Cause:** the mission manager keeps one completion bit per mission in 8 words, one per group (`mgr+0x90`,
      `0x8d018`); group 8 is Reginald's five missions in Little Haiti (`_packMac\file002627.dat`, the mission
      list: records 62 and 67-70, bits 0, 1, 5, 6, 7). The save writer's MMSK loop (`0x98699`-`0x986da`) stops at
      the count of another list, the scripts' tutorial list (`scripts+0x3a8`, 7 entries, read by `0x33400`), so
      the 8th word is never written and Reginald's missions read as not done after any load. The loader
      (`0x977ac`) takes indices up to 7, so the file has room for it.
    - **Fix:** `fix_mission_save.c`: the loop's test `cmp [eax+0x3a8],edi; jg` at `0x986d4` becomes
      `cmp edi,8; jl`, so the writer writes all 8 words. A save made this way still loads in the original game.
      `MISSION_REPLAY`'s SIDE list (`mr_key`) took groups up to 7 only; it now takes group 8 too, so Reginald's
      finished missions can be replayed (their scripts start at a `0x8f`, as the other side missions do).
    - **The save:** the user's `Gangstar2.sav` got an MMSK block with index 7 = `0xe3` and the collected bit on
      all ten CCOL and all ten BCOL entries (each entry is `number << 4 | collected`; the loader keeps bit 0, in
      order). The old file is `savegame\Gangstar2 - before edit 2026-11-06.sav`. All five Reginald scripts were
      already idle in it, so nothing had to change there. Not given by the edit: the moment of the last pickup,
      which is when the game shows "Surfboards collected" / "Wheels collected" (Sea-Ride and W3C-H unlocked); no
      code that grants those at load was found. The achievements block (ACHI) was left as it was.
    - **Test:** a probe run showed the loader taking the 8th word (word 8 = `e3` in memory) and the game's next
      save dropping it without the fix; with the fix, that save kept it (71 of 71 missions done). mac-audit and
      boot passed; the macaudit lists the one patched place under the preset, as for `STOPLOCK_FIX`.

## Diagnostics

The diagnostics are `[Diagnostics]` and `[Testing]` keys of the ini. They work in the plain and `-Dev`
builds only: a release build does not contain them (item 30). The `trace_*.bat` and `pace_*.bat` files named in the fix log were one-off scripts and
are no longer in `build\`.

- `GMV_DUMP_FRAME=<n>` captures back-buffer frame *n* to `gmv_frame.bmp` (written in the game's cwd,
  the project root).
- `GMV_FLICKER_CAP=1` (`flicker_cap` in `shimsgl.c`) watches every presented frame for 60 s from the
  rating splash or the title / main menu state and logs `[flickcap]` per-frame stats. It saves frames
  that differ from both neighbours (A-B-A) or flash dark/bright, with their neighbours, as
  `gmv_flick_e<n>_{1,2,3}_f<frame>.bmp` (plus every 30th frame as `gmv_flick_ctx_*`).
- `GMV_CAM_TRACE=1` logs `[camtrace]` camera-follow stats every 2 s (frame dt, the camera's trail
  behind the character, character and eye speeds).

**Error box.** Every way the game or the host stops on an error goes through `gmv_fatal` in
[`host/core/log.c`](../host/core/log.c): it writes the crash dump, closes the log, silences the sound, lets go of the
mouse, shows a message box with the reason and ends the process with the same exit code as before. The
box runs on a thread of its own, because the stopped thread may own the game's window, and a box there
would run that window's messages through the game's WndProc. A second thread that stops meanwhile waits.

| Code | Stop |
|---|---|
| 1 | The static initializers or `main` could not start |
| 3 | A missing import was called |
| 4 | Missing data was touched, or `abort`, `__stack_chk_fail`, a failed `assert` |
| 5 | C++ runtime: pure virtual call, `bad_alloc`, `length_error` and the like |
| 6 | An access violation (the vectored handler in [`host/core/veh.c`](../host/core/veh.c)) |
| 7, 8 | An unhandled Objective-C message; an Objective-C exception nothing caught |
| 9 | Any other unhandled exception, such as an illegal instruction (`last_chance` in `veh.c`) |
| 10 | The window, pixel format or OpenGL context could not be made |

Problems the port survives do not show a box: the guards such as `CRASH58_GUARD` and an enhancement that
falls back only log a line. `TEST_FATAL` (plain and `-Dev` builds) stops the game about 10 s in, to try it.

**Crash dump.** On every code but 1 and 10, where the game never got to run, `gmv_fatal` first writes
`cache\GangstarMV.dmp` ([`host/core/crashdump.c`](../host/core/crashdump.c)). A helper thread does it, with dbghelp
from System32 loaded only then, and gets 20 s; a file it could not finish or clean is deleted, and the box
names only the log. The exception record comes from the first fault (`gmv_dump_note`), so the dump opens on
the faulting thread; a stop with no exception shows every thread where it stands. Every build uses
`MiniDumpNormal | MiniDumpFilterModulePaths`. `MiniDumpWithIndirectlyReferencedMemory` was tried and dropped:
it brings in the heap, where the C library's copy of the environment (the user's name, anything kept there) and
freed paths broken into pieces cannot all be found again. `log_scrub_bytes` then blanks every copy of the log's
hidden folders with the rest of the path after them, and every value of the environment, in either slash and any
ASCII case, as UTF-8, the ANSI code page and UTF-16, keeping the length so the file stays valid. A bare file name
can remain, which is why the README warns about it. A log line that held a hidden folder leaves the rest of the
path behind the shorter text in its stack buffer, where nothing could find it again, so `out` wipes the buffer.
Game code sits at its own fixed addresses, so a dump's stack can be read against the Mach-O with no symbols.

On Linux the same file is an ELF core ([`host/core/crashdump_linux.c`](../host/core/crashdump_linux.c)), which gdb
opens with the program: `gdb GangstarMV GangstarMV.dmp`. The faulting thread writes it inside its signal handler,
so it uses only system calls and static buffers, and it sends memory to the file with `write()`, which answers
EFAULT for a page it cannot read rather than faulting. The other threads, listed from `/proc/self/task`, get a
real-time signal, keep their registers and wait in the handler until the file is written. Mesa's threads block
every signal, so they are left out (6 of 40 threads with llvmpipe). The file holds an NT_PRSTATUS for each thread
(the faulting one first, with NT_PRPSINFO, NT_SIGINFO and NT_FILE), and each thread's stack from esp up, at most
1 MB. NT_FILE has the mapped files by name only, so gdb warns that it cannot open them. On Linux, `log_scrub_bytes`
blanks the hidden folders (each also with its links resolved), every value of the environment and the machine's
name, longest first: blanking the user's name first left the game's folder, OLDPWD and PATH in the file, the user's
name gone from them but the rest no longer matching. `TEST_FATAL` 1-4 ran in WSL (2026-11-05): each wrote the file,
gdb showed the faulting line and the threads, and the file held no user name, machine name or folder.
`LOG=0` means no dump, and each start deletes the previous one, since the new log would not match it. The
playtest's `crash-*` scenarios try all of this in the game (`dumpclean`).

**AZERTY keyboards.** The game has its own answer. `0x19f0b0` compares the layout's id with
`com.apple.keylayout.French`, and the four input handlers pass the result to `0x18ac4e`, which binds Z and Q to
forward and left, A and W to what Q and Z do on other layouts. `0x18214` also adds the top row's unshifted symbols
as digits, which the port does not need: it sends digits by their virtual key, and those stay in the table.
`TISGetInputSourceProperty` answers with that id when the key at W's place is Z and the key at Q's place is A,
so Belgian counts too, which the Mac's test left out; any other layout gets NULL as before. Keys reach the game by
their label through `gmv_vk_to_sdl`, as they do through the Mac's SDL, so a scancode table would swap them twice,
and QWERTZ needs nothing: W, A, S and D stand where they do on QWERTY. The bindings are kept in
`Gangstar2.preferences`, so a preferences file from a QWERTY start keeps W: the `azerty` scenario needs `noprefs`.
It is the Mac's own behaviour, so it has no key and stays on under `ORIGINAL_MAC_EXPERIENCE`.

**Build check.** [`.github/workflows/build.yml`](../.github/workflows/build.yml) compiles the `-Release` and `-Dev`
builds on every push and pull request, with no game files and no exe kept. They compile different code (`GMV_DEV`).
A compile error or any warning fails it.

**Playtest.** [`tools/playtest/playtest.ps1`](../tools/playtest/playtest.ps1) plays the scenarios in
`tools/playtest/scenarios` and says PASS or FAIL for each. A run has a folder of its own, `build\playtest\run`,
with a copy of the exe and only the keys the scenario names, so `build\savegame` is never touched; what it leaves
goes to `build\playtest\results\<time>\<scenario>`. Input is real (`SendInput`), since parts of the port read key
states and raw mouse, so the window needs the foreground: if it loses it, the scenario stops with FOCUS LOST and
sends nothing more. Screenshots are copied from the screen, so a release exe can be run too (`-Exe`; scenarios
marked `dev` are skipped). Any `[fault]`, `[MISSING]` or `[crash58]` line, unexpected box, unexpected exit or hung
window is a FAIL. `selftest-must-fail` holds only wrong expectations: it must say FAIL on each. `moved` and `still`
compare two screenshots, so walking is checked without a person; `layout` and `holdscan` press keys by their place
on the keyboard under a layout that is only loaded for the run. The main menu's scenes change by themselves, so `moved`
says nothing while the menu is up, and the log has no line for reaching the city: `leftmenu` compares where the menu's
buttons are. `pad` and `padfor` hold what a dev build's virtual pad (`PAD_TEST`) presses, through the same SDL3 calls as a
real pad; it says nothing of real drivers. `camera` and `uncapped` are for comparing two builds with `-Exe`: how far a known
mouse move turns the camera, and the frame rate with no pacer. `drag` sizes the window by Windows' own sizing loop,
started by `WM_SYSCOMMAND`, as the game keeps the cursor inside its picture and a press on the border never gets there;
the corner moves as far as the mouse does from where it stands, and only as far as the picture lets the cursor go, so
`resize` and `resize-option` place it first or allow for it. A person's drag at the border is not tested by this.
`drag <dx> <dy> <shot>` also takes a shot with the mouse still down.

**The probe loop still applies** for any new `[MISSING]`/`[objc]`/`[fault]` wall: implement the one
named symbol, rebuild, repeat. The fault handler dumps registers, the faulting module and a game-range
stack scan. Keep every change building clean under `-Wall -Wextra` (36 host C translation units + 3 asm)
and stage 1 passing.

## Bring-up history

How the game was first brought up, kept as it was written at the time. Later sections override it
where they differ. For example, the window is no longer a fixed 1024×768, and v1.0 was the name of the
target then.

### Status snapshot of 2026-10-22

> Status snapshot: **2026-10-22**. ~**465 / 528** imports shimmed. **The game runs AND renders.**
> It boots its entire Cocoa launch path, loads its **whole 2660-file asset pack** (no missing asset),
> enters the engine's own loop, and — **confirmed by screenshot** (`screenshots/`) — draws the full
> **title screen** and the interactive **main menu** (CONTINUE / NEW GAME / OPTIONS / INFO / QUIT)
> over a live 3D background flythrough, at ~60 fps (OpenGL, NVIDIA RTX 3050 Ti), with no missing
> symbols and no faults. **The "black window" is solved and was never a rendering bug.** Startup
> runs a ~15 s timed wait (the stubbed intro video, which times out) → a brief sequencer → a
> **region rating/legal splash** that waits for a **tap** → the **title/menu state** (`0x659f28`),
> which also waits for a tap ("Press any key"). **Mouse/touch input is wired**, so a tap dismisses
> the splash and advances the title to the menu; with no input the game simply halts at the first
> gate (the splash) and the only thing on screen is that splash's text — which also used to
> **flicker** (now fixed: the engine draws every other frame but was swapping every frame, so a
> blank back-buffer alternated with the drawn one; the present shim now skips the swap on
> nothing-drawn frames). **Menu buttons work, NEW GAME launches the game, and it reaches
> actual in-world gameplay** — NEW GAME → intro cutscene (3D Miami street, "JOHNNY", dialogue, SKIP)
> → after SKIP, the **full 3D open world** loads: the player character next to a car, fields/road/
> skyline, and a working **HUD** (minimap + health bar). All confirmed by screenshots (`screenshots/`).
> The core engine works end to end. **Keyboard input moves the player (ARROW keys), mouse
> camera-look is wired (hands-free, like a normal PC game), and audio plays** (a `waveOut` backend
> driving the game's own CoreAudio render callback). **The game is playable into missions** — taken
> on foot and driving through several story missions (up to "Swamp Boogie"). Two blockers from the
> last snapshot are now **fixed**: the **movement-triggered crash** (the engine's vertex-format
> converter missed one `cl==0xFF` guard; the host reinstates the engine's *own* guard — see below)
> and a separate **"3D world turns black"** bug (the camera went NaN from a `dt`=0 produced by a
> coarse ~15 ms system clock — fixed with a high-resolution timer). Since then: **right-click
> auto-target** (on-foot and drive-by) is wired, the **quit/teardown crash** is fixed (clean exit),
> the **host window follows the in-game resolution + fullscreen** choice live (`glViewport`-tracked;
> fullscreen draws at the chosen resolution, scaled to the monitor with black bars as the Mac's mode
> switch showed it), and **saving is confirmed working** — the game writes
> a real tagged save (`Gangstar2.sav`: CASH/ARMR/player-pos/stats) and its data now lives **next to
> the exe** in `savegame\` and `cache\` (portable, no longer under `%LOCALAPPDATA%`). The
> **periodic null-read crash** (`0x58c1ab`, ~13 min into play) is now **fixed** — a permanent guard
> on the game's own null-delegate forwarder converts a 100%-fatal `*NULL` read into a logged no-op
> (verified: a 26+ min session with zero faults). The **car engine sound** is now understood to be a
> **faithful port, not a bug**: all vehicle audio loads from the packs and the idle/onidle/offidle
> rev loops crossfade by volume — this build's mixer simply has no runtime pitch-sweep (only base
> sample-rate + doppler), so the engine "rev" is a loop crossfade, not a pitch ramp. Remaining
> toward a fully-polished v1.0 (still a **pure** port — no graphics enhancements until the base game
> is 100%): deeper save/load + full-mission playthrough testing. Earlier real fixes still stand: drawable size was
> never applied (`-windowDidResize:` never fires here), the saved resolution read back as 0×0
> (`NSUserDefaults` returned nil), and — the big one — **every asset failed to open** because the
> engine built `./C:\…\_packMac\file000000.dat` (a `./` before an absolute Windows path), now
> stripped by the `fopen` shim. Testing gotcha: a **minimized** window makes the back buffer
> unreadable (read-backs all-black even when rendering works) — always test with a normal window.
> See **Where it stands**.

### Bring-up stages

| Stage | What | State |
|---|---|---|
| 1 | Load image at its own addresses, patch low range, bind every import, verify | ✅ `STAGE 1 PASSED`, exit 0 |
| 2a–2c | Shims: libc, Win32 (pthread/mmap/time/stdio), C++/libstdc++ | ✅ |
| 2d | Run all **451** C++ static initializers | ✅ all return normally |
| 3a–3b | Real Objective-C runtime: `objc_msgSend` / `_stret` / `Super`, `__OBJC` class registry, exceptions | ✅ |
| 3c | `NSApplicationMain` → create app + delegate → `-applicationDidFinishLaunching:` | ✅ runs real game code |
| 3d–3g | Cocoa launch: Foundation/AppKit, `std::string`, CoreGraphics, real Win32 window + WGL OpenGL 4.6 | ✅ |
| 3g | Run loop: timers, deferred `performSelector`, per-frame `-update`, logo skip via notification | ✅ |
| GL bind | ~112 OpenGL imports bound to `opengl32` (lazy cdecl→stdcall trampolines) | ✅ |
| iostream | `std::ostringstream` + `str()` by-value ABI + `std::string` `_Rep` | ✅ |
| display | real resolution list from `EnumDisplaySettings` (resolution picker) | ✅ |
| render | game enters its own loop, draws, presents ~60 fps via `SDL_GL_SwapBuffers` | ✅ **runs** |
| input | keyboard (SDL3) moves the player; **mouse camera-look** (SDL3's relative mouse → `CGGetLastMouseDelta`); touch/menu pointer | ✅ |
| controllers | the game's own SDL joystick support over **SDL3** (`shimsjoy.c`); its gamepad calibration / remap pages work as on the Mac | ✅ played with a pad over XInput + winmm (2026-10-30); on SDL3, by the virtual pad only so far |
| pad icons | `PAD_ICONS=1` (enhancement, off by default and under `ORIGINAL_MAC_EXPERIENCE`): the options menu's gamepad pictures show Xbox (A B X Y, LB RB LT RT, View/Menu) or PlayStation (✕ ○ □ △, L1 R1 L2 R2, L3 R3, SHARE/CREATE, OPTIONS) buttons for the pad last used; drawn by the port into the game's own atlas texture (no overlay, the menu file unchanged) | ✅ ran OK with a DualSense |
| audio | the game's own CoreAudio render callback driven by an SDL3 backend; all SFX/vehicle samples load from the packs | ✅ **sound plays (faithful)** |
| gameplay | on-foot + driving; 66 of 66 missions done in a save (2026-10-31) | ✅ **playable** |
| resolution | host window follows the in-game resolution + fullscreen live (`glViewport`-tracked; full screen at the chosen resolution, scaled to fit with black bars) | ✅ |
| targeting | right-click **auto-target** on-foot and drive-by (`WM_RBUTTONDOWN/UP` → `rightMouseDown:/Up:`) | ✅ |
| save | the game writes/reads its real save (`Gangstar2.sav`); data lives next to the exe (`savegame\`, `cache\`) | ✅ **saves** |
| quit | clean teardown (`-terminate:` → `ExitProcess(0)`), no exit-time crash | ✅ |
| stability | permanent guard on the `0x58c1ab` null-delegate forwarder (the ~13-min null-read crash) | ✅ **26+ min clean** |
| pacing | game ticks at a steady **60 Hz** (as on a 60 Hz Mac) on any monitor; 1 ms timer for the game's sleeps | ✅ **2026-10-25** |

### Where it stood

**Where it stands.** `--run-main` boots the game all the way through: Cocoa launch, logo skip,
localization, the GameLoft-Live / IGP analytics thread ("sending Game information" → "done"), its
resolution pick, texture-atlas loading (it builds names like `atlas15.tga_80` through the
`ostringstream` path), and then **into the game's own render loop**. From there it issues real
draw calls — `glDrawElements(GL_TRIANGLES, 450, GL_UNSIGNED_SHORT, …)` — and presents frames via
`SDL_GL_SwapBuffers` at ~60 fps onto a real 1024×768 window, vsync-locked. There is **no missing
symbol, no fault, and no unresolved GL call** on this path; the process keeps running because a
game's main loop runs until you quit it.

> If `--run-main` "doesn't return", that's expected — look at the window. A thread sampler
> (`probe.exe`, no longer in the tree) will show the main thread momentarily parked in a vsync wait *inside*
> `glDrawElements` (the NVIDIA driver); that is normal frame pacing, not a hang.

**Root-causing the black window (2026-10-15)** uncovered, in order:

*The viewport was never set (fixed).* `-[EAGLView reshape]` calls `glViewport(0,0,backingWidth,
backingHeight)`, and `backingWidth/Height` are set only by `-[EAGLView setBackingSize::]`, which the
game runs **only when a "backing-dirty" flag is set** — a flag `-[GameDelegate windowDidResize:]`
sets, and macOS fires that when the window is first shown. This host has no Cocoa event loop, so it
never fired. Fix: right after `-applicationDidFinishLaunching:` the host calls
`[eaglView setBackingSize:W:H]` then `[eaglView reshape]` with the GL client size, and the window is
created with a true 1024×768 client. Now `glViewport(0,0,1024,768)` fires.

*The saved resolution read back as 0 (fixed).* `-[GameDelegate loadUserSettings]` sets the game's
`screenWidth/Height` from `[[NSUserDefaults objectForKey:@"ScreenWidth"/@"ScreenHeight"] integerValue]`.
Our stub returned nil → `[nil integerValue]` = 0 → a 0×0 game resolution (hence the earlier logs
`window … h=-768`, `view 0×0`). The shim now returns 1024/768/false for those keys (and a minimal
`NSNumber` was added), so the window/view come out 1024×768.

*Every asset file failed to open (fixed — the big one).* `GMV_FOPEN_TRACE=1` showed ~800 failed
opens: the engine's file system builds `./` + an already-absolute bundle path, e.g.
`./C:\…\GangstarMV.app/Contents/Resources/_packMac/file000000.dat`, and the `./` before a `C:\`
drive letter is invalid, so **none** of the 2660-file asset pack loaded → empty scene. The `fopen`
shim now retries from the drive letter when the direct open fails; 352 `.dat` files then load (the
remaining failures are logical names the game resolves to `fileNNN.dat` through the pack index).

*Testing gotcha that wasted time:* a **minimized** window (`Start-Process -WindowStyle Minimized`)
makes the GL back buffer non-composited, so `glReadPixels`/`GMV_DUMP_FRAME` return all-black even
when rendering is fine (a blue-clear self-test reads `0,0,0,0` minimized, `0,0,255,255` normal).
Always inspect with a normal window.

*Where it actually is now (2026-10-16) — the window renders.* A full-window screenshot in the
title/menu state shows the complete Gangstar Miami Vindication **title screen** (character art,
Miami skyline, logo, "© 2011 Gameloft", "Press any key to continue") and, after one tap, the
**main menu** (CONTINUE / NEW GAME / OPTIONS / INFO / QUIT, the Gameloft LIVE! badge, a "More
Games!" corner) over a **live 3D background flythrough** that cycles scenes (Miami rooftop →
everglades). `GMV_GL_TRACE=1` confirms the menu state issues **~38 `glDrawElements` calls, ~11 300
verts, 36 texture binds per frame** (vs the splash's 1 call/450 verts). So the loader + GL shims +
draw pipeline are all correct. **This overturns the earlier "menu widgets never loaded / nothing
draws" diagnosis** — that was sampled during the ~3-frame "enter menu" transition (fe-screen
`0xa9dc34` is a transient animation: a quick fade widget + two instant action widgets, then it
legitimately closes). The real cause of the black window was simply that the game **waits for input
at the rating/legal splash** (state `0x65a388`); with no tap it halts there and only the splash text
shows. Once the splash is dismissed (a tap — `GMV_CLICK_S3=1` auto-injects one for testing), the
state machine advances: ~15 s wait `0x65a488` → sequencer `0x65a288` → splash `0x65a388` → title/menu
`0x659f28`. **Flicker fixed** in `shimsgl.c`: the engine clears+draws every *other* frame but
`SwapBuffers` ran every frame, so with double buffering a blank buffer alternated with the drawn one
(the "kedip"); the present shim now skips the swap on frames where nothing was drawn/cleared
(default on; `GMV_NO_FLICKER=0` to disable). The root cause, swap-exchange instead of the Mac's
retained back buffer, is now fixed directly with a `PFD_SWAP_COPY` pixel format (item 7 of
the fix log), which also targets the cutscene fade-to-black.

Getting here this session meant fixing, in order: the **resolution picker** (it enumerates
`CGDisplayCopyAllDisplayModes` and indexed a NULL list — now backed by the real Windows display
modes), `stat$INODE64` (file-exists checks during asset load), the **`ostringstream::str()`
return ABI** (`std::string` is returned by value through a hidden struct-return pointer the callee
pops — mingw returns the 4-byte object in a register instead, so it is done in asm), the **stream
accumulator** (it missed streams whose `ios_base` ctor was inlined and whose sub-objects sat
outside the old address window — now lazy-created and widened), the streambuf get/put accessors,
and `std::string::_Rep::_M_destroy`.

The intro-logo idle problem from earlier **is solved**: a real `NSNotificationCenter` plus a
non-nil fake `QTMovie`, and the run loop posts `QTMovieDidEndNotification` once (frame 3), so the
game runs its own logo-done handler, which starts the game and enters the render loop.

`main()` is a thin Cocoa wrapper:

```objc
pool = [[NSAutoreleasePool alloc] init];
rc   = NSApplicationMain(argc, argv);   // the game is a standard Cocoa app
[pool release];
return rc;
```
