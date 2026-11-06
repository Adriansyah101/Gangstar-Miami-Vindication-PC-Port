/* draw_hooks.c -- what Gangstar's effects do around each draw, and how the GL shim reaches them (game.h). */
#include <stdlib.h>
#include <string.h>
#include "compat.h"
#include <GL/gl.h>

#include "game.h"
#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "render.h"

int g_ssr_want, g_ssr = 1;                       /* WATER_REFLECTIONS; 0 once its setup failed */
int g_vr_want, g_vr = 1;                         /* VEHICLE_REFLECTIONS; 0 once its setup failed */
int g_ds_want, g_ds = 1;                         /* DYNAMIC_SHADOWS; 0 once its setup failed */

static int fog_track, sf_want;

/* SPAWN_FADE (the patches says how far a draw has faded): a car or pedestrian coming or going is
   drawn through a dot pattern that holds that many 64ths of the pixels (an 8x8 ordered dither, so
   each step only adds dots), which needs neither blending nor sorting.  The pattern stays on for the
   passes that redraw the same triangles (the glow layer, the car reflections' marking), so they leave
   the same holes. */
#ifndef GL_POLYGON_STIPPLE
#define GL_POLYGON_STIPPLE 0x0B42
#endif
static int sf_stipple_begin(void)
{
    static const unsigned char bayer[8][8] = {
        {0, 32, 8, 40, 2, 34, 10, 42}, {48, 16, 56, 24, 50, 18, 58, 26},
        {12, 44, 4, 36, 14, 46, 6, 38}, {60, 28, 52, 20, 62, 30, 54, 22},
        {3, 35, 11, 43, 1, 33, 9, 41}, {51, 19, 59, 27, 49, 17, 57, 25},
        {15, 47, 7, 39, 13, 45, 5, 37}, {63, 31, 55, 23, 61, 29, 53, 21},
    };
    static GLubyte pat[64][128];
    static int built, cur = -1;
    int lv = gmv_spawn_fade_level(g_blend_on && g_blend_dst == GL_ONE);
    if (lv >= 64)
        return 0;
    if (lv < 0)
        lv = 0;
    if (!built) {
        for (int l = 0; l < 64; l++)
            for (int y = 0; y < 32; y++)
                for (int x = 0; x < 32; x++)
                    if (bayer[y & 7][x & 7] < l)
                        pat[l][y * 4 + x / 8] |= (GLubyte)(0x80 >> (x & 7));
        built = 1;
    }
    if (lv != cur) {
        glPolygonStipple(pat[lv]);     /* the game never sets one, so it stays between draws */
        cur = lv;
    }
    glEnable(GL_POLYGON_STIPPLE);
    return 1;
}

static void sf_stipple_end(int on)
{
    if (on)
        glDisable(GL_POLYGON_STIPPLE);
}

