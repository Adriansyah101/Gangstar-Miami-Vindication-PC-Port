/* gl_draw.c -- the draw wrappers and TEXFIX */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"
#include <GL/gl.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "glshim.h"

/* ---- untextured draws with a stale texture (NOTES item 8: the death fade never turned black) ----
   glitch's setTexture(stage, NULL) only records the NULL and never unbinds, and a material without
   texture coordinates can leave GL_TEXTURE_2D enabled on unit 0 with whatever the previous 2D batch
   bound (the HUD atlas, texture 2).  For the texcoord array it disables, the driver sets the current
   texcoord to (0,0,0,0) via glMultiTexCoord4f, so the fixed pipeline samples at s/q = 0/0: undefined.
   On the Mac the full-screen black rectangles (draw2DRectangle: the death/respawn fade 0x1a5cea, the
   generic fade 0x1b5094, the cutscene fader) came out black; here the sample is a transparent texel
   and MODULATE turns the black into alpha 0, so the fade draws but changes nothing.  A draw with
   no texture coordinates cannot mean to show a texture, so for the duration of such a draw every
   unit that is enabled without a texcoord array is disabled, leaving plain vertex colour (= a white
   opaque sample under MODULATE).  Point sprites (GL_POINTS) generate their own coordinates and are
   left alone.  GMV_TEXFIX=0 turns this off for comparison. */
int g_texfix = 1;
unsigned g_tf_active, g_tf_client;               /* glActiveTexture / glClientActiveTexture unit */
uint8_t g_tf_tex2d, g_tf_tcarr;                  /* per-unit bits: GL_TEXTURE_2D on, texcoord array on */
static unsigned g_tf_fixed;                             /* draws patched (for the fade trace) */

void(APIENTRY *tf_active_real(void))(GLenum)
{
    static void(APIENTRY * real)(GLenum);
    if (!real) { real = gl_real("glActiveTexture"); if (!real) real = gl_real("glActiveTextureARB"); }
    return real;
}
static void tr_ActiveTexture(GLenum unit)
{
    void(APIENTRY * real)(GLenum) = tf_active_real();
    g_tf_active = (unit - 0x84C0) & 7;                  /* GL_TEXTURE0 */
    if (real) real(unit);
}
static void tr_ClientActiveTexture(GLenum unit)
{
    static void(APIENTRY * real)(GLenum);
    if (!real) { real = gl_real("glClientActiveTexture"); if (!real) real = gl_real("glClientActiveTextureARB"); }
    g_tf_client = (unit - 0x84C0) & 7;
    gmv_game_gl_client_unit(g_tf_client);
    if (real) real(unit);
}
static void tr_Enable(GLenum cap)
{
    static void(APIENTRY * real)(GLenum);
    if (!real) real = gl_real("glEnable");
    if (cap == GL_TEXTURE_2D) g_tf_tex2d |= (uint8_t)(1u << g_tf_active);
    gmv_game_gl_cap(cap, 1);
    if (real) real(cap);
}
static void tr_Disable(GLenum cap)
{
    static void(APIENTRY * real)(GLenum);
    if (!real) real = gl_real("glDisable");
    if (cap == GL_TEXTURE_2D) g_tf_tex2d &= (uint8_t)~(1u << g_tf_active);
    gmv_game_gl_cap(cap, 0);
    if (real) real(cap);
}
static void tr_EnableClientState(GLenum a)
{
    static void(APIENTRY * real)(GLenum);
    if (!real) real = gl_real("glEnableClientState");
    if (a == GL_TEXTURE_COORD_ARRAY) g_tf_tcarr |= (uint8_t)(1u << g_tf_client);
    gmv_game_gl_client_state(a, 1);
    if (real) real(a);
}
static void tr_DisableClientState(GLenum a)
{
    static void(APIENTRY * real)(GLenum);
    if (!real) real = gl_real("glDisableClientState");
    if (a == GL_TEXTURE_COORD_ARRAY) g_tf_tcarr &= (uint8_t)~(1u << g_tf_client);
    gmv_game_gl_client_state(a, 0);
    if (real) real(a);
}
/* Each unit's texture environment as the game set it, so the hybrid fog (FOG_MODE=2) can pick the
   shader that combines the textures as it does without asking GL at every draw.  Starts at GL's
   defaults: MODULATE, and for COMBINE: sources texture / previous / constant, colour operands. */
