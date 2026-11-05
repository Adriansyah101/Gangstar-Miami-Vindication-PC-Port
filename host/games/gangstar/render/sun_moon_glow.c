/* sun_moon_glow.c -- SUN_MOON: the glows behind the world */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"
#include <xmmintrin.h>
#include <GL/gl.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "render.h"

/* ---- SUN_MOON: the glows behind the world ------------------------------------------------------------
   g_sky_glow_draw is set by the patches while a sun or moon glow renders.  The glow's material has no
   depth test and it is drawn after the world, in the world's depth range (0..0.5), so it showed through
   buildings.  Its draw now gets a shader in place of the material's fixed-function shading (unit 0
   texture times vertex colour, added ONE/ONE) that reads the depth drawn so far: where it is sky (at
   the far end of that range or beyond) the glow is as before; on the world it leaks a little way in
   past the silhouettes, as light wraps around an edge, and fades out GLOW_WRAP of the screen height
   inside them.  At the user's request the glow is GLOW_GAIN times as bright as the material's.  The
   glow textures are DXT5, whose 5- and 6-bit colours step the gradient into visible rings (red falls
   90, 82, 74 ... in steps of 8 every ~16 texels), so the shader reads a smoothed 16-bit copy of each
   (glow_smooth) and dithers its output below one 8-bit step.  If the shader or the depth copy cannot be had, or the glow goes to one of the game's
   own framebuffers, a plain depth test (LEQUAL, no writes, every fragment at the far end of the range)
   hides it behind the world with a hard edge.
   SUN_MOON=2 (IPHONE, g_sky_glow_iphone) keeps the material's own draw and the Mac's texture as it is: no
   depth test, no gain, no shader, no smoothing, so the DXT5 rings show (the player's choice). */
int g_sky_glow_draw;
int g_sky_glow_iphone;
#define GLOW_WRAP 0.03f
#define GLOW_GAIN 1.6f
#define GLOW_DEPTH_UNIT 3                               /* texture unit of the depth copy (the game uses 0) */
#define GLOW_SMOOTH_UNIT 4                              /* texture unit of the smoothed glow texture */

static const char glow_vs[] =
    "#version 120\n"
    "void main() {\n"
    "    gl_Position = ftransform();\n"
    "    gl_TexCoord[0] = gl_TextureMatrix[0] * gl_MultiTexCoord0;\n"
    "    gl_FrontColor = gl_Color;\n"
    "}\n";

/* Off the sky, the glow keeps twice the share of a disc of taps that see sky: 1 at a silhouette, 0 a
   radius inside it, and most of it across anything thinner than the radius. */
static const char glow_fs[] =
    "#version 120\n"
    "uniform sampler2D tex, dep;\n"
    "uniform vec2 full;\n"
    "uniform float far, rad, gain;\n"
    "float sky(vec2 px) { return step(far, texture2D(dep, clamp(px, vec2(0.5), full - 0.5) / full).r); }\n"
    "void main() {\n"
    "    vec4 c = texture2D(tex, gl_TexCoord[0].st) * gl_Color * gain;\n"
    "    vec2 px = gl_FragCoord.xy;\n"
    "    if (sky(px) == 0.0) {\n"
    "        float s = 0.0;\n"
    "        for (int i = 0; i < 32; i++) {\n"
    "            float a = float(i) * 2.3999632;\n"
    "            s += sky(px + vec2(cos(a), sin(a)) * (rad * sqrt((float(i) + 0.5) / 32.0)));\n"
    "        }\n"
    "        c *= smoothstep(0.0, 1.0, min(s / 16.0, 1.0));\n"
    "    }\n"
    "    float n = fract(sin(dot(px, vec2(12.9898, 78.233))) * 43758.5453)\n"
    "            + fract(sin(dot(px, vec2(39.3468, 11.1353))) * 24634.6345) - 1.0;\n"
    "    c.rgb += n / 255.0 * step(0.5 / 255.0, max(c.r, max(c.g, c.b)));\n"
    "    gl_FragColor = c;\n"
    "}\n";

static struct {
    int ready;                                          /* 0 = not tried, 1 = shader, -1 = depth test only */
    GLuint p, fbo, t;
    GLint u_tex, u_dep, u_full, u_far, u_rad, u_gain;
    int w, h;
    int copied;                                         /* the depth copy is current (no draw since) */
    unsigned draws;                                     /* draw count when it was made */
} glw;

