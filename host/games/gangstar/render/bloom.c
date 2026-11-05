/* bloom.c -- BLOOM */
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

#define AO_MIN_DRAWS 4

/* ---- BLOOM: an enhancement, off by default and under ORIGINAL_MAC_EXPERIENCE ------------------------
   A glow around the bright parts of the 3D world (neon, lamps, headlights, the sun), applied where and
   when the ambient occlusion is (between the world and the 2D layer, once per frame, inside ao_run's
   state save) and after it.  The screen and its depth are resolved to textures; a bright pass with a
   soft knee keeps what is above BLOOM_THRESHOLD at half resolution, weighting each tap down by its
   brightness so single bright pixels do not flicker.  On the world the brightness is the brightest
   channel, so saturated neon counts as much as white; on the sky (at the far end of the depth range the
   most draws had, or beyond) it is the dimmest, so the sun and its glow bloom but a sunset or a blue sky,
   bright in one channel only, does not turn white;
   BL_LEVELS dual-filter downsamples and tent upsamples, each added onto the level above, spread it; and
   the sum is screen-blended into the frame (1 - (1 - frame)(1 - glow), so it brightens without clipping)
   at BLOOM_INTENSITY.  The bright pass also adds the glow layer (BLOOM_LIGHTS, BLOOM_WINDOWS; bg_draw_begin),
   resolved to a texture: what glows whatever its brightness.  BLOOM=1 (LOW) is the bloom alone, BLOOM=2
   (HIGH) adds the glow layer. */
int g_bloom_want;                                /* BLOOM=1 or 2 */
static int g_bloom = 1;                                 /* 0 once the setup failed */
float g_bloom_intensity = 1.0f, g_bloom_threshold = 0.75f;
float g_bloom_lights, g_bloom_windows;           /* BLOOM_LIGHTS, BLOOM_WINDOWS at BLOOM=2; 0 = off */
int g_bg_want;                                   /* either of them, with BLOOM */

/* BLOOM_LIGHTS or BLOOM_WINDOWS (blank = 1) at BLOOM=2, 0 below it. */
float bloom_part(int level, const char *env)
{
    const char *s = gmv_getenv(env);
    float k = s && *s ? (float)atof(s) : 1.0f;
    return level >= 2 && k > 0 ? k : 0;
}

static const char bl_fs_pre[] =
    "#version 120\n"
    "uniform sampler2D src, dep, glw;\n"
    "uniform vec2 dst, tx;\n"
    "uniform vec3 curve;\n"                             /* threshold, knee, 1 / (1 - threshold) */
    "uniform float sky, gg;\n"                          /* depth from which a pixel is sky; the glow layer's gain */
    "vec3 tap(vec2 uv, inout float ws) {\n"
    "    vec3 c = texture2D(src, uv).rgb;\n"
    "    float b = texture2D(dep, uv).r >= sky ? min(c.r, min(c.g, c.b)) : max(c.r, max(c.g, c.b));\n"
    "    float s = clamp(b - curve.x + curve.y, 0.0, 2.0 * curve.y);\n"
    "    s = s * s / (4.0 * curve.y + 1e-4);\n"
    "    c *= max(s, b - curve.x) * curve.z / max(b, 1e-4);\n"
    "    c += max(texture2D(glw, uv).rgb, 0.0) * gg;\n"
    "    float w = 1.0 / (1.0 + max(c.r, max(c.g, c.b)));\n"
    "    ws += w;\n"
    "    return c * w;\n"
    "}\n"
    "void main() {\n"
    "    vec2 uv = gl_FragCoord.xy / dst;\n"
    "    float ws = 0.0;\n"
    "    vec3 c = tap(uv + tx * vec2(-1.0, -1.0), ws) + tap(uv + tx * vec2(1.0, -1.0), ws)\n"
    "           + tap(uv + tx * vec2(-1.0, 1.0), ws) + tap(uv + tx * vec2(1.0, 1.0), ws);\n"
    "    gl_FragColor = vec4(c / ws, 1.0);\n"
    "}\n";

