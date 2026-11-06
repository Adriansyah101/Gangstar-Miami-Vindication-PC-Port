/* bloom_glow.c -- BLOOM_LIGHTS and BLOOM_WINDOWS: the glow layer */
#include <stdio.h>
#include <string.h>
#include "compat.h"
#include <GL/gl.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "render.h"

/* ---- BLOOM_LIGHTS / BLOOM_WINDOWS: the glow layer (with BLOOM, so off by default and under
   ORIGINAL_MAC_EXPERIENCE) ----
   A survey of the game's draws (2026-10-27) found its lights too dim for the bloom's brightness threshold.  The street-lamp
   cones, neon, crane and highway lights are additive draws (depth-tested, from 0x50a5ad) of a nearly
   black texture (95, average 9,7,9).  The lit windows are painted into the buildings' one wall texture
   and darkened by the lighting along with it.  (The two-texture setup with unit 1 added is the
   characters' and the vehicles' shine: unit 1 holds sky reflection maps.)  So the screen FBO gets a
   second colour attachment, the glow layer (RGBA16F, multisampled like the screen).  It is written while
   the world is drawn and read by the bloom's bright pass:
   - Every world draw also writes the layer, through its own blending on draw buffer 1 (glBlendFunci).
     Depth-tested additive draws add their colour (BLOOM_LIGHTS).  Opaque draws clear it, and blended ones
     fade it by their own destination factor.  So whatever stands in front hides a light's glow as it
     hides the light.
   - Opaque single-texture draws (the buildings) are drawn a second time, into the layer alone, through
     a small shader, at night by the game's clock (the patches, g_tod_ms).  It adds the texels that look
     like a lit window, times BLOOM_WINDOWS, in the fog the world has.  At night the game binds its night
     atlases (night<n>.tga): navy walls, the lit windows and signs warm.  The shader knows one by its last
     mipmap (the whole atlas): dark, and bluer than red; the day atlases are bright, their cream walls
     warm.  In a night atlas a lit texel is bright (0.42 up) and warm (red or green above blue by 0.06 up).
   - The sky's draws and the draws without a depth test (the sun and moon) leave the layer alone.
   Fixed-function draws and gl_FragColor write every draw buffer, so neither the game's draws nor the
   host's shaders change.  The layer needs MSAA (the screen FBO) and per-buffer blending (OpenGL 4);
   without them it is logged and left out.  After the bloom pass only the screen itself is drawn. */
#define BG_LIGHT_GAIN 2.0f                              /* BLOOM_LIGHTS=1: a light glows at twice its colour */

static const char bg_fs_main[] =
    "uniform float gain, fogon;\n"
    "void main() {\n"
    "    vec4 t = texture2DProj(t0, gl_TexCoord[0]);\n"
    "    vec3 a = texture2DProj(t0, gl_TexCoord[0], 16.0).rgb;\n"   /* the last mipmap: the atlas's average */
    "    float k = smoothstep(0.42, 0.6, max(t.r, max(t.g, t.b))) * smoothstep(0.06, 0.18, max(t.r, t.g) - t.b)\n"
    "            * step(a.r + 0.015, a.b) * step(max(a.r, max(a.g, a.b)), 0.33);\n"
    "    float f = fogon > 0.0 ? fog_clear() : 1.0;\n"
    "    vec3 c = t.rgb * k;\n"
    "    gl_FragColor = vec4(c * (gain * f), t.a * gl_Color.a);\n"
    "}\n";

bg_t bg;

/* The lit windows' program, once BLOOM_WINDOWS is on (at the start, or later from the options menu). */
static void bg_windows_init(void)
{
    static int tried;
    if (tried || !(g_bloom_windows > 0))
        return;
    tried = 1;
    char src[fh_fs_head_size + sizeof bg_fs_main];
    snprintf(src, sizeof src, "%s%s", fh_fs_head, bg_fs_main);
    bg.p = ao_program("lit windows", NULL, src);
    if (!bg.p) {
        gmv_log("[bloom] BLOOM_WINDOWS left out\n");
        return;
    }
    bg.u_gain = aof.GetUniformLocation(bg.p, "gain");
    bg.u_fogon = aof.GetUniformLocation(bg.p, "fogon");
    bg.u_invp = aof.GetUniformLocation(bg.p, "invp");
    bg.u_vp = aof.GetUniformLocation(bg.p, "vp");
    bg.u_dr = aof.GetUniformLocation(bg.p, "dr");
    bg.u_fogp = aof.GetUniformLocation(bg.p, "fogp");
    bg.u_hgt = aof.GetUniformLocation(bg.p, "hgt");
    GLint prog = 0;
    glGetIntegerv(0x8B8D, &prog);                       /* GL_CURRENT_PROGRAM */
    aof.UseProgram(bg.p);
    aof.Uniform1i(aof.GetUniformLocation(bg.p, "t0"), 0);
    aof.UseProgram((GLuint)prog);
}

