/* dynamic_shadows.c -- DYNAMIC_SHADOWS */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"
#include <GL/gl.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "render.h"

/* ---- DYNAMIC_SHADOWS: an enhancement, off by default and under ORIGINAL_MAC_EXPERIENCE -------------------
   Shadows that follow the sun and the moon.  The game's own are a ready-made shadow-mesh under each model,
   whatever the hour; the patches hides them (gmv_ds_node) while this runs.
   - Neither is up for part of the day (measured: the sun from about 5.05 h to 16.9 h, the moon from about 18.1 h
     to 3.2 h), and at those hours a thing would stand on nothing.  So a low light's direction is drawn towards
     straight up: as it sets the shadow shortens to a soft one under the thing, and it stays that way
     until the next light rises and draws it out again.  It never jumps, and no second map is needed.
   - Each frame the casters are drawn a second time, into a depth map seen from the sun: an orthographic box
     DS_RADIUS around a point ahead of the camera, moved in whole texels so that its edges do not crawl.  The
     view comes from the first caster: its modelview if that puts the eye where the game's camera is (vertices
     already in world space), or else that with its node's absolute transformation taken out.  The casters are
     whatever solid is drawn for a node of a vehicle or a person (the patches keeps the table, gmv_ds_caster) and
     the bodies shine_body knows, which also has the people without a game shadow; the map only holds what the
     game draws, so a car just off the screen casts nothing.
   - Only a caster can shadow itself by rounding, so only its pixels (a stencil bit set as it is drawn) need the
     bias.  On a low sun the bias slides a shadow along the ground by nearly its own length (about 70 cm), which
     left it short of the feet, so the rest of the world takes next to none.
   - The pass runs where the reflections do: each pixel's position comes from the frame's depth (as the AO's)
     and is looked up in the map, 16 filtered taps for a soft edge.  Shadowed pixels are multiplied by
     1 - the light's strength.  A surface turned away from the light takes none, so walls behind a car do not.
     Below DS_SEA_Z (the sea does not write
     depth, so this is its bed) nothing is darkened, nor in the fog, or towards the map's edge.
   - Only the casters are in the map: buildings throw no shadow, so a car in a building's shade still
     casts one. */
#define DS_SIZE 2048                                    /* texels of the map a side */
#define DS_RADIUS 6000.0f                               /* half its width in world units (about 1 cm): 60 m */
#define DS_DEPTH 20000.0f                               /* half its depth: 200 m up and down the sun's line */
#define DS_AHEAD 0.6f                                   /* its centre, ahead of the camera, in DS_RADIUS */
#define DS_BIAS 25.0f                                   /* world units a caster's pixel may be behind the map's */
#define DS_BIAS_WORLD 1.0f                              /* the same for the rest of the world */
#define DS_BIT 0x40                                     /* the stencil bit a caster sets */
#define DS_EYE_NEAR 500.0f                              /* world units from the game's camera for a modelview to be the view */
#define DS_STRENGTH 0.45f
#define DS_MOON_STRENGTH 0.30f                          /* moonlight is the lesser part of a night's light */
#define DS_SKY_STRENGTH 0.40f                        /* the one under a thing, with neither up */
#define DS_SKY_SOFT 2.0f                                /* its taps' spacing, in texels */
#define DS_LOW 0.03f                                    /* sine of a light's height where it no longer counts */
#define DS_SUN_HIGH 0.20f                               /* the same where it has the whole say: about 11.5 degrees */
#define DS_MOON_HIGH 0.14f                              /* about 8; it peaks at 27 */
#define DS_SEA_Z (-20.0f)
extern float g_sun_dir[3];                              /* the patches: towards the sun; 0,0,0 until it is known */
extern float g_moon_dir[3];                             /* the same for the moon */
extern float g_cam_world[3];                            /* the patches: the camera's position at the last tick */

