/* survey_fog.c -- diagnostic: FOG_SURVEY */
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

/* ---- FOG_SURVEY (diagnostic): the fixed-function setups the world is drawn with ----
   A fog shader on the world has to draw each material as the fixed-function pipeline does.  Every
   perspective, depth-tested draw to the screen is described by what such a shader would have to
   reproduce (texture units and their environments, alpha test, blending, lighting, the game's own fog,
   which fog_force_begin kind it gets); each distinct setup is logged when first seen, with the game's
   call site, and a table of draws and vertices per setup every 30 s. */
#define FS_UNITS 4
#define FS_MAX   128

typedef struct {
    uint8_t prim, sky, glow, gamefog, ff, blend, atest, light, cmat, carr, narr, dmask, colsum, sprite;
    GLint bsrc, bdst, afunc, aref, fogmode, prog;
    struct {
        uint8_t on, tc, gen, texmat;
        GLint env, crgb, calpha, src[6], op[6], scale[2], ifmt;
    } u[FS_UNITS];
} fs_sig;

static struct {
    fs_sig sig[FS_MAX];
    unsigned draws[FS_MAX], frames_seen[FS_MAX], last_frame[FS_MAX];
    unsigned long long verts[FS_MAX];
    int n, full;
    DWORD next_dump;
} fsv;

static const char *fs_env(GLint e)
{
    switch (e) {
    case GL_MODULATE: return "MODULATE";
    case GL_REPLACE: return "REPLACE";
    case GL_DECAL: return "DECAL";
    case GL_BLEND: return "BLEND";
    case GL_ADD: return "ADD";
    case 0x8570: return "COMBINE";
    default: return "?";
    }
}

static void fs_log_sig(int id, void *caller)
{
    const fs_sig *s = &fsv.sig[id];
    static const char *prim[] = {"points", "lines", "tris"};
    char b[1024];
    size_t l = snprintf(b, sizeof b, "[fogsurvey] setup %d (first from game 0x%06x): %s%s%s, blend %s 0x%x/0x%x, "
                        "alpha test %s 0x%x ref %d/255, depth write %d, game fog %s mode 0x%x, forced kind %d, "
                        "lighting %d colmat %d, colour array %d normal array %d, colour sum %d sprite %d, program %d",
                        id, (unsigned)(uintptr_t)caller, prim[s->prim], s->sky ? " (sky)" : "", s->glow ? " (glow)" : "",
                        s->blend ? "on" : "off", s->bsrc, s->bdst, s->atest ? "on" : "off", s->afunc, s->aref, s->dmask,
                        s->gamefog ? "on" : "off", s->fogmode, s->ff, s->light, s->cmat, s->carr, s->narr, s->colsum,
                        s->sprite, s->prog);
    for (int u = 0; u < FS_UNITS && l < sizeof b - 200; u++) {
        if (!s->u[u].on)
            continue;
        l += snprintf(b + l, sizeof b - l, " | u%d %s ifmt 0x%x coords %d gen %d texmat %d", u, fs_env(s->u[u].env),
                      s->u[u].ifmt, s->u[u].tc, s->u[u].gen, s->u[u].texmat);
        if (s->u[u].env == 0x8570)
            l += snprintf(b + l, sizeof b - l, " rgb 0x%x(0x%x/0x%x 0x%x/0x%x 0x%x/0x%x)x%d alpha 0x%x(0x%x/0x%x 0x%x/0x%x 0x%x/0x%x)x%d",
                          s->u[u].crgb, s->u[u].src[0], s->u[u].op[0], s->u[u].src[1], s->u[u].op[1], s->u[u].src[2],
                          s->u[u].op[2], s->u[u].scale[0], s->u[u].calpha, s->u[u].src[3], s->u[u].op[3], s->u[u].src[4],
                          s->u[u].op[4], s->u[u].src[5], s->u[u].op[5], s->u[u].scale[1]);
    }
    gmv_log("%s\n", b);
}

static void fs_dump(void)
{
    gmv_log("[fogsurvey] %d setups so far%s (id: draws, vertices, frames seen in):\n", fsv.n,
            fsv.full ? ", table full" : "");
    for (int i = 0; i < fsv.n; i++)
        gmv_log("[fogsurvey]   %3d: %u draws, %llu vertices, %u frames\n", i, fsv.draws[i], fsv.verts[i],
                fsv.frames_seen[i]);
}

