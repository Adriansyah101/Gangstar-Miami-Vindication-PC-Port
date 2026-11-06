/* shims.h -- host implementations of the dylib entry points the game imports.  The table is
   NULL-terminated and consumed by imports_bind (core/imports.c), which matches each
   import's Mach-O symbol name against host_symbol.name. */
#ifndef GMV_SHIMS_H
#define GMV_SHIMS_H

#include <stdarg.h>
#include <wchar.h>
#include <stddef.h>
#include <stdint.h>

#include "game.h"
#include "imports.h"
#include "log.h"

extern const host_symbol gmv_host_symbols[];        /* libc (CRT-forwardable), shims.c */
extern const host_symbol gmv_host_symbols_win32[];  /* Win32-backed, shimsw.c */
extern const host_symbol gmv_host_symbols_cxx[];    /* C++ runtime / libstdc++, shimscxx.c */
extern const host_symbol gmv_host_symbols_sdl[];    /* SDL, shimssdl.c */
extern const host_symbol gmv_host_symbols_objc[];   /* Objective-C runtime, shimsobjc.c */
extern const host_symbol gmv_host_symbols_appkit[]; /* AppKit/Foundation C ABI, shimsappkit.c */
extern const host_symbol gmv_host_symbols_str[];    /* std::string COW ABI, shimsstr.c */
extern const host_symbol gmv_host_symbols_stream[]; /* std::ostringstream (accumulator), shimsstream.c */
extern const host_symbol gmv_host_symbols_cg[];     /* CoreGraphics display, shimscg.c */
extern const host_symbol gmv_game_host_symbols[];   /* what the game adds, if any (game.h) */
extern const host_symbol gmv_host_symbols_gl[];     /* CGL / OpenGL-framework C ABI, the GL shim */

/* Called once, early, before anything reads a shim's state (e.g. the stdio FILE* data
   symbols).  Safe to call before the game image is mapped. */
void gmv_shims_init(void);

/* Host settings (config.c).  gmv_config_load reads GangstarMV.ini from exe_dir\savegame (creating it with
   the defaults if missing); call it first, before anything asks for a setting.  gmv_getenv("GMV_X")
   returns the environment variable GMV_X if set, else ini key X if non-empty, else NULL. */
void gmv_config_load(const char *exe_dir);
const char *gmv_getenv(const char *name);
/* A [Diagnostics] or [Testing] switch: in a release build always unset, so the compiler drops the
   code behind it.  gmv_envkey: the environment's GMV_<key> for one run, which only a dev build reads. */
#if GMV_DEV
#define gmv_dbgenv(name) gmv_getenv(name)
#define gmv_envkey(name) getenv(name)
#else
#define gmv_dbgenv(name) ((const char *)0)
#define gmv_envkey(name) ((const char *)0)
#endif
/* Key X as GangstarMV.ini holds it now (NULL = blank), and the rewrite of its line; gmv_getenv
   keeps answering what was read at startup.  gmv_config_save returns 1 on success. */
const char *gmv_config_file_value(const char *name);
const char *gmv_config_live(const char *name);   /* for what the menu puts in force at once: follows a save */
int gmv_config_save(const char *name, const char *val);
/* ORIGINAL_MAC_EXPERIENCE switched in game (the options tab): gmv_getenv answers the preset's values
   for its keys while on, and the file's again when off. */
void gmv_config_set_mac(int on);
int gmv_config_mac(void);
int gmv_config_tab(void);
void gmv_fopen_info(const char **fail_path, int *fail_errno, long *open_files);

/* Address registered for a Mach-O import name, across all shim tables, or NULL. */
void *gmv_shim_lookup(const char *name);

/* Bind a `_gl*` import to opengl32 via a cdecl->stdcall thunk, or NULL (the GL shim). */
void *gmv_gl_resolve(const char *name);
/* A GL function by its GL name ("glClear"), or NULL: every lookup of the host goes through it (SDL's
   loader on both systems).  The GL context must be current. */
void *gl_real(const char *name);
/* Present the GL window (SDL_GL_SwapBuffers). */
void gmv_gl_swap(void);
/* The tick pacer's period in ms (GMV_FRAME_MS, default 16.667; 0 = pacer off). */
double gmv_pace_frame_ms(void);
/* GAMEPATCH_TAB, the GL shim: settings changed from the options menu while the game runs.  The pacer,
   the screen and the post effects take theirs at the next present.  The MSAA and anisotropy setters return 0 when the
   start-up set-up left no way to change them (they take effect at the next start). */
void gmv_set_frame_ms(double ms);
int gmv_set_swap_interval(int v);   /* SWAP_INTERVAL (VSYNC), -1 = blank */
int gmv_set_msaa(int samples);
/* The last frame again, at the window's present size (the player is sizing it, and the game waits). */
void gmv_gl_redraw(void);
/* The GL window's client size. */
void gmv_gl_client_size(int *w, int *h);
/* The size the game draws at (its backing size should match): the client size in a window, the
   chosen resolution in full screen, where the screen FBO is scaled to the monitor with black bars. */
