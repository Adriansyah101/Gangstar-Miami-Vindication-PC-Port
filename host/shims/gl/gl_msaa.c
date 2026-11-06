/* gl_msaa.c -- MSAA: the screen framebuffer */
#include <stdlib.h>
#include "compat.h"
#include <GL/gl.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "glshim.h"

/* ---- MSAA: the multisampling the Mac game asked for ------------------------------------------
   -[EAGLView initWithFrame:shareContext:] (0x199bd0) creates its NSOpenGLPixelFormat from the list at
   0x5f6de0 -- Accelerated, NoRecovery, DoubleBuffer, colour 32, depth 24, Multisample, SampleBuffers 1,
   Samples 4 -- unless -[GameDelegate isOldDevice] picks the same list without the last three
   (0x5f6e40).  isOldDevice is byte +0x13, set at 0x19e06e only when hw.model is one of eight early
   Intel-graphics Macs (Macmini1,1 ... iMac10,1); our sysctl reports no model, so the game takes the 4x
   branch.  The window's pixel format here was single-sampled, so that anti-aliasing was lost.
   Rather than a multisampled pixel format (probing WGL_ARB_pixel_format needs a second window, and
   it may not combine with the copy-swap the game relies on), the game's screen is a multisampled FBO
   of the window's size: binding framebuffer 0 binds it, and each present resolves it into a
   single-sampled FBO and copies that to the window's back buffer.  The FBO keeps its contents across
   presents, as the Mac back buffer did, and reads of the screen (glReadPixels, glCopyTexSubImage2D)
   come from the resolved copy, as reads of a multisampled Mac back buffer resolve.  Colour is RGB8
   like the window format (no alpha), so destination alpha still reads as 1.  GMV_MSAA=0 turns it off;
   2 or 8 pick another sample count, capped at the GPU's GL_MAX_SAMPLES.  The options menu's
   ANTI-ALIASING changes the count with gmv_set_msaa while the screen is this FBO (OFF = a
   single-sampled one); after the next present its buffers are made again with the new count. */
int g_msaa;                                      /* samples in use; 0 = off */
static int g_msaa_max;                                  /* GL_MAX_SAMPLES */
static int g_msaa_want = -1;                            /* gmv_set_msaa's count, for msaa_sync_samples */
int g_tab_live;                                  /* GAMEPATCH_TAB: settings change in game, so the screen
                                                           FBO is made even at MSAA=0, the textures are noted
                                                           and the post wrappers stay in */
GLuint g_ms_fbo, g_ms_rb[2];                     /* the multisampled screen: colour, depth+stencil */
static GLuint g_rs_fbo, g_rs_rb;                        /* its resolved copy */
int g_ms_w, g_ms_h;
GLuint g_ms_glow;                                /* BLOOM_LIGHTS / BLOOM_WINDOWS: the glow layer, colour attachment 1 */
int g_glow_bufs;                                 /* the screen FBO's draw buffers: 2 = with the glow layer */
int g_glow_used;                                 /* the glow layer was drawn this frame */
static GLint g_diag_rfb, g_diag_dfb;                    /* bindings saved by diag_read_begin */

msf_t msf;

void *ms_proc(const char *core, const char *ext)
{
    void *p = gl_real(core);
    return p ? p : gl_real(ext);
}

/* Clear the screen FBO to black, leaving the game's scissor, colour mask and clear colour alone. */
static void msaa_clear(void)
{
    GLint dfb = 0;
    GLfloat cc[4];
    GLboolean cm[4], sc = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(MS_DRAW_FB_BINDING, &dfb);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    msf.BindFramebuffer(MS_DRAW_FB, g_ms_fbo);
    if (sc) glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    glColorMask(cm[0], cm[1], cm[2], cm[3]);
    if (sc) glEnable(GL_SCISSOR_TEST);
    msf.BindFramebuffer(MS_DRAW_FB, (GLuint)dfb);
}

static void msaa_alloc(int w, int h)
{
    GLint rb = 0;
    glGetIntegerv(0x8CA7, &rb);                         /* GL_RENDERBUFFER_BINDING */
    msf.BindRenderbuffer(MS_RENDERBUFFER, g_ms_rb[0]);
    msf.RenderbufferStorageMultisample(MS_RENDERBUFFER, g_msaa, 0x8051, w, h);       /* GL_RGB8 */
    msf.BindRenderbuffer(MS_RENDERBUFFER, g_ms_rb[1]);
    msf.RenderbufferStorageMultisample(MS_RENDERBUFFER, g_msaa, 0x88F0, w, h);       /* GL_DEPTH24_STENCIL8 */
    msf.BindRenderbuffer(MS_RENDERBUFFER, g_rs_rb);
    msf.RenderbufferStorageMultisample(MS_RENDERBUFFER, 0, 0x8051, w, h);
    if (g_ms_glow) {
        msf.BindRenderbuffer(MS_RENDERBUFFER, g_ms_glow);
        msf.RenderbufferStorageMultisample(MS_RENDERBUFFER, g_msaa, 0x881A, w, h);   /* RGBA16F */
    }
    msf.BindRenderbuffer(MS_RENDERBUFFER, (GLuint)rb);
    g_ms_w = w;
    g_ms_h = h;
}