static struct {
    unsigned frame;                                     /* g_frame_no + 1 of the last frame with a map */
    unsigned tried;                                     /* the same, of the last frame looked at */
    unsigned n[4];                                      /* casters of that frame, by ds_kind's kind */
    unsigned mask;                                      /* g_frame_no + 1 of the last frame the stencil bit was cleared */
#if GMV_DEV
    int eye_said;                                       /* first casters that gave no view, logged */
    unsigned eye_frame;                                 /* g_frame_no + 1 of the last frame with one */
    unsigned eye_bad, eye_frames, eye_nomap, eye_nocam; /* since the last summary */
    unsigned eye_kind[4];                               /* by ds_kind's kind */
    unsigned eye_reach, eye_reach_frames, eye_reach_nomap, eye_reach_frame; /* those the map could have held */
    float eye_min;                                      /* the nearest, world units */
    DWORD eye_t;                                        /* when that was */
#endif
    int ready;                                         /* 1 = the programs are built, -1 = they failed */
    GLuint p_map, p_pass, fbo, t;
    GLfloat lvp[16];                                    /* eye space to the map's clip space */
    GLfloat iv[16];                                     /* eye space to world */
    GLfloat leye[3];                                    /* towards the sun, in eye space */
    GLfloat strength, soft;                             /* of the frame's light */
    GLfloat dir[3];                                     /* towards it, in the world */
    const char *source;                                 /* for the log */
} ds;

int ds_frame(void)
{
    return ds.frame == g_frame_no + 1;
}

/* Before a draw to the screen, with shine_body's answer: 1 shiny car, 2 character, 3 another part, 0 no caster. */
int ds_kind(GLenum mode, GLsizei count, int body)
{
    if (!g_ds_want || !g_ds)
        return 0;
    if (body)
        return body;
    if (g_glt.last_fb || g_sky_draw || mode < GL_TRIANGLES || count < 3 || g_blend_on || !g_ao_dmask || !g_ao_depth_on ||
        !gmv_ds_caster())
        return 0;
    ao_proj_refresh();
    return g_ao_persp ? 3 : 0;
}

static const char ds_vs_map[] =
    "#version 120\n"
    "uniform mat4 lvp;\n"
    "void main() { gl_Position = lvp * gl_ModelViewMatrix * gl_Vertex; }\n";

static const char ds_fs_map[] =
    "#version 120\n"
    "void main() { gl_FragColor = vec4(1.0); }\n";

