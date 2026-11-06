/* gl_trace.c -- the per-frame GL call trace, and the wrappers of clear and viewport */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "compat.h"
#include <GL/gl.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "glshim.h"

/* ---- optional per-frame GL call tracing (GMV_GL_TRACE=1) ------------------------------
   To diagnose a blank frame we need to see what the game draws each frame.  gmv_gl_resolve
   (below) hands these C shims out instead of the plain forwarding thunk for a handful of hot
   calls; each records what it saw and forwards to the real stdcall GL entry.  The game calls GL
   cdecl and these shims are cdecl, so there is no ABI mismatch (the real opengl32 call inside is
   declared stdcall and called correctly).  Counters are per-frame; gmv_gl_swap/flushBuffer print
   a one-line summary and reset them. */

#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif

struct gltrace g_glt;

/* Wrappers are cdecl (the game calls GL cdecl); the real opengl32 entries are stdcall (APIENTRY). */
void tr_ClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
    static void(APIENTRY * real)(GLfloat, GLfloat, GLfloat, GLfloat);
    if (!real) real = gl_real("glClearColor");
    g_glt.cc[0] = r; g_glt.cc[1] = g; g_glt.cc[2] = b; g_glt.cc[3] = a;
    if (real) real(r, g, b, a);
}
void tr_Clear(GLbitfield m)
{
    static void(APIENTRY * real)(GLbitfield);
    if (!real) real = gl_real("glClear");
    g_glt.clears++;
    PM_CLEAR();
    g_cap_clears++;
    cap_seq("C ");
    g_drew_since_swap = 1;
    if (g_gl_trace) {
        static int logn;
        if (logn < 10) {
            gmv_log("[gltrace] glClear #%d mask=0x%x caller=%p\n",
                    logn, m, __builtin_return_address(0));
            logn++;
        }
    }
    gmv_game_gl_clear_begin(m);
    if (real) real(m);
    gmv_game_gl_clear_end(m);
    post_fade_probe("clear mask", m, (unsigned)(uintptr_t)__builtin_return_address(0));
}
/* Make the host window track the game's MAIN (default-framebuffer) viewport, so the resolution the
   game applies in its Video options always fills the window instead of being cut off (when bigger
   than the window) or drawn small in a corner (when smaller).  The game applies a resolution change
   LIVE via glViewport while our window kept its startup size -- this closes that gap.  Windowed:
   resize the window CLIENT to the game's requested WxH if that is a new size (gl_window_fit), and
   the screen FBO is that size whatever the player has made of the window.  Fullscreen: keep the window covering the
   whole screen and render at the chosen resolution (gmv_gl_render_size), which msaa_present scales
   to fit the screen.  Only the default framebuffer drives this; FBO viewports pass through untouched
   (the caller checks g_glt.last_fb==0). */
static void gmv_gl_follow_viewport(GLsizei *w, GLsizei *h)
{
    if (!g_window || *w <= 0 || *h <= 0)
        return;
    int fs = gmv_prefs_get_long("FullScreen", 1) != 0;   /* same default as gmv_effective_screen */
    gl_window_fit(fs, *w, *h);
    if (fs) {
        int rw = *w, rh = *h;
        gmv_gl_render_size(&rw, &rh);
        *w = rw; *h = rh;                           /* the whole screen FBO (the chosen resolution) */
    }
}

/* Apply the game's current saved resolution/fullscreen option to the host window RIGHT NOW.
   Called from the NSUserDefaults setters (shimsfnd.c) the moment the Video options are written, so
   toggling Full Screen or changing the resolution takes effect immediately.  The old path relied on
   the game re-issuing a main-framebuffer glViewport, but a bare Full Screen toggle often does not do
   that -- so the window stayed at the old size and "fullscreen failed / showed the old resolution".
   Runs on the game's main thread (the setter's caller), which also owns the window and GL context. */
void gmv_gl_apply_mode(void)
{
    if (!g_window)
        return;
    long w = 0, h = 0; int fs = 0;
    gmv_effective_screen(&w, &h, &fs);
    if (w <= 0 || h <= 0)
        return;
    gl_window_fit(fs, (int)w, (int)h);
    /* Re-point the GL viewport at the new client size now, so the game fills the window on the very
       next frame even if it never re-issues glViewport itself (same thread as the GL context). */
    int rw = 0, rh = 0;
    gmv_gl_render_size(&rw, &rh);
    void(APIENTRY * rvp)(GLint, GLint, GLsizei, GLsizei) = gl_real("glViewport");
    if (rvp && rw > 0 && rh > 0) {
        rvp(0, 0, rw, rh);
        g_glt.vp[0] = 0; g_glt.vp[1] = 0; g_glt.vp[2] = rw; g_glt.vp[3] = rh;
    }
    msaa_sync_size();
}