void gmv_gl_render_size(int *w, int *h);
/* Where the game's picture lies in the client area (top-left origin) and its size in pixels. */
void gmv_gl_view_rect(int *x, int *y, int *w, int *h, int *gw, int *gh);
/* The GL window, or NULL before it exists (the GL shim); and its Windows handle, for what still needs one. */
struct SDL_Window *gmv_gl_window(void);
void *gmv_gl_window_hwnd(void);
/* gl_context.c: SDL's video, on the main thread before the game; the primary monitor's size. */
void gmv_video_start(void);
void gmv_video_desktop(int *w, int *h);
/* Raw context helpers for the intro-video player (shimsvideo.c): guarantee the GL context is
   current on this thread, and present the back buffer directly (bypassing the game's swap path). */
void gmv_gl_make_current_raw(void);
void gmv_gl_present_raw(void);
/* Apply the game's saved resolution/fullscreen option to the host window immediately (called from
   the NSUserDefaults setters when the Video options change, so fullscreen toggles take effect at
   once instead of waiting for the game to re-issue glViewport). */
void gmv_gl_apply_mode(void);
/* Diagnostic (GMV_FADE_TRACE), the GL shim: gmv_gl_fade_mark is called on every full-screen fade
   rectangle; gmv_gl_death_capture_arm starts a 12 s capture of presented frames to gmv_fadecap_*.bmp,
   with a [fadecap] line per tick saying what was drawn, in what order, and whether it was presented.
   gmv_death_poll (the patches), called once per present, arms it on the tick the player's health
   reaches 0. */
void gmv_gl_fade_mark(uint32_t caller, int main_fade);
void gmv_gl_death_capture_arm(int health, int max_health);
/* Diagnostic (GMV_LOAD_PROFILE), loadprof.c: break the long ticks down into uploads, file I/O,
   waits and the rest.  Timed spots call gmv_prof_add(kind, gmv_prof_now() taken before, bytes)
   only when gmv_prof_on(); gmv_gl_swap marks each tick's end (before pacing) and start (after).
   GMV_PERF_PROFILE turns the same timing on for every tick and logs a summary every 5 s. */
enum { GMV_PROF_TEX, GMV_PROF_BUF, GMV_PROF_IO, GMV_PROF_LOCK, GMV_PROF_WAIT, GMV_PROF_SLEEP,
       GMV_PROF_SWAP, GMV_PROF_N };
#if GMV_DEV
int gmv_prof_on(void);
int gmv_perf_on(void);                       /* PERF_PROFILE */
long long gmv_prof_now(void);
void gmv_prof_add(int kind, long long t0, long long bytes);
void gmv_prof_sleep_note(unsigned usec);     /* main-thread sleep length + its game callers */
void gmv_prof_tick_end(unsigned frame, int drew);
void gmv_prof_tick_start(void);
#else                                        /* release: loadprof.c is not built */
#define gmv_prof_on() 0
#define gmv_perf_on() 0
#define gmv_prof_now() 0LL
#define gmv_prof_add(kind, t0, bytes) ((void)(t0))
#define gmv_prof_sleep_note(usec) ((void)0)
#define gmv_prof_tick_end(frame, drew) ((void)0)
#define gmv_prof_tick_start() ((void)0)
#endif

/* the GL shim: frames presented so far */
unsigned gmv_gl_frame_no(void);

/* Stage 3b: parse the mapped __OBJC segment into the Objective-C class registry.  Call once,
   after the image is mapped and before any objc_msgSend (i.e. before main runs). */
void gmv_objc_init(const macho *m);

/* objc_msgSend itself (shimsobjc.S) and a C helper to allocate an instance by class name, so
   the host can drive the Cocoa bootstrap (stage 3c). */
void gmv_objc_msgSend(void);
void *gmv_objc_new_instance(const char *clsname);
/* Quiet check: 1 if `self` (an instance) implements `sel` (no fatal "unhandled" on miss). */
int gmv_objc_responds(void *self, const char *sel);
int gmv_objc_is_cfstring(const void *p);               /* a compile-time NSString literal */
const char *gmv_objc_instance_class_name(const void *p); /* class name of a heap instance, or NULL */

/* Foundation/Cocoa method tables (shimsfnd.c): the IMP for a (class, selector), or NULL.
   gmv_objc_lookup consults these after game methods and the NSObject-level defaults. */
