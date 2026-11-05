/* shimssdl.c -- SDL stand-ins.

   Only the SDL entry points the game reaches are implemented here.  SDL_Init / InitSubSystem /
   QuitSubSystem matter only for SDL_INIT_JOYSTICK: the joystick API is backed by real controllers
   in shimsjoy.c (through SDL3), and the joystick init flag is what makes it (re)scan.

   The video / input / GL calls the game makes from gameplay (SDL_SetVideoMode, SDL_PumpEvents,
   SDL_GL_SwapBuffers, ...) are deliberately left as name-reporting thunks, so the first one hit
   in a later stage names itself instead of silently returning a wrong value. */

#include <stdint.h>

#include "log.h"
#include "shims.h"

#define FN(mach_name, host_fn) {mach_name, (void *)(host_fn)}

static int sdl_logged;

static int sdl_init(uint32_t flags)
{
    if (!sdl_logged) {
        gmv_trace("[shim] SDL_Init(0x%x) -> 0 (no SDL video; joysticks in shimsjoy.c)\n", flags);
        sdl_logged = 1;
    }
    gmv_joy_init(flags);
    return 0;
}

static int sdl_init_subsystem(uint32_t flags) { gmv_joy_init(flags); return 0; }
static void sdl_quit_subsystem(uint32_t flags) { gmv_joy_quit(flags); }

/* ---- gameplay: video / input / GL (minimal; our own Win32 window + WGL do the real work) --- */
/* The game polls SDL_PumpEvents every frame; we use it as our once-per-frame hook to pump the
   Win32 message queue (keeps the window responsive) and deliver mouse input to the EAGLView. */
static void sdl_pump_events(void) { gmv_input_pump(); }
static int sdl_peep_events(void *ev, int num, int act, uint32_t mn, uint32_t mx)
{
    /* The game drains keyboard with SDL_GETEVENT (act==2), one event per call.  Hand back one
       queued key event (captured from Win32 in shimsinput.c) when the range overlaps 0x300/0x301;
       every other category returns 0: quit has no source, mouse goes via Cocoa, and the joystick
       events (0x600..0x604) are only drained and discarded by the game, which polls instead. */
    if (ev && num > 0 && act == 2 /* SDL_GETEVENT */)
        return gmv_input_get_sdl_event(ev, mn, mx);
    return 0;
}
static void sdl_gl_swap(void) { gmv_gl_swap(); }
static int sdl_gl_set_attribute(int attr, int value) { (void)attr; (void)value; return 0; }
static void *sdl_set_video_mode(int w, int h, int bpp, uint32_t flags)
{
    (void)w; (void)h; (void)bpp; (void)flags;
    return (void *)1;                           /* a non-null "surface" */
}
static void sdl_wm_set_caption(const char *title, const char *icon) { (void)title; (void)icon; }
static int sdl_enable_unicode(int enable) { (void)enable; return 0; }
static const char *sdl_empty_name(int code) { (void)code; return ""; }

const host_symbol gmv_host_symbols_sdl[] = {
    FN("_SDL_Init", sdl_init),
    FN("_SDL_InitSubSystem", sdl_init_subsystem),
    FN("_SDL_QuitSubSystem", sdl_quit_subsystem),
    FN("_SDL_NumJoysticks", gmv_joy_count),
    FN("_SDL_JoystickOpen", gmv_joy_open),
    FN("_SDL_JoystickOpened", gmv_joy_opened),
    FN("_SDL_JoystickClose", gmv_joy_close),

    FN("_SDL_PumpEvents", sdl_pump_events),
    FN("_SDL_PeepEvents", sdl_peep_events),
    FN("_SDL_GL_SwapBuffers", sdl_gl_swap),
    FN("_SDL_GL_SetAttribute", sdl_gl_set_attribute),
    FN("_SDL_SetVideoMode", sdl_set_video_mode),
    FN("_SDL_WM_SetCaption", sdl_wm_set_caption),
    FN("_SDL_EnableUNICODE", sdl_enable_unicode),
    FN("_SDL_GetKeyName", sdl_empty_name),
    FN("_SDL_GetScancodeName", sdl_empty_name),
    FN("_SDL_JoystickUpdate", gmv_joy_update),
    FN("_SDL_JoystickGetAxis", gmv_joy_axis),
    FN("_SDL_JoystickGetButton", gmv_joy_button),
    FN("_SDL_JoystickGetHat", gmv_joy_hat),
    FN("_SDL_JoystickNumAxes", gmv_joy_num_axes),
    FN("_SDL_JoystickNumButtons", gmv_joy_num_buttons),
    FN("_SDL_JoystickNumHats", gmv_joy_num_hats),

    {NULL, NULL},
};
