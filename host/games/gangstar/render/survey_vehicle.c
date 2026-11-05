/* survey_vehicle.c -- diagnostic: VEHICLE_SURVEY */
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

/* The city's sea (alpha-blended, no depth writes, unit 0 texture times vertex colour, client arrays
   interleaved by 24 bytes) fills the box SEA_X0..SEA_X1 x SEA_Y0..SEA_Y1, deep blue at its edge, and
   stops there.  Near the map's edges and from the air that edge is in view well inside the fog, a
   straight line with nothing past it.  With fog on the whole world the sea goes on past the box, around
   the camera to beyond the pass's far plane, in the edge's colour and with the texture laid as the
   sea's own (fitted to the chunk's texture coordinates, so it scrolls with them).  It is drawn right
   after the first chunk of sea on the box's edge in each pass of a frame (the city's near pass and its
   low-detail far pass, each with its own depth range; drawn in the far pass alone, it was all past the
   fog's clip whenever the near pass reached beyond it), in that chunk's state and fog, in squares of
   SEA_CELL so the fog is right across each. */
#define SEA_X0   -120719.0f
#define SEA_X1    119609.0f
#define SEA_Y0   -110358.0f
#define SEA_Y1    146129.0f
#define SEA_CELL  25000.0f
#define SEA_MAX   1024                                  /* squares a frame */

/* ---- VEHICLE_SURVEY (diagnostic): how the game draws its vehicles ----
   Before reflections on the cars' paint and glass can be planned, what a car is to the renderer.  Its
   body is the two-texture setup with a 256² sphere map of the sky added on unit 1 (the characters'
   has a 64² one).  Every later draw in the frame with the modelview of a body drawn before it is taken
   for a part of that car (glass, lights, the interior, wheels if they share its placement).  Each
   distinct setup is logged once with its blending, depth, alpha test, culling, texture combine,
   textures (unit 0's alpha spread says whether it could mask a reflection), lighting and arrays, and
   the game's calls that led to it, and tinted in its number's colour (red, green, blue, yellow, cyan,
   magenta, then again).  The first bodies' first vertices are logged with their normals and both
   texture coordinates, to tell how the sphere map's coordinates are made.  A table of draws per setup
   follows every 30 s.  Needs WATER_REFLECTIONS=1 (the shader entry points and the projection's
   tracking). */
#define VS_MAX   96
#define VS_CARS  64

typedef struct {
    uint8_t body, blend, dmask, dtest, atest, cull, units, light, narr, carr, gen1;
    GLint bsrc, bdst, afunc, aref, te0, te1, crgb, calpha, src[4], op[4], tw, th, ifmt, tw1, th1;
    uint32_t caller;
} vs_sig;

static struct {
    vs_sig sig[VS_MAX];
    unsigned draws[VS_MAX];
    int n, full;
    unsigned frame;                                     /* g_frame_no + 1 of the bodies below */
    GLfloat mv[VS_CARS][16];                            /* the modelviews of this frame's bodies */
    int ncars, most, parts, warned, dumps;
    DWORD next_dump;
    GLint stex[256];                                    /* textures whose colour and alpha spread are known */
    unsigned char st[256][8];                           /* mean r, g, b, a; alpha min, max; % under 250, under 128 */
    int ns;
} vsv;