void *gmv_fnd_class_method(const char *cls, const char *sel);
void *gmv_fnd_instance_method(const char *cls, const char *sel);
void *gmv_fnd_stret_method(const char *cls, const char *sel);   /* struct-returning methods */
/* The real .app directory, so NSBundle paths resolve to the extracted game on disk. */
void gmv_fnd_set_bundle_path(const char *path);
int gmv_in_bundle(const char *path);         /* the .app folder or a path inside it */

#ifndef _WIN32
/* ffpipe_linux.c: the system's ffmpeg/ffprobe as a program, its output read from a pipe.  gmv_ff_start: the pid
   (-1 when it cannot run, errno ENOENT when it is not installed) and *fd its standard output; gmv_ff_read: up to n
   bytes, fewer only at the end; gmv_ff_stop: closes fd, ends the program if it still runs, its exit code;
   gmv_ff_output: the whole output of a program that ended well (malloc'd, NUL-ended), else NULL. */
int gmv_ff_start(const char *const *argv, int *fd);
size_t gmv_ff_read(int fd, void *buf, size_t n);
int gmv_ff_stop(int pid, int fd);
char *gmv_ff_output(const char *const *argv);
#endif
/* songdec.c: the custom radio's own decoders (mp3, flac, wav, ogg; the same code on Windows and Linux).  Paths are
   UTF-16 on Windows and UTF-8 on Linux.  gmv_song_builtin: the file is of one of those kinds (by its extension);
   gmv_song_open: NULL when it is not, or will not open; gmv_song_read: up to `frames` 16-bit stereo frames at the
   song's own rate, 0 at the end; gmv_song_decoder: the decoder's name, for the log. */
#ifdef _WIN32
typedef wchar_t gmv_pathch;
#else
typedef char gmv_pathch;
#endif
typedef struct gmv_song gmv_song;
int gmv_song_builtin(const gmv_pathch *path);
gmv_song *gmv_song_open(const gmv_pathch *path);
unsigned gmv_song_rate(const gmv_song *s);
unsigned gmv_song_channels(const gmv_song *s);
const char *gmv_song_decoder(const gmv_song *s);
size_t gmv_song_read(gmv_song *s, int16_t *out, size_t frames);
void gmv_song_close(gmv_song *s);
/* NSUserDefaults-backed prefs (shimsfnd.c; ScreenWidth/ScreenHeight/FullScreen are stored in
   GangstarMV.ini [Display]).  gmv_prefs_get_long returns a stored integer setting or `def`.  gmv_effective_screen gives the resolution the window + the game's backing should
   use this run (windowed: the stored ScreenWidth/Height; fullscreen: the primary monitor size). */
long gmv_prefs_get_long(const char *key, long def);
void gmv_effective_screen(long *w, long *h, int *fullscreen);
void gmv_chosen_resolution(long *w, long *h);
/* Make an NSString from a C string (host helper; needs the ObjC runtime up). */
void *gmv_make_nsstring(const char *s);
/* The backing C string of an NSString/CFString object, or NULL (shimsfnd.c). */
const char *gmv_nsstring_cstr(const void *o);
/* printf-style formatter understanding %@ (NSString arg); used by NSString formatting and NSLog. */
void gmv_nsformat(char *out, size_t cap, const char *fmt, va_list ap);
/* AudioUnit output config captured from the game (shimsappkit.c), consumed by the audio backend:
   the render callback + its refcon, and the stream format (ASBD) fields. */
extern void    *gmv_au_render_cb;
extern void    *gmv_au_render_refcon;
extern double   gmv_au_sample_rate;
extern uint32_t gmv_au_channels, gmv_au_bits, gmv_au_format_flags, gmv_au_fmtid;
/* SDL3 playback backend (shimsaudio.c): start/stop driving the captured render callback. */
void gmv_audio_start(void);
void gmv_audio_stop(void);
/* Its streams for other sound (the intro movie): PCM to the default device, already playing. */
void *gmv_snd_open(uint32_t rate, uint32_t chans, uint32_t bits);
void gmv_snd_write(void *st, const void *pcm, size_t len);
int gmv_snd_queued(void *st);
void gmv_snd_close(void *st);
/* Frames the backend pulls per render call (kAudioUnitProperty_MaximumFramesPerSlice). */
uint32_t gmv_audio_frames(void);

/* Make a fake NSEvent whose -locationInWindow returns (x, y) (shimsfnd.c; for input injection). */
void *gmv_make_nsevent(float x, float y);
/* As above, plus a scroll amount returned by -[NSEvent deltaY] (for -scrollWheel:). */
void *gmv_make_nsscroll(float x, float y, float deltaY);

/* Input (shimsinput.c): pump SDL's events and deliver mouse activity to the game's EAGLView; called from
   SDL_PumpEvents.  set_client_size feeds the Y-flip + auto-click centre.  gmv_input_drain: only the events
   (the intro movie); with skip, a skip key or a click sets it instead of reaching the game. */