static int glow_init(void)
{
    if (!ao_procs("the glows' light wrap") || !(glw.p = ao_program("glow", glow_vs, glow_fs)))
        return 0;
    glw.u_tex = aof.GetUniformLocation(glw.p, "tex");
    glw.u_dep = aof.GetUniformLocation(glw.p, "dep");
    glw.u_full = aof.GetUniformLocation(glw.p, "full");
    glw.u_far = aof.GetUniformLocation(glw.p, "far");
    glw.u_rad = aof.GetUniformLocation(glw.p, "rad");
    glw.u_gain = aof.GetUniformLocation(glw.p, "gain");
    glGenTextures(1, &glw.t);
    msf.GenFramebuffers(1, &glw.fbo);
    return 1;
}

/* Copy the depth of the framebuffer being drawn to into glw.t (bound on the active unit); 0 on failure. */
static int glow_copy_depth(int w, int h)
{
    GLint dfb = 0, rfb = 0;
    glGetIntegerv(MS_DRAW_FB_BINDING, &dfb);
    glGetIntegerv(MS_READ_FB_BINDING, &rfb);
    int ok = 1;
    if (w != glw.w || h != glw.h) {
        ao_tex(glw.t, 0x88F0, w, h, 0x84F9, 0x84FA);              /* DEPTH24_STENCIL8, DEPTH_STENCIL, 24_8 */
        glTexParameteri(GL_TEXTURE_2D, 0x884C, GL_NONE);            /* GL_TEXTURE_COMPARE_MODE */
        msf.BindFramebuffer(MS_FRAMEBUFFER, glw.fbo);
        aof.FramebufferTexture2D(MS_FRAMEBUFFER, 0x821A, GL_TEXTURE_2D, glw.t, 0);   /* DEPTH_STENCIL_ATTACHMENT */
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        GLenum st = msf.CheckFramebufferStatus(MS_FRAMEBUFFER);
        if (st == 0x8CD5) {
            glw.w = w;
            glw.h = h;
        } else {
            gmv_log("[sunmoon] depth copy framebuffer incomplete (0x%x); the glows get a hard edge\n", st);
            ok = 0;
        }
    } else {
        glBindTexture(GL_TEXTURE_2D, glw.t);
    }
    if (ok) {
        GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);           /* it would clip the blit */
        if (sc) glDisable(GL_SCISSOR_TEST);
        msf.BindFramebuffer(MS_READ_FB, (GLuint)dfb);
        msf.BindFramebuffer(MS_DRAW_FB, glw.fbo);
        msf.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        if (sc) glEnable(GL_SCISSOR_TEST);
    }
    msf.BindFramebuffer(MS_READ_FB, (GLuint)rfb);
    msf.BindFramebuffer(MS_DRAW_FB, (GLuint)dfb);
    return ok;
}

/* A clamped box blur of w x h RGBA floats along the rows, the four channels together.  One texture took
   53.8 ms a channel and a line at a time, and it takes 1.3 ms like this (offline, 2026-11-03; no value
   of the 16-bit result off by more than 1), so the first draw of the glows is no longer a long frame. */
__attribute__((target("sse2")))
static void glow_rows(const float *in, float *out, int w, int h, int r)
{
    const __m128 k = _mm_set1_ps(1.0f / (float)(2 * r + 1));
    for (int y = 0; y < h; y++, in += w * 4, out += w * 4) {
        __m128 sum = _mm_setzero_ps();
        for (int i = -r; i <= r; i++)
            sum = _mm_add_ps(sum, _mm_loadu_ps(in + (i < 0 ? 0 : i >= w ? w - 1 : i) * 4));
        for (int i = 0; i < w; i++) {
            _mm_storeu_ps(out + i * 4, _mm_mul_ps(sum, k));
            int add = i + r + 1, sub = i - r;
            sum = _mm_add_ps(sum, _mm_sub_ps(_mm_loadu_ps(in + (add >= w ? w - 1 : add) * 4),
                                             _mm_loadu_ps(in + (sub < 0 ? 0 : sub) * 4)));
        }
    }
}