static const char bl_fs_down[] =
    "#version 120\n"
    "uniform sampler2D src;\n"
    "uniform vec2 dst, tx;\n"
    "void main() {\n"
    "    vec2 uv = gl_FragCoord.xy / dst;\n"
    "    vec3 c = texture2D(src, uv).rgb * 4.0\n"
    "           + texture2D(src, uv + tx * vec2(-1.0, -1.0)).rgb + texture2D(src, uv + tx * vec2(1.0, -1.0)).rgb\n"
    "           + texture2D(src, uv + tx * vec2(-1.0, 1.0)).rgb + texture2D(src, uv + tx * vec2(1.0, 1.0)).rgb;\n"
    "    gl_FragColor = vec4(c / 8.0, 1.0);\n"
    "}\n";

/* A 3x3 tent of the smaller level, times k: the upsamples (k = 1, added onto the level above) and the
   composite (k = BLOOM_INTENSITY / BL_LEVELS, screen-blended, with a dither against banding). */
static const char bl_fs_up[] =
    "#version 120\n"
    "uniform sampler2D src;\n"
    "uniform vec2 dst, tx;\n"
    "uniform float k, dith;\n"
    "void main() {\n"
    "    vec2 uv = gl_FragCoord.xy / dst;\n"
    "    vec3 c = texture2D(src, uv).rgb * 4.0;\n"
    "    c += (texture2D(src, uv + vec2(tx.x, 0.0)).rgb + texture2D(src, uv - vec2(tx.x, 0.0)).rgb\n"
    "        + texture2D(src, uv + vec2(0.0, tx.y)).rgb + texture2D(src, uv - vec2(0.0, tx.y)).rgb) * 2.0;\n"
    "    c += texture2D(src, uv + tx).rgb + texture2D(src, uv - tx).rgb\n"
    "       + texture2D(src, uv + vec2(tx.x, -tx.y)).rgb + texture2D(src, uv + vec2(-tx.x, tx.y)).rgb;\n"
    "    c *= k / 16.0;\n"
    "    if (dith > 0.0) {\n"
    "        c = min(c, vec3(1.0));\n"
    "        c += (fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) - 0.5) * dith;\n"
    "        c = max(c, vec3(0.0));\n"
    "    }\n"
    "    gl_FragColor = vec4(c, 1.0);\n"
    "}\n";

bl_t bl;

static int bl_init(void)
{
    if (!ao_procs("bloom"))
        return 0;
    bl.p_pre = ao_program("bloom bright pass", ao_vs, bl_fs_pre);
    bl.p_down = bl.p_pre ? ao_program("bloom downsample", ao_vs, bl_fs_down) : 0;
    bl.p_up = bl.p_down ? ao_program("bloom upsample", ao_vs, bl_fs_up) : 0;
    if (!bl.p_up) {
        gmv_log("[bloom] bloom off\n");
        return 0;
    }
    glGenTextures(1, &bl.t_scene);
    glGenTextures(1, &bl.t_depth);
    glGenTextures(BL_LEVELS, bl.t);
    glGenTextures(1, &bl.t_glow);
    msf.GenFramebuffers(1, &bl.fbo_scene);
    msf.GenFramebuffers(1, &bl.fbo_depth);
    msf.GenFramebuffers(BL_LEVELS, bl.fbo);
    msf.GenFramebuffers(1, &bl.fbo_glow);
    return 1;
}

void bl_linear(void)
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

