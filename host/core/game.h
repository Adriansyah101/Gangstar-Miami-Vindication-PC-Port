/* game.h -- what a game supplies to the generic host (core\ and shims\).

   The host maps a non-PIE i386 Mach-O at its own addresses, binds its imports to the shims and runs it.  Nothing in
   core\ or shims\ knows one game: each game has a folder under games\ holding
     layout.h    its address-space layout and its names (see games\template\layout.h)
     build.cfg   the exe's name and the libraries it links, for build.ps1
   and sources that define everything below.  games\template is a game with no patches: copy it to start a new one.
   A hook is an ordinary function, called directly: a game with nothing to do leaves its body empty. */
#ifndef GMV_GAME_H
#define GMV_GAME_H

#include <stddef.h>
#include <stdint.h>

/* The settings file: name, section, default written to a new file ("" = built-in default left blank), help text.
   Also the list of known keys: anything else in the file is reported as a likely typo. */
typedef struct { const char *name, *section, *def, *help; } gmv_setting;
/* A preset's fixed answer for a key (NULL = the call site's built-in default). */
typedef struct { const char *name, *val; } gmv_preset_value;
typedef struct {
    const gmv_setting *list;
    size_t n;
    const char *header;                 /* the new file's first lines */
    const char *const (*section_help)[2];   /* the sections in file order, each with its comment */
    size_t nsections;
    const gmv_preset_value *preset;     /* what ORIGINAL_MAC_EXPERIENCE=1 answers */
    size_t npreset;
    const char *preset_log;             /* the log's line when it is on */
    const char *const *retired;         /* keys taken out: an older file's lines for them are skipped quietly */
    size_t nretired;
} gmv_settings_table;
extern const gmv_settings_table gmv_game_settings;

/* The imports a game adds to the shims go in gmv_game_host_symbols[] (shims.h), a table ending in {NULL, NULL}. */

/* ---- start and end ---- */

/* Once, after the image is mapped and before game code runs: the game's patches to its own code. */
void gmv_apply_gamepatch(void);
/* A crash is being reported, in a dev build: what the game adds to the log.  r: the registers at the fault. */
typedef struct { uint32_t eip, eax, ecx, edx, ebx, esi, edi, ebp, esp; } gmv_regs;
void gmv_game_crash_note(const gmv_regs *r);

/* ---- files and sound ---- */

/* The file to open instead of a path the game asked for (written to buf), or NULL. */
const char *gmv_game_redirect(const char *path, char *buf, size_t n);
/* Add to each buffer the mixer rendered (on the audio thread). */
void gmv_game_audio_mix(int16_t *buf, uint32_t frames, uint32_t chans, uint32_t rate);

/* ---- input ---- */

/* A key going down: 1 keeps it, and its release, from the game. */
int gmv_game_key(unsigned sym);
/* Each pump, before the game's events go out. */
void gmv_game_input_pump(void);
/* Each pump, after the game's events went out. */
void gmv_game_input_tick(void);
/* Alt+Enter was pressed, and each pump after the message queue is drained. */
void gmv_game_alt_enter(void);
void gmv_game_full_screen_tick(void);
/* 1 while the pad should drive the game's menu pointer. */
int gmv_game_pad_pointer(void);
/* The game shows a pointer now: the mouse is a normal one; otherwise SDL's relative mode, for the camera. */
int gmv_game_pointer_shown(void);

/* ---- the frame ---- */

/* Once a tick, before the present is decided. */
void gmv_game_tick(void);
/* Between frames: checks that must not run during one. */
void gmv_game_between_frames(void);
/* The frame's drawing is over: what is drawn over the whole of it goes here. */
void gmv_game_frame_end(void);
/* 1 keeps this tick's frame off the screen. */
int gmv_game_present_hold(unsigned frame, int drew);
/* What was decided, for a diagnostic. */
void gmv_game_present_decided(unsigned frame, int presented);
/* After a present: settings that wait for the frame's end. */
void gmv_game_after_present(void);

/* ---- OpenGL ---- */

/* At the first GL import: the game reads its settings. */
void gmv_game_gl_setup(void);
/* A wrapper of the game's own for a GL import (name with its leading underscore), or NULL. */
void *gmv_game_gl_intercept(const char *name);
/* 1 if the draw hooks below are needed even when the shim's own fixes are off. */
int gmv_game_gl_wants_draws(void);
/* One draw: the hook makes it through real, a stdcall glDrawArrays or glDrawElements.  caller: the game's return
   address.  The texfix_begin/texfix_end pair of glshim.h goes around it. */
void gmv_game_draw_arrays(void *real, unsigned mode, int first, int count, void *caller);
void gmv_game_draw_elements(void *real, unsigned mode, int count, unsigned type, const void *idx, void *caller);
/* State as the game sets it. */
void gmv_game_gl_cap(unsigned cap, int on);
void gmv_game_gl_clear_begin(unsigned mask);
void gmv_game_gl_clear_end(unsigned mask);
void gmv_game_gl_client_unit(unsigned unit);
void gmv_game_gl_client_state(unsigned array, int on);
/* A wrapper for an array-state import, ahead of the shim's, or NULL. */
void *gmv_game_gl_draw_intercept(const char *name);

/* ---- diagnostics (dev builds only) ---- */

/* The PERF_PROFILE summary, and counters for its per-tick line. */
void gmv_game_perf_report(double seconds);
void gmv_game_perf_counts(unsigned *made, unsigned *gone, int *bounds);

#endif