/* The same along the columns, row by row so the memory is walked in order; sums holds one row. */
__attribute__((target("sse2")))
static void glow_cols(const float *in, float *out, float *sums, int w, int h, int r)
{
    const __m128 k = _mm_set1_ps(1.0f / (float)(2 * r + 1));
    const int stride = w * 4;
    memset(sums, 0, (size_t)stride * sizeof *sums);
    for (int i = -r; i <= r; i++) {
        const float *row = in + (i < 0 ? 0 : i >= h ? h - 1 : i) * stride;
        for (int x = 0; x < w; x++)
            _mm_storeu_ps(sums + x * 4, _mm_add_ps(_mm_loadu_ps(sums + x * 4), _mm_loadu_ps(row + x * 4)));
    }
    for (int y = 0; y < h; y++) {
        int add = y + r + 1, sub = y - r;
        const float *ra = in + (add >= h ? h - 1 : add) * stride, *rs = in + (sub < 0 ? 0 : sub) * stride;
        float *o = out + y * stride;
        for (int x = 0; x < w; x++) {
            __m128 sum = _mm_loadu_ps(sums + x * 4);
            _mm_storeu_ps(o + x * 4, _mm_mul_ps(sum, k));
            _mm_storeu_ps(sums + x * 4, _mm_add_ps(sum, _mm_sub_ps(_mm_loadu_ps(ra + x * 4), _mm_loadu_ps(rs + x * 4))));
        }
    }
}

/* The smoothed copy of the glow texture `src` (made on the first use, on the active unit); 0 if none.
   Three box blurs each way, of 1/64 of the width, make a Gaussian of about 1/60 that turns the steps
   into a ramp. */
static GLuint glow_smooth(GLuint src)
{
    static struct { GLuint src, t; } c[4];
    static int next;
    for (int k = 0; k < 4; k++)
        if (c[k].src == src)
            return c[k].t;
    GLint act = 0, w = 0, h = 0, ws = 0, wt = 0, pack = 4;
    glGetIntegerv(0x84E0, &act);
    aof.ActiveTexture(0x84C0);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &ws);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wt);
    GLuint t = 0;
    size_t n = (size_t)w * h * 4;
    unsigned char *px = w >= 16 && h >= 16 && w <= 2048 && h <= 2048 ? malloc(n) : NULL;
    float *a = px ? malloc(n * sizeof(float)) : NULL, *b = a ? malloc(n * sizeof(float)) : NULL;
    float *sums = b ? malloc((size_t)w * 4 * sizeof(float)) : NULL;
    if (sums) {
        glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glPixelStorei(GL_PACK_ALIGNMENT, pack);
        for (size_t i = 0; i < n; i++)
            a[i] = px[i];
        int r = w / 64 > 1 ? w / 64 : 1;
        for (int p = 0; p < 3; p++) {
            glow_rows(a, b, w, h, r);
            glow_cols(b, a, sums, w, h, r);
        }
        unsigned short *o = (unsigned short *)b;              /* reuses b, which is twice as big */
        for (size_t i = 0; i < n; i++) {
            float v = a[i] * 257.0f + 0.5f;
            o[i] = (unsigned short)(v < 0 ? 0 : v > 65535 ? 65535 : v);
        }
        aof.ActiveTexture((GLenum)act);
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, 0x805B, w, h, 0, GL_RGBA, GL_UNSIGNED_SHORT, o);   /* GL_RGBA16 */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ws);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wt);
        gmv_trace("[sunmoon] glow texture %u (%dx%d) smoothed into a 16-bit copy\n", src, w, h);
    } else {
        gmv_trace("[sunmoon] glow texture %u (%dx%d) left as it is\n", src, w, h);
    }
    aof.ActiveTexture((GLenum)act);
    free(px);
    free(a);
    free(b);
    free(sums);
    if (c[next].t)
        glDeleteTextures(1, &c[next].t);
    c[next].src = src;
    c[next].t = t;
    next = (next + 1) % 4;
    return t;
}