static const char ds_fs[] =
    "#version 120\n"
    "uniform sampler2D dep;\n"
    "uniform sampler2DShadow smap;\n"
    "uniform vec2 full, dr;\n"
    "uniform vec4 vp, fogp, wz, leye;\n"                /* wz: the row of eye to world giving the height */
    "uniform mat4 proj, invp, lvp;\n"
    "uniform float str, texel, soft, tw, zs, bias, sl, seaz;\n"   /* tw: a texel in world units; zs: map depth per world unit */
    "vec3 pos(vec2 px) {\n"
    "    float d = texture2D(dep, px / full).r;\n"
    "    vec4 p = invp * vec4((px - vp.xy) / vp.zw * 2.0 - 1.0, (d - dr.x) / (dr.y - dr.x) * 2.0 - 1.0, 1.0);\n"
    "    return p.xyz / p.w;\n"
    "}\n"
    "void main() {\n"
    "    vec2 px = gl_FragCoord.xy;\n"
    "    float d = texture2D(dep, px / full).r;\n"
    "    if (d < dr.x || d >= dr.y - 1e-5) discard;\n"
    "    vec3 pc = pos(px);\n"
    "    if (dot(wz, vec4(pc, 1.0)) < seaz) discard;\n"
    "    vec3 q = (lvp * vec4(pc, 1.0)).xyz;\n"
    "    float e = max(abs(q.x), abs(q.y));\n"
    "    if (e >= 1.0 || abs(q.z) >= 1.0) discard;\n"
    /* the surface's facing, each way from the nearer neighbour, so a silhouette's other side does not count */
    "    vec3 xa = pos(px + vec2(1.0, 0.0)) - pc, xb = pc - pos(px - vec2(1.0, 0.0));\n"
    "    vec3 ya = pos(px + vec2(0.0, 1.0)) - pc, yb = pc - pos(px - vec2(0.0, 1.0));\n"
    "    vec3 n = normalize(cross(dot(xa, xa) < dot(xb, xb) ? xa : xb, dot(ya, ya) < dot(yb, yb) ? ya : yb));\n"
    "    if (dot(n, pc) > 0.0) n = -n;\n"
    "    float nl = dot(n, leye.xyz);\n"
    "    float face = smoothstep(0.02, 0.2, nl);\n"
    "    if (face <= 0.0) discard;\n"
    "    float clear = 1.0;\n"
    "    if (fogp.z == 2.0) {\n"
    "        float s = clamp((length(pc) - fogp.x) * fogp.y, 0.0, 1.0), s8 = s * s;\n"
    "        s8 *= s8; s8 *= s8;\n"
    "        clear = (fogp.w > 1e-3 ? (exp(-fogp.w * s * s) - exp(-fogp.w)) / (1.0 - exp(-fogp.w)) : 1.0 - s * s) * (1.0 - s8);\n"
    "    } else if (fogp.z == 1.0) {\n"
    "        float wc = proj[0][3] * pc.x + proj[1][3] * pc.y + proj[2][3] * pc.z + proj[3][3];\n"
    "        clear = 1.0 - clamp((wc - fogp.x) * fogp.y, 0.0, 1.0);\n"
    "    }\n"
    "    if (clear <= 0.0) discard;\n"
    "    float slope = min(sqrt(max(1.0 - nl * nl, 0.0)) / max(nl, 0.05), 4.0);\n"
    "    vec3 uvz = q * 0.5 + 0.5;\n"
    "    uvz.z -= (bias + sl * tw * slope) * zs;\n"
    "    float lit = 0.0;\n"
    "    for (float j = -1.5; j < 2.0; j += 1.0)\n"
    "        for (float i = -1.5; i < 2.0; i += 1.0)\n"
    "            lit += shadow2D(smap, vec3(uvz.xy + vec2(i, j) * texel * soft, uvz.z)).r;\n"
    "    float k = (1.0 - lit / 16.0) * face * clear * (1.0 - smoothstep(0.85, 1.0, e)) * str;\n"
    "    if (k <= 0.002) discard;\n"
    "    gl_FragColor = vec4(vec3(1.0 - k), 1.0);\n"
    "}\n";

static int ds_init(void)
{
    if (!ao_procs("dynamic shadows"))
        return 0;
    ds.p_map = ao_program("dynamic shadows' map", ds_vs_map, ds_fs_map);
    ds.p_pass = ds.p_map ? ao_program("dynamic shadows", ao_vs, ds_fs) : 0;
    if (!ds.p_pass) {
        gmv_log("[shadow] dynamic shadows off\n");
        return 0;
    }
    glGenTextures(1, &ds.t);
    msf.GenFramebuffers(1, &ds.fbo);
    GLint dfb = 0, rfb = 0;
    glGetIntegerv(MS_DRAW_FB_BINDING, &dfb);
    glGetIntegerv(MS_READ_FB_BINDING, &rfb);
    glPushAttrib(GL_TEXTURE_BIT | GL_COLOR_BUFFER_BIT | GL_PIXEL_MODE_BIT);
    ao_tex(ds.t, 0x81A6, DS_SIZE, DS_SIZE, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT);   /* DEPTH_COMPONENT24 */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, 0x884C, 0x884E);     /* GL_TEXTURE_COMPARE_MODE, COMPARE_R_TO_TEXTURE */
    glTexParameteri(GL_TEXTURE_2D, 0x884D, GL_LEQUAL);  /* GL_TEXTURE_COMPARE_FUNC */
    msf.BindFramebuffer(MS_FRAMEBUFFER, ds.fbo);
    aof.FramebufferTexture2D(MS_FRAMEBUFFER, 0x8D00, GL_TEXTURE_2D, ds.t, 0);   /* GL_DEPTH_ATTACHMENT */
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    GLenum st = msf.CheckFramebufferStatus(MS_FRAMEBUFFER);
    msf.BindFramebuffer(MS_READ_FB, (GLuint)rfb);       /* first: the pop's draw buffer is the game's framebuffer's */
    msf.BindFramebuffer(MS_DRAW_FB, (GLuint)dfb);
    glPopAttrib();
    if (st != 0x8CD5) {
        gmv_log("[shadow] framebuffer incomplete (0x%x); dynamic shadows off\n", st);
        return 0;
    }
    return 1;
}