#define TE_DEFAULT {GL_MODULATE, GL_MODULATE, GL_MODULATE, {0x1702, 0x8578, 0x8576, 0x1702, 0x8578, 0x8576}, \
                    {0x300, 0x300, 0x302, 0x302, 0x302, 0x302}, {1, 1}}
g_te_t g_te[4] = {TE_DEFAULT, TE_DEFAULT, TE_DEFAULT, TE_DEFAULT};

static void te_note(GLenum target, GLenum pname, GLfloat v)
{
    if (target != 0x2300 || g_tf_active >= 4)           /* GL_TEXTURE_ENV */
        return;
    GLint i = (GLint)v;
    if (pname == 0x2200) g_te[g_tf_active].mode = i;                    /* GL_TEXTURE_ENV_MODE */
    else if (pname == 0x8571) g_te[g_tf_active].crgb = i;               /* GL_COMBINE_RGB */
    else if (pname == 0x8572) g_te[g_tf_active].calpha = i;             /* GL_COMBINE_ALPHA */
    else if (pname >= 0x8580 && pname <= 0x8582) g_te[g_tf_active].src[pname - 0x8580] = i;      /* GL_SRCn_RGB */
    else if (pname >= 0x8588 && pname <= 0x858A) g_te[g_tf_active].src[pname - 0x8588 + 3] = i;  /* GL_SRCn_ALPHA */
    else if (pname >= 0x8590 && pname <= 0x8592) g_te[g_tf_active].op[pname - 0x8590] = i;       /* GL_OPERANDn_RGB */
    else if (pname >= 0x8598 && pname <= 0x859A) g_te[g_tf_active].op[pname - 0x8598 + 3] = i;   /* GL_OPERANDn_ALPHA */
    else if (pname == 0x8573) g_te[g_tf_active].scale[0] = v;           /* GL_RGB_SCALE */
    else if (pname == 0x0D1C) g_te[g_tf_active].scale[1] = v;           /* GL_ALPHA_SCALE */
}
static void tr_TexEnvi(GLenum target, GLenum pname, GLint v)
{
    static void(APIENTRY * real)(GLenum, GLenum, GLint);
    if (!real) real = gl_real("glTexEnvi");
    te_note(target, pname, (GLfloat)v);
    if (real) real(target, pname, v);
}
static void tr_TexEnvf(GLenum target, GLenum pname, GLfloat v)
{
    static void(APIENTRY * real)(GLenum, GLenum, GLfloat);
    if (!real) real = gl_real("glTexEnvf");
    te_note(target, pname, v);
    if (real) real(target, pname, v);
}
static void tr_TexEnviv(GLenum target, GLenum pname, const GLint *v)
{
    static void(APIENTRY * real)(GLenum, GLenum, const GLint *);
    if (!real) real = gl_real("glTexEnviv");
    if (v) te_note(target, pname, (GLfloat)v[0]);
    if (real) real(target, pname, v);
}
static void tr_TexEnvfv(GLenum target, GLenum pname, const GLfloat *v)
{
    static void(APIENTRY * real)(GLenum, GLenum, const GLfloat *);
    if (!real) real = gl_real("glTexEnvfv");
    if (v) te_note(target, pname, v[0]);
    if (real) real(target, pname, v);
}

/* Before a draw: disable the units that would sample without coordinates; returns their bits. */
unsigned texfix_begin(GLenum mode)
{
    unsigned m = g_texfix && mode != GL_POINTS ? (unsigned)(g_tf_tex2d & ~g_tf_tcarr) & 0xF : 0;
    void(APIENTRY * act)(GLenum) = tf_active_real();
    if (!m || !act)
        return 0;
    for (unsigned u = 0; u < 4; u++)
        if (m & (1u << u)) { act(0x84C0 + u); glDisable(GL_TEXTURE_2D); }
    act(0x84C0 + g_tf_active);
    g_tf_fixed++;
    return m;
}
void texfix_end(unsigned m)
{
    void(APIENTRY * act)(GLenum) = tf_active_real();
    if (!m || !act)
        return;
    for (unsigned u = 0; u < 4; u++)
        if (m & (1u << u)) { act(0x84C0 + u); glEnable(GL_TEXTURE_2D); }
    act(0x84C0 + g_tf_active);
}