/* Around the glow's draw (inside tr_DrawArrays / tr_DrawElements, whose counters it has bumped). */
void glow_begin(glow_state_t *s)
{
    unsigned draws = (unsigned)(g_glt.da_calls + g_glt.de_calls);
    glGetFloatv(GL_DEPTH_RANGE, s->range);
    s->mode = 0;
    if (g_sky_glow_iphone)
        return;
    if (!glw.ready)
        glw.ready = glow_init() ? 1 : -1;
    int w = g_ms_w, h = g_ms_h;
    if (!g_ms_fbo)
        gmv_gl_client_size(&w, &h);
    if (glw.ready > 0 && !g_glt.last_fb && w > 0 && h > 0) {
        glGetIntegerv(0x8B8D, &s->prog);                        /* GL_CURRENT_PROGRAM */
        glGetIntegerv(0x84E0, &s->act);                         /* GL_ACTIVE_TEXTURE */
        aof.ActiveTexture(0x84C0 + GLOW_DEPTH_UNIT);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->tex);
        /* the sun's and moon's glows are drawn one after the other and write no depth: one copy serves both */
        int ok = glw.copied && draws == glw.draws + 1 && w == glw.w && h == glw.h;
        if (ok)
            glBindTexture(GL_TEXTURE_2D, glw.t);
        else
            ok = glow_copy_depth(w, h);
        glw.copied = ok;
        glw.draws = draws;
        GLuint sm = 0;
        if (ok) {
            GLint t0 = 0;
            aof.ActiveTexture(0x84C0);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &t0);
            aof.ActiveTexture(0x84C0 + GLOW_SMOOTH_UNIT);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->tex2);
            if ((sm = t0 ? glow_smooth((GLuint)t0) : 0))
                glBindTexture(GL_TEXTURE_2D, sm);
        }
        aof.ActiveTexture((GLenum)s->act);
        if (ok) {
            aof.UseProgram(glw.p);
            aof.Uniform1i(glw.u_tex, sm ? GLOW_SMOOTH_UNIT : 0);
            aof.Uniform1i(glw.u_dep, GLOW_DEPTH_UNIT);
            aof.Uniform2f(glw.u_full, (GLfloat)w, (GLfloat)h);
            aof.Uniform1f(glw.u_far, s->range[1] - 1e-5f);
            aof.Uniform1f(glw.u_rad, GLOW_WRAP * (GLfloat)h);
            aof.Uniform1f(glw.u_gain, GLOW_GAIN);
            static int logged;
            if (!logged++)
                gmv_log("[sunmoon] glows hidden behind the world with a light wrap of %.0f px at %dx%d, %.1fx bright\n",
                        GLOW_WRAP * h, w, h, GLOW_GAIN);
            s->mode = 1;
            return;
        }
        glw.ready = -1;
        aof.ActiveTexture(0x84C0 + GLOW_DEPTH_UNIT);
        glBindTexture(GL_TEXTURE_2D, (GLuint)s->tex);
        aof.ActiveTexture((GLenum)s->act);
    }
    s->test = glIsEnabled(GL_DEPTH_TEST);
    glGetIntegerv(GL_DEPTH_FUNC, &s->func);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthRange(s->range[1], s->range[1]);
    s->mode = 2;
}

void glow_end(const glow_state_t *s)
{
    if (s->mode == 1) {
        aof.UseProgram((GLuint)s->prog);
        aof.ActiveTexture(0x84C0 + GLOW_DEPTH_UNIT);
        glBindTexture(GL_TEXTURE_2D, (GLuint)s->tex);
        aof.ActiveTexture(0x84C0 + GLOW_SMOOTH_UNIT);
        glBindTexture(GL_TEXTURE_2D, (GLuint)s->tex2);
        aof.ActiveTexture((GLenum)s->act);
    } else if (s->mode == 2) {
        glDepthRange(s->range[0], s->range[1]);
        glDepthFunc(s->func);
        if (!s->test)
            glDisable(GL_DEPTH_TEST);
    }
}