static void ds_mul(GLfloat *o, const GLfloat *a, const GLfloat *b)
{
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            o[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
}

/* The frame's light, from the sun or else the moon: 0 until the sun is known. */
static int ds_light(void)
{
    const float *m = g_sun_dir, *d = NULL;
    float len = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]), high = DS_SUN_HIGH, full = DS_STRENGTH;
    if (len < 1e-3f)
        return 0;
    if (m[2] > DS_LOW * len) {
        d = m;
        ds.source = "sun";
    } else {
        m = g_moon_dir;
        len = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
        high = DS_MOON_HIGH, full = DS_MOON_STRENGTH;
        if (len >= 1e-3f && m[2] > DS_LOW * len) {
            d = m;
            ds.source = "moon";
        }
    }
    float w = 0;
    if (d) {
        w = (d[2] / len - DS_LOW) / (high - DS_LOW);
        w = w > 1 ? 1 : w;
        w = w * w * (3 - 2 * w);
    } else {
        ds.source = "neither up";
    }
    float l[3] = {d ? w * d[0] / len : 0, d ? w * d[1] / len : 0, (d ? w * d[2] / len : 0) + 1 - w};
    float ll = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
    for (int i = 0; i < 3; i++)
        ds.dir[i] = l[i] / ll;
    ds.strength = DS_SKY_STRENGTH + (full - DS_SKY_STRENGTH) * w;
    ds.soft = DS_SKY_SOFT + (1 - DS_SKY_SOFT) * w;
    return 1;
}

static int ds_eye_near(const GLfloat *iv)
{
    float ex = iv[12] - g_cam_world[0], ey = iv[13] - g_cam_world[1], ez = iv[14] - g_cam_world[2];
    return ex * ex + ey * ey + ez * ez <= DS_EYE_NEAR * DS_EYE_NEAR;
}

#if GMV_DEV
/* A first caster that gave no view: it alone stays out of this frame's map, and the next caster is asked.  The
   camera is 0,0,0 until the game's first tick with one (nothing can be near it then), and a caster with a
   transform of its own needs its node in the table, or the eye comes out in that transform's space: its length is
   then how far the caster is, and only one within the map's reach can lose a shadow by this. */
static void ds_eye_note(const float *abs, const GLfloat *iv, int kind)
{
    int nocam = !g_cam_world[0] && !g_cam_world[1] && !g_cam_world[2];
    ds.eye_bad++;
    ds.eye_nocam += (unsigned)nocam;
    if (ds.eye_frame != g_frame_no + 1) {
        ds.eye_frame = g_frame_no + 1;
        ds.eye_frames++;
    }
    ds.eye_kind[kind & 3]++;
    if (!nocam && !abs) {
        float d = sqrtf(iv[12] * iv[12] + iv[13] * iv[13] + iv[14] * iv[14]);
        if (!ds.eye_min || d < ds.eye_min)
            ds.eye_min = d;
        if (d < DS_RADIUS * (1.0f + DS_AHEAD)) {
            ds.eye_reach++;
            if (ds.eye_reach_frame != g_frame_no + 1) {
                ds.eye_reach_frame = g_frame_no + 1;
                ds.eye_reach_frames++;
            }
        }
    }
    if (!nocam && ds.eye_said < 3) {
        ds.eye_said++;
        gmv_log("[shadow] no view from this caster (kind %d, node %s), eye at %.0f %.0f %.0f, the camera is at %.0f %.0f %.0f\n",
                kind, abs ? "known" : "not in the table", iv[12], iv[13], iv[14], g_cam_world[0], g_cam_world[1], g_cam_world[2]);
    }
}