static int bg_init(void)
{
    if (!g_ms_fbo) {
        gmv_log("[bloom] BLOOM_LIGHTS and BLOOM_WINDOWS need MSAA (the screen framebuffer); left out\n");
        return 0;
    }
    if (!ao_procs("BLOOM_LIGHTS and BLOOM_WINDOWS"))
        return 0;
    bg.BlendFunci = ms_proc("glBlendFunci", "glBlendFunciARB");
    bg.Enablei = ms_proc("glEnablei", "glEnableIndexedEXT");
    bg.ColorMaski = ms_proc("glColorMaski", "glColorMaskIndexedEXT");
    bg.DrawBuffers = ms_proc("glDrawBuffers", "glDrawBuffersARB");
    bg.ClearBufferfv = gl_real("glClearBufferfv");
    if (!bg.BlendFunci || !bg.Enablei || !bg.ColorMaski || !bg.DrawBuffers || !bg.ClearBufferfv) {
        gmv_log("[bloom] the driver has no per-buffer blending (OpenGL 4); BLOOM_LIGHTS and BLOOM_WINDOWS left out\n");
        return 0;
    }
    GLint rb = 0, dfb = 0;
    glGetIntegerv(0x8CA7, &rb);                         /* GL_RENDERBUFFER_BINDING */
    glGetIntegerv(MS_DRAW_FB_BINDING, &dfb);
    msf.GenRenderbuffers(1, &g_ms_glow);
    msf.BindRenderbuffer(MS_RENDERBUFFER, g_ms_glow);
    msf.RenderbufferStorageMultisample(MS_RENDERBUFFER, g_msaa, 0x881A, g_ms_w, g_ms_h);  /* RGBA16F */
    msf.BindRenderbuffer(MS_RENDERBUFFER, (GLuint)rb);
    msf.BindFramebuffer(MS_DRAW_FB, g_ms_fbo);
    msf.FramebufferRenderbuffer(MS_DRAW_FB, 0x8CE1, MS_RENDERBUFFER, g_ms_glow);   /* COLOR_ATTACHMENT1 */
    GLenum st = msf.CheckFramebufferStatus(MS_DRAW_FB);
    if (st != 0x8CD5)
        msf.FramebufferRenderbuffer(MS_DRAW_FB, 0x8CE1, MS_RENDERBUFFER, 0);
    msf.BindFramebuffer(MS_DRAW_FB, (GLuint)dfb);
    if (st != 0x8CD5) {
        gmv_log("[bloom] the glow layer's framebuffer is incomplete (0x%x); BLOOM_LIGHTS and BLOOM_WINDOWS left out\n", st);
        g_ms_glow = 0;
        return 0;
    }
    bg.frame = g_frame_no - 1;                          /* cleared at the next world draw */
    bg_windows_init();
    gmv_log("[bloom] glow layer ready (%dx%d, %d samples): lights %g, windows %g%s\n", g_ms_w, g_ms_h, g_msaa,
            g_bloom_lights, g_bloom_windows, g_tod_ms < 0 && g_bloom_windows > 0 ? " (the game's clock not read yet)" : "");
    return 1;
}

/* How much the windows are lit by the game's clock: off by day, rising from 18:30 to 20:00, all night,
   falling from 5:00 to 6:30. */
static float bg_night(void)
{
    if (g_tod_ms < 0)
        return 0;
    float h = g_tod_ms / 3600000.0f, n = h >= 12 ? (h - 18.5f) / 1.5f : (6.5f - h) / 1.5f;
    n = n < 0 ? 0 : n > 1 ? 1 : n;
    return n * n * (3 - 2 * n);
}

/* What the bloom's bright pass multiplies the layer by. */
float bg_gain(void)
{
    return g_bloom_lights > 0 ? g_bloom_lights * BG_LIGHT_GAIN : 1.0f;
}

static void bg_frame_log(void)
{
    DWORD now = GetTickCount();
    if (!bg.next_log)
        bg.next_log = now + 10000;
    else if ((int)(now - bg.next_log) >= 0) {
        bg.next_log = now + 30000;
        int m = g_tod_ms < 0 ? -1 : g_tod_ms / 60000;
        gmv_trace("[bloom] glow layer, frame %u: %u light draws, %u window draws; the game's clock %02d:%02d, windows lit %.0f%%\n",
                bg.frame, bg.lights, bg.windows, m < 0 ? 0 : m / 60, m < 0 ? 0 : m % 60, bg_night() * 100);
    }
    bg.lights = bg.windows = 0;
}