/* Size the textures to the screen and check the framebuffers; called inside ao_run's state save. */
static int bl_alloc(int w, int h)
{
    bl.w = w;
    bl.h = h;
    ao_tex(bl.t_scene, GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE);
    bl_linear();
    msf.BindFramebuffer(MS_FRAMEBUFFER, bl.fbo_scene);
    aof.FramebufferTexture2D(MS_FRAMEBUFFER, 0x8CE0, GL_TEXTURE_2D, bl.t_scene, 0);
    GLenum st = msf.CheckFramebufferStatus(MS_FRAMEBUFFER);
    ao_tex(bl.t_depth, 0x88F0, w, h, 0x84F9, 0x84FA);          /* DEPTH24_STENCIL8, DEPTH_STENCIL, 24_8 */
    glTexParameteri(GL_TEXTURE_2D, 0x884C, GL_NONE);            /* GL_TEXTURE_COMPARE_MODE */
    msf.BindFramebuffer(MS_FRAMEBUFFER, bl.fbo_depth);
    aof.FramebufferTexture2D(MS_FRAMEBUFFER, 0x821A, GL_TEXTURE_2D, bl.t_depth, 0);   /* DEPTH_STENCIL_ATTACHMENT */
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    GLenum sd = msf.CheckFramebufferStatus(MS_FRAMEBUFFER);
    if (st == 0x8CD5)
        st = sd;
    if (g_bg_want && st == 0x8CD5) {
        ao_tex(bl.t_glow, 0x881A, w, h, GL_RGBA, GL_FLOAT);    /* RGBA16F, as the layer */
        bl_linear();
        msf.BindFramebuffer(MS_FRAMEBUFFER, bl.fbo_glow);
        aof.FramebufferTexture2D(MS_FRAMEBUFFER, 0x8CE0, GL_TEXTURE_2D, bl.t_glow, 0);
        st = msf.CheckFramebufferStatus(MS_FRAMEBUFFER);
    }
    bl.n = 0;
    for (int k = 0, lw = w, lh = h; k < BL_LEVELS && st == 0x8CD5; k++) {
        lw = (lw + 1) / 2;
        lh = (lh + 1) / 2;
        if (lw < 2 || lh < 2)
            break;
        bl.lw[k] = lw;
        bl.lh[k] = lh;
        ao_tex(bl.t[k], 0x881A, lw, lh, GL_RGBA, GL_FLOAT);    /* RGBA16F */
        bl_linear();
        msf.BindFramebuffer(MS_FRAMEBUFFER, bl.fbo[k]);
        aof.FramebufferTexture2D(MS_FRAMEBUFFER, 0x8CE0, GL_TEXTURE_2D, bl.t[k], 0);
        st = msf.CheckFramebufferStatus(MS_FRAMEBUFFER);
        bl.n = k + 1;
    }
    if (st != 0x8CD5 || bl.n < 2) {
        gmv_log("[bloom] framebuffer incomplete (0x%x) at %dx%d; bloom off\n", st, w, h);
        return 0;
    }
    return 1;
}

static void bl_size(GLuint p, int dw, int dh, int sw, int sh)
{
    aof.Uniform2f(aof.GetUniformLocation(p, "dst"), (GLfloat)dw, (GLfloat)dh);
    aof.Uniform2f(aof.GetUniformLocation(p, "tx"), 1.0f / sw, 1.0f / sh);
}

/* Inside ao_run's state save (depth test, blend, scissor and the rest off; unit 0 active).  `sky` is
   the depth from which a pixel counts as sky (above 1 = none). */