void fog_survey(GLenum mode, GLsizei count, int ff, void *caller)
{
    if (g_glt.last_fb || !g_ao_depth_on)
        return;
    ao_proj_refresh();
    if (!g_ao_persp)
        return;
    fs_sig s;
    memset(&s, 0, sizeof s);
    s.prim = mode == GL_POINTS ? 0 : mode <= GL_LINE_STRIP ? 1 : 2;
    s.sky = (uint8_t)g_sky_draw;
    s.glow = (uint8_t)g_sky_glow_draw;
    s.gamefog = (uint8_t)g_ao_fog_on;
    s.fogmode = g_ao_fog_on ? (GLint)g_ao_fogp[0] : 0;
    s.ff = (uint8_t)ff;
    s.blend = (uint8_t)g_blend_on;
    if (g_blend_on)
        s.bsrc = (GLint)g_blend_src, s.bdst = (GLint)g_blend_dst;
    s.dmask = (uint8_t)g_ao_dmask;
    s.atest = glIsEnabled(GL_ALPHA_TEST);
    if (s.atest) {
        GLfloat ref = 0;
        glGetIntegerv(GL_ALPHA_TEST_FUNC, &s.afunc);
        glGetFloatv(GL_ALPHA_TEST_REF, &ref);
        s.aref = (GLint)(ref * 255 + 0.5f);
    }
    s.light = glIsEnabled(GL_LIGHTING);
    s.cmat = glIsEnabled(GL_COLOR_MATERIAL);
    s.carr = glIsEnabled(GL_COLOR_ARRAY);
    s.narr = glIsEnabled(GL_NORMAL_ARRAY);
    s.colsum = glIsEnabled(0x8458);                     /* GL_COLOR_SUM */
    s.sprite = glIsEnabled(0x8861);                     /* GL_POINT_SPRITE */
    if (ff != 5)
        glGetIntegerv(0x8B8D, &s.prog);                 /* GL_CURRENT_PROGRAM */
    void(APIENTRY * act)(GLenum) = tf_active_real();
    for (unsigned u = 0; u < FS_UNITS && act; u++) {
        act(0x84C0 + u);
        if (!glIsEnabled(GL_TEXTURE_2D))
            continue;
        s.u[u].on = 1;
        s.u[u].tc = (uint8_t)((g_tf_tcarr >> u) & 1);
        s.u[u].gen = (uint8_t)(glIsEnabled(GL_TEXTURE_GEN_S) | glIsEnabled(GL_TEXTURE_GEN_T) << 1);
        GLfloat m[16];
        glGetFloatv(GL_TEXTURE_MATRIX, m);
        for (int k = 0; k < 16; k++)
            if (m[k] != (k % 5 ? 0.0f : 1.0f))
                s.u[u].texmat = 1;
        GLint tex = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
        if (tex)
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &s.u[u].ifmt);
        glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &s.u[u].env);
        if (s.u[u].env != 0x8570)                       /* GL_COMBINE */
            continue;
        glGetTexEnviv(GL_TEXTURE_ENV, 0x8571, &s.u[u].crgb);        /* GL_COMBINE_RGB */
        glGetTexEnviv(GL_TEXTURE_ENV, 0x8572, &s.u[u].calpha);      /* GL_COMBINE_ALPHA */
        for (int k = 0; k < 3; k++) {
            glGetTexEnviv(GL_TEXTURE_ENV, 0x8580 + k, &s.u[u].src[k]);      /* GL_SRC0_RGB */
            glGetTexEnviv(GL_TEXTURE_ENV, 0x8590 + k, &s.u[u].op[k]);       /* GL_OPERAND0_RGB */
            glGetTexEnviv(GL_TEXTURE_ENV, 0x8588 + k, &s.u[u].src[3 + k]);  /* GL_SRC0_ALPHA */
            glGetTexEnviv(GL_TEXTURE_ENV, 0x8598 + k, &s.u[u].op[3 + k]);   /* GL_OPERAND0_ALPHA */
        }
        GLfloat sc[2] = {1, 1};
        glGetTexEnvfv(GL_TEXTURE_ENV, 0x8573, &sc[0]);              /* GL_RGB_SCALE */
        glGetTexEnvfv(GL_TEXTURE_ENV, 0x0D1C, &sc[1]);              /* GL_ALPHA_SCALE */
        s.u[u].scale[0] = (GLint)sc[0], s.u[u].scale[1] = (GLint)sc[1];
    }
    if (act)
        act(0x84C0 + g_tf_active);
    int id = 0;
    while (id < fsv.n && memcmp(&fsv.sig[id], &s, sizeof s))
        id++;
    if (id == fsv.n) {
        if (fsv.n == FS_MAX) {
            fsv.full = 1;
            return;
        }
        memcpy(&fsv.sig[fsv.n++], &s, sizeof s);
        fs_log_sig(id, caller);
    }
    fsv.draws[id]++;
    fsv.verts[id] += (unsigned)count;
    if (fsv.last_frame[id] != g_frame_no + 1)
        fsv.last_frame[id] = g_frame_no + 1, fsv.frames_seen[id]++;
    DWORD now = GetTickCount();
    if (!fsv.next_dump)
        fsv.next_dump = now + 30000;
    else if ((int)(now - fsv.next_dump) >= 0) {
        fsv.next_dump = now + 30000;
        fs_dump();
    }
}

/* FOG_SURVEY=2: the draws lying flat at sea level and over 100 m across (the sea's chunks), with their
   extent around the camera, in one frame every 10 s.  idx NULL: glDrawArrays from vertex `first`. */
