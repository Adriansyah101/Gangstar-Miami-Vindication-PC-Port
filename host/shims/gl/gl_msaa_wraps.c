/* gl_msaa_wraps.c -- what the game sees of its screen while the screen is the MSAA framebuffer */
#include "compat.h"
#include <GL/gl.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "glshim.h"

/* ---- MSAA (see msaa_init): what the game sees of its screen while the screen is the MSAA FBO ----
   Framebuffer 0 stays 0 to the game (tr_BindFramebuffer binds the FBO for it, and the binding queries
   answer 0), screen reads come from the resolved copy, and the window buffer names it hands
   glDrawBuffers for the screen mean the FBO's colour attachment. */
void ms_GetIntegerv(GLenum pname, GLint *v)
{
    static void(APIENTRY * real)(GLenum, GLint *);
    if (!real) real = gl_real("glGetIntegerv");
    if (real) real(pname, v);
    if (g_ms_fbo && v && (pname == MS_DRAW_FB_BINDING || pname == MS_READ_FB_BINDING) && *v == (GLint)g_ms_fbo)
        *v = 0;
}
void ms_ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *px)
{
    static void(APIENTRY * real)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    if (!real) real = gl_real("glReadPixels");
    if (!real) return;
    if (!g_ms_fbo || g_glt.last_fb) {
        real(x, y, w, h, format, type, px);
        return;
    }
    msaa_resolve(x, y, w, h);
    msf.BindFramebuffer(MS_DRAW_FB, g_ms_fbo);
    real(x, y, w, h, format, type, px);
    msf.BindFramebuffer(MS_READ_FB, g_ms_fbo);
}
void ms_CopyTexSubImage2D(GLenum target, GLint level, GLint xo, GLint yo, GLint x, GLint y,
                                 GLsizei w, GLsizei h)
{
    static void(APIENTRY * real)(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei);
    if (!real) real = gl_real("glCopyTexSubImage2D");
    if (!real) return;
    if (!g_ms_fbo || g_glt.last_fb) {
        real(target, level, xo, yo, x, y, w, h);
        return;
    }
    msaa_resolve(x, y, w, h);
    msf.BindFramebuffer(MS_DRAW_FB, g_ms_fbo);
    real(target, level, xo, yo, x, y, w, h);
    msf.BindFramebuffer(MS_READ_FB, g_ms_fbo);
}
void ms_DrawBuffers(GLsizei n, const GLenum *bufs)
{
    static void(APIENTRY * real)(GLsizei, const GLenum *);
    if (!real) { real = gl_real("glDrawBuffers"); if (!real) real = gl_real("glDrawBuffersARB"); }
    if (!real) return;
    if (!g_ms_fbo || g_glt.last_fb || n <= 0 || n > 8) {
        real(n, bufs);
        return;
    }
    GLenum b[8];
    for (int i = 0; i < n; i++)                         /* GL_FRONT_LEFT .. GL_AUX3 -> COLOR_ATTACHMENT0 */
        b[i] = bufs[i] >= 0x0400 && bufs[i] <= 0x040C ? (i ? GL_NONE : 0x8CE0) : bufs[i];
    real(n, b);
    g_glow_bufs = 0;                                    /* the glow layer's draw buffer is set again (bg_draw_begin) */
}
