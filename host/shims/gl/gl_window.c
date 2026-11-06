/* gl_window.c -- the GL window, its context and the frame pacer's settings */
#include <stdlib.h>
#include "compat.h"
#include <GL/gl.h>
#include <SDL3/SDL.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "glshim.h"

/* The renderer: a real SDL window + OpenGL context behind the
   game's NSOpenGLContext.

   The game renders through Cocoa's NSOpenGLContext (makeCurrentContext / flushBuffer) drawing
   into an NSView (its EAGLView).  We back that with an actual window and a GL context so
   the game's OpenGL calls have somewhere to draw.  The context is created lazily the first time
   NSOpenGLContext is initialised or made current.

   The game's GL imports themselves (glClear, ...) are macOS-cdecl while Windows' opengl32 is
   stdcall; those are bound separately (cdecl->stdcall trampolines) -- this file only stands up
   the drawable + context.  Host code here calls opengl32 directly with correct decls, so no ABI
   issue for us. */

#ifndef GL_BGR_EXT
#endif

#if GMV_DEV
int g_gl_trace;                                  /* set from GMV_GL_TRACE (defined below) */
#else                                                   /* release: the diagnostics' switches are constant 0 */
#endif
int g_no_flicker;                                /* set from GMV_NO_FLICKER: enables draw-tracking + present-skip */
volatile int g_drew_since_swap;                  /* any clear/draw since the last present? (anti-flicker) */

SDL_Window *g_window;
void *g_glctx;

#if GMV_DEV
#endif

/* Tick period of the frame pacer in ms (0 = off).  The game is dt-driven but was tuned on Macs with
   a 60 Hz display: SDL_GL_SwapBuffers waited one vblank per tick, so it ran 60 ticks/s and rendered
   every other one (30 fps).  On a faster panel the driver vsync lets it tick at the panel rate --
   measured ~150 ticks/s with the whole-ms dt alternating 0 <-> 14 ms on this machine -- which is
   what destabilised the engine rev and broke box-climbing at high rates.  So pace to 60 Hz by
   default, as the Mac display did.  GMV_FRAME_MS=<ms> overrides; GMV_FRAME_MS=0 turns it off.
   The options menu's FRAME RATE changes it with gmv_set_frame_ms; the pacer takes it on its next
   tick, with the swap interval it would have had from the start. */
static double g_pace_ms = -1.0;
volatile LONG g_pace_gen;                        /* bumped by gmv_set_frame_ms */
int g_swap_default = 1;                          /* the context's swap interval before ours */
static int g_swap_want = -2;                            /* SWAP_INTERVAL: -1 blank, -2 not read yet */

double pace_frame_ms(void)
{
    if (g_pace_ms < 0.0) {
        const char *fm = gmv_getenv("GMV_FRAME_MS");
        g_pace_ms = (fm && *fm) ? atof(fm) : 1000.0 / 60.0;
        if (g_pace_ms < 0.0)
            g_pace_ms = 0.0;
    }
    return g_pace_ms;
}

double gmv_pace_frame_ms(void)
{
    return pace_frame_ms();
}

void gmv_set_frame_ms(double ms)
{
    g_pace_ms = ms > 0.0 ? ms : 0.0;
    g_pace_gen++;
}

int swap_interval(int v)
{
    return SDL_GL_SetSwapInterval(v);
}

/* SWAP_INTERVAL as set (-1 = blank), from the file at startup or the options menu since. */
int swap_want(void)
{
    if (g_swap_want == -2) {
        const char *si = gmv_getenv("GMV_SWAP_INTERVAL");
        g_swap_want = (si && *si) ? atoi(si) : -1;
    }
    return g_swap_want;
}

/* VSYNC from the options menu: taken with the pacer's, after the next present. */
int gmv_set_swap_interval(int v)
{
    g_swap_want = v < 0 ? -1 : v;
    g_pace_gen++;
    return 1;
}

/* The GL window; NULL before it is created. */
SDL_Window *gmv_gl_window(void)
{
    return g_window;
}

/* The window's handle for what must still speak to Windows itself; NULL elsewhere. */
void *gmv_gl_window_hwnd(void)
{
    return g_window ? SDL_GetPointerProperty(SDL_GetWindowProperties(g_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL)
                    : NULL;
}