static void tr_DrawArrays(GLenum mode, GLint first, GLsizei count)
{
    static void(APIENTRY * real)(GLenum, GLint, GLsizei);
    if (!real) real = gl_real("glDrawArrays");
    g_glt.da_calls++; g_glt.da_verts += count;
    PM_DRAW();
    g_cap_draws++;
    if (g_cap_ticks >= 0) g_cap_run++;
    if (g_fade_pending) fade_state_log("arrays", mode, count);
    g_drew_since_swap = 1;
    gmv_game_draw_arrays((void *)real, mode, first, count, __builtin_return_address(0));
    post_fade_probe("drawarrays mode/count", mode, (unsigned)count);
}
#if GMV_DEV
/* The fade rectangle itself: the first 6-index glDrawElements after a fade call.  Dumps its
   vertices (position / colour / texcoord as the client arrays describe them), the texture units
   that are enabled and what is bound on them, the bound texture's size and a few texels, and the GL
   error state, so a fade that draws but does not darken can be compared with one that does (the
   cutscene fader).  Called before the draw. */
static void fade_quad_dump(GLsizei count, GLenum type, const void *idx)
{
    static struct { uint32_t caller; unsigned n; } seen[8];   /* a few lines per caller */
    uint32_t caller = g_quad_probe;
    if (count != 6)
        return;
    g_quad_probe = 0;
    unsigned *n = &seen[7].n;
    for (int i = 0; i < 8; i++)
        if (seen[i].caller == caller || !seen[i].caller) {
            seen[i].caller = caller;
            n = &seen[i].n;
            break;
        }
    if ((*n)++ >= 4)
        return;
    g_corner_watch = 1;
    GLenum err_before = glGetError();
    void(APIENTRY * active_tex)(GLenum) = gl_real("glActiveTexture");
    if (!active_tex) active_tex = gl_real("glActiveTextureARB");
    GLint act = 0, cact = 0, ebo = 0, abo = 0;
    glGetIntegerv(0x84E0, &act);                        /* GL_ACTIVE_TEXTURE */
    glGetIntegerv(0x84E1, &cact);                       /* GL_CLIENT_ACTIVE_TEXTURE */
    glGetIntegerv(0x8895, &ebo);                        /* GL_ELEMENT_ARRAY_BUFFER_BINDING */
    glGetIntegerv(0x8894, &abo);                        /* GL_ARRAY_BUFFER_BINDING */
    char units[160] = "";
    if (active_tex) {
        size_t ul = 0;
        for (int u = 0; u < 4; u++) {
            active_tex(0x84C0 + u);                     /* GL_TEXTURE0 + u */
            GLint t = 0, env = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &t);
            glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &env);
            ul += snprintf(units + ul, sizeof units - ul, " u%d:%s tex=%d env=0x%x", u,
                           glIsEnabled(GL_TEXTURE_2D) ? "on" : "off", t, env);
        }
        active_tex(act);
    }
    GLint tw = 0, th = 0, tif = 0, tmin = 0, tex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &tif);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &tmin);
    gmv_log("[fadequad] caller=0x%06x err_before=0x%x active=0x%x clientactive=0x%x ebo=%d abo=%d%s | bound tex=%d %dx%d ifmt=0x%x min=0x%x\n",
            caller, err_before, act, cact, ebo, abo, units, tex, tw, th, tif, tmin);
    GLfloat tc[4] = {0, 0, 0, 0};
    glGetFloatv(0x0B03, tc);                            /* GL_CURRENT_TEXTURE_COORDS (active unit) */
    gmv_log("[fadequad]   current texcoord=%.3f,%.3f,%.3f,%.3f  texfix=%s tex2d_bits=0x%x tcarray_bits=0x%x patched_draws=%u\n",
            tc[0], tc[1], tc[2], tc[3], g_texfix ? "on" : "off", g_tf_tex2d, g_tf_tcarr, g_tf_fixed);
    /* client arrays */
    struct { GLenum en, sz, ty, st, ptr; const char *nm; } arr[3] = {
        {GL_VERTEX_ARRAY, GL_VERTEX_ARRAY_SIZE, GL_VERTEX_ARRAY_TYPE, GL_VERTEX_ARRAY_STRIDE, GL_VERTEX_ARRAY_POINTER, "pos"},
        {GL_COLOR_ARRAY, GL_COLOR_ARRAY_SIZE, GL_COLOR_ARRAY_TYPE, GL_COLOR_ARRAY_STRIDE, GL_COLOR_ARRAY_POINTER, "col"},
        {GL_TEXTURE_COORD_ARRAY, GL_TEXTURE_COORD_ARRAY_SIZE, GL_TEXTURE_COORD_ARRAY_TYPE, GL_TEXTURE_COORD_ARRAY_STRIDE,
         GL_TEXTURE_COORD_ARRAY_POINTER, "uv"},
    };
    GLint asz[3], aty[3], ast[3];
    const unsigned char *aptr[3];
    for (int a = 0; a < 3; a++) {
        void *p = NULL;
        glGetIntegerv(arr[a].sz, &asz[a]);
        glGetIntegerv(arr[a].ty, &aty[a]);
        glGetIntegerv(arr[a].st, &ast[a]);
        glGetPointerv(arr[a].ptr, &p);
        aptr[a] = glIsEnabled(arr[a].en) ? p : NULL;
        gmv_log("[fadequad]   %s enabled=%d size=%d type=0x%x stride=%d ptr=%p\n", arr[a].nm,
                glIsEnabled(arr[a].en), asz[a], aty[a], ast[a], p);
    }
    GLfloat pm[16], mv[16];
    glGetFloatv(GL_PROJECTION_MATRIX, pm);
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    gmv_log("[fadequad]   projection x=%.6f,%.6f,%.6f,%.6f y=%.6f,%.6f,%.6f,%.6f (scale, translate in rows)\n",
            pm[0], pm[4], pm[8], pm[12], pm[1], pm[5], pm[9], pm[13]);
    gmv_log("[fadequad]   modelview  x=%.6f,%.6f,%.6f,%.6f y=%.6f,%.6f,%.6f,%.6f\n",
            mv[0], mv[4], mv[8], mv[12], mv[1], mv[5], mv[9], mv[13]);
    GLint qvp[4] = {0}, qsc[4] = {0}, pmode[2] = {0}, cullm = 0;
    glGetIntegerv(GL_VIEWPORT, qvp);
    glGetIntegerv(GL_SCISSOR_BOX, qsc);
    glGetIntegerv(GL_POLYGON_MODE, pmode);
    glGetIntegerv(GL_CULL_FACE_MODE, &cullm);
    gmv_log("[fadequad]   viewport %d,%d,%d,%d scissor=%d %d,%d,%d,%d polygon mode 0x%x cull=%d 0x%x smooth=%d stipple=%d\n",
            qvp[0], qvp[1], qvp[2], qvp[3], glIsEnabled(GL_SCISSOR_TEST), qsc[0], qsc[1], qsc[2], qsc[3], pmode[0],
            glIsEnabled(GL_CULL_FACE), cullm, glIsEnabled(GL_POLYGON_SMOOTH), glIsEnabled(GL_POLYGON_STIPPLE));
    if (ebo || abo || !idx || IsBadReadPtr(idx, 12))
        return;                                         /* buffer objects: pointers are offsets */
    float u0 = -1, v0 = -1;
    for (int k = 0; k < 6; k++) {
        unsigned vi = type == GL_UNSIGNED_SHORT ? ((const uint16_t *)idx)[k]
                    : type == GL_UNSIGNED_BYTE ? ((const uint8_t *)idx)[k] : ((const uint32_t *)idx)[k];
        char line[200];
        size_t ll = snprintf(line, sizeof line, "idx%d=%u", k, vi);
        for (int a = 0; a < 3; a++) {
            if (!aptr[a])
                continue;
            int esz = aty[a] == GL_FLOAT ? 4 : aty[a] == GL_SHORT || aty[a] == GL_UNSIGNED_SHORT ? 2 : 1;
            int ncomp = asz[a] == 0x80E1 ? 4 : asz[a];  /* GL_BGRA as size */
            int stride = ast[a] ? ast[a] : esz * ncomp;
            const unsigned char *p = aptr[a] + (size_t)vi * stride;
            if (IsBadReadPtr(p, esz * ncomp))
                continue;
            ll += snprintf(line + ll, sizeof line - ll, " %s=", arr[a].nm);
            for (int c = 0; c < ncomp && ll < sizeof line - 16; c++) {
                if (aty[a] == GL_FLOAT)
                    ll += snprintf(line + ll, sizeof line - ll, "%s%.3f", c ? "," : "", ((const float *)p)[c]);
                else if (esz == 2)
                    ll += snprintf(line + ll, sizeof line - ll, "%s%d", c ? "," : "", ((const int16_t *)p)[c]);
                else
                    ll += snprintf(line + ll, sizeof line - ll, "%s%u", c ? "," : "", p[c]);
            }
            if (a == 2 && aty[a] == GL_FLOAT && k == 0) { u0 = ((const float *)p)[0]; v0 = ((const float *)p)[1]; }
        }
        gmv_log("[fadequad]   %s\n", line);
    }
    /* texel of the bound texture under vertex 0's texcoord */
    if (tex && tw > 0 && th > 0 && (long)tw * th <= 4096L * 4096L) {
        unsigned char *px = malloc((size_t)tw * th * 4);
        if (px) {
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
            int x = u0 < 0 ? 0 : (int)(u0 * tw), y = v0 < 0 ? 0 : (int)(v0 * th);
            if (x >= tw) x = tw - 1;
            if (y >= th) y = th - 1;
            const unsigned char *t = px + ((size_t)y * tw + x) * 4;
            gmv_log("[fadequad]   texel at uv0 (%d,%d) = %u,%u,%u,%u; texel(0,0) = %u,%u,%u,%u\n", x, y, t[0], t[1],
                    t[2], t[3], px[0], px[1], px[2], px[3]);
            free(px);
        }
    }
}
#endif