/* What it came to, at most once a minute.  A frame that found no view at all has no shadows. */
void ds_eye_frame_end(void)
{
    if (ds.eye_frame == g_frame_no + 1 && ds.tried != ds.eye_frame) {
        ds.eye_nomap++;
        ds.eye_reach_nomap += ds.eye_reach_frame == ds.eye_frame;
    }
    if (!ds.eye_bad || GetTickCount() - ds.eye_t < 60000)
        return;
    ds.eye_t = GetTickCount();
    gmv_log("[shadow] %u casters gave no view (%u with the camera not known yet; shiny %u, characters %u, parts %u), in %u frames, %u of those frames with no shadows; within the map's reach: %u casters in %u frames, %u of them frames with no shadows; the nearest %.0f m\n",
            ds.eye_bad, ds.eye_nocam, ds.eye_kind[1], ds.eye_kind[2], ds.eye_kind[3], ds.eye_frames, ds.eye_nomap,
            ds.eye_reach, ds.eye_reach_frames, ds.eye_reach_nomap, ds.eye_min / 100.0f);
    ds.eye_bad = ds.eye_nocam = ds.eye_frames = ds.eye_nomap = 0;
    ds.eye_reach = ds.eye_reach_frames = ds.eye_reach_nomap = 0;
    ds.eye_min = 0;
    memset(ds.eye_kind, 0, sizeof ds.eye_kind);
}
#endif

/* The first caster of a frame: where the sun's map lies, from the view.  0 = no sun this frame, -1 = no view
   from this caster. */
static int ds_begin(int kind)
{
    if (!ds_light())
        return 0;
    GLfloat v[16], iv[16], lw[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, v);
    if (!ao_invert(v, iv))
        return -1;
    if (!ds_eye_near(iv)) {
        const float *abs = gmv_ds_caster();             /* a transform of its own: the rest is the view */
        GLfloat ia[16], own[16];
        if (abs && ao_invert(abs, ia)) {
            memcpy(own, v, sizeof v);
            ds_mul(v, own, ia);
        }
        if (!abs || !ao_invert(v, iv) || !ds_eye_near(iv)) {
#if GMV_DEV
            ds_eye_note(abs, iv, kind);
#else
            (void)kind;
#endif
            return -1;
        }
    }
    const float *l = ds.dir;
    float u[3] = {-l[1], l[0], 0};                      /* the map's axes: across the sun's line, Z being up */
    float ul = sqrtf(u[0] * u[0] + u[1] * u[1]);
    if (ul < 1e-3f)
        u[0] = 1, u[1] = 0, ul = 1;
    u[0] /= ul, u[1] /= ul;
    float w[3] = {l[1] * u[2] - l[2] * u[1], l[2] * u[0] - l[0] * u[2], l[0] * u[1] - l[1] * u[0]};
    float fw = g_ao_proj[11] > 0 ? 1.0f : -1.0f;        /* the way the eye looks along its own z */
    float c[3];
    for (int i = 0; i < 3; i++)
        c[i] = iv[12 + i] + iv[8 + i] * fw * DS_RADIUS * DS_AHEAD;
    float tw = 2.0f * DS_RADIUS / DS_SIZE;
    float cu = floorf((u[0] * c[0] + u[1] * c[1] + u[2] * c[2]) / tw) * tw;
    float cw = floorf((w[0] * c[0] + w[1] * c[1] + w[2] * c[2]) / tw) * tw;
    float sx = 1.0f / DS_RADIUS, sz = 1.0f / DS_DEPTH;
    memset(lw, 0, sizeof lw);
    for (int i = 0; i < 3; i++) {
        lw[i * 4] = sx * u[i];
        lw[i * 4 + 1] = sx * w[i];
        lw[i * 4 + 2] = -sz * l[i];
    }
    lw[12] = -sx * cu;
    lw[13] = -sx * cw;
    lw[14] = sz * (l[0] * c[0] + l[1] * c[1] + l[2] * c[2]);
    lw[15] = 1;
    ds_mul(ds.lvp, lw, iv);
    memcpy(ds.iv, iv, sizeof iv);
    for (int i = 0; i < 3; i++)
        ds.leye[i] = v[i] * l[0] + v[4 + i] * l[1] + v[8 + i] * l[2];
    return 1;
}