void gmv_game_gl_setup(void)
{
    const char *af = gmv_getenv("GMV_ANISOTROPIC_FILTERING");    /* default off */
    g_aniso = af ? (float)atof(af) : 0;
    if (g_aniso > 16)
        g_aniso = 16;
    const char *ao = gmv_getenv("GMV_AMBIENT_OCCLUSION");    /* default off */
    g_ao_want = ao && *ao && *ao != '0';
    const char *as = gmv_getenv("GMV_AO_STRENGTH"), *ar = gmv_getenv("GMV_AO_RADIUS");
    if (as && *as) g_ao_strength = (float)atof(as);
    if (ar && *ar && atof(ar) > 0) g_ao_radius = (float)atof(ar);
    if (g_ao_want)
        gmv_log("[ao] AMBIENT_OCCLUSION=1: radius %g, strength %g\n", g_ao_radius, g_ao_strength);
    const char *bl_on = gmv_getenv("GMV_BLOOM");             /* default off */
    int bl_level = bl_on && *bl_on ? atoi(bl_on) : 0;
    g_bloom_want = bl_level > 0;
    const char *bi = gmv_getenv("GMV_BLOOM_INTENSITY"), *bt = gmv_getenv("GMV_BLOOM_THRESHOLD");
    if (bi && *bi && atof(bi) >= 0) g_bloom_intensity = (float)atof(bi);
    if (bt && *bt) g_bloom_threshold = (float)atof(bt);
    g_bloom_threshold = g_bloom_threshold < 0 ? 0 : g_bloom_threshold > 0.95f ? 0.95f : g_bloom_threshold;
    g_bloom_lights = bloom_part(bl_level, "GMV_BLOOM_LIGHTS");
    g_bloom_windows = bloom_part(bl_level, "GMV_BLOOM_WINDOWS");
    g_bg_want = g_bloom_want && (g_bloom_lights > 0 || g_bloom_windows > 0);
    if (g_bloom_want)
        gmv_log("[bloom] BLOOM=%d: threshold %g, intensity %g; lights %g, windows %g\n", bl_level, g_bloom_threshold,
                g_bloom_intensity, g_bloom_lights, g_bloom_windows);
    const char *wr = gmv_getenv("GMV_WATER_REFLECTIONS"), *rs = gmv_getenv("GMV_REFLECTION_STRENGTH");   /* default off */
    g_ssr_want = wr && *wr && *wr != '0';
    if (rs && *rs) g_ssr_strength = (float)atof(rs);
    g_ssr_strength = g_ssr_strength < 0 ? 0 : g_ssr_strength > 2 ? 2 : g_ssr_strength;
    if (g_ssr_want)
        gmv_log("[ssr] WATER_REFLECTIONS=1: strength %g\n", g_ssr_strength);
    const char *vr_on = gmv_getenv("GMV_VEHICLE_REFLECTIONS"), *vs = gmv_getenv("GMV_VEHICLE_REFLECTION_STRENGTH");
    g_vr_want = vr_on && *vr_on && *vr_on != '0';
    if (vs && *vs) g_vr_strength = (float)atof(vs);
    g_vr_strength = g_vr_strength < 0 ? 0 : g_vr_strength > 2 ? 2 : g_vr_strength;
    if (g_vr_want)
        gmv_log("[vr] VEHICLE_REFLECTIONS=1: strength %g\n", g_vr_strength);
    const char *ds_on = gmv_getenv("GMV_DYNAMIC_SHADOWS");
    g_ds_want = ds_on && *ds_on && *ds_on != '0';
    if (g_ds_want)
        gmv_log("[shadow] DYNAMIC_SHADOWS=1\n");
#if GMV_DEV
    const char *vrd_on = gmv_dbgenv("GMV_VEHICLE_REFLECTION_DEBUG");
    g_vr_debug = vrd_on && *vrd_on && *vrd_on != '0';
    if (g_vr_debug)
        gmv_log("[vr] VEHICLE_REFLECTION_DEBUG=1: the cars' pixels tinted by what became of them (red covered, grey "
                "fogged, blue edge-on, magenta normal turned round, yellow ray missed (screen sphere map), green hit, cyan hit past "
                "the car itself)%s\n", g_vr_want ? "" : "; VEHICLE_REFLECTIONS is off at startup");
#endif
    const char *sfw = gmv_getenv("GMV_SPAWN_FADE");     /* the draw wrappers apply its dot pattern */
    sf_want = sfw && *sfw && *sfw != '0';
    g_fog_mode = gmv_fog_mode();
    fog_track = g_fog_mode > 0;                     /* for fog_force_begin */
    const char *fh_h = gmv_getenv("GMV_FOG_HEIGHT");
    if (fh_h && *fh_h && atof(fh_h) >= 0) g_fog_height = (float)atof(fh_h) * 100;
    if (g_fog_mode == 2)
        gmv_log("[fog] FOG_MODE=2: the hybrid fog, thinning with height over %g m\n", g_fog_height / 100);
}

int gmv_game_gl_wants_draws(void)
{
    return g_ao_want || g_bloom_want || g_ssr_want || g_vr_want || g_ds_want || fog_track || g_tab_live || sf_want;
}

