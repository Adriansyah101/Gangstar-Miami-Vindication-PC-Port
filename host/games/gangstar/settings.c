/* settings.c -- GangstarMV.ini: every key the game's settings file knows, and the ORIGINAL_MAC_EXPERIENCE preset. */
#include "game.h"
#include "log.h"


#if GMV_DEV
#define SOLO_CAR_ "SOLO_CAR, "
#define LOG_SECTION "Diagnostics"
#else
#define SOLO_CAR_ ""
#define LOG_SECTION "Log"
#endif

/* The words that differ by system.  The paths' backslashes become the system's separator when config.c writes the
   file, so the texts keep writing them the Windows way. */
#ifdef _WIN32
#define PORT_ "the Win32 port (GangstarMV.exe)"
#define EXE_ "exe"
#define ARROW_ "the Windows arrow"
#define LOG_HIDES_ "your Windows user folder, this folder and the game's appear in it as %USERPROFILE%, %EXE% and %APP%"
#define FTELL_HELP_ "Answer the game's ftell from a kept file position instead of asking Windows each time (removes the ~2 s freeze while a scene loads)."
#define FSEEK_HELP_ "Serve the game's seeks inside the already-read part of a file without asking Windows, with a 64 KB read buffer, as the Mac's C library does (shortens the start-up loading hitches; needs FAST_FTELL)."
#define MEMMOVE_BY_ "Windows' slow backward copy"
#define SONGS_ "mp3, ogg, flac, wav, m4a, aac or wma"
#define DPI_HELP_ "Draw at the screen's real pixels when Windows display scaling is above 100%, so the picture is sharp and a window keeps the size you chose in pixels (it looks smaller than with 0). 0 = Windows stretches the window, which blurs it. Takes effect at the next start."
#else
#define PORT_ "the Linux port (GangstarMV)"
#define EXE_ "program"
#define ARROW_ "the system's arrow"
#define LOG_HIDES_ "your home folder, this folder and the game's appear in it as $HOME, %EXE% and %APP%"
#define FTELL_HELP_ "Windows only: Linux's C library already keeps the file position, as the Mac's does."
#define FSEEK_HELP_ "Windows only: Linux's C library already serves seeks inside the already-read part of a file, as the Mac's does."
#define MEMMOVE_BY_ "the C library's"
#define DPI_HELP_ "Windows only: how the window behaves when Windows display scaling is above 100%."
#define SONGS_ "mp3, ogg, flac or wav; m4a, aac and wma also play when ffmpeg is installed"
#endif