static void bl_pass(int w, int h, float sky)
{
    static int ready;
    if (!ready)
        ready = bl_init() ? 1 : -1;
    if (ready < 0 || ((w != bl.w || h != bl.h) && !bl_alloc(w, h))) {
        g_bloom = 0;
        return;
    }
    /* the frame as drawn so far, resolved if multisampled */
    msf.BindFramebuffer(MS_READ_FB, g_ms_fbo);
    msf.BindFramebuffer(MS_DRAW_FB, bl.fbo_scene);
    msf.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    msf.BindFramebuffer(MS_DRAW_FB, bl.fbo_depth);
    msf.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    int glow = g_ms_glow && g_glow_used && g_bg_want;   /* the glow layer was drawn this frame */
    if (glow) {
        glReadBuffer(0x8CE1);                           /* COLOR_ATTACHMENT1, of the screen FBO bound for reading */
        msf.BindFramebuffer(MS_DRAW_FB, bl.fbo_glow);
        msf.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glReadBuffer(0x8CE0);
    }
    glDisable(GL_BLEND);

    GLuint p = bl.p_pre;
    float t = g_bloom_threshold, knee = t * 0.5f + 1e-3f;
    aof.UseProgram(p);
    aof.Uniform1i(aof.GetUniformLocation(p, "src"), 0);
    aof.Uniform1i(aof.GetUniformLocation(p, "dep"), 1);
    aof.Uniform1i(aof.GetUniformLocation(p, "glw"), 2);
    aof.Uniform3fv(aof.GetUniformLocation(p, "curve"), 1, (const GLfloat[]){t, knee, 1.0f / (1.0f - t)});
    aof.Uniform1f(aof.GetUniformLocation(p, "sky"), sky);
    aof.Uniform1f(aof.GetUniformLocation(p, "gg"), glow ? bg_gain() : 0.0f);
    msf.BindFramebuffer(MS_FRAMEBUFFER, bl.fbo[0]);
    glViewport(0, 0, bl.lw[0], bl.lh[0]);
    aof.ActiveTexture(0x84C2);
    glBindTexture(GL_TEXTURE_2D, glow ? bl.t_glow : 0);
    aof.ActiveTexture(0x84C1);
    glBindTexture(GL_TEXTURE_2D, bl.t_depth);
    aof.ActiveTexture(0x84C0);
    glBindTexture(GL_TEXTURE_2D, bl.t_scene);
    bl_size(p, bl.lw[0], bl.lh[0], w, h);
    ao_tri();

    p = bl.p_down;
    aof.UseProgram(p);
    aof.Uniform1i(aof.GetUniformLocation(p, "src"), 0);
    for (int k = 1; k < bl.n; k++) {
        msf.BindFramebuffer(MS_FRAMEBUFFER, bl.fbo[k]);
        glViewport(0, 0, bl.lw[k], bl.lh[k]);
        glBindTexture(GL_TEXTURE_2D, bl.t[k - 1]);
        bl_size(p, bl.lw[k], bl.lh[k], bl.lw[k - 1], bl.lh[k - 1]);
        ao_tri();
    }

    p = bl.p_up;
    aof.UseProgram(p);
    aof.Uniform1i(aof.GetUniformLocation(p, "src"), 0);
    aof.Uniform1f(aof.GetUniformLocation(p, "k"), 1.0f);
    aof.Uniform1f(aof.GetUniformLocation(p, "dith"), 0.0f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    aof.BlendEquation(0x8006);                                  /* GL_FUNC_ADD */
    for (int k = bl.n - 1; k > 0; k--) {
        msf.BindFramebuffer(MS_FRAMEBUFFER, bl.fbo[k - 1]);
        glViewport(0, 0, bl.lw[k - 1], bl.lh[k - 1]);
        glBindTexture(GL_TEXTURE_2D, bl.t[k]);
        bl_size(p, bl.lw[k - 1], bl.lh[k - 1], bl.lw[k], bl.lh[k]);
        ao_tri();
    }

    msf.BindFramebuffer(MS_FRAMEBUFFER, g_ms_fbo);             /* the screen */
    glViewport(0, 0, w, h);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    aof.Uniform1f(aof.GetUniformLocation(p, "k"), g_bloom_intensity / bl.n);
    aof.Uniform1f(aof.GetUniformLocation(p, "dith"), 1.0f / 255);
    glBindTexture(GL_TEXTURE_2D, bl.t[0]);
    bl_size(p, w, h, bl.lw[0], bl.lh[0]);
    ao_tri();
    if (g_bloom_survey >= 2) {                                  /* the glow layer alone, on black */
        glDisable(GL_BLEND);
        aof.Uniform1f(aof.GetUniformLocation(p, "k"), glow ? bg_gain() : 0.0f);
        aof.Uniform1f(aof.GetUniformLocation(p, "dith"), 0.0f);
        glBindTexture(GL_TEXTURE_2D, bl.t_glow);
        bl_size(p, w, h, w, h);
        ao_tri();
    }

    static unsigned runs;
    if (runs++ % 1200 == 0)
        gmv_trace("[bloom] pass %u at %dx%d, %d levels from %dx%d; threshold %g, intensity %g\n", runs, w, h, bl.n,
                bl.lw[0], bl.lh[0], g_bloom_threshold, g_bloom_intensity);
}

static void ao_run(int why)
{
    static int ready;
    g_ao_done = 1;
    int do_ao = 0, li = 0;
    const GLfloat *scene = NULL, *dr = NULL, *fogs = NULL;
    const GLint *vp = NULL;
    GLfloat inv[16];
    for (int k = 1; k < g_ao_nlayers; k++)
        if (g_ao_layer[k].n > g_ao_layer[li].n)
            li = k;
    if (g_ao_want && g_ao) {
        if (!ready) {
            ready = ao_init() ? 1 : -1;
            if (ready < 0)
                g_ao = 0;
        }
        if (ready > 0 && g_ao_nlayers) {
            scene = g_ao_layer[li].m, dr = g_ao_layer[li].dr, fogs = g_ao_layer[li].fog;
            vp = g_ao_layer[li].vp;
            do_ao = ao_invert(scene, inv) && vp[2] > 0 && vp[3] > 0 && dr[1] > dr[0];
        }
    }
    int do_bloom = g_bloom_want && g_bloom;
    int do_ssr = g_ssr_want && g_ssr && g_ao_nlayers && ssr_frame();
    int do_vr = g_vr_want && g_vr && g_ao_nlayers && vr_frame();
    int do_ds = g_ds_want && g_ds && g_ao_nlayers && ds_frame();
    if (!do_ao && !do_bloom && !do_ssr && !do_vr && !do_ds)
        return;
    int w = g_ms_w, h = g_ms_h;
    if (!g_ms_fbo)
        gmv_gl_client_size(&w, &h);
    if (w <= 0 || h <= 0)
        return;

    GLint dfb = 0, rfb = 0, prog = 0, act = 0;
    glGetIntegerv(MS_DRAW_FB_BINDING, &dfb);
    glGetIntegerv(MS_READ_FB_BINDING, &rfb);
    glGetIntegerv(0x8B8D, &prog);                               /* GL_CURRENT_PROGRAM */
    glGetIntegerv(0x84E0, &act);                                /* GL_ACTIVE_TEXTURE */
    bg_single();                                                /* the AO and the bloom draw the screen alone */
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    static const GLenum off[] = {GL_DEPTH_TEST, GL_BLEND, GL_ALPHA_TEST, GL_CULL_FACE, GL_SCISSOR_TEST,
                                 GL_STENCIL_TEST, GL_POLYGON_OFFSET_FILL, GL_COLOR_LOGIC_OP, GL_FOG,
                                 0x80A0, 0x809E,                /* SAMPLE_COVERAGE, SAMPLE_ALPHA_TO_COVERAGE */
                                 0x3000, 0x3001, 0x3002, 0x3003, 0x3004, 0x3005};  /* CLIP_PLANE0-5 */
    for (size_t i = 0; i < sizeof off / sizeof *off; i++)
        glDisable(off[i]);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    aof.ActiveTexture(0x84C0);

    pm_mark(PM_POST);
    if (do_ds)                                                  /* first, so the reflections show the shadows */
        ds_pass(w, h, li);
    if (do_ssr)                                                 /* first, so the AO and the bloom see the reflections */
        ssr_pass(w, h, li);
    pm_mark(PM_SSR);
    if (do_vr)                                                  /* after the sea's, so the cars reflect it */
        vr_pass(w, h, li);
    pm_mark(PM_VR);
    int ok = 1;
    if (do_ao && (w != ao.w || h != ao.h)) {
        ao_alloc(w, h);
        ok = ao_attach();
    }
    if (do_ao && ok) {
        /* the depth, as drawn so far (sample 0 of each pixel when multisampled) */
        msf.BindFramebuffer(MS_READ_FB, g_ms_fbo);
        msf.BindFramebuffer(MS_DRAW_FB, ao.fbo_depth);
        msf.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);

        GLuint p = ao.p_ao;
        msf.BindFramebuffer(MS_FRAMEBUFFER, ao.fbo[0]);
        glViewport(0, 0, ao.hw, ao.hh);
        aof.UseProgram(p);
        glBindTexture(GL_TEXTURE_2D, ao.t_depth);
        aof.Uniform1i(aof.GetUniformLocation(p, "dep"), 0);
        aof.Uniform2f(aof.GetUniformLocation(p, "full"), (GLfloat)w, (GLfloat)h);
        aof.Uniform4f(aof.GetUniformLocation(p, "vp"), (GLfloat)vp[0], (GLfloat)vp[1], (GLfloat)vp[2], (GLfloat)vp[3]);
        aof.UniformMatrix4fv(aof.GetUniformLocation(p, "invp"), 1, GL_FALSE, inv);
        aof.Uniform2f(aof.GetUniformLocation(p, "dr"), dr[0], dr[1]);
        aof.Uniform1f(aof.GetUniformLocation(p, "pxs"), 0.5f * fabsf(scene[0]) * (GLfloat)vp[2]);
        aof.Uniform1f(aof.GetUniformLocation(p, "rad"), g_ao_radius);
        aof.Uniform1f(aof.GetUniformLocation(p, "str"), g_ao_strength);
        GLfloat fm = fogs[0];
        aof.Uniform4f(aof.GetUniformLocation(p, "fog"), fm == 0x2601 ? 1.0f : fm == GL_EXP ? 2.0f : fm == GL_EXP2 ? 3.0f : 0.0f,
                      fogs[1], fogs[2], fogs[3]);                  /* 0x2601 = GL_LINEAR */
        ao_tri();

        p = ao.p_blur;
        aof.UseProgram(p);
        aof.Uniform1i(aof.GetUniformLocation(p, "src"), 0);
        aof.Uniform2f(aof.GetUniformLocation(p, "hres"), (GLfloat)ao.hw, (GLfloat)ao.hh);
        for (int k = 0; k < 2; k++) {                           /* t0 -> t1 across, t1 -> t0 down */
            msf.BindFramebuffer(MS_FRAMEBUFFER, ao.fbo[!k]);
            glBindTexture(GL_TEXTURE_2D, ao.t[k]);
            aof.Uniform2f(aof.GetUniformLocation(p, "dir"), k ? 0.0f : 1.0f, k ? 1.0f : 0.0f);
            ao_tri();
        }

        p = ao.p_comp;
        msf.BindFramebuffer(MS_FRAMEBUFFER, g_ms_fbo);         /* the screen */
        glViewport(0, 0, w, h);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ZERO, GL_SRC_COLOR);
        aof.BlendEquation(0x8006);                              /* GL_FUNC_ADD */
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
        aof.UseProgram(p);
        glBindTexture(GL_TEXTURE_2D, ao.t[0]);
        aof.ActiveTexture(0x84C1);
        glBindTexture(GL_TEXTURE_2D, ao.t_depth);
        aof.ActiveTexture(0x84C0);
        aof.Uniform1i(aof.GetUniformLocation(p, "aot"), 0);
        aof.Uniform1i(aof.GetUniformLocation(p, "dep"), 1);
        aof.Uniform2f(aof.GetUniformLocation(p, "full"), (GLfloat)w, (GLfloat)h);
        aof.Uniform2f(aof.GetUniformLocation(p, "hres"), (GLfloat)ao.hw, (GLfloat)ao.hh);
        aof.Uniform4f(aof.GetUniformLocation(p, "vp"), (GLfloat)vp[0], (GLfloat)vp[1], (GLfloat)vp[2], (GLfloat)vp[3]);
        aof.UniformMatrix4fv(aof.GetUniformLocation(p, "invp"), 1, GL_FALSE, inv);
        aof.Uniform2f(aof.GetUniformLocation(p, "dr"), dr[0], dr[1]);
        ao_tri();
    } else if (do_ao) {
        g_ao = 0;
    }
    pm_mark(PM_AO);
    if (do_bloom)
        bl_pass(w, h, g_ao_nlayers ? g_ao_layer[li].dr[1] - 1e-5f : 2.0f);
    pm_mark(PM_BLOOM);

    aof.UseProgram((GLuint)prog);
    msf.BindFramebuffer(MS_READ_FB, (GLuint)rfb);
    msf.BindFramebuffer(MS_DRAW_FB, (GLuint)dfb);
    glPopAttrib();
    aof.ActiveTexture((GLenum)act);

    if (!do_ao)
        return;
    g_ao_why[why]++;