/* The screen FBO is made whatever MSAA says (without samples at MSAA=0): full screen draws at the
   chosen resolution into it and msaa_present scales it to the monitor.  g_fb_scale_off: the
   driver could not make one, so full screen draws at the monitor's size as before. */
int g_fb_scale_off;

void msaa_init(void)
{
    const char *e = gmv_getenv("GMV_MSAA");             /* default 4, as the Mac game asked */
    int want = (e && *e) ? atoi(e) : 4;
    g_fb_scale_off = 1;                                 /* until the FBO is made */
    msf.GenFramebuffers = ms_proc("glGenFramebuffers", "glGenFramebuffersEXT");
    msf.BindFramebuffer = ms_proc("glBindFramebuffer", "glBindFramebufferEXT");
    msf.GenRenderbuffers = ms_proc("glGenRenderbuffers", "glGenRenderbuffersEXT");
    msf.BindRenderbuffer = ms_proc("glBindRenderbuffer", "glBindRenderbufferEXT");
    msf.RenderbufferStorageMultisample = ms_proc("glRenderbufferStorageMultisample",
                                                 "glRenderbufferStorageMultisampleEXT");
    msf.FramebufferRenderbuffer = ms_proc("glFramebufferRenderbuffer", "glFramebufferRenderbufferEXT");
    msf.CheckFramebufferStatus = ms_proc("glCheckFramebufferStatus", "glCheckFramebufferStatusEXT");
    msf.BlitFramebuffer = ms_proc("glBlitFramebuffer", "glBlitFramebufferEXT");
    if (!msf.GenFramebuffers || !msf.BindFramebuffer || !msf.GenRenderbuffers || !msf.BindRenderbuffer ||
        !msf.RenderbufferStorageMultisample || !msf.FramebufferRenderbuffer || !msf.CheckFramebufferStatus ||
        !msf.BlitFramebuffer) {
        gmv_log("[msaa] MSAA=%d: the driver has no framebuffer objects; anti-aliasing off, and full screen "
                "draws at the monitor's resolution\n", want);
        return;
    }
    GLint mx = 0;
    glGetIntegerv(0x8D57, &mx);                         /* GL_MAX_SAMPLES */
    g_msaa_max = mx;
    g_msaa = want < mx ? want : mx;
    if (g_msaa <= 1) {
        if (want > 1)
            gmv_log("[msaa] MSAA=%d: GPU maximum %d samples; anti-aliasing off\n", want, mx);
        g_msaa = 0;                                     /* the options menu can still turn it on (mx > 1) */
    }
    g_fb_scale_off = 0;
    int w = 1024, h = 768;
    gmv_gl_render_size(&w, &h);
    msf.GenFramebuffers(1, &g_ms_fbo);
    msf.GenFramebuffers(1, &g_rs_fbo);
    msf.GenRenderbuffers(2, g_ms_rb);
    msf.GenRenderbuffers(1, &g_rs_rb);
    msaa_alloc(w, h);
    msf.BindFramebuffer(MS_FRAMEBUFFER, g_rs_fbo);
    msf.FramebufferRenderbuffer(MS_FRAMEBUFFER, 0x8CE0, MS_RENDERBUFFER, g_rs_rb);       /* COLOR_ATTACHMENT0 */
    GLenum rs = msf.CheckFramebufferStatus(MS_FRAMEBUFFER);
    msf.BindFramebuffer(MS_FRAMEBUFFER, g_ms_fbo);
    msf.FramebufferRenderbuffer(MS_FRAMEBUFFER, 0x8CE0, MS_RENDERBUFFER, g_ms_rb[0]);
    msf.FramebufferRenderbuffer(MS_FRAMEBUFFER, 0x8D00, MS_RENDERBUFFER, g_ms_rb[1]);    /* DEPTH_ATTACHMENT */
    msf.FramebufferRenderbuffer(MS_FRAMEBUFFER, 0x8D20, MS_RENDERBUFFER, g_ms_rb[1]);    /* STENCIL_ATTACHMENT */
    GLenum ms = msf.CheckFramebufferStatus(MS_FRAMEBUFFER);
    if (ms != 0x8CD5 || rs != 0x8CD5) {                 /* GL_FRAMEBUFFER_COMPLETE */
        gmv_log("[msaa] framebuffer incomplete (0x%x / 0x%x); anti-aliasing off, and full screen draws at "
                "the monitor's resolution\n", ms, rs);
        msf.BindFramebuffer(MS_FRAMEBUFFER, 0);
        g_ms_fbo = 0;
        g_msaa = 0;
        g_fb_scale_off = 1;
        return;
    }
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    if (g_msaa)
        gmv_log("[msaa] MSAA=%d, GPU maximum %d: the game draws into a %dx multisampled %dx%d screen\n",
                want, mx, g_msaa, w, h);
    else
        gmv_log("[msaa] MSAA=%d, GPU maximum %d: the game draws into a %dx%d screen without samples\n",
                want, mx, w, h);
}