/* Before a draw, in the game's state: draw buffer 1's blending for it.  0 = the layer is not being
   drawn, 1 = set, 2 = also bg_draw_end's windows pass after the draw. */
int bg_draw_begin(GLenum mode)
{
    bg.light = 0;
    if (!g_bg_want || bg.ready < 0 || g_glt.last_fb || g_ao_done || !g_ms_fbo)
        return 0;
    ao_proj_refresh();
    if (!g_ao_persp)
        return 0;
    if (!bg.ready && (bg.ready = bg_init() ? 1 : -1) < 0)
        return 0;
    bg_windows_init();
    if (g_glow_bufs != 2 || bg.frame != g_frame_no) {
        static const GLenum both[2] = {0x8CE0, 0x8CE1};  /* COLOR_ATTACHMENT0, 1 */
        bg.DrawBuffers(2, both);
        g_glow_bufs = 2;
        if (bg.frame != g_frame_no) {                   /* the frame's first world draw */
            static const GLfloat zero[4];
            GLboolean cm[4], sc = glIsEnabled(GL_SCISSOR_TEST);
            glGetBooleanv(GL_COLOR_WRITEMASK, cm);
            if (sc) glDisable(GL_SCISSOR_TEST);
            bg.ColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            bg.ClearBufferfv(GL_COLOR, 1, zero);
            bg.ColorMaski(1, cm[0], cm[1], cm[2], cm[3]);
            if (sc) glEnable(GL_SCISSOR_TEST);
            bg_frame_log();
            bg.frame = g_frame_no;
            g_glow_used = 1;
        }
    }
    GLenum s = GL_ZERO, d = GL_ZERO;
    int kind = 1;
    if (g_sky_draw || g_sky_glow_draw || !g_ao_depth_on)
        d = GL_ONE;                                     /* left alone */
    else if (g_blend_on && g_blend_dst == GL_ONE) {
        d = GL_ONE;
        if (g_bloom_lights > 0) {
            s = g_blend_src;
            bg.lights++;
            bg.light = 1;
        }
    } else if (g_blend_on) {
        d = g_blend_src == GL_DST_COLOR && g_blend_dst == GL_ZERO ? GL_SRC_COLOR : g_blend_dst;
    } else if (bg.p && g_bloom_windows > 0 && g_ao_dmask && mode != GL_POINTS && g_te[0].mode == GL_MODULATE &&
               (g_tf_tex2d & (g_texfix ? g_tf_tcarr : 0xF) & 0xF) == 1 && bg_night() > 0) {
        kind = 2;
    }
    bg.Enablei(GL_BLEND, 1);
    bg.BlendFunci(1, s, d);
    return kind;
}

/* After the draw (bg_draw_begin gave 2), before the fog's clip plane goes: the same draw again into the
   glow layer alone, through the windows' shader. */
void bg_draw_end(int kind, int elems, GLenum mode, GLint first, GLsizei count, GLenum type, const void *idx)
{
    static void(APIENTRY * da)(GLenum, GLint, GLsizei);
    static void(APIENTRY * de)(GLenum, GLsizei, GLenum, const void *);
    if (kind != 2)
        return;
    if (!da) da = gl_real("glDrawArrays");
    if (!de) de = gl_real("glDrawElements");
    if (!da || !de)
        return;
    GLfloat u[30];
    int fog = (g_fog_all || g_ao_fog_on) && g_ao_fogp[0] == 0x2601 && g_ao_fogp[2] > g_ao_fogp[1] && fh_params(u);
    GLint prog = 0;
    glGetIntegerv(0x8B8D, &prog);                       /* GL_CURRENT_PROGRAM */
    glPushAttrib(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    bg.ColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    bg.BlendFunci(1, GL_ONE, GL_ONE);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_LEQUAL);
    aof.UseProgram(bg.p);
    aof.Uniform1f(bg.u_gain, g_bloom_windows * bg_night() / bg_gain());
    aof.Uniform1f(bg.u_fogon, fog ? 1.0f : 0.0f);
    if (fog) {
        aof.UniformMatrix4fv(bg.u_invp, 1, GL_FALSE, u);
        aof.Uniform4f(bg.u_vp, u[16], u[17], u[18], u[19]);
        aof.Uniform2f(bg.u_dr, u[20], u[21]);
        aof.Uniform4f(bg.u_fogp, u[22], u[23], u[24], u[25]);
        aof.Uniform4f(bg.u_hgt, u[26], u[27], u[28], u[29]);
    }
    if (elems)
        de(mode, count, type, idx);
    else
        da(mode, first, count);
    glPopAttrib();
    aof.UseProgram((GLuint)prog);
    bg.windows++;
}