void gmv_input_pump(void);
void gmv_input_drain(int *skip);
/* Keys and mouse buttons gone down so far, modifiers and Tab left out; a key held now, by SDL scancode. */
unsigned gmv_input_presses(void);
int gmv_input_scancode_held(int scancode);
void gmv_input_set_client_size(int w, int h);
/* The picture moved within the window (gmv_gl_view_rect): confine the cursor to it again. */
void gmv_input_view_changed(void);
/* Relative mouse-look delta since the last call (0 when look isn't active); consumed by shimscg.c's
   CGGetLastMouseDelta, which is how the engine's per-frame poll turns the camera. */
void gmv_input_take_mouse_delta(int *dx, int *dy);
/* The game's cursor-visibility intent, from the NSCursor hide/unhide shims: hidden!=0 when the game
   has hidden the cursor (it does this to enter relative mouse-look / gameplay).  Drives the hands-free
   auto mouse-look (GMV_AUTO_LOOK) and the tracked CGCursorIsVisible.  Query with ...is_cursor_hidden. */
void gmv_input_set_cursor_hidden(int hidden);
int  gmv_input_is_cursor_hidden(void);
/* Synthesise one queued SDL keyboard event (type 0x300 KEYDOWN / 0x301 KEYUP) into buf when the
   requested [minType,maxType] range overlaps the keyboard range; returns 1 if one was written
   (and removes it from the queue), else 0.  Called from SDL_PeepEvents (shimssdl.c). */
int gmv_input_get_sdl_event(void *buf, unsigned minType, unsigned maxType);
/* Controllers (shimsjoy.c): the game's SDL 1.3 joystick API over SDL3.  init/quit take the game's
   SDL_Init / SDL_QuitSubSystem flags and act on its joystick flag; a joystick handle is void *. */
void  gmv_joy_init(uint32_t flags);
void  gmv_joy_quit(uint32_t flags);
int   gmv_joy_count(void);
void *gmv_joy_open(int index);
int   gmv_joy_opened(int index);
void  gmv_joy_close(void *joystick);
void  gmv_joy_update(void);
int   gmv_joy_num_axes(void *joystick);
int   gmv_joy_num_buttons(void *joystick);
int   gmv_joy_num_hats(void *joystick);
int   gmv_joy_axis(void *joystick, int axis);
int   gmv_joy_button(void *joystick, int button);
int   gmv_joy_hat(void *joystick, int hat);
/* The kind of pad last used (PAD_ICONS): 0 none or another pad, 1 Xbox, 2 DualShock 4, 3 DualSense. */
int   gmv_joy_style(void);
int   gmv_joy_any_button(void);
/* Every pad read into copies: the left stick deflected furthest (-1..1, down positive), the buttons
   of any pad as bits, the hat bits of any pad.  Returns the number of pads (shimsjoy.c). */
int   gmv_joy_peek(float *x, float *y, unsigned *buttons, int *hat);
/* PAD_HINTS: the time in gmv_input_tick's ms of the last pad use (shimsjoy.c; 0 = never) and of the
   last key press or mouse click (shimsinput.c; 0 = never). */
unsigned gmv_joy_last_use(void);
unsigned gmv_input_last_key(void);
unsigned gmv_input_tick(void);
void *gmv_tis_input_source_property(void *source, void *key);
/* Make a one-element NSArray. */
void *gmv_make_nsarray1(void *elem);
/* Fill the AppKit/Foundation NSString constant globals; call after gmv_objc_init. */
void gmv_appkit_init_constants(void);

/* The frame timer the game scheduled (NSTimer); the run loop fires target/selector each frame. */
void *gmv_timer_target(void);
const char *gmv_timer_selector(void);
void *gmv_timer_object(void);
/* Deferred -performSelector:withObject:afterDelay: queue; the run loop drains and fires it. */
int gmv_defer_take(void **target, const char **sel, void **arg);

/* Notification delivery: post one to all matching observers; fire the simulated movie-end once
   (advances the game past the intro logo). Both call game code -> are force-aligned. */
void gmv_notif_post(const char *name, void *notification);
void gmv_fire_movie_end(void);

/* Intro movie: the file the game handed to -[QTMovie initWithFile:] (shimsfnd.c captures it), and
   the player that decodes+shows it before the menu (shimsvideo.c).  gmv_intro_play() is a blocking,
   skippable phase; it returns having presented the video (or immediately if there is nothing/off). */
const char *gmv_intro_movie_path(void);
void gmv_intro_play(void);

/* NSOpenGLContext IMPs backed by a real Win32 window + WGL context (the GL shim). */
void *gmv_gl_context_init(void *self, const char *sel, void *format, void *share);
void *gmv_gl_make_current(void *self, const char *sel, ...);
void *gmv_gl_flush_buffer(void *self, const char *sel, ...);

#endif