void *gmv_game_gl_intercept(const char *name)
{
    if (g_ao_want || g_bloom_want || g_ssr_want || g_vr_want || g_ds_want || fog_track || g_tab_live) {
        void *t = gl_ao_intercept(name);
        if (t)
            return t;
    }
    if (gmv_padicons_on()) {
        if (!strcmp(name, "_glCompressedTexImage2D"))
            return (void *)pi_CompressedTexImage2D;
        if (!strcmp(name, "_glDeleteTextures"))
            return (void *)pi_DeleteTextures;
    }
    if (g_aniso > 1 || g_tab_live) {
        if (!strcmp(name, "_glTexParameterf"))
            return (void *)af_TexParameterf;
        if (!strcmp(name, "_glTexParameteri"))
            return (void *)af_TexParameteri;
        if (g_tab_live && !strcmp(name, "_glDeleteTextures"))
            return (void *)af_DeleteTextures;
    }
    return NULL;
}

void *gmv_game_gl_draw_intercept(const char *name)
{
    (void)name;
    return NULL;
}

void gmv_game_gl_cap(unsigned cap, int on)
{
    ao_cap(cap, on);
}

void gmv_game_gl_clear_begin(unsigned mask)
{
    ao_clear(mask);
}

void gmv_game_gl_clear_end(unsigned mask)
{
    bg_clear(mask);
}

void gmv_game_gl_client_unit(unsigned unit)
{
    (void)unit;
}

void gmv_game_gl_client_state(unsigned array, int on)
{
    (void)array;
    (void)on;
}

void gmv_game_frame_end(void)
{
    ao_frame_end();
}

void gmv_game_after_present(void)
{
    live_sync();
}

void gmv_game_draw_arrays(void *real, unsigned mode, int first, int count, void *caller)
{
    (void)caller;
    ao_draw();
    unsigned tf = texfix_begin(mode);
    int ff = fog_force_begin();
    int cl = fog_clip_begin();
    int fh = fog_hybrid_begin(mode, ff, tf);
    glow_state_t gs;
    if (g_sky_glow_draw) glow_begin(&gs);
    int bg = bg_draw_begin(mode);
    int sfs = sf_stipple_begin();
    if (real) ((void(APIENTRY *)(GLenum, GLint, GLsizei))real)(mode, first, count);
    if (g_sky_glow_draw) glow_end(&gs);
    if (bg) bg_draw_end(bg, 0, mode, first, count, 0, NULL);
    sf_stipple_end(sfs);
    if (cl) glDisable(FOG_CLIP_PLANE);
    fog_hybrid_end(fh);
    fog_force_end(ff);
    texfix_end(tf);
}

void gmv_game_draw_elements(void *real, unsigned mode, int count, unsigned type, const void *idx, void *caller)
{
    (void)caller;
    ao_draw();
    int body = (g_vr_want && g_vr) || (g_ds_want && g_ds) ? shine_body(mode, count) : 0;
    int dk = ds_kind(mode, count, body);
    int dm = ds_mask_begin(dk);
    unsigned tf = texfix_begin(mode);
    int sf = sky_read(count, type, idx);
    int ff = fog_force_begin();
    int cl = fog_clip_begin();
    int fh = fog_hybrid_begin(mode, ff, tf);
    glow_state_t gs;
    if (g_sky_glow_draw) glow_begin(&gs);
    int bg = bg_draw_begin(mode);
    int sm = sea_mark_begin(count, type, idx);
    int sfs = sf_stipple_begin();
    if (real) ((void(APIENTRY *)(GLenum, GLsizei, GLenum, const void *))real)(mode, count, type, idx);
    if (g_sky_glow_draw) glow_end(&gs);
    if (bg) bg_draw_end(bg, 1, mode, 0, count, type, idx);
    if (sf) sky_floor_end();
    if (ff == 1) sea_beyond(count, type, idx);
    sea_mark_end(sm);
    if (cl) glDisable(FOG_CLIP_PLANE);
    fog_hybrid_end(fh);
    fog_force_end(ff);
    texfix_end(tf);
    ds_mask_end(dm);
    if (body == 1 && g_vr_want && g_vr) car_mark(mode, count, type, idx);
    if (dk) ds_mark(dk, mode, count, type, idx);
    sf_stipple_end(sfs);
}