/* The colour and alpha spread of the texture bound on the active unit, from a mip level of 64² or less. */
static const unsigned char *vs_tex_stats(GLint tex)
{
    static const unsigned char none[8];
    for (int i = 0; i < vsv.ns; i++)
        if (vsv.stex[i] == tex)
            return vsv.st[i];
    if (vsv.ns == 256)
        return none;
    int i = vsv.ns++, lv = 0;
    GLint w = 0, h = 0;
    for (int k = 0; k < 14; k++) {
        GLint kw = 0, kh = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, k, GL_TEXTURE_WIDTH, &kw);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, k, GL_TEXTURE_HEIGHT, &kh);
        if (kw <= 0 || kh <= 0)
            break;
        lv = k, w = kw, h = kh;
        if (kw <= 64 && kh <= 64)
            break;
    }
    vsv.stex[i] = tex;
    memset(vsv.st[i], 0, 8);
    unsigned char *px = w > 0 && h > 0 && (long)w * h <= 2048L * 2048L ? malloc((size_t)w * h * 4) : NULL;
    if (px) {
        GLint pack = 4;
        glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, lv, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glPixelStorei(GL_PACK_ALIGNMENT, pack);
        unsigned long long s[4] = {0, 0, 0, 0};
        long n = (long)w * h, lo250 = 0, lo128 = 0;
        unsigned char amin = 255, amax = 0;
        for (long p = 0; p < n; p++) {
            for (int c = 0; c < 4; c++)
                s[c] += px[p * 4 + c];
            unsigned char a = px[p * 4 + 3];
            amin = a < amin ? a : amin;
            amax = a > amax ? a : amax;
            lo250 += a < 250;
            lo128 += a < 128;
        }
        for (int c = 0; c < 4; c++)
            vsv.st[i][c] = (unsigned char)(s[c] / (unsigned long long)n);
        vsv.st[i][4] = amin;
        vsv.st[i][5] = amax;
        vsv.st[i][6] = (unsigned char)(lo250 * 100 / n);
        vsv.st[i][7] = (unsigned char)(lo128 * 100 / n);
        free(px);
    }
    return vsv.st[i];
}

static int vs_same(const GLfloat *a, const GLfloat *b)
{
    for (int k = 0; k < 16; k++)
        if (fabsf(a[k] - b[k]) > 1e-3f * (1 + fabsf(a[k])))
            return 0;
    return 1;
}

/* Element i of a client array as floats (unnormalized), into out[0..size-1]. */
static void vs_attr(const char *p, GLint type, GLint size, GLint stride, unsigned i, float *out)
{
    int es = type == GL_FLOAT || type == GL_INT || type == GL_UNSIGNED_INT ? 4 :
             type == GL_SHORT || type == GL_UNSIGNED_SHORT ? 2 : 1;
    const char *e = p + (size_t)i * (stride ? stride : es * size);
    for (int c = 0; c < size && c < 4; c++) {
        const char *q = e + c * es;
        out[c] = type == GL_FLOAT ? *(const float *)q : type == GL_SHORT ? *(const int16_t *)q :
                 type == GL_UNSIGNED_SHORT ? *(const uint16_t *)q : type == GL_BYTE ? *(const int8_t *)q :
                 type == GL_UNSIGNED_BYTE ? *(const uint8_t *)q : type == GL_INT ? (float)*(const int32_t *)q :
                 (float)*(const uint32_t *)q;
    }
}