/* The projection and fog trackers; they only note what changed. */
static void ao_MatrixMode(GLenum mode)
{
    static void(APIENTRY * real)(GLenum);
    if (!real) real = gl_real("glMatrixMode");
    g_ao_mode = mode;
    if (real) real(mode);
}
static void ao_LoadMatrixf(const GLfloat *m)
{
    static void(APIENTRY * real)(const GLfloat *);
    if (!real) real = gl_real("glLoadMatrixf");
    if (g_ao_mode == GL_PROJECTION) g_ao_proj_dirty = 1;
    if (real) real(m);
}
static void ao_MultMatrixf(const GLfloat *m)
{
    static void(APIENTRY * real)(const GLfloat *);
    if (!real) real = gl_real("glMultMatrixf");
    if (g_ao_mode == GL_PROJECTION) g_ao_proj_dirty = 1;
    if (real) real(m);
}
static void ao_LoadIdentity(void)
{
    static void(APIENTRY * real)(void);
    if (!real) real = gl_real("glLoadIdentity");
    if (g_ao_mode == GL_PROJECTION) g_ao_proj_dirty = 1;
    if (real) real();
}
static void ao_PopMatrix(void)
{
    static void(APIENTRY * real)(void);
    if (!real) real = gl_real("glPopMatrix");
    if (g_ao_mode == GL_PROJECTION) g_ao_proj_dirty = 1;
    if (real) real();
}
static void ao_Ortho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    static void(APIENTRY * real)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
    if (!real) real = gl_real("glOrtho");
    if (g_ao_mode == GL_PROJECTION) g_ao_proj_dirty = 1;
    if (real) real(l, r, b, t, n, f);
}
static void ao_DepthRange(GLclampd n, GLclampd f)
{
    static void(APIENTRY * real)(GLclampd, GLclampd);
    if (!real) real = gl_real("glDepthRange");
    g_ao_dr[0] = (GLfloat)(n < 0 ? 0 : n > 1 ? 1 : n);
    g_ao_dr[1] = (GLfloat)(f < 0 ? 0 : f > 1 ? 1 : f);
    g_ao_cur = -1;
    if (real) real(n, f);
}
static void ao_DepthMask(GLboolean on)
{
    static void(APIENTRY * real)(GLboolean);
    if (!real) real = gl_real("glDepthMask");
    g_ao_dmask = on != GL_FALSE;
    if (real) real(on);
}
static void ao_fog_note(GLenum pname, GLfloat v)
{
    switch (pname) {
    case GL_FOG_MODE: g_ao_fogp[0] = v; break;
    case GL_FOG_START: g_ao_fogp[1] = v; break;
    case GL_FOG_END: g_ao_fogp[2] = v; break;
    case GL_FOG_DENSITY: g_ao_fogp[3] = v; break;
    }
}
static void ao_Fogf(GLenum pname, GLfloat v)
{
    static void(APIENTRY * real)(GLenum, GLfloat);
    if (!real) real = gl_real("glFogf");
    ao_fog_note(pname, v);
    if (real) real(pname, v);
}
static void ao_Fogfv(GLenum pname, const GLfloat *v)
{
    static void(APIENTRY * real)(GLenum, const GLfloat *);
    if (!real) real = gl_real("glFogfv");
    if (v) ao_fog_note(pname, v[0]);
    if (v && pname == GL_FOG_COLOR) {
        memcpy(g_fog_game, v, sizeof g_fog_game);
        g_fog_game_set = 1;
        if (g_fog_all && sky.n)
            v = sky.hz;                                     /* the sky's colour, not the game's */
        memcpy(g_fog_col, v, sizeof g_fog_col);
    }
    if (real) real(pname, v);
}
static void ao_BlendFunc(GLenum s, GLenum d)
{
    static void(APIENTRY * real)(GLenum, GLenum);
    if (!real) real = gl_real("glBlendFunc");
    g_blend_src = s;
    g_blend_dst = d;
    if (real) real(s, d);
}

void *gl_ao_intercept(const char *name)
{
    static const struct { const char *n; void *f; } t[] = {
        {"_glMatrixMode", (void *)ao_MatrixMode},
        {"_glLoadMatrixf", (void *)ao_LoadMatrixf},
        {"_glMultMatrixf", (void *)ao_MultMatrixf},
        {"_glLoadIdentity", (void *)ao_LoadIdentity},
        {"_glPopMatrix", (void *)ao_PopMatrix},
        {"_glOrtho", (void *)ao_Ortho},
        {"_glFogf", (void *)ao_Fogf},
        {"_glFogfv", (void *)ao_Fogfv},
        {"_glBlendFunc", (void *)ao_BlendFunc},
        {"_glDepthRange", (void *)ao_DepthRange},
        {"_glDepthMask", (void *)ao_DepthMask},
        {NULL, NULL},
    };
    for (int i = 0; t[i].n; i++)
        if (!strcmp(t[i].n, name))
            return t[i].f;
    return NULL;
}