static const gmv_setting settings[] = {
    {"ORIGINAL_MAC_EXPERIENCE", "Preset", "0", "1 = play it as on the Mac: the [Timing] improvements but the loading ones, the [Graphics] enhancements, the fixes of the original game's bugs (MENUFLASH_FIX, FADE_EDGE_FIX, MENU_FADE_CLICKS, RADIO_LOGO_FIX, SWITCH_SAVE_FIX), " SOLO_CAR_ "HIDE_MORE_GAMES, HIDE_GAMELOFT_LIVE, SHORT_WARNING, CUTSCENE_KEYS, TUTORIAL_KEYS, ALLOW_WEAPON_SPRINT, MINIMAP_SHOPS, MISSION_REPLAY, PAD_ICONS, PAD_LAYOUT, PAD_MENU, PAD_HINTS, KEY_MOVE, PLAYERSWITCH_INVENTORY, LC_SWITCH, UC_CRUSHER, CUSTOM_RADIO, RADIO_SONGS, NO_INTRO and SHOW_CURSOR are forced off, the population densities set to 1, the port's other [Fixes] and the loading speed-ups (LOAD_SLEEP_SKIP, LOAD_BUDGET=200, FAST_LOAD) forced on and MSAA set to the Mac's 4, whatever they say below."},

    {"FULLSCREEN",     "Display", "1", "1 = full screen: the game is drawn at SCREEN_WIDTH x SCREEN_HEIGHT and scaled to fill the monitor, with black bars if their shapes differ (pick the monitor's own resolution for the sharpest picture); 0 = a window of SCREEN_WIDTH x SCREEN_HEIGHT."},
    {"SCREEN_WIDTH",   "Display", "",  "The game's Screen Resolution: the width in pixels, 320-7680 (blank = the monitor's)."},
    {"SCREEN_HEIGHT",  "Display", "",  "The game's Screen Resolution: the height in pixels, 240-4320 (blank = the monitor's)."},

    {"TEXFIX",       "Fixes", "1", "Draw untextured shapes in plain colour, so the death and map-menu fades turn the screen black."},
    {"SWAP_COPY",      "Fixes", "1", "Ask for a copy-swap pixel format, so the screen keeps the last drawn frame between game ticks (cutscene fades)."},
    {"NO_FLICKER",    "Fixes", "1", "Present only the frames the game actually drew (fallback if copy-swap is unavailable)."},
    {"VTXFIX",         "Fixes", "1", "Guard against the vertex-converter crash when the player moves."},
    {"CRASH58_GUARD",  "Fixes", "1", "Guard against the game's null-forward crash (logged as [crash58])."},
    {"STOPLOCK_FIX",   "Fixes", "1", "Stop a car's gas and brake from dying for good after a full stop."},
    {"FAST_FTELL",     "Fixes", "1", FTELL_HELP_},
    {"FAST_FSEEK",     "Fixes", "1", FSEEK_HELP_},
    {"FAST_MEMMOVE",   "Fixes", "1", "Do the game's overlapping backward memory moves with SSE2, as the Mac's C library does, instead of " MEMMOVE_BY_ " (shortens the physics stutters while driving)."},
    {"DPI_AWARE",      "Fixes", "1", DPI_HELP_},
    {"MENUFLASH_FIX",  "Fixes", "1","Keep the title on screen until the main menu's fade from black starts (hides two half-ready frames the original game shows)."},
    {"FADE_EDGE_FIX",  "Fixes", "1", "Make the black fades (after CONTINUE, a death, a menu) cover the screen's top row and left column too: with anti-aliasing on, the original leaves a line of picture there."},
    {"STATIC_VBO",     "Fixes", "1", "Keep the game's models that never change (the city, the cars) in the graphics card's memory instead of sending them to it again on every draw as the Mac build does (about 10% of each frame back; a model the game changes goes back to the old way)."},

    {"MSAA",           "Graphics", "4", "Multisample anti-aliasing (smooth edges): 4 = as on the Mac (the game asks for 4x), 0 = off, or 2 or 8 (capped at what the GPU supports)."},
    {"ANISOTROPIC_FILTERING", "Graphics", "16", "Anisotropic filtering for the game's textures: 0 = only what the game asks for (as on the Mac), or 2, 4, 8 or 16 (roads and walls stay sharp at a distance; capped at what the GPU supports)."},
    {"AMBIENT_OCCLUSION", "Graphics", "1", "1 = ambient occlusion: soft contact shadows in corners and under cars, drawn under the HUD (not on the Mac)."},
    {"AO_STRENGTH",    "Graphics", "",  "How dark the ambient occlusion gets (blank = 1.2; lower is lighter, higher is darker)."},
    {"AO_RADIUS",      "Graphics", "",  "How far the ambient occlusion reaches, in the game's world units of about 1 cm (blank = 60)."},
    {"BLOOM",          "Graphics", "2", "Bloom, a soft glow around the bright parts of the world (neon, street lights, headlights, the sun), drawn under the HUD (not on the Mac): 0 = off, 1 = low (the bloom alone), 2 = high (the bloom, plus BLOOM_LIGHTS and BLOOM_WINDOWS)."},
    {"BLOOM_INTENSITY", "Graphics", "", "How strong the bloom is (blank = 1.0; 0 = none, 2 = twice as strong)."},
    {"BLOOM_THRESHOLD", "Graphics", "0.5", "How bright a pixel must be to glow, 0-0.95 (blank = 0.75; lower makes more of the picture glow)."},
    {"BLOOM_LIGHTS",   "Graphics", "", "With BLOOM=2: how strongly the game's light sprites glow, however dim (street-lamp cones, neon, crane and highway lights), hidden by whatever stands in front (blank = 1; 0 = not at all, higher = stronger)."},
    {"BLOOM_WINDOWS",  "Graphics", "2","With BLOOM=2: how strongly the lit windows painted on the buildings glow at night, by the game's clock (picked out as bright, warm-white spots in the wall pictures, so now and then something else that looks like one glows too) (blank = 1; 0 = not at all, higher = stronger)."},
    {"WATER_REFLECTIONS", "Graphics", "1", "1 = the sea reflects the city and the sky (screen-space reflections: only what is on screen can be reflected, so the reflection fades to the sky's colour near the screen's edges and when you look down at the water) (not on the Mac)."},
    {"REFLECTION_STRENGTH", "Graphics", "", "How strong the sea's reflections are (blank = 1.0; 0 = none, lower is fainter, up to 2)."},
    {"DYNAMIC_SHADOWS", "Graphics", "1", "1 = shadows that follow the sun and the moon, in place of the fixed one under each vehicle and person (not on the Mac). Only vehicles and people cast them. Also on the GAMEPATCH tab (DYNAMIC SHADOWS), at once."},
    {"VEHICLE_REFLECTIONS", "Graphics", "1", "1 = the shiny cars (the ones the game gives its sky shine) reflect the street, the buildings and the other cars (screen-space reflections: only what is on screen can be reflected; where nothing is, the game's own shine stays). Matte cars stay matte (not on the Mac)."},
    {"VEHICLE_REFLECTION_STRENGTH", "Graphics", "", "How strong the cars' reflections are (blank = 2.0, the strongest; 0 = none, lower is fainter)."},
    {"SUN_MOON",       "Graphics", "1", "A sun by day and a moon at night in the sky, with their glow (not on the Mac). 2 = as the iPhone version draws them: the glow shines through buildings. 1 = enhanced: the glow is hidden behind buildings with a soft edge and is brighter. 0 = none, as on the Mac."},
    {"SPAWN_FADE",     "Graphics", "1", "1 = the cars and pedestrians that come and go around you fade in and out over about a second (drawn through a fine dot pattern that fills in or thins out) instead of popping in and out (not on the Mac)."},
    {"DRAW_DISTANCE",  "Graphics", "2", "How far the city is drawn, as a multiple of the Mac's 450 m in full detail and 900 m in low detail: 1 = as on the Mac, up to 4 (traffic and pedestrians still come and go as on the Mac; above 1 the buildings near the map's centre also get their collision at once, which the original only gives them after you have moved away and back)."},
    {"FOG_MODE",       "Graphics", "2", "How the fog is drawn: 0 = the game's own, on only some of its materials, as on the Mac, from 450 m to 900 m at DRAW_DISTANCE=1, 50 m further for each 0.5 more (with ORIGINAL_MAC_EXPERIENCE=1 the Mac's 450 m to 1200 m); 1 = linear fog over the whole city in the sky's colour, from 100 m to 800 m at DRAW_DISTANCE=1, 50 m further for each 0.5 more (900 m at 2, 1100 m at 4); 2 = the hybrid fog: like 1, but by the true distance from the eye at every pixel, thickening along a smooth curve to complete at the fog's end, and thinner higher up (FOG_HEIGHT) (blank = 0). Nothing past the fog's end can be seen."},
    {"FOG_HEIGHT",     "Graphics", "", "With FOG_MODE=2: how fast the fog thins with height, in metres; this high above the sea it is about a third as thick (blank = 60; 0 = as thick at every height). Whatever the height, everything is fully fogged at the fog's end."},

    {"FRAME_MS",       "Timing", "8.3333", "Frame period in ms (blank = 16.667, i.e. 60 ticks/s like the Mac; 0 = uncapped). The game draws every other tick, so F drawn frames/s is 500/F ms (120 fps = 4.1667)."},
    {"SWAP_INTERVAL",  "Timing", "", "Vsync swap interval (blank = 0 while frame pacing is on, otherwise the driver's default)."},
    {"PHYSICS_SYNC",   "Timing", "1", "1 = step vehicle/object physics once per tick at the FRAME_MS rate instead of 1/60 s (smooth driving when FRAME_MS is not 16.667); 0 = the game's own step, as with ORIGINAL_MAC_EXPERIENCE=1."},
    {"CAMERA_SYNC",    "Timing", "1", "Keep the on-foot camera's follow (the running sway) as at 60 ticks/s when FRAME_MS is below 16.667; no effect at 60."},
    {"WALK_SYNC",      "Timing", "1", "Keep the walking characters in the main menu's scenes and in cutscenes steering around obstacles as at 60 ticks/s when FRAME_MS is below 16.667 (they would otherwise flick their facing back and forth); no effect at 60."},
    {"AI_SYNC",        "Timing", "1", "Keep the characters' AI judging whether they are stuck as at 60 ticks/s when FRAME_MS is below 16.667 (running characters would otherwise be taken as stuck and restart their run over and over, like the main menu's shotgun walker); no effect at 60."},
    {"LOAD_BUDGET",    "Timing", "200", "Items a tick the game takes from the city's queue while it waits for streaming, in place of its 1500, so the city's collision is spread over more frames and the main menu comes in smoothly; 1-1499; 0 = the game's 1500, as on the Mac."},
    {"LOAD_SLEEP_SKIP", "Timing", "1", "Skip the 16 ms sleep the game takes every tick while it waits for streamed objects (2-4 s after the menu appears, a respawn or a zone change), so those seconds run at full frame rate instead of ~60 fps."},

    {"ALLOW_WEAPON_SPRINT", "Gameplay", "1", "1 = keep your weapon in hand while you sprint, as on the iPhone (the Mac puts it away and shows fists until you let go of Sprint)."},
    {"MINIMAP_SHOPS", "Gameplay", "1", "1 = the minimap also shows the weapon shops, car rentals and repair shops near you, with the icons of the pause map's legend (the game keeps them in the minimap's list but never draws them there). Only the ones inside the minimap show; the teleports stay hidden (not on the Mac)."},
    {"MISSION_REPLAY", "Gameplay", "1", "1 = a MISSION REPLAY tab in the in-game SERVICES menu, below CORRUPTION, showing the missions you have completed one at a time with their giver, location, type and reward (not on the Mac). The arrows at the top of the page switch between story, side missions and races, and START plays one again. You keep the cash you earn in a replay; everything else goes back when it ends. Also on the GAMEPATCH tab (FIXES: MISSION REPLAY), from the next opening of SERVICES."},
    {"TRAFFIC_DENSITY", "Gameplay", "3", "How many cars drive around you, as a multiple of the Mac's (at most 18 at once, fewer at night): 1 = as on the Mac, 0 to 4 (2 = up to 36). New cars also turn up that much sooner."},
    {"PEDESTRIAN_DENSITY", "Gameplay", "3", "How many pedestrians walk around you, as a multiple of the Mac's (at most 14 at once, fewer at night): 1 = as on the Mac, 0 to 4."},
    {"PARKED_CAR_DENSITY", "Gameplay", "3", "How many parked cars stand around you, as a multiple of the Mac's (at most 10 at once): 1 = as on the Mac, 0 to 4."},
    {"UC_CRUSHER", "Gameplay", "1", "1 = after ON A LION HUNT, the Crusher monster truck of Urban Crime (the freemium Gangstar Miami for iPhone) stands parked on the beach by LC's hotel, with its own engine sound; nobody else drives it (not on the Mac). Also on the GAMEPATCH tab (FIXES: CRUSHER MONSTER TRUCK), at once: turned off, or with ORIGINAL_MAC_EXPERIENCE switched on in game, it leaves the city, a D340 in its place if you are driving it. Needs add-on\\crusher.pak (or the add-on\\crusher folder) next to the " EXE_ ", made from your own copy of Urban Crime. With the add-on there, its files are read at every start, whatever this says, so that it can come without a new start."},
    {"SWITCH_SAVE_FIX", "Gameplay", "1", "Fix of the original game: a save made while you play as someone else (Betty, after the story switches you to her) keeps Johnny's cash, armour, outfit and weapons, instead of handing hers to Johnny when the save is loaded; you still start where you saved."},
    {"PLAYERSWITCH_INVENTORY", "Gameplay", "1", "1 = when the story switches you to someone else (Betty) in free roam, they share Johnny's cash and weapons (with ammo): what they spend or pick up is Johnny's too. Missions are left as the game has them. 0 = the Mac's: Betty starts with 10000 and no weapons (not on the Mac)."},
    {"LC_SWITCH",      "Gameplay", "1", "1 = once every mission is done, a marker stands at L.C.'s hotel in free roam: walk into it to play as him, as the game's own marker for Betty does, and into it again to go back to Johnny. His cash and weapons follow PLAYERSWITCH_INVENTORY (not on the Mac)."},

    {"NO_AUDIO",       "Audio", "0", "1 = no sound at all."},
    {"CUSTOM_RADIO",   "Audio", "1", "1 = the car radio gets a sixth station after the game's five that plays your own music: the files in the \"custom radio\" folder next to the " EXE_ " (" SONGS_ "), in a shuffled order. Switch to it like any station. It follows the game's MUSIC volume, and the HUD radio shows PLAY LIST / MY MUSIC for it (the iPhone's logo, not on the Mac). Also on the game's SOUND page: the row Custom Radio (ON / OFF) under Custom Radio Volume."},
    {"CUSTOM_RADIO_VOLUME", "Audio", "0.53", "How loud the custom station is, as a share of the game's MUSIC volume, 0 to 1 (blank = 1): the SOUND page's Custom Radio Volume slider (its speaker mutes it)."},
    {"RADIO_SONGS",    "Audio", "1", "1 = the game's radio stations also play the nine songs of the iPhone version that the Mac does not have (not on the Mac). Not included: they need add-on\\radio.pak next to the " EXE_ ", made from your own copy of the iPhone version. Also on the GAMEPATCH tab (FIXES: RESTORED RADIO SONGS), at once. With the add-on there, the songs are listed at every start, whatever this says, and left out of the stations while this is off."},
    {"AUDIO_FRAMES",   "Audio", "", "Frames per audio render call, 32-4096 (blank = 256)."},
    {"AUDIO_NB",       "Audio", "", "Number of audio buffers, 2-32 (blank = about 93 ms worth)."},
#if GMV_DEV
    {"SOLO_CAR",       "Audio", "0", "Test aid: hear only the player's car (mutes traffic, peds, music, radio)."},
#endif
    {"RADIO_LOGO_FIX", "Audio", "1", "Show the car's radio station on the HUD when you get in (the original keeps showing the station that played last)."},

    {"SHOW_CURSOR",    "Input", "0", "1 = keep " ARROW_ " visible even when the game hides its cursor."},
    {"MENU_FADE_CLICKS", "Input", "1", "Let the main menu take clicks while its background scene fades (the original ignores them for the length of each fade)."},
    {"PAD_ICONS",      "Input", "1", "1 = the gamepad pictures of the options menu (the button list and the calibration page) show the buttons of the pad you last used: A B X Y, LB RB LT RT, View and Menu for an Xbox pad; cross, circle, square, triangle, L1 R1 L2 R2, L3 R3, SHARE (CREATE) and OPTIONS for a DualShock 4 or DualSense. Other pads keep the game's own pictures, which show only where the button is (not on the Mac). Also on the GAMEPATCH tab (FIXES: PAD BUTTON ICONS), at once."},
    {"PAD_LAYOUT",     "Input", "1", "0 = the Mac's gamepad buttons; 1 = a GTA V-like layout (Xbox names; it assumes the standard pad layout, not the calibration page). On foot: A sprint, B reload, X shops/buildings, Y enter a vehicle, LB previous weapon, RB cover, hold LT to lock on, RT shoot, D-pad left/right previous/next weapon. In a car: RT gas, LT brake/reverse, RB handbrake, LB shoot, hold A to lock on (drive-by), Y get out, X/B previous/next weapon, R3 look back, D-pad left/right radio station, down radio on/off, up the CB's special mission (the left stick only steers). Flying: RT up, LT down, LB/RB turn, A shoot, Y get out. Sniper: RT shoot, RB/LB zoom in/out, B leave. Start pauses in all of them."},
    {"PAD_MENU",       "Input", "1", "1 = the gamepad works the menus (not on the Mac): while the game shows its yellow pointer, the left stick or the D-pad moves it, A (cross) clicks and B (circle) goes back like Esc; a button also skips the intro movie."},
    {"PAD_HINTS",      "Input", "1", "1 = the tutorial boxes name the gamepad's buttons (those of the PAD_LAYOUT in use) while the pad is the last thing you used (not on the Mac). The texts are English; in another language the keyboard texts stay."},
    {"KEY_MOVE",       "Input", "1", "0 = the Mac's on-foot keys (one direction at a time, the character snaps round, W with A/D turns the camera); 1 = enhanced (GTA-like): W/A/S/D together walk diagonally, the character turns smoothly, and holding Alt walks instead of running (not on the Mac)."},

    {"NO_INTRO",       "Startup", "0", "1 = skip the intro movie."},
    {"INTRO_FLIP",     "Startup", "0", "1 = flip the intro movie vertically (only if a decoder shows it upside down)."},
    {"SHORT_WARNING",  "Startup", "1", "1 = the \"coarse language\" warning at start-up shows for 3 s instead of the game's 15, and any key, click or gamepad button skips it."},
    {"FAST_LOAD",      "Startup", "1", "1 = the \"Loading\" screen after the start-up warning takes as many of its steps a frame as fit, instead of one step every 33 ms, so the title comes about 1.5 s sooner; what it loads is the same. 0 = one step a frame, as on the Mac."},
    {"CUTSCENE_KEYS",  "Startup", "1", "1 = cutscenes as on a PC: no SKIP button, dialog arrows or pointer on screen; Esc or Enter skips the cutscene, Space shows the next line of dialog."},
    {"TUTORIAL_KEYS",  "Startup", "1", "1 = Esc, Enter or Space presses the OK of a tutorial popup (\"Combat ... OK\"); in a question popup (\"Repair? YES / NO\") Enter or Space answers YES and Esc NO."},
#if GMV_DEV
    {"TERM_NOOP",      "Startup", "0", "1 = ignore the game's own Quit request instead of exiting (debug escape hatch)."},
#endif
    {"HIDE_MORE_GAMES", "Startup", "1", "1 = no \"More Games!\" corner on the main menu (it opened Gameloft's ads page, which is long gone; the Mac shows it)."},
    {"HIDE_GAMELOFT_LIVE", "Startup", "1", "1 = no Gameloft LIVE! badge on the main menu (it opened Gameloft's online sign-in, which is long gone; the Mac shows it)."},
    {"GAMEPATCH_TAB",  "Startup", "1", "1 = a GAMEPATCH tab in the game's options menu, below VIDEO, to change the settings of this file from inside the game (not on the Mac); 0 = no tab; blank = 1. Changes are saved here at once and take effect at once. With ORIGINAL_MAC_EXPERIENCE=1 the tab holds only that switch, which also takes effect at once (the settings it covers go to the Mac's values or back to this file's). The enhancements' hooks are not in at all while it is on: its switch puts them in and takes them out, the collision tables growing as it goes off and going back to the Mac's as the city next loads."},

    {"LOG",            LOG_SECTION, "1", "1 = write what the port does to cache\\GangstarMV.log (the file to send with a bug report; it stays on this computer, and " LOG_HIDES_ ", never by name); 0 = no log file."},
#if GMV_DEV
    {"GL_TRACE",       "Diagnostics", "0", "Per-frame draw counts and GL state probes."},
    {"FADE_TRACE",     "Diagnostics", "0", "Fade rectangles, cutscene fades and the death-triggered screenshots (gmv_fadecap_*.bmp)."},
    {"FLICKER_CAP",    "Diagnostics", "0", "Start-up flicker capture: from the splash / 'press any key' screen, 60 s of per-frame stats and screenshots of suspect frames (gmv_flick_*.bmp)."},
    {"LOOP_TRACE",    "Diagnostics", "0", "Main-loop caller and stack scans."},
    {"LOAD_PROFILE",   "Diagnostics", "0", "[loadprof] what the main thread spends each slow tick on (uploads, file reads, waits, the rest); 1 = ticks over 40 ms, or give the threshold in ms."},
    {"PERF_PROFILE",   "Diagnostics", "0", "[perf] every 5 s: the main thread's work per tick, where its samples land (game, host, GL driver), and the GPU's time for the scene and each post pass."},
    {"PACE_LOG",       "Diagnostics", "","[framepace] timing stats every 2 s (blank = on only if FRAME_MS or SWAP_INTERVAL is set)."},
    {"STATE_TRACE",    "Diagnostics", "0", "Game state / front-end probes."},
    {"INPUT_TRACE",    "Diagnostics", "0", "Mouse, key and window-message trace."},
    {"THREAD_TRACE",   "Diagnostics", "0", "Thread creation trace."},
    {"FOPEN_TRACE",    "Diagnostics", "0", "File-open trace (2 = cutscene file misses only)."},
    {"RES_TRACE",      "Diagnostics", "0", "Resolution / window-size trace."},
    {"INTRO_TRACE",    "Diagnostics", "0", "Intro movie playback trace."},
    {"CARSND_TRACE",   "Diagnostics", "0", "Car sound and engine pitch trace."},
    {"ENGSRC_TRACE",   "Diagnostics", "0", "Engine sound source trace."},
    {"VEH_TRACE",      "Diagnostics", "0", "Vehicle AI drive-to destination trace."},
    {"CAM_TRACE",      "Diagnostics", "0", "[camtrace] how the camera follows the running character, every 2 s."},
    {"RADIO_TRACE",    "Diagnostics", "0", "[radio] station table, radio start, station switches, song picks and the HUD radio logo."},
    {"FOG_SURVEY",     "Diagnostics", "0", "[fogsurvey] the distinct fixed-function setups (textures, blending, alpha test, lighting, fog) the world's 3D draws use, with draw counts every 30 s; 2 = instead the sea's chunks around the camera in one frame every 10 s."},
    {"BLOOM_SURVEY",   "Diagnostics", "0", "[bloomsurvey] with BLOOM on (2 for the glow layer's modes): tint the draws that could be lights (magenta additive, cyan lit windows, yellow other blended, green drawn after the bloom, blue flat additive) and log their setups; 2 = instead show only what BLOOM_LIGHTS and BLOOM_WINDOWS make glow, on black; 3 = as 2, with the BLOOM_WINDOWS walls drawn at any hour, dim, the parts taken for lit windows magenta (only in the game's night textures), and those night textures saved to bloomtex\\ next to the " EXE_ "; 4 = each kind of light BLOOM_LIGHTS makes glow tinted in its own colour (red, green, blue, yellow, cyan, magenta, then again) and logged with its number and the game code that drew it."},
    {"VEHICLE_SURVEY", "Diagnostics", "0", "[vehsurvey] with WATER_REFLECTIONS on: how the game draws its vehicles. Each part of a car (anything drawn with a car body's own placement) is tinted in its setup's colour (red, green, blue, yellow, cyan, magenta, then again) and logged once with its blending, depth, alpha test, texture combine, textures (with their alpha's spread), lighting and normals; a few of the body's vertices are logged too. A table of draws per setup follows every 30 s."},
    {"VEHICLE_REFLECTION_DEBUG", "Diagnostics", "0", "[vr] with VEHICLE_REFLECTIONS on: each shiny car's pixels are tinted by what the reflection pass made of them: red = something drawn later covers it, grey = lost in the fog, blue = seen edge-on (faded out), magenta = the model's normal pointed into the car and was turned round, yellow = the reflected ray found nothing on screen (the game's own shine stays), green = it reflects what the ray hit, cyan = the same after passing the car itself."},
    {"SPAWN_SURVEY",   "Diagnostics", "0", "[spawnsurvey] the city's spawn points (kind, category, caps, delays), each logged once, and every 15 s the world's count of each category and the classes of the objects in it."},
    {"UC_SPOT_SURVEY", "Diagnostics", "0", "[crusher] with UC_CRUSHER on: each time you get out of a car, its position, heading and navmesh place are logged; walk 10 m away from it on foot and the car is switched off and the Crusher stands in its place for this run (once ON A LION HUNT is done). Getting into a vehicle first cancels it."},
    {"MISSION_SURVEY", "Diagnostics", "0", "[mission] every mission record (its numbers, texts and whether its flag in the save is set) when the game loads them, then once a second any change of the mission under way, its state, the save's mission flags and the race's state."},
    {"VTXLOG",         "Diagnostics", "1", "Log the cases the VTXFIX guard catches."},
    {"AUDIO_DUMP",     "Diagnostics", "0", "1 = record the mixed sound to gmv_audio_dump.wav next to the " EXE_ " (or give a file path)."},
    {"DUMP_FRAME",     "Diagnostics", "", "Save frame number N to gmv_frame.bmp (blank = off)."},
    {"GL_FINISH",      "Diagnostics", "0", "Force a full GPU sync every frame."},
    {"CHECK_FRONT",    "Diagnostics", "0", "Read back the front buffer around frame 650."},
    {"WAVMAP",         "Diagnostics", "0", "Log which sound pack each .wav request is served from."},
    {"WATCH",          "Diagnostics", "0", "Hardware write-watchpoint (address in WATCH_ADDR)."},
    {"WATCH_ADDR",     "Diagnostics", "", "Hex address for WATCH (blank = 0xa9dc4c)."},

    {"DISMISS_SPLASH", "Testing", "0", "Auto-dismiss the rating splash screen."},
    {"CLICK_S3",       "Testing", "0", "Inject a click to get past the splash screen."},
    {"TEST_CLICK",     "Testing", "", "Inject a click in the window centre at frame N (blank = off)."},
    {"TEST_FATAL",     "Testing", "", "Stop the game about 10 s in, to try the error box: 1 = a bad memory write, 2 = a missing import, 3 = an illegal instruction, 4 = a crash on another thread (blank = off)."},
    {"PAD_TEST",       "Testing", "", "A virtual pad for tools\\playtest: xbox, ds4, ds5 or raw; its state is the text of cache\\padtest.txt (blank = off)."},
    {"FORCE_DRAW",     "Testing", "0", "Force the game's draw flag on."},
    {"FORCE_GATE6",    "Testing", "0", "Force the front-end gate 6 flag."},
    {"MENU_LOOP_SCENE", "Testing", "", "Show only the main menu's Nth background scene, over and over (1-16 in the menu's order; 4 = the countryside walk, 5 = the downtown ambulance; blank = off)."},
    {"MENU_LOOP_CAM",  "Testing", "0", "With MENU_LOOP_SCENE: 1 = a camera that follows right behind the scene's walker; F9 switches to the next walker. Moving the camera changes how the scene plays out, so use 0 to see the real scene."},
#endif
};