#if GMV_DEV
    if (g_ao_runs++ % 1200 == 0) {
        char layers[512];
        int len = 0;
        for (int k = 0; k < g_ao_nlayers && len < (int)sizeof layers - 96; k++) {
            GLfloat lin[16];
            double ln = 0, lf = 0;
            if (ao_invert(g_ao_layer[k].m, lin)) {
                ln = ao_eye_depth(lin, -1);
                lf = ao_eye_depth(lin, 1);
            }
            len += snprintf(layers + len, sizeof layers - len, "%s %.3g..%.3g depth %.3g..%.3g x%u",
                            k == li ? " [used]" : "", ln, lf, g_ao_layer[k].dr[0], g_ao_layer[k].dr[1], g_ao_layer[k].n);
        }
        gmv_log("[ao] pass %u at %dx%d: fog mode 0x%x %g..%g density %g; radius %g, strength %g; "
                "applied at the 2D switch %u, a depth clear %u, the present %u times; layers (near..far, depth range, draws):%s\n",
                g_ao_runs, w, h, (unsigned)fogs[0], fogs[1], fogs[2], fogs[3], g_ao_radius, g_ao_strength,
                g_ao_why[0], g_ao_why[1], g_ao_why[2], layers);
    }
#endif
}

/* The layer the current draw goes to, added if new (-1 = no room). */
static int ao_layer(void)
{
    for (int k = 0; k < g_ao_nlayers; k++)
        if (!memcmp(g_ao_layer[k].m, g_ao_proj, sizeof g_ao_proj) && !memcmp(g_ao_layer[k].dr, g_ao_dr, sizeof g_ao_dr))
            return k;
    if (g_ao_nlayers == AO_LAYERS)
        return -1;
    int k = g_ao_nlayers++;
    memcpy(g_ao_layer[k].m, g_ao_proj, sizeof g_ao_proj);
    memcpy(g_ao_layer[k].dr, g_ao_dr, sizeof g_ao_dr);
    g_ao_layer[k].fog[0] = 0;
    g_ao_layer[k].n = 0;
    return k;
}