void sea_survey(GLsizei count, GLenum type, const void *idx, GLint first, int ff, void *caller)
{
    static unsigned frame, logs;
    static DWORD next;
    if (logs >= 4 || g_sky_draw || g_glt.last_fb || !g_ao_depth_on || count < 3 || (idx && type != GL_UNSIGNED_SHORT))
        return;
    DWORD now = GetTickCount();
    if (frame != g_frame_no) {
        if (frame && (int)(now - next) < 0)
            return;
        if (frame)
            logs++;
        frame = g_frame_no;
        next = now + 10000;
        if (logs >= 4)
            return;
    }
    GLint abo = 0, ebo = 0, st = 0;
    const char *p = NULL;
    glGetIntegerv(0x8894, &abo);
    glGetIntegerv(0x8895, &ebo);
    glGetIntegerv(GL_VERTEX_ARRAY_STRIDE, &st);
    glGetPointerv(GL_VERTEX_ARRAY_POINTER, (void **)&p);
    float pn = g_ao_proj[14] / (g_ao_proj[10] - 1), pf = g_ao_proj[14] / (g_ao_proj[10] + 1);
    if (abo || ebo || !p) {
        if (g_blend_on && !g_ao_dmask)
            gmv_log("[seasurvey] frame %u: %d vertices from buffers %d/%d, blend 0x%x/0x%x, kind %d, view %.0f..%.0f m, "
                    "depth %.2f..%.2f\n", g_frame_no, count, abo, ebo, g_blend_src, g_blend_dst, ff, fabsf(pn) / 100,
                    fabsf(pf) / 100, g_ao_dr[0], g_ao_dr[1]);
        return;
    }
    if (!st) st = 12;
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
    GLfloat m[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    float cx = -(m[0] * m[12] + m[1] * m[13] + m[2] * m[14]), cy = -(m[4] * m[12] + m[5] * m[13] + m[6] * m[14]);
    float rmax = 0, emin = 1e30f, emax = 0;
    const uint16_t *ix = idx;
    for (int k = 0; k < count; k++) {
        const float *v = (const float *)(p + (size_t)(ix ? ix[k] : first + k) * st);
        x0 = fminf(x0, v[0]), x1 = fmaxf(x1, v[0]), y0 = fminf(y0, v[1]), y1 = fmaxf(y1, v[1]);
        z0 = fminf(z0, v[2]), z1 = fmaxf(z1, v[2]);
        if (z0 < -100 || z1 > 100)
            return;                                     /* not flat at sea level */
        rmax = fmaxf(rmax, hypotf(v[0] - cx, v[1] - cy));
        if (k % 3 == 2) {                               /* the triangle's longest edge */
            const float *a = (const float *)(p + (size_t)(ix ? ix[k - 2] : first + k - 2) * st),
                        *b = (const float *)(p + (size_t)(ix ? ix[k - 1] : first + k - 1) * st);
            float e = fmaxf(fmaxf(hypotf(a[0] - b[0], a[1] - b[1]), hypotf(b[0] - v[0], b[1] - v[1])),
                            hypotf(v[0] - a[0], v[1] - a[1]));
            emin = fminf(emin, e), emax = fmaxf(emax, e);
        }
    }
    if (x1 - x0 < 10000 && y1 - y0 < 10000)
        return;
    float dx = fmaxf(fmaxf(x0 - cx, cx - x1), 0), dy = fmaxf(fmaxf(y0 - cy, cy - y1), 0);
    float fx = -m[2], fy = -m[6], fz = -m[10];          /* the view direction in the world */
    float yaw = atan2f(fy, fx) * 57.29578f, pitch = atan2f(fz, hypotf(fx, fy)) * 57.29578f;
    gmv_log("[seasurvey] frame %u: %d %s, blend %s 0x%x/0x%x depth write %d kind %d, x %.0f..%.0f y %.0f..%.0f z %.1f..%.1f (m); "
            "camera %.0f %.0f %.0f m looking at %.0f deg (from +x towards +y), %.0f deg up, horizontal fov %.0f deg; "
            "nearest point %.0f m away, farthest vertex %.0f m, triangles %.0f..%.0f m across; "
            "view %.0f..%.0f m, depth %.2f..%.2f, caller 0x%06x\n",
            g_frame_no, count, ix ? "indices" : "vertices", g_blend_on ? "on" : "off", g_blend_src, g_blend_dst, g_ao_dmask, ff,
            x0 / 100, x1 / 100, y0 / 100, y1 / 100, z0 / 100, z1 / 100, cx / 100, cy / 100, g_cam_z / 100, yaw, pitch,
            2 * atanf(1 / g_ao_proj[0]) * 57.29578f,
            sqrtf(dx * dx + dy * dy) / 100, rmax / 100, emin / 100, emax / 100, fabsf(pn) / 100, fabsf(pf) / 100,
            g_ao_dr[0], g_ao_dr[1], (unsigned)(uintptr_t)caller);
}