static void tr_DrawElements(GLenum mode, GLsizei count, GLenum type, const void *idx)
{
    static void(APIENTRY * real)(GLenum, GLsizei, GLenum, const void *);
    if (!real) real = gl_real("glDrawElements");
    g_glt.de_calls++; g_glt.de_verts += count;
    PM_DRAW();
    g_cap_draws++;
    if (g_cap_ticks >= 0) g_cap_run++;
    if (g_fade_pending) fade_state_log("elements", mode, count);
#if GMV_DEV
    if (g_quad_probe) fade_quad_dump(count, type, idx);
#endif
    g_drew_since_swap = 1;
    if (g_gl_trace) {
        static int logged;
        if (logged < 2) {
            logged++;
            gmv_log("[gltrace] glDrawElements mode=0x%x count=%d type=0x%x caller=%p\n",
                    mode, count, type, __builtin_return_address(0));
            const uint32_t *sp = (const uint32_t *)&mode;   /* scan the stack for the game call chain */
            for (int k = 0; k < 64; k++)
                if (sp[k] >= GMV_TEXT_LO && sp[k] < GMV_TEXT_HI)
                    gmv_log("        [sp+%02x] -> game 0x%08x\n", k * 4, sp[k]);
        }
    }
    gmv_game_draw_elements((void *)real, mode, count, type, idx, __builtin_return_address(0));
#if GMV_DEV
    if (g_corner_watch == 1) {
        fade_corners("after the fade");
        g_corner_watch = 2;
    }
#endif
    post_fade_probe("drawelements mode/count", mode, (unsigned)count);
}
static void tr_BindFramebuffer(GLenum target, GLuint fb)
{
    static void(APIENTRY * real)(GLenum, GLuint);
    if (!real) { real = gl_real("glBindFramebuffer"); if (!real) real = gl_real("glBindFramebufferEXT"); }
    g_glt.last_fb = fb;
#if GMV_DEV
    {
        char b[16];
        snprintf(b, sizeof b, "B%u ", fb);
        cap_seq(b);
    }
#endif
    if (real) real(target, fb ? fb : g_ms_fbo);         /* the screen is the MSAA framebuffer when on */
}
static void tr_BufferData(GLenum target, intptr_t size, const void *data, GLenum usage)
{
    static void(APIENTRY * real)(GLenum, intptr_t, const void *, GLenum);
    if (!real) real = gl_real("glBufferData");
    if ((long)size > g_glt.max_buf) g_glt.max_buf = (long)size;
    if (real) real(target, size, data, usage);
}
static void tr_BindTexture(GLenum target, GLuint tex)
{
    static void(APIENTRY * real)(GLenum, GLuint);
    if (!real) real = gl_real("glBindTexture");
    g_glt.tex_binds++;
    if (real) real(target, tex);
}