/* A body's modelview and its first three vertices: position, normal, both texture coordinates. */
static void vs_dump(int elems, GLint first, GLsizei count, GLenum type, const void *idx, const GLfloat *mv)
{
    static void(APIENTRY * cat)(GLenum);
    if (!cat) cat = gl_real("glClientActiveTexture");
    GLint abo = 0, ebo = 0;
    glGetIntegerv(0x8894, &abo);                        /* GL_ARRAY_BUFFER_BINDING */
    glGetIntegerv(0x8895, &ebo);                        /* GL_ELEMENT_ARRAY_BUFFER_BINDING */
    if (abo || (elems && ebo) || !cat || count < 3) {
        gmv_log("[vehsurvey]   (vertices not readable: array buffer %d, element buffer %d)\n", abo, ebo);
        return;
    }
    gmv_log("[vehsurvey]   modelview %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.1f %.1f %.1f %.1f\n",
            mv[0], mv[1], mv[2], mv[3], mv[4], mv[5], mv[6], mv[7], mv[8], mv[9], mv[10], mv[11], mv[12], mv[13],
            mv[14], mv[15]);
    GLint vs = 0, vt = 0, vst = 0, nt = 0, nst = 0, ts[2] = {0, 0}, tt[2] = {0, 0}, tst[2] = {0, 0};
    const char *vp = NULL, *np = NULL, *tp[2] = {NULL, NULL};
    glGetIntegerv(GL_VERTEX_ARRAY_SIZE, &vs);
    glGetIntegerv(GL_VERTEX_ARRAY_TYPE, &vt);
    glGetIntegerv(GL_VERTEX_ARRAY_STRIDE, &vst);
    glGetPointerv(GL_VERTEX_ARRAY_POINTER, (void **)&vp);
    if (glIsEnabled(GL_NORMAL_ARRAY)) {
        glGetIntegerv(GL_NORMAL_ARRAY_TYPE, &nt);
        glGetIntegerv(GL_NORMAL_ARRAY_STRIDE, &nst);
        glGetPointerv(GL_NORMAL_ARRAY_POINTER, (void **)&np);
    }
    for (int u = 0; u < 2; u++) {
        cat(0x84C0 + u);
        if (!glIsEnabled(GL_TEXTURE_COORD_ARRAY))
            continue;
        glGetIntegerv(GL_TEXTURE_COORD_ARRAY_SIZE, &ts[u]);
        glGetIntegerv(GL_TEXTURE_COORD_ARRAY_TYPE, &tt[u]);
        glGetIntegerv(GL_TEXTURE_COORD_ARRAY_STRIDE, &tst[u]);
        glGetPointerv(GL_TEXTURE_COORD_ARRAY_POINTER, (void **)&tp[u]);
    }
    cat(0x84C0 + g_tf_client);
    gmv_log("[vehsurvey]   arrays (pointers from the position's): position %d x 0x%x stride %d, normal 0x%x stride %d "
            "at %+ld, texcoord 0 %d x 0x%x stride %d at %+ld, texcoord 1 %d x 0x%x stride %d at %+ld (0 = off)\n", vs, vt,
            vst, nt, nst, np ? (long)(np - vp) : 0L, ts[0], tt[0], tst[0], tp[0] ? (long)(tp[0] - vp) : 0L, ts[1],
            tt[1], tst[1], tp[1] ? (long)(tp[1] - vp) : 0L);
    if (!vp)
        return;
    for (int k = 0; k < 3; k++) {
        unsigned i = !elems ? (unsigned)(first + k) : type == GL_UNSIGNED_SHORT ? ((const uint16_t *)idx)[k] :
                     type == GL_UNSIGNED_BYTE ? ((const uint8_t *)idx)[k] : ((const uint32_t *)idx)[k];
        float p[4] = {0}, n[4] = {0}, t0[4] = {0}, t1[4] = {0};
        vs_attr(vp, vt, vs, vst, i, p);
        if (np) vs_attr(np, nt, 3, nst, i, n);
        if (tp[0]) vs_attr(tp[0], tt[0], ts[0], tst[0], i, t0);
        if (tp[1]) vs_attr(tp[1], tt[1], ts[1], tst[1], i, t1);
        gmv_log("[vehsurvey]   vertex %u: position %.2f %.2f %.2f, normal %.4f %.4f %.4f, uv0 %.4f %.4f, uv1 %.4f %.4f\n",
                i, p[0], p[1], p[2], n[0], n[1], n[2], t0[0], t0[1], t1[0], t1[1]);
    }
}