static void ao_layers_reset(void)
{
    g_ao_nlayers = 0;
    g_ao_cur = -1;
    g_ao_3d = 0;
}

/* A pass is wanted: AMBIENT_OCCLUSION, BLOOM, WATER_REFLECTIONS, VEHICLE_REFLECTIONS or DYNAMIC_SHADOWS, while
   its setup has not failed. */
static int post_on(void)
{
    return (g_ao_want && g_ao) || (g_bloom_want && g_bloom) || (g_ssr_want && g_ssr) || (g_vr_want && g_vr) ||
           (g_ds_want && g_ds);
}

static int ao_pending(void)
{
    return post_on() && !g_ao_done && g_ao_3d >= AO_MIN_DRAWS && !g_glt.last_fb;
}

void ao_proj_refresh(void)
{
    if (g_ao_proj_dirty) {
        g_ao_proj_dirty = 0;
        glGetFloatv(GL_PROJECTION_MATRIX, g_ao_proj);
        g_ao_persp = fabsf(g_ao_proj[11]) > 0.5f && fabsf(g_ao_proj[15]) < 0.5f;
        g_ao_cur = -1;
    }
}

/* Before each draw (tr_DrawArrays / tr_DrawElements). */
void ao_draw(void)
{
    if (!post_on() || g_ao_done || g_glt.last_fb)
        return;
    ao_proj_refresh();
    if (!g_ao_persp) {
        if (ao_pending())
            ao_run(0);
        return;
    }
    if (!g_ao_depth_on || !g_ao_dmask)
        return;
    g_ao_3d++;
    if (g_ao_cur < 0 && (g_ao_cur = ao_layer()) < 0)
        return;
    g_ao_layer[g_ao_cur].n++;
    memcpy(g_ao_layer[g_ao_cur].vp, g_glt.vp, sizeof g_ao_layer[g_ao_cur].vp);
    if (g_ao_fog_on)
        memcpy(g_ao_layer[g_ao_cur].fog, g_ao_fogp, sizeof g_ao_fogp);
}

/* Before glClear: a depth clear would lose the scene's depth. */
void ao_clear(GLbitfield m)
{
    if (!(m & GL_DEPTH_BUFFER_BIT) || g_glt.last_fb)
        return;
    if (ao_pending())
        ao_run(1);
    else
        ao_layers_reset();                                  /* what was drawn so far is gone */
}

/* Before the present: the frame's 2D layer never came. */
unsigned g_frame_no;                              /* presents so far */

unsigned gmv_gl_frame_no(void)
{
    return g_frame_no;
}

void ao_frame_end(void)
{
#if GMV_DEV
    ds_eye_frame_end();
#endif
    g_frame_no++;
    if (ao_pending())
        ao_run(2);
    g_ao_done = 0;
    g_glow_used = 0;
    ao_layers_reset();
}

void ao_cap(GLenum cap, int on)
{
    if (cap == GL_DEPTH_TEST)
        g_ao_depth_on = on;
    else if (cap == GL_FOG)
        g_ao_fog_on = on;
    else if (cap == GL_BLEND)
        g_blend_on = on;
}