void *gl_trace_intercept(const char *name)
{
    /* name still has the leading underscore here (e.g. "_glClear") */
    void *gs = gmv_game_gl_draw_intercept(name);
    if (gs)
        return gs;
    static const struct { const char *n; void *f; } t[] = {
        {"_glClearColor", (void *)tr_ClearColor},
        {"_glClear", (void *)tr_Clear},
        {"_glViewport", (void *)tr_Viewport},
        {"_glDrawArrays", (void *)tr_DrawArrays},
        {"_glDrawElements", (void *)tr_DrawElements},
        {"_glBindFramebuffer", (void *)tr_BindFramebuffer},
        {"_glBindFramebufferEXT", (void *)tr_BindFramebuffer},
        {"_glBufferData", (void *)tr_BufferData},
        {"_glBufferDataARB", (void *)tr_BufferData},
        {"_glBindTexture", (void *)tr_BindTexture},
        {"_glActiveTexture", (void *)tr_ActiveTexture},
        {"_glActiveTextureARB", (void *)tr_ActiveTexture},
        {"_glClientActiveTexture", (void *)tr_ClientActiveTexture},
        {"_glClientActiveTextureARB", (void *)tr_ClientActiveTexture},
        {"_glEnable", (void *)tr_Enable},
        {"_glDisable", (void *)tr_Disable},
        {"_glEnableClientState", (void *)tr_EnableClientState},
        {"_glDisableClientState", (void *)tr_DisableClientState},
        {"_glTexEnvi", (void *)tr_TexEnvi},
        {"_glTexEnvf", (void *)tr_TexEnvf},
        {"_glTexEnviv", (void *)tr_TexEnviv},
        {"_glTexEnvfv", (void *)tr_TexEnvfv},
        {NULL, NULL},
    };
    for (int i = 0; t[i].n; i++)
        if (!strcmp(t[i].n, name))
            return t[i].f;
    return NULL;
}