/* After each draw to the screen, in the game's state (the host's own changes undone). */
void vehicle_survey(int elems, GLenum mode, GLint first, GLsizei count, GLenum type, const void *idx,
                           void *caller)
{
    static const GLfloat col[6][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0.3f, 1}, {1, 1, 0}, {0, 1, 1}, {1, 0, 1}};
    static const char *const cname[6] = {"red", "green", "blue", "yellow", "cyan", "magenta"};
    void(APIENTRY * act)(GLenum) = tf_active_real();
    if (g_glt.last_fb || count <= 0 || g_sky_draw || !act || !ao_procs("the vehicle survey"))
        return;
    ao_proj_refresh();
    if (!g_ao_persp)
        return;
    if (vsv.frame != g_frame_no + 1) {
        vsv.frame = g_frame_no + 1;
        vsv.most = vsv.ncars > vsv.most ? vsv.ncars : vsv.most;
        if (vsv.parts > 400 && !vsv.warned++)
            gmv_log("[vehsurvey] %d draws in one frame matched a car's placement: the bodies may share the world's "
                    "(pre-placed vertices), so the parts below are not only cars'\n", vsv.parts);
        vsv.ncars = vsv.parts = 0;
    }
    unsigned on = g_tf_tex2d & (g_texfix && mode != GL_POINTS ? g_tf_tcarr : 0xF) & 0xF;
    GLint tex1 = 0, tw1 = 0, th1 = 0;
    if (on & 2) {
        act(0x84C1);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex1);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw1);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th1);
    }
    int body = (on & 3) == 3 && g_te[1].mode == 0x8570 && g_te[1].crgb == GL_ADD && tw1 > 64;
    GLfloat mv[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    int car = -1;
    for (int i = 0; i < vsv.ncars && car < 0; i++)
        if (vs_same(mv, vsv.mv[i]))
            car = i;
    if (body && car < 0 && vsv.ncars < VS_CARS)
        memcpy(vsv.mv[car = vsv.ncars++], mv, sizeof mv);
    if (car < 0) {
        act(0x84C0 + g_tf_active);
        return;
    }
    vsv.parts++;
    vs_sig s;
    memset(&s, 0, sizeof s);
    s.body = (uint8_t)body;
    s.blend = (uint8_t)g_blend_on;
    if (g_blend_on)
        s.bsrc = (GLint)g_blend_src, s.bdst = (GLint)g_blend_dst;
    s.dmask = (uint8_t)g_ao_dmask;
    s.dtest = (uint8_t)g_ao_depth_on;
    s.atest = glIsEnabled(GL_ALPHA_TEST);
    if (s.atest) {
        GLfloat ref = 0;
        glGetIntegerv(GL_ALPHA_TEST_FUNC, &s.afunc);
        glGetFloatv(GL_ALPHA_TEST_REF, &ref);
        s.aref = (GLint)(ref * 255 + 0.5f);
    }
    if (glIsEnabled(GL_CULL_FACE)) {
        GLint cm = GL_BACK;
        glGetIntegerv(GL_CULL_FACE_MODE, &cm);
        s.cull = cm == GL_FRONT ? 2 : cm == GL_BACK ? 1 : 3;
    }
    s.units = (uint8_t)on;
    s.light = glIsEnabled(GL_LIGHTING);
    s.narr = glIsEnabled(GL_NORMAL_ARRAY);
    s.carr = glIsEnabled(GL_COLOR_ARRAY);
    if (on & 2) {
        s.te1 = g_te[1].mode;
        s.tw1 = tw1, s.th1 = th1;
        s.gen1 = glIsEnabled(GL_TEXTURE_GEN_S);         /* unit 1 is still the active one */
        if (s.te1 == 0x8570) {                          /* GL_COMBINE */
            s.crgb = g_te[1].crgb, s.calpha = g_te[1].calpha;
            s.src[0] = g_te[1].src[0], s.src[1] = g_te[1].src[1], s.src[2] = g_te[1].src[3], s.src[3] = g_te[1].src[4];
            s.op[0] = g_te[1].op[0], s.op[1] = g_te[1].op[1], s.op[2] = g_te[1].op[3], s.op[3] = g_te[1].op[4];
        }
    }
    GLint tex0 = 0;
    act(0x84C0);
    if (on & 1) {
        s.te0 = g_te[0].mode;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &s.tw);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &s.th);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &s.ifmt);
    }
    s.caller = (uint32_t)(uintptr_t)caller;
    int id = 0;
    while (id < vsv.n && memcmp(&vsv.sig[id], &s, sizeof s))
        id++;
    if (id == vsv.n && vsv.n < VS_MAX) {
        memcpy(&vsv.sig[vsv.n++], &s, sizeof s);
        static const unsigned char none[8];
        const unsigned char *m0 = tex0 ? vs_tex_stats(tex0) : none;
        unsigned char m1[8];
        memcpy(m1, none, sizeof m1);
        if (tex1) {
            act(0x84C1);                                /* vs_tex_stats reads the active unit's texture */
            memcpy(m1, vs_tex_stats(tex1), sizeof m1);
            act(0x84C0);
        }
        uint32_t chain[LS_CHAIN] = {0};
        game_calls((const uint32_t *)__builtin_frame_address(0), chain, LS_CHAIN);
        char b[LS_CHAIN * 12 + 1] = "", u1[256] = "", at[48] = "off";
        for (int i = 0, l = 0; i < LS_CHAIN && chain[i]; i++)
            l += snprintf(b + l, sizeof b - l, " 0x%06x", chain[i]);
        if (on & 2)
            snprintf(u1, sizeof u1, "; unit 1 env 0x%x combine 0x%x/0x%x sources 0x%x,0x%x/0x%x,0x%x operands "
                     "0x%x,0x%x/0x%x,0x%x texgen %d, texture %d %dx%d average %u,%u,%u,%u", s.te1, s.crgb, s.calpha,
                     s.src[0], s.src[1], s.src[2], s.src[3], s.op[0], s.op[1], s.op[2], s.op[3], s.gen1, tex1, tw1,
                     th1, m1[0], m1[1], m1[2], m1[3]);
        if (s.atest)
            snprintf(at, sizeof at, "func 0x%x ref %d", s.afunc, s.aref);
        gmv_log("[vehsurvey] setup %d (%s): %s, blend %s 0x%x/0x%x, depth write %d test %d, alpha test %s, cull %s, "
                "lighting %d, normals %d, colours %d, units 0x%x; unit 0 env 0x%x texture %d %dx%d format 0x%x average "
                "%u,%u,%u alpha mean %u min %u max %u, %u%% under 250, %u%% under 128%s; %d %s, car %d of this frame, "
                "the game's calls (innermost first):%s\n",
                id, cname[id % 6], body ? "a body" : "a part", s.blend ? "on" : "off", s.bsrc, s.bdst, s.dmask, s.dtest,
                at, s.cull == 1 ? "back" : s.cull == 2 ? "front" : s.cull == 3 ? "both" : "off", s.light, s.narr,
                s.carr, on, s.te0, tex0, s.tw, s.th, s.ifmt, m0[0], m0[1], m0[2], m0[3], m0[4], m0[5], m0[6], m0[7], u1,
                (int)count, elems ? "indices" : "vertices", car, b);
        if (body && vsv.dumps < 4) {
            vsv.dumps++;
            vs_dump(elems, first, count, type, idx, mv);
        }
    } else if (id == vsv.n && !vsv.full) {
        vsv.full = 1;
        gmv_log("[vehsurvey] %d setups: table full, later ones are not logged\n", VS_MAX);
    }
    act(0x84C0 + g_tf_active);
    if (id < vsv.n) {
        vsv.draws[id]++;
        bs_tint(1, col[id % 6], elems, mode, first, count, type, idx);
    }
    DWORD now = GetTickCount();
    if (!vsv.next_dump)
        vsv.next_dump = now + 30000;
    else if ((int)(now - vsv.next_dump) >= 0) {
        vsv.next_dump = now + 30000;
        char b[1024];
        size_t l = 0;
        for (int i = 0; i < vsv.n && l < sizeof b - 32; i++)
            if (vsv.draws[i])
                l += snprintf(b + l, sizeof b - l, " %d:%u", i, vsv.draws[i]);
        int mn = g_tod_ms < 0 ? 0 : g_tod_ms / 60000;
        gmv_log("[vehsurvey] draws per setup over the last 30 s, to the game's %02d:%02d, at most %d cars in a frame "
                "(setup:draws):%s\n", mn / 60, mn % 60, vsv.most, b);
        memset(vsv.draws, 0, sizeof vsv.draws);
        vsv.most = 0;
    }
}