static const char *const header =
    "; GangstarMV.ini -- settings for " PORT_ "\r\n"
    ";\r\n"
    "; Read once at startup from the savegame folder next to the " EXE_ ". If it is missing it is created with the\r\n"
    "; defaults below, so deleting it restores every default. A blank value, or a setting missing from the\r\n"
    "; file, means the built-in value its comment gives (for the enhancements: off, as on the Mac).\r\n"
    "; Values: 1 = on, 0 = off, unless the comment says otherwise. Lines starting with ; are comments.\r\n"
#if GMV_DEV
    ";\r\n"
    "; An environment variable named GMV_<setting> (for example GMV_TEXFIX=0) overrides the value\r\n"
    "; here for that run (dev builds only).\r\n"
#endif
    ;

static const char *const section_help[][2] = {
    {"Preset",      "One switch for the original Mac experience."},
    {"Display",     "Resolution and full screen. The game's own Video options change these too: a choice made there\r\n"
                    "; is saved here at once."},
    {"Fixes",      "Fixes for crashes, rendering and collision bugs, all on by default. 0 turns one off, which is only\r\n"
                    "; useful for comparing with the original behaviour."},
    {"Graphics",    "Anti-aliasing, texture filtering, ambient occlusion, bloom, water reflections, shadows, the sun and moon, the draw distance and the fog. MSAA=4 is what the Mac game used; the rest are\r\n"
                    "; optional enhancements beyond the Mac original, on by default (ORIGINAL_MAC_EXPERIENCE=1 turns them off)."},
    {"Timing",      "Frame pacing."},
    {"Gameplay",    "Optional gameplay changes beyond the Mac original, on by default (ORIGINAL_MAC_EXPERIENCE=1 turns them off)."},
    {"Audio",       "Sound."},
    {"Input",       "Mouse and gamepad."},
    {"Startup",     "Start-up, the main menu and quit."},
#if GMV_DEV
    {"Diagnostics", "The log, and extra logging to cache\\GangstarMV.log for tracking down problems (all off by default)."},
    {"Testing",     "Development test aids. Leave these off for normal play."},
#else
    {"Log",         "The log."},
#endif
};