#if GMV_DEV
/* GMV_LOAD_PROFILE (loadprof.c): time the texture and buffer uploads.  Any single upload over
   10 ms is also logged with its size and caller (the first 40). */
static long long tex_bytes(GLsizei w, GLsizei h, GLenum format, GLenum type)
{
    int bpp;
    switch (type) {
    case 0x8363: case 0x8033: case 0x8034: bpp = 2; break;    /* 5_6_5, 4_4_4_4, 5_5_5_1 */
    case GL_FLOAT: bpp = 16; break;
    default:
        switch (format) {
        case GL_RGBA: case 0x80E1: bpp = 4; break;              /* RGBA, BGRA */
        case GL_RGB: case GL_BGR_EXT: bpp = 3; break;
        case GL_LUMINANCE_ALPHA: bpp = 2; break;
        default: bpp = 1; break;
        }
    }
    return (long long)w * h * bpp;
}

static void pf_done(int kind, long long t0, long long bytes, const char *what, GLsizei w, GLsizei h,
                    GLenum fmt, void *caller)
{
    (void)kind;                         /* without the profiler gmv_prof_add is empty */
    gmv_prof_add(kind, t0, bytes);
    static int logged;
    static LARGE_INTEGER freq;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    double ms = (double)(gmv_prof_now() - t0) * 1000.0 / (double)freq.QuadPart;
    if (ms >= 10.0 && logged < 40) {
        logged++;
        gmv_log("[loadprof] slow %s %dx%d fmt 0x%x %lld KB: %.1f ms (caller %p)\n", what, w, h, fmt,
                bytes >> 10, ms, caller);
    }
}