void tr_Viewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    static void(APIENTRY * real)(GLint, GLint, GLsizei, GLsizei);
    if (!real) real = gl_real("glViewport");
    /* Only the main (default-framebuffer) viewport at the origin drives the host window size. */
    if (g_glt.last_fb == 0 && x == 0 && y == 0) {
        gmv_gl_follow_viewport(&w, &h);
        msaa_sync_size();
    }
    static int n;
    if (GMV_DEV && (n < 10 || w != g_glt.vp[2] || h != g_glt.vp[3])) {
        gmv_log("[gltrace] glViewport(%d,%d,%d,%d) fb=%u\n", x, y, w, h, g_glt.last_fb);
        n++;
    }
    g_glt.vp[0] = x; g_glt.vp[1] = y; g_glt.vp[2] = w; g_glt.vp[3] = h;
    if (real) real(x, y, w, h);
}
void gl_trace_frame_end(unsigned frame)
{
    if (!g_gl_trace)
        return;
    /* Also log ANY frame that actually drew/cleared (catches brief draw blips -- e.g. the menu
       flashing for a few frames before it switches away -- that the every-60 sampling misses). */
    if (frame < 6 || frame % 60 == 0 || g_glt.clears || g_glt.da_calls || g_glt.de_calls) {
        gmv_log("[gltrace] f%u clr=%d color=(%.2f,%.2f,%.2f,%.2f) vp=(%d,%d,%d,%d) fb=%u "
                "drawArrays=%d/%ldv drawElements=%d/%ldv maxBuf=%ld texBind=%d\n",
                frame, g_glt.clears, g_glt.cc[0], g_glt.cc[1], g_glt.cc[2], g_glt.cc[3],
                g_glt.vp[0], g_glt.vp[1], g_glt.vp[2], g_glt.vp[3], g_glt.last_fb,
                g_glt.da_calls, g_glt.da_verts, g_glt.de_calls, g_glt.de_verts,
                g_glt.max_buf, g_glt.tex_binds);
        GLboolean sc = glIsEnabled(0x0C11);             /* GL_SCISSOR_TEST */
        GLint sb[4] = {0, 0, 0, 0};
        GLboolean cm[4] = {0, 0, 0, 0}, dm = 0;
        glGetIntegerv(0x0C10, sb);                      /* GL_SCISSOR_BOX */
        glGetBooleanv(0x0C23, cm);                      /* GL_COLOR_WRITEMASK */
        glGetBooleanv(0x0B46, &dm);                     /* GL_DEPTH_WRITEMASK */
        gmv_log("[gltrace]    scissor=%d box=(%d,%d,%d,%d) colormask=%d%d%d%d depthmask=%d "
                "depthtest=%d cullface=%d blend=%d\n",
                (int)sc, sb[0], sb[1], sb[2], sb[3], cm[0], cm[1], cm[2], cm[3], dm,
                glIsEnabled(0x0B71), glIsEnabled(0x0B44), glIsEnabled(0x0BE2));
        /* Reliable readback: sample the just-presented FRONT buffer centre + a corner. */
        unsigned char c0[4] = {0}, c1[4] = {0};
        diag_read_begin(GL_FRONT);
        glReadPixels(512, 384, 1, 1, 0x1908, 0x1401, c0);   /* GL_RGBA, GL_UNSIGNED_BYTE */
        glReadPixels(40, 40, 1, 1, 0x1908, 0x1401, c1);
        diag_read_end();
        gmv_log("[gltrace]    FRONT centre=%d,%d,%d,%d corner=%d,%d,%d,%d  curctx=%p tid=%lu\n",
                c0[0], c0[1], c0[2], c0[3], c1[0], c1[1], c1[2], c1[3],
                (void *)wglGetCurrentContext(), GetCurrentThreadId());
    }
    unsigned fb = g_glt.last_fb;
    float cc0 = g_glt.cc[0], cc1 = g_glt.cc[1], cc2 = g_glt.cc[2], cc3 = g_glt.cc[3];
    int vp0 = g_glt.vp[0], vp1 = g_glt.vp[1], vp2 = g_glt.vp[2], vp3 = g_glt.vp[3];
    memset(&g_glt, 0, sizeof g_glt);            /* reset per-frame counters */
    g_glt.last_fb = fb;                         /* but keep sticky GL state */
    g_glt.cc[0] = cc0; g_glt.cc[1] = cc1; g_glt.cc[2] = cc2; g_glt.cc[3] = cc3;
    g_glt.vp[0] = vp0; g_glt.vp[1] = vp1; g_glt.vp[2] = vp2; g_glt.vp[3] = vp3;
}
