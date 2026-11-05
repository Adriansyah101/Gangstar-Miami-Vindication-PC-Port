/* game.c -- a game with no patches, the starting point of a new one: every hook of game.h, doing nothing.
   Also what keeps core\ and shims\ honest: build.ps1 -Game template links them with no Gangstar source. */
#include <stdint.h>
#include <stddef.h>
#include <GL/gl.h>

#include "glshim.h"
#include "game.h"
#include "log.h"
#include "shims.h"

static const gmv_setting settings[] = {
    {"FULLSCREEN",    "Display", "0", "1 = full screen, 0 = a window of SCREEN_WIDTH x SCREEN_HEIGHT."},
    {"SCREEN_WIDTH",  "Display", "",  "The width in pixels (blank = the monitor's)."},
    {"SCREEN_HEIGHT", "Display", "",  "The height in pixels (blank = the monitor's)."},
    {"LOG",           "Log",     "1", "1 = write what the host does to the log in the cache folder."},
};

static const char *const section_help[][2] = {
    {"Display", "Resolution and full screen."},
    {"Log",     "The log."},
};

static const gmv_preset_value preset[] = {
    {"LOG", "1"},
};

const gmv_settings_table gmv_game_settings = {
    settings, sizeof settings / sizeof *settings,
    "; Settings. If this file is missing it is created with these defaults.\r\n",
    section_help, sizeof section_help / sizeof *section_help,
    preset, sizeof preset / sizeof *preset,
    "[config] ORIGINAL_MAC_EXPERIENCE=1\n",
    NULL, 0,
};

void gmv_apply_gamepatch(void) {}
void gmv_game_crash_note(const gmv_regs *r) { (void)r; }
const char *gmv_game_redirect(const char *path, char *buf, size_t n) { (void)path; (void)buf; (void)n; return NULL; }
void gmv_game_audio_mix(int16_t *buf, uint32_t frames, uint32_t chans, uint32_t rate)
{
    (void)buf; (void)frames; (void)chans; (void)rate;
}
int gmv_game_key(unsigned sym) { (void)sym; return 0; }
void gmv_game_input_pump(void) {}
void gmv_game_input_tick(void) {}
void gmv_game_alt_enter(void) {}
void gmv_game_full_screen_tick(void) {}
int gmv_game_pad_pointer(void) { return 0; }
int gmv_game_pointer_shown(void) { return 1; }
void gmv_game_tick(void) {}
void gmv_game_between_frames(void) {}
void gmv_game_frame_end(void) {}
int gmv_game_present_hold(unsigned frame, int drew) { (void)frame; (void)drew; return 0; }
void gmv_game_present_decided(unsigned frame, int presented) { (void)frame; (void)presented; }
void gmv_game_after_present(void) {}
void gmv_game_gl_setup(void) {}
void *gmv_game_gl_intercept(const char *name) { (void)name; return NULL; }
int gmv_game_gl_wants_draws(void) { return 0; }
void gmv_game_gl_cap(unsigned cap, int on) { (void)cap; (void)on; }
void gmv_game_gl_clear_begin(unsigned mask) { (void)mask; }
void gmv_game_gl_clear_end(unsigned mask) { (void)mask; }
void gmv_game_gl_client_unit(unsigned unit) { (void)unit; }
void gmv_game_gl_client_state(unsigned array, int on) { (void)array; (void)on; }
void *gmv_game_gl_draw_intercept(const char *name) { (void)name; return NULL; }
void gmv_game_perf_report(double seconds) { (void)seconds; }
void gmv_game_perf_counts(unsigned *made, unsigned *gone, int *bounds) { *made = *gone = 0; *bounds = 0; }

/* Plain draws, with the shim's TEXFIX around them. */
void gmv_game_draw_arrays(void *real, unsigned mode, int first, int count, void *caller)
{
    (void)caller;
    unsigned tf = texfix_begin(mode);
    if (real) ((void(APIENTRY *)(GLenum, GLint, GLsizei))real)(mode, first, count);
    texfix_end(tf);
}

void gmv_game_draw_elements(void *real, unsigned mode, int count, unsigned type, const void *idx, void *caller)
{
    (void)caller;
    unsigned tf = texfix_begin(mode);
    if (real) ((void(APIENTRY *)(GLenum, GLsizei, GLenum, const void *))real)(mode, count, type, idx);
    texfix_end(tf);
}

const host_symbol gmv_game_host_symbols[] = {
    {NULL, NULL},
};