/* ORIGINAL_MAC_EXPERIENCE=1: these keys answer with the value here, ahead of the environment and
   the file.  The stability and rendering fixes stay on (without them the port crashes or loses
   fades the Mac showed), and so do the speed-ups, which change no picture or rule: since 2026-11-05 at
   the user's choice also the loading ones (LOAD_SLEEP_SKIP, LOAD_BUDGET, FAST_LOAD), whose stalls in the
   main menu's scene changes depend on the machine, not on the game.  Every fix of an original-game bug
   is off, the visible ones too (MENUFLASH_FIX, FADE_EDGE_FIX, MENU_FADE_CLICKS, also 2026-11-05);
   so is everything that changes timing, physics, what you hear, which radio logo the HUD shows, whether the
   main menu has its More Games corner and Gameloft LIVE! badge or whether a sprint puts the weapon away.  NULL = the call site's built-in default (FRAME_MS blank = 60 ticks/s). */
static const gmv_preset_value mac_preset[] = {
    {"TEXFIX", "1"}, {"SWAP_COPY", "1"}, {"NO_FLICKER", "1"},
    {"VTXFIX", "1"}, {"CRASH58_GUARD", "1"}, {"STOPLOCK_FIX", "1"}, {"FAST_FTELL", "1"},
    {"FAST_FSEEK", "1"}, {"FAST_MEMMOVE", "1"}, {"STATIC_VBO", "1"}, {"DPI_AWARE", "1"},
    {"LOAD_SLEEP_SKIP", "1"}, {"LOAD_BUDGET", "200"}, {"FAST_LOAD", "1"},
    {"MENUFLASH_FIX", "0"}, {"FADE_EDGE_FIX", "0"}, {"MENU_FADE_CLICKS", "0"},
    {"FRAME_MS", NULL}, {"SWAP_INTERVAL", NULL}, {"PHYSICS_SYNC", "0"}, {"CAMERA_SYNC", "0"}, {"WALK_SYNC", "0"}, {"AI_SYNC", "0"},
#if GMV_DEV
    {"SOLO_CAR", "0"},
#endif
    {"RADIO_LOGO_FIX", "0"}, {"SWITCH_SAVE_FIX", "0"}, {"MSAA", "4"}, {"ANISOTROPIC_FILTERING", "0"},
    {"AMBIENT_OCCLUSION", "0"}, {"BLOOM", "0"}, {"WATER_REFLECTIONS", "0"}, {"VEHICLE_REFLECTIONS", "0"}, {"SUN_MOON", "0"},
    {"DYNAMIC_SHADOWS", "0"}, {"SPAWN_FADE", "0"}, {"DRAW_DISTANCE", "1"},
    {"FOG_MODE", "0"}, {"FOG_HEIGHT", NULL},
    {"HIDE_MORE_GAMES", "0"}, {"HIDE_GAMELOFT_LIVE", "0"}, {"SHORT_WARNING", "0"}, {"CUTSCENE_KEYS", "0"}, {"TUTORIAL_KEYS", "0"},
    {"ALLOW_WEAPON_SPRINT", "0"}, {"MINIMAP_SHOPS", "0"}, {"MISSION_REPLAY", "0"}, {"PAD_ICONS", "0"}, {"PAD_LAYOUT", "0"}, {"PAD_MENU", "0"}, {"PAD_HINTS", "0"}, {"KEY_MOVE", "0"}, {"PLAYERSWITCH_INVENTORY", "0"}, {"LC_SWITCH", "0"}, {"UC_CRUSHER", "0"}, {"CUSTOM_RADIO", "0"}, {"RADIO_SONGS", "0"},
    {"NO_INTRO", "0"}, {"SHOW_CURSOR", "0"},
    {"TRAFFIC_DENSITY", "1"}, {"PEDESTRIAN_DENSITY", "1"}, {"PARKED_CAR_DENSITY", "1"},
};