void sea_beyond(GLsizei count, GLenum type, const void *idx)
{
    static unsigned done[2] = {~0u, ~0u};               /* by pass: the near one, the low-detail far one */
    static unsigned char edge[4] = {30, 58, 111, 255};
    static GLfloat pos[SEA_MAX * 6][3], uv[SEA_MAX * 6][2];
    static unsigned char col[SEA_MAX * 6][4];
    int pass = g_ao_dr[0] >= 0.25f;
    if (done[pass] == g_frame_no || g_sky_draw || !g_blend_on || g_blend_src != GL_SRC_ALPHA ||
        g_blend_dst != GL_ONE_MINUS_SRC_ALPHA || type != GL_UNSIGNED_SHORT || count < 3 || count > 20000)
        return;
    GLboolean dmask = 1;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask);
    if (dmask)
        return;
    GLint cat = 0, sz = 0, ty = 0, st = 0, csz = 0, cty = 0, cst = 0, tsz = 0, tty = 0, tst = 0;
    const char *p = NULL, *cp = NULL, *tp = NULL;
    glGetIntegerv(0x84E1, &cat);                        /* GL_CLIENT_ACTIVE_TEXTURE */
    if (cat != 0x84C0 || !glIsEnabled(GL_COLOR_ARRAY) || !glIsEnabled(GL_TEXTURE_COORD_ARRAY))
        return;
    glGetIntegerv(GL_VERTEX_ARRAY_SIZE, &sz);
    glGetIntegerv(GL_VERTEX_ARRAY_TYPE, &ty);
    glGetIntegerv(GL_VERTEX_ARRAY_STRIDE, &st);
    glGetPointerv(GL_VERTEX_ARRAY_POINTER, (void **)&p);
    glGetIntegerv(GL_COLOR_ARRAY_SIZE, &csz);
    glGetIntegerv(GL_COLOR_ARRAY_TYPE, &cty);
    glGetIntegerv(GL_COLOR_ARRAY_STRIDE, &cst);
    glGetPointerv(GL_COLOR_ARRAY_POINTER, (void **)&cp);
    glGetIntegerv(GL_TEXTURE_COORD_ARRAY_SIZE, &tsz);
    glGetIntegerv(GL_TEXTURE_COORD_ARRAY_TYPE, &tty);
    glGetIntegerv(GL_TEXTURE_COORD_ARRAY_STRIDE, &tst);
    glGetPointerv(GL_TEXTURE_COORD_ARRAY_POINTER, (void **)&tp);
    p = cl_ptr(0x8896, p, NULL);                        /* GL_VERTEX_ARRAY_BUFFER_BINDING */
    cp = cl_ptr(0x8898, cp, NULL);                      /* GL_COLOR_ARRAY_BUFFER_BINDING */
    tp = cl_ptr(0x889A, tp, NULL);                      /* GL_TEXTURE_COORD_ARRAY_BUFFER_BINDING (unit 0) */
    const uint16_t *ix = (const uint16_t *)cl_ptr(0x8895, idx, NULL);
    if (!ix || !p || !cp || !tp || sz != 3 || ty != GL_FLOAT || st != 24 || csz != 4 || cty != GL_UNSIGNED_BYTE ||
        cst != 24 || tsz != 2 || tty != GL_FLOAT || tst != 24)
        return;
    /* Flat at sea level, on the box's edge, and three corners to fit the texture to. */
    int lo = -1, hi = -1, on_edge = -1;
    for (int k = 0; k < count; k++) {
        const float *v = (const float *)(p + (size_t)ix[k] * 24);
        if (fabsf(v[2]) > 1)
            return;
        if (lo < 0 || v[0] < ((const float *)(p + (size_t)lo * 24))[0]) lo = ix[k];
        if (hi < 0 || v[0] > ((const float *)(p + (size_t)hi * 24))[0]) hi = ix[k];
        if (on_edge < 0 && (fabsf(v[0] - SEA_X0) < 2 || fabsf(v[0] - SEA_X1) < 2 || fabsf(v[1] - SEA_Y0) < 2 ||
                            fabsf(v[1] - SEA_Y1) < 2))
            on_edge = ix[k];
    }
    if (on_edge < 0)
        return;
    const float *a = (const float *)(p + (size_t)lo * 24), *b = (const float *)(p + (size_t)hi * 24);
    double ex = b[0] - a[0], ey = b[1] - a[1], best = 0;
    int third = -1;
    for (int k = 0; k < count; k++) {
        const float *v = (const float *)(p + (size_t)ix[k] * 24);
        double area = fabs(ex * (v[1] - a[1]) - ey * (v[0] - a[0]));
        if (area > best) best = area, third = ix[k];
    }
    if (third < 0 || best < 1e6)
        return;
    const float *c = (const float *)(p + (size_t)third * 24);
    const float *ta = (const float *)(tp + (size_t)lo * 24), *tb = (const float *)(tp + (size_t)hi * 24),
                *tc = (const float *)(tp + (size_t)third * 24);
    double dx1 = b[0] - a[0], dy1 = b[1] - a[1], dx2 = c[0] - a[0], dy2 = c[1] - a[1];
    double det = dx1 * dy2 - dy1 * dx2, fit[2][3];
    for (int j = 0; j < 2; j++) {
        double d1 = tb[j] - ta[j], d2 = tc[j] - ta[j];
        fit[j][0] = (d1 * dy2 - dy1 * d2) / det;
        fit[j][1] = (dx1 * d2 - d1 * dx2) / det;
        fit[j][2] = ta[j] - fit[j][0] * a[0] - fit[j][1] * a[1];
    }
    for (int k = 0; k < count; k += count / 8 + 1) {   /* the texture must lie flat across the sea */
        const float *v = (const float *)(p + (size_t)ix[k] * 24), *t = (const float *)(tp + (size_t)ix[k] * 24);
        for (int j = 0; j < 2; j++)
            if (fabs(fit[j][0] * v[0] + fit[j][1] * v[1] + fit[j][2] - t[j]) > 0.01)
                return;
    }
    memcpy(edge, cp + (size_t)on_edge * 24, 4);
    done[pass] = g_frame_no;
    /* The camera, and how far this pass sees (corners of the screen included). */
    GLfloat m[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    float camx = -(m[0] * m[12] + m[1] * m[13] + m[2] * m[14]), camy = -(m[4] * m[12] + m[5] * m[13] + m[6] * m[14]);
    float far1 = fabsf(g_ao_proj[14] / (g_ao_proj[10] + 1)), far2 = fabsf(g_ao_proj[14] / (g_ao_proj[10] - 1));
    float r = 1.5f * (far1 > far2 ? far1 : far2);
    if (!(r > 0) || r > 2e6f)
        return;
    float x0 = camx - r, x1 = camx + r, y0 = camy - r, y1 = camy + r;
    float rect[4][4] = {{x0, SEA_X0, y0, y1}, {SEA_X1, x1, y0, y1},
                        {x0 > SEA_X0 ? x0 : SEA_X0, x1 < SEA_X1 ? x1 : SEA_X1, y0, SEA_Y0},
                        {x0 > SEA_X0 ? x0 : SEA_X0, x1 < SEA_X1 ? x1 : SEA_X1, SEA_Y1, y1}};
    int n = 0;
    for (int q = 0; q < 4; q++) {
        float *R = rect[q];
        if (R[1] <= R[0] || R[3] <= R[2])
            continue;
        int nx = (int)ceilf((R[1] - R[0]) / SEA_CELL), ny = (int)ceilf((R[3] - R[2]) / SEA_CELL);
        for (int i = 0; i < nx; i++)
            for (int j = 0; j < ny && n < SEA_MAX; j++) {
                float qx0 = R[0] + (R[1] - R[0]) * i / nx, qx1 = R[0] + (R[1] - R[0]) * (i + 1) / nx;
                float qy0 = R[2] + (R[3] - R[2]) * j / ny, qy1 = R[2] + (R[3] - R[2]) * (j + 1) / ny;
                const float corner[6][2] = {{qx0, qy0}, {qx1, qy0}, {qx1, qy1}, {qx0, qy0}, {qx1, qy1}, {qx0, qy1}};
                for (int k = 0; k < 6; k++) {
                    GLfloat *P = pos[n * 6 + k];
                    P[0] = corner[k][0], P[1] = corner[k][1], P[2] = 0;
                    for (int t = 0; t < 2; t++)
                        uv[n * 6 + k][t] = (GLfloat)(fit[t][0] * P[0] + fit[t][1] * P[1] + fit[t][2]);
                    memcpy(col[n * 6 + k], edge, 4);
                }
                n++;
            }
    }
    if (!n)
        return;
    static void(APIENTRY * draw)(GLenum, GLint, GLsizei);
    static void(APIENTRY * client_tex)(GLenum);
    if (!draw) draw = gl_real("glDrawArrays");
    if (!client_tex) client_tex = gl_real("glClientActiveTexture");
    if (!draw || !client_tex)
        return;
    GLint abo = 0;
    glGetIntegerv(0x8894, &abo);                        /* GL_ARRAY_BUFFER_BINDING */
    glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
    if (abo)
        cl_array_buffer(0);                             /* the squares are client arrays */
    for (int u = 3; u >= 0; u--) {
        client_tex(0x84C0 + u);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(0x845E);                       /* GL_SECONDARY_COLOR_ARRAY */
    glDisableClientState(0x8457);                       /* GL_FOG_COORD_ARRAY */
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, pos);
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, col);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    draw(GL_TRIANGLES, 0, n * 6);
    glPopClientAttrib();
    if (abo)                                            /* the pop restores it too; the game's own bind cache relies on it */
        cl_array_buffer((GLuint)abo);
    client_tex(0x84C0);
    static int logged;
    if (!logged++)
        gmv_trace("[fog] the sea goes on past the map's edge (%d squares now, colour %u %u %u)\n", n, edge[0], edge[1],
                edge[2]);
}