/* After the game's glClear: a colour clear filled the layer with the clear colour. */
void bg_clear(GLbitfield m)
{
    static const GLfloat zero[4];
    if ((m & GL_COLOR_BUFFER_BIT) && g_glow_bufs == 2 && !g_glt.last_fb && bg.ready > 0)
        bg.ClearBufferfv(GL_COLOR, 1, zero);
}

/* Only the screen drawn from here on (the bloom pass, the 2D layer). */
void bg_single(void)
{
    if (g_glow_bufs != 2 || bg.ready <= 0)
        return;
    GLint dfb = 0;
    glGetIntegerv(MS_DRAW_FB_BINDING, &dfb);
    msf.BindFramebuffer(MS_DRAW_FB, g_ms_fbo);
    static const GLenum one = 0x8CE0;
    bg.DrawBuffers(1, &one);
    msf.BindFramebuffer(MS_DRAW_FB, (GLuint)dfb);
    g_glow_bufs = 1;
}

/* GAMEPATCH_TAB: AMBIENT_OCCLUSION, BLOOM (with the glow layer at 2), FOG_MODE, WATER_REFLECTIONS,
   VEHICLE_REFLECTIONS and DYNAMIC_SHADOWS from the options menu.  The wrappers they need are in from the start
   (gmv_gl_resolve); the new values are taken after a present, so no frame is drawn half with the old ones. */
static const char *const g_post_keys[] = {"AMBIENT_OCCLUSION", "BLOOM", "FOG_MODE", "WATER_REFLECTIONS",
                                          "VEHICLE_REFLECTIONS", "BLOOM_THRESHOLD", "DYNAMIC_SHADOWS"};
#define POST_N (sizeof g_post_keys / sizeof *g_post_keys)
static float g_post_next[POST_N] = {-1, -1, -1, -1, -1, -1, -1};   /* -1 = no change */

int gmv_set_post(const char *key, float v)
{
    if (!g_tab_live)
        return 0;
    if (!strcmp(key, "FOG_HEIGHT")) {                   /* metres; read at each draw */
        g_fog_height = v < 0 ? 0 : v * 100;
        return 1;
    }
    for (size_t i = 0; i < POST_N; i++)
        if (!strcmp(key, g_post_keys[i])) {
            g_post_next[i] = v < 0 ? 0 : v;
            return 1;
        }
    return 0;
}

static void post_sync(void)
{
    float *n = g_post_next;
    int any = 0;
    for (size_t i = 0; i < POST_N; i++)
        any |= n[i] >= 0;
    if (!any)
        return;
    if (n[0] >= 0) g_ao_want = n[0] != 0;
    if (n[1] >= 0) {
        int level = (int)(n[1] + 0.5f);
        g_bloom_want = level > 0;
        g_bloom_lights = bloom_part(level, "GMV_BLOOM_LIGHTS");
        g_bloom_windows = bloom_part(level, "GMV_BLOOM_WINDOWS");
    }
    if (n[2] >= 0 && (int)n[2] != g_fog_mode) {
        g_fog_mode = gmv_fog_mode_set((int)n[2]);
        if (g_fog_game_set) {                           /* the fog colour for the new mode, before the game sets it again */
            const GLfloat *c = g_fog_all && sky.n ? sky.hz : g_fog_game;
            glFogfv(GL_FOG_COLOR, c);
            memcpy(g_fog_col, c, sizeof g_fog_col);
        }
    }
    if (n[3] >= 0) g_ssr_want = n[3] != 0;
    if (n[4] >= 0) g_vr_want = n[4] != 0;
    if (n[5] >= 0) g_bloom_threshold = n[5] > 0.95f ? 0.95f : n[5];   /* read at each bloom pass */
    if (n[6] >= 0) g_ds_want = n[6] != 0;
    int bgw = g_bloom_want && (g_bloom_lights > 0 || g_bloom_windows > 0);
    if (bgw != g_bg_want) {
        if (!bgw)
            bg_single();
        g_bg_want = bgw;
        bl.w = 0;                                       /* the bloom's textures again, with or without the layer's */
    }
    for (size_t i = 0; i < POST_N; i++)
        n[i] = -1;
    gmv_log("[post] now: ambient occlusion %d, bloom %d (threshold %g, lights %g, windows %g), fog mode %d, "
            "water reflections %d, vehicle reflections %d, dynamic shadows %d\n", g_ao_want, g_bloom_want, g_bloom_threshold,
            g_bloom_lights, g_bloom_windows, g_fog_mode, g_ssr_want, g_vr_want, g_ds_want);
}

void live_sync(void)
{
    msaa_sync_samples();
    aniso_sync();
    post_sync();
    gmv_padicons_sync();
}