/* After a caster's draw, in the game's state: it is drawn again into the map. */
void ds_mark(int kind, GLenum mode, GLsizei count, GLenum type, const void *idx)
{
    static void(APIENTRY * de)(GLenum, GLsizei, GLenum, const void *);
    if (!ds.ready)
        ds.ready = ds_init() ? 1 : -1;
    if (!de)
        de = gl_real("glDrawElements");
    if (ds.ready < 0 || !de) {
        g_ds = 0;
        return;
    }
    if (ds.tried == g_frame_no + 1 && !ds_frame())
        return;                                         /* no sun to cast from, this frame */
    int first = ds.tried != g_frame_no + 1;
    if (first) {
        int b = ds_begin(kind);
        if (b < 0)
            return;                                     /* a later caster's may be the view */
        ds.tried = g_frame_no + 1;
        if (!b)
            return;
    }
    GLint dfb = 0, rfb = 0, prog = 0;
    glGetIntegerv(MS_DRAW_FB_BINDING, &dfb);
    glGetIntegerv(MS_READ_FB_BINDING, &rfb);
    glGetIntegerv(0x8B8D, &prog);                       /* GL_CURRENT_PROGRAM */
    glPushAttrib(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_ENABLE_BIT | GL_VIEWPORT_BIT | GL_SCISSOR_BIT);
    msf.BindFramebuffer(MS_FRAMEBUFFER, ds.fbo);
    glViewport(0, 0, DS_SIZE, DS_SIZE);
    glDepthRange(0, 1);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);                            /* a model may have no underside */
    glDisable(GL_POLYGON_OFFSET_FILL);
    if (first) {
        ds.frame = g_frame_no + 1;
        ds.n[1] = ds.n[2] = ds.n[3] = 0;
        glClearDepth(1);
        glClear(GL_DEPTH_BUFFER_BIT);
    }
    ds.n[kind]++;
    aof.UseProgram(ds.p_map);
    aof.UniformMatrix4fv(aof.GetUniformLocation(ds.p_map, "lvp"), 1, GL_FALSE, ds.lvp);
    de(mode, count, type, idx);
    aof.UseProgram((GLuint)prog);
    msf.BindFramebuffer(MS_READ_FB, (GLuint)rfb);       /* first: the pop's draw buffer is the game's framebuffer's */
    msf.BindFramebuffer(MS_DRAW_FB, (GLuint)dfb);
    glPopAttrib();
}

/* Around a caster's draw to the screen: the stencil bit marks its pixels.  Returns whether to pop. */
int ds_mask_begin(int body)
{
    if (!body || !g_ds_want || !g_ds)
        return 0;
    glPushAttrib(GL_STENCIL_BUFFER_BIT | GL_SCISSOR_BIT);
    if (ds.mask != g_frame_no + 1) {
        ds.mask = g_frame_no + 1;
        glDisable(GL_SCISSOR_TEST);
        glStencilMask(DS_BIT);
        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);
    }
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, DS_BIT, DS_BIT);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glStencilMask(DS_BIT);
    return 1;
}

void ds_mask_end(int on)
{
    if (on)
        glPopAttrib();
}