/* Resolve a rectangle of the screen into the resolved copy (left bound for reading). */
void msaa_resolve(GLint x, GLint y, GLint w, GLint h)
{
    GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);        /* blits are scissored */
    if (sc) glDisable(GL_SCISSOR_TEST);
    msf.BindFramebuffer(MS_READ_FB, g_ms_fbo);
    msf.BindFramebuffer(MS_DRAW_FB, g_rs_fbo);
    msf.BlitFramebuffer(x, y, x + w, y + h, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    msf.BindFramebuffer(MS_READ_FB, g_rs_fbo);
    if (sc) glEnable(GL_SCISSOR_TEST);
}

/* Before SwapBuffers: put the frame in the window's back buffer -- as it is, or at another size than
   the client area (full screen at another resolution, or a window the player has sized) scaled to fit
   with its shape kept and black bars around it (gmv_gl_view_rect), as the Mac's monitor showed the
   game's display mode. */
void msaa_present(void)
{
    if (!g_ms_fbo)
        return;
    GLint dfb = 0, rfb = 0;
    glGetIntegerv(MS_DRAW_FB_BINDING, &dfb);
    glGetIntegerv(MS_READ_FB_BINDING, &rfb);
    msaa_resolve(0, 0, g_ms_w, g_ms_h);
    GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
    if (sc) glDisable(GL_SCISSOR_TEST);
    msf.BindFramebuffer(MS_DRAW_FB, 0);
    int x, y, w, h, cw = g_ms_w, ch = g_ms_h;
    gmv_gl_view_rect(&x, &y, &w, &h, NULL, NULL);
    gmv_gl_client_size(&cw, &ch);
    if (x == 0 && y == 0 && w == g_ms_w && h == g_ms_h)
        msf.BlitFramebuffer(0, 0, g_ms_w, g_ms_h, 0, 0, g_ms_w, g_ms_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    else {
        GLfloat cc[4];
        GLboolean cm[4];
        glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
        glGetBooleanv(GL_COLOR_WRITEMASK, cm);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);                   /* the bars */
        glClearColor(cc[0], cc[1], cc[2], cc[3]);
        glColorMask(cm[0], cm[1], cm[2], cm[3]);
        int gy = ch - y - h;                            /* GL's origin is bottom-left */
        msf.BlitFramebuffer(0, 0, g_ms_w, g_ms_h, x, gy, x + w, gy + h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    if (sc) glEnable(GL_SCISSOR_TEST);
    msf.BindFramebuffer(MS_READ_FB, (GLuint)rfb);
    msf.BindFramebuffer(MS_DRAW_FB, (GLuint)dfb);
}

int gmv_set_msaa(int samples)
{
    if (!g_ms_fbo)
        return 0;
    g_msaa_want = samples <= 1 ? 0 : samples < g_msaa_max ? samples : g_msaa_max;
    return 1;
}

/* After a present: gmv_set_msaa's count. */
void msaa_sync_samples(void)
{
    int want = g_msaa_want;
    if (!g_ms_fbo || want < 0)
        return;
    g_msaa_want = -1;
    if (want == g_msaa)
        return;
    g_msaa = want;
    msaa_alloc(g_ms_w, g_ms_h);
    msaa_clear();
    gmv_log("[msaa] the screen now has %d samples%s\n", g_msaa, g_msaa ? "" : " (anti-aliasing off)");
}

/* Follow the size the game draws at (resolution / fullscreen changes). */
void msaa_sync_size(void)
{
    if (!g_ms_fbo)
        return;
    int w = g_ms_w, h = g_ms_h;
    gmv_gl_render_size(&w, &h);
    if (w == g_ms_w && h == g_ms_h)
        return;
    msaa_alloc(w, h);
    msaa_clear();
    int cw = w, ch = h;
    gmv_gl_client_size(&cw, &ch);
    if (cw == w && ch == h)
        gmv_log("[msaa] screen resized to %dx%d\n", w, h);
    else
        gmv_log("[msaa] screen resized to %dx%d, scaled to fit the %dx%d window\n", w, h, cw, ch);
    gmv_input_view_changed();
}

/* The host's own diagnostics (DUMP_FRAME, GL_TRACE, FADE_TRACE, FLICKER_CAP, CHECK_FRONT) read the
   window: GL_BACK before the present means the frame being built, i.e. the screen FBO's resolved
   copy; GL_FRONT after it is the window itself. */
void diag_read_begin(GLenum buf)
{
    if (g_ms_fbo) {
        glGetIntegerv(MS_DRAW_FB_BINDING, &g_diag_dfb);
        glGetIntegerv(MS_READ_FB_BINDING, &g_diag_rfb);
        if (buf == GL_BACK) {
            msaa_resolve(0, 0, g_ms_w, g_ms_h);
            return;
        }
        msf.BindFramebuffer(MS_READ_FB, 0);
    }
    glReadBuffer(buf);
}
void diag_read_end(void)
{
    if (g_ms_fbo) {
        msf.BindFramebuffer(MS_READ_FB, (GLuint)g_diag_rfb);
        msf.BindFramebuffer(MS_DRAW_FB, (GLuint)g_diag_dfb);
    }
}