static void pf_TexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border,
                          GLenum format, GLenum type, const void *px)
{
    static void(APIENTRY * real)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                                 const void *);
    if (!real) real = gl_real("glTexImage2D");
    long long t0 = gmv_prof_now();
    if (real) real(target, level, ifmt, w, h, border, format, type, px);
    pf_done(GMV_PROF_TEX, t0, px ? tex_bytes(w, h, format, type) : 0, "glTexImage2D", w, h,
            (GLenum)ifmt, __builtin_return_address(0));
}
static void pf_TexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                             GLenum format, GLenum type, const void *px)
{
    static void(APIENTRY * real)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                                 const void *);
    if (!real) real = gl_real("glTexSubImage2D");
    long long t0 = gmv_prof_now();
    if (real) real(target, level, x, y, w, h, format, type, px);
    pf_done(GMV_PROF_TEX, t0, tex_bytes(w, h, format, type), "glTexSubImage2D", w, h, format,
            __builtin_return_address(0));
}
void pf_CompressedTexImage2D(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h,
                                    GLint border, GLsizei size, const void *data)
{
    static void(APIENTRY * real)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
    if (!real) real = gl_real("glCompressedTexImage2D");
    long long t0 = gmv_prof_now();
    if (real) real(target, level, ifmt, w, h, border, size, data);
    pf_done(GMV_PROF_TEX, t0, size, "glCompressedTexImage2D", w, h, ifmt, __builtin_return_address(0));
}
static void pf_CompressedTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w,
                                       GLsizei h, GLenum format, GLsizei size, const void *data)
{
    static void(APIENTRY * real)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei,
                                 const void *);
    if (!real) real = gl_real("glCompressedTexSubImage2D");
    long long t0 = gmv_prof_now();
    if (real) real(target, level, x, y, w, h, format, size, data);
    pf_done(GMV_PROF_TEX, t0, size, "glCompressedTexSubImage2D", w, h, format,
            __builtin_return_address(0));
}
static void pf_GenerateMipmap(GLenum target)
{
    static void(APIENTRY * real)(GLenum);
    if (!real) real = gl_real("glGenerateMipmapEXT");
    long long t0 = gmv_prof_now();
    if (real) real(target);
    pf_done(GMV_PROF_TEX, t0, 0, "glGenerateMipmapEXT", 0, 0, 0, __builtin_return_address(0));
}
static void pf_BufferData(GLenum target, intptr_t size, const void *data, GLenum usage)
{
    long long t0 = gmv_prof_now();
    tr_BufferData(target, size, data, usage);
    pf_done(GMV_PROF_BUF, t0, data ? size : 0, "glBufferData", 0, 0, 0, __builtin_return_address(0));
}
static void pf_BufferSubData(GLenum target, intptr_t off, intptr_t size, const void *data)
{
    static void(APIENTRY * real)(GLenum, intptr_t, intptr_t, const void *);
    if (!real) real = gl_real("glBufferSubData");
    long long t0 = gmv_prof_now();
    if (real) real(target, off, size, data);
    pf_done(GMV_PROF_BUF, t0, size, "glBufferSubData", 0, 0, 0, __builtin_return_address(0));
}

void *gl_prof_intercept(const char *name)
{
    static const struct { const char *n; void *f; } t[] = {
        {"_glTexImage2D", (void *)pf_TexImage2D},
        {"_glTexSubImage2D", (void *)pf_TexSubImage2D},
        {"_glCompressedTexImage2D", (void *)pf_CompressedTexImage2D},
        {"_glCompressedTexSubImage2D", (void *)pf_CompressedTexSubImage2D},
        {"_glGenerateMipmapEXT", (void *)pf_GenerateMipmap},
        {"_glBufferData", (void *)pf_BufferData},
        {"_glBufferSubData", (void *)pf_BufferSubData},
        {NULL, NULL},
    };
    for (int i = 0; t[i].n; i++)
        if (!strcmp(t[i].n, name))
            return t[i].f;
    return NULL;
}
#endif