/* Inside ao_run's state save, on the frame's map; li is the layer most draws had. */
void ds_pass(int w, int h, int li)
{
    const GLfloat *P = g_ao_layer[li].m, *dr = g_ao_layer[li].dr, *fogs = g_ao_layer[li].fog;
    const GLint *vp = g_ao_layer[li].vp;
    GLfloat invp[16], fogp[4];
    if (!ao_invert(P, invp) || vp[2] <= 0 || vp[3] <= 0 || !(dr[1] > dr[0]))
        return;
    if (ssr.bad || ((w != ssr.w || h != ssr.h) && !ssr_alloc(w, h))) {
        ssr.bad = 1;
        g_ds = 0;
        return;
    }
    msf.BindFramebuffer(MS_READ_FB, g_ms_fbo);
    msf.BindFramebuffer(MS_DRAW_FB, ssr.fbo_depth);
    msf.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    ssr_fog(fogs, fogp);

    GLuint p = ds.p_pass;
    aof.UseProgram(p);
    aof.Uniform1i(aof.GetUniformLocation(p, "dep"), 0);
    aof.Uniform1i(aof.GetUniformLocation(p, "smap"), 1);
    aof.Uniform2f(aof.GetUniformLocation(p, "full"), (GLfloat)w, (GLfloat)h);
    aof.Uniform2f(aof.GetUniformLocation(p, "dr"), dr[0], dr[1]);
    aof.Uniform4f(aof.GetUniformLocation(p, "vp"), (GLfloat)vp[0], (GLfloat)vp[1], (GLfloat)vp[2], (GLfloat)vp[3]);
    aof.Uniform4f(aof.GetUniformLocation(p, "fogp"), fogp[0], fogp[1], fogp[2], fogp[3]);
    aof.Uniform4f(aof.GetUniformLocation(p, "wz"), ds.iv[2], ds.iv[6], ds.iv[10], ds.iv[14]);
    aof.UniformMatrix4fv(aof.GetUniformLocation(p, "proj"), 1, GL_FALSE, P);
    aof.UniformMatrix4fv(aof.GetUniformLocation(p, "invp"), 1, GL_FALSE, invp);
    aof.UniformMatrix4fv(aof.GetUniformLocation(p, "lvp"), 1, GL_FALSE, ds.lvp);
    aof.Uniform4f(aof.GetUniformLocation(p, "leye"), ds.leye[0], ds.leye[1], ds.leye[2], 0);
    aof.Uniform1f(aof.GetUniformLocation(p, "str"), ds.strength);
    aof.Uniform1f(aof.GetUniformLocation(p, "texel"), 1.0f / DS_SIZE);
    aof.Uniform1f(aof.GetUniformLocation(p, "soft"), ds.soft);
    aof.Uniform1f(aof.GetUniformLocation(p, "tw"), 2.0f * DS_RADIUS / DS_SIZE);
    aof.Uniform1f(aof.GetUniformLocation(p, "zs"), 0.5f / DS_DEPTH);
    aof.Uniform1f(aof.GetUniformLocation(p, "seaz"), DS_SEA_Z);
    msf.BindFramebuffer(MS_FRAMEBUFFER, g_ms_fbo);             /* the screen */
    glViewport(0, 0, w, h);
    aof.ActiveTexture(0x84C1);
    glBindTexture(GL_TEXTURE_2D, ds.t);
    aof.ActiveTexture(0x84C0);
    glBindTexture(GL_TEXTURE_2D, ssr.t_depth);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ZERO, GL_SRC_COLOR);
    aof.BlendEquation(0x8006);                                  /* GL_FUNC_ADD */
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    GLint bias = aof.GetUniformLocation(p, "bias"), sl = aof.GetUniformLocation(p, "sl"), sbits = 0;
    glGetIntegerv(GL_STENCIL_BITS, &sbits);
    if (sbits) {
        glEnable(GL_STENCIL_TEST);
        glStencilMask(0);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glStencilFunc(GL_NOTEQUAL, DS_BIT, DS_BIT);
        aof.Uniform1f(bias, DS_BIAS_WORLD);
        aof.Uniform1f(sl, 0);
        ao_tri();
        glStencilFunc(GL_EQUAL, DS_BIT, DS_BIT);
    }
    aof.Uniform1f(bias, DS_BIAS);                               /* no stencil: all of it, as a caster's */
    aof.Uniform1f(sl, 2);
    ao_tri();
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);

    static unsigned runs;
    if (runs++ % 1200 == 0)
        gmv_trace("[shadow] pass %u at %dx%d: %u shiny bodies, %u character draws, %u other parts, %s, light towards %.2f %.2f %.2f, hour %.2f, strength %.2f\n",
                runs, w, h, ds.n[1], ds.n[2], ds.n[3], ds.source, ds.dir[0], ds.dir[1], ds.dir[2], g_tod_ms / 3600000.0f, ds.strength);
}

/* the patches: the shadows are being drawn, so the game's own are to go. */
int gmv_ds_active(void)
{
    return g_ds_want && g_ds;
}