/* Keys taken out of the game.  An older GangstarMV.ini still has their lines; they are skipped quietly. */
static const char *const retired[] = {
    "NO_MOUSELOOK", "NO_RMB",           /* 2026-11-05: the mouse always turns the camera, the right button always locks on */
};

const gmv_settings_table gmv_game_settings = {
    settings, sizeof settings / sizeof *settings, header, section_help, sizeof section_help / sizeof *section_help,
    mac_preset, sizeof mac_preset / sizeof *mac_preset,
    "[config] ORIGINAL_MAC_EXPERIENCE=1: 60 ticks/s, original physics, no MENUFLASH_FIX, FADE_EDGE_FIX, MENU_FADE_CLICKS, " SOLO_CAR_ "RADIO_LOGO_FIX, SWITCH_SAVE_FIX, ANISOTROPIC_FILTERING, AMBIENT_OCCLUSION, BLOOM, WATER_REFLECTIONS, VEHICLE_REFLECTIONS, SUN_MOON, SPAWN_FADE, DRAW_DISTANCE, FOG_MODE/FOG_HEIGHT, HIDE_MORE_GAMES, HIDE_GAMELOFT_LIVE, SHORT_WARNING, FAST_LOAD, CUTSCENE_KEYS, TUTORIAL_KEYS, ALLOW_WEAPON_SPRINT, MINIMAP_SHOPS, MISSION_REPLAY, PAD_ICONS, PAD_LAYOUT, PAD_MENU, PAD_HINTS, KEY_MOVE, PLAYERSWITCH_INVENTORY, LC_SWITCH, UC_CRUSHER, CUSTOM_RADIO, RADIO_SONGS, NO_INTRO, SHOW_CURSOR or the population densities; "
            "TEXFIX, SWAP_COPY, NO_FLICKER, VTXFIX, CRASH58_GUARD, STOPLOCK_FIX, STATIC_VBO, LOAD_SLEEP_SKIP, LOAD_BUDGET=200 and FAST_LOAD on; MSAA=4\n",
    retired, sizeof retired / sizeof *retired,
};
