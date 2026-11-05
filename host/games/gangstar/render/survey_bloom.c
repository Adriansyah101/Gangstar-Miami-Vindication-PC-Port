/* survey_bloom.c -- diagnostic: BLOOM_SURVEY */
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

/* ---- BLOOM_SURVEY (diagnostic): how the game draws its lights ----
   The bloom only sees the finished picture.  To find what a glow source is to the renderer, the draws
   that could be one are tinted on the screen (drawn again over themselves, the tint carrying the
   texture's alpha) and each distinct setup is logged with its texture's average colour and the game's
   call site:
     magenta  additive blending (destination factor ONE): halos, light cones, flares
     cyan     unit 1 added onto unit 0 (the lit windows' setup): unit 1's layer alone, cyan times its colour
     yellow   other blending without depth writes (alpha-blended billboards, glass, the sea, smoke)
     green    any 3D draw after the bloom pass has already run this frame (never bloomed)
     blue     additive blending under a flat projection (screen-space flares or HUD)
   A table of draws per setup follows every 30 s.  Needs BLOOM=1 (BLOOM_INTENSITY=0 keeps the tints
   from glowing themselves). */
#define BS_MAX 512

typedef struct {
    uint8_t cls, persp, blend, dmask, dtest, units, light, shaded;
    GLint bsrc, bdst, tex, tw, th, ifmt, tex1, tw1, th1;
    uint32_t caller;
} bs_sig;

static struct {
    bs_sig sig[BS_MAX];
    unsigned draws[BS_MAX];
    int n, full;
    DWORD next_dump;
    GLint mtex[512];                                    /* textures whose average colour is known */
    unsigned char mcol[512][4];
    int nm;
} bsv;

static const char *const bs_name[] = {"", "additive", "windows", "blended", "late", "flat additive"};

/* The average colour of the texture bound on the active unit, from a mip level of 16 texels or less. */
static const unsigned char *bs_tex_mean(GLint tex)
{
    static const unsigned char none[4];
    for (int i = 0; i < bsv.nm; i++)
        if (bsv.mtex[i] == tex)
            return bsv.mcol[i];
    if (bsv.nm == 512)
        return none;
    int i = bsv.nm++, lv = 0;
    GLint w = 0, h = 0;
    for (int k = 0; k < 14; k++) {
        GLint kw = 0, kh = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, k, GL_TEXTURE_WIDTH, &kw);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, k, GL_TEXTURE_HEIGHT, &kh);
        if (kw <= 0 || kh <= 0)
            break;
        lv = k, w = kw, h = kh;
        if (kw <= 16 && kh <= 16)
            break;
    }
    bsv.mtex[i] = tex;
    memset(bsv.mcol[i], 0, 4);
    unsigned char *px = w > 0 && h > 0 && (long)w * h <= 2048L * 2048L ? malloc((size_t)w * h * 4) : NULL;
    if (px) {
        GLint pack = 4;
        glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, lv, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glPixelStorei(GL_PACK_ALIGNMENT, pack);
        unsigned long long s[4] = {0, 0, 0, 0};
        for (long p = 0; p < (long)w * h; p++)
            for (int c = 0; c < 4; c++)
                s[c] += px[p * 4 + c];
        for (int c = 0; c < 4; c++)
            bsv.mcol[i][c] = (unsigned char)(s[c] / ((unsigned long long)w * h));
        free(px);
    }
    return bsv.mcol[i];
}

/* The draw again over itself in the class's colour (or rgb), additively, with the texture's alpha. */
void bs_tint(int cls, const GLfloat *rgb, int elems, GLenum mode, GLint first, GLsizei count, GLenum type,
                    const void *idx)
{
    static const GLfloat tint[][3] = {{0, 0, 0}, {1, 0, 1}, {0, 1, 1}, {1, 1, 0}, {0, 1, 0}, {0.2f, 0.4f, 1}};
    static void(APIENTRY * da)(GLenum, GLint, GLsizei);
    static void(APIENTRY * de)(GLenum, GLsizei, GLenum, const void *);
    if (!da) da = gl_real("glDrawArrays");
    if (!de) de = gl_real("glDrawElements");
    void(APIENTRY * act)(GLenum) = tf_active_real();
    if (!act || !da || !de || !aof.UseProgram || !aof.BlendEquation)
        return;                                         /* the GL 2 entry points come with the bloom's setup */
    GLint prog = 0;
    glGetIntegerv(0x8B8D, &prog);                       /* GL_CURRENT_PROGRAM */
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_CURRENT_BIT | GL_TEXTURE_BIT |
                 GL_LIGHTING_BIT | GL_POLYGON_BIT);
    glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
    if (prog)
        aof.UseProgram(0);
    int tex0 = mode != GL_POINTS && (g_tf_tex2d & g_tf_tcarr & 1);
    /* The windows: only unit 1's added layer is tinted (cyan times its colour), so the tint shows where
       the light is. */
    int layer = cls == 2 && mode != GL_POINTS && (g_tf_tex2d & g_tf_tcarr & 2);
    for (unsigned u = 1; u < 4; u++) {
        act(0x84C0 + u);
        if (u == 1 && layer) {
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, 0x8570);          /* GL_COMBINE */
            glTexEnvi(GL_TEXTURE_ENV, 0x8571, GL_MODULATE);                  /* rgb: previous x texture */
            glTexEnvi(GL_TEXTURE_ENV, 0x8580, 0x8578);                       /* GL_PREVIOUS */
            glTexEnvi(GL_TEXTURE_ENV, 0x8590, GL_SRC_COLOR);
            glTexEnvi(GL_TEXTURE_ENV, 0x8581, GL_TEXTURE);
            glTexEnvi(GL_TEXTURE_ENV, 0x8591, GL_SRC_COLOR);
            glTexEnvi(GL_TEXTURE_ENV, 0x8572, GL_REPLACE);                   /* alpha: previous */
            glTexEnvi(GL_TEXTURE_ENV, 0x8588, 0x8578);
            glTexEnvi(GL_TEXTURE_ENV, 0x8598, GL_SRC_ALPHA);
            glTexEnvf(GL_TEXTURE_ENV, 0x8573, 1.0f);
            glTexEnvf(GL_TEXTURE_ENV, 0x0D1C, 1.0f);
        } else {
            glDisable(GL_TEXTURE_2D);
        }
    }
    act(0x84C0);
    if (tex0) {
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, 0x8570);              /* GL_COMBINE */
        glTexEnvi(GL_TEXTURE_ENV, 0x8571, GL_REPLACE);                       /* rgb: the primary colour */
        glTexEnvi(GL_TEXTURE_ENV, 0x8580, 0x8577);                           /* GL_PRIMARY_COLOR */
        glTexEnvi(GL_TEXTURE_ENV, 0x8590, GL_SRC_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, 0x8572, layer ? GL_REPLACE : GL_MODULATE); /* alpha: (texture x) primary */
        glTexEnvi(GL_TEXTURE_ENV, 0x8588, layer ? 0x8577 : GL_TEXTURE);
        glTexEnvi(GL_TEXTURE_ENV, 0x8598, GL_SRC_ALPHA);
        glTexEnvi(GL_TEXTURE_ENV, 0x8589, 0x8577);
        glTexEnvi(GL_TEXTURE_ENV, 0x8599, GL_SRC_ALPHA);
        glTexEnvf(GL_TEXTURE_ENV, 0x8573, 1.0f);
        glTexEnvf(GL_TEXTURE_ENV, 0x0D1C, 1.0f);
    } else {
        glDisable(GL_TEXTURE_2D);
    }
    /* The hybrid fog's shader drew the world with a vertex stage that need not give the fixed
       pipeline's depths exactly; pulled forward a little, the tint is not lost to them. */
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -8.0f);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glDisable(0x8458);                                  /* GL_COLOR_SUM */
    glDisableClientState(GL_COLOR_ARRAY);
    if (!rgb)
        rgb = tint[cls];
    glColor4f(rgb[0], rgb[1], rgb[2], 0.55f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    aof.BlendEquation(0x8006);                          /* GL_FUNC_ADD */
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_LEQUAL);
    if (elems)
        de(mode, count, type, idx);
    else
        da(mode, first, count);
    glPopClientAttrib();
    glPopAttrib();
    act(0x84C0 + g_tf_active);
    if (prog)
        aof.UseProgram((GLuint)prog);
}

/* After each draw to the screen, in the game's state (the host's own changes undone).  elems 0:
   glDrawArrays from vertex `first`; shaded: the hybrid fog's shader drew it. */
void bloom_survey(int elems, GLenum mode, GLint first, GLsizei count, GLenum type, const void *idx, int shaded,
                         void *caller)
{
    if (g_glt.last_fb || count <= 0 || (g_blend_on && g_blend_src == GL_ZERO && g_blend_dst == GL_ONE))
        return;                                         /* ZERO/ONE draws nothing (the game's texture warm-up) */
    ao_proj_refresh();
    unsigned on = g_tf_tex2d & (g_texfix && mode != GL_POINTS ? g_tf_tcarr : 0xF) & 0xF;
    int add = g_blend_on && g_blend_dst == GL_ONE, cls;
    if (!g_ao_persp)
        cls = add ? 5 : 0;
    else if (g_ao_done)
        cls = 4;
    else if (add)
        cls = 1;
    else if ((on & 3) == 3 && g_te[1].mode == 0x8570 && g_te[1].crgb == GL_ADD)
        cls = 2;
    else if (g_blend_on && !g_ao_dmask)
        cls = 3;
    else
        cls = 0;
    if (!cls)
        return;
    bs_sig s;
    memset(&s, 0, sizeof s);
    s.cls = (uint8_t)cls;
    s.persp = (uint8_t)g_ao_persp;
    s.blend = (uint8_t)g_blend_on;
    if (g_blend_on)
        s.bsrc = (GLint)g_blend_src, s.bdst = (GLint)g_blend_dst;
    s.dmask = (uint8_t)g_ao_dmask;
    s.dtest = (uint8_t)g_ao_depth_on;
    s.units = (uint8_t)on;
    s.light = glIsEnabled(GL_LIGHTING);
    s.shaded = (uint8_t)shaded;
    s.caller = (uint32_t)(uintptr_t)caller;
    void(APIENTRY * act)(GLenum) = tf_active_real();
    if (act && (on & 2)) {
        act(0x84C1);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex1);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &s.tw1);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &s.th1);
    }
    if (act && (on & 1)) {
        act(0x84C0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &s.tw);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &s.th);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &s.ifmt);
    }
    int id = 0;
    while (id < bsv.n && memcmp(&bsv.sig[id], &s, sizeof s))
        id++;
    if (id == bsv.n && bsv.n < BS_MAX) {
        memcpy(&bsv.sig[bsv.n++], &s, sizeof s);
        const unsigned char *m = s.tex ? bs_tex_mean(s.tex) : NULL;
        char u1[160] = "";
        if ((on & 3) == 3) {
            unsigned char m1[4] = {0, 0, 0, 0};
            if (s.tex1) {
                act(0x84C1);                            /* bs_tex_mean reads the active unit's texture */
                memcpy(m1, bs_tex_mean(s.tex1), 4);
                act(0x84C0);
            }
            snprintf(u1, sizeof u1, ", unit 1 env 0x%x combine 0x%x/0x%x texture %d %dx%d average %u,%u,%u,%u",
                     g_te[1].mode, g_te[1].crgb, g_te[1].calpha, s.tex1, s.tw1, s.th1, m1[0], m1[1], m1[2], m1[3]);
        }
        gmv_log("[bloomsurvey] setup %d: %s, %s, blend %s 0x%x/0x%x, depth write %d test %d, lighting %d, fog shader %d, "
                "units 0x%x%s, unit 0 texture %d %dx%d format 0x%x average %u,%u,%u,%u, %d vertices, from game 0x%06x\n",
                id, bs_name[cls], s.persp ? "3D" : "flat", s.blend ? "on" : "off", s.bsrc, s.bdst, s.dmask, s.dtest,
                s.light, s.shaded, on, u1, s.tex, s.tw, s.th, s.ifmt, m ? m[0] : 0, m ? m[1] : 0, m ? m[2] : 0,
                m ? m[3] : 0, (int)count, s.caller);
    } else if (id == bsv.n && !bsv.full) {
        bsv.full = 1;
        gmv_log("[bloomsurvey] %d setups: table full, later ones are tinted but not logged\n", BS_MAX);
    }
    if (act)
        act(0x84C0 + g_tf_active);
    if (id < bsv.n)
        bsv.draws[id]++;
    bs_tint(cls, NULL, elems, mode, first, count, type, idx);
    DWORD now = GetTickCount();
    if (!bsv.next_dump)
        bsv.next_dump = now + 30000;
    else if ((int)(now - bsv.next_dump) >= 0) {
        bsv.next_dump = now + 30000;
        char b[1024];
        size_t l = 0;
        for (int i = 0; i < bsv.n && l < sizeof b - 32; i++)
            if (bsv.draws[i])
                l += snprintf(b + l, sizeof b - l, " %d:%u", i, bsv.draws[i]);
        gmv_log("[bloomsurvey] draws per setup over the last 30 s (setup:draws):%s\n", b);
        memset(bsv.draws, 0, sizeof bsv.draws);
    }
}

/* BLOOM_SURVEY=4: each kind of light (an additive draw BLOOM_LIGHTS puts in the glow layer) logged once,
   with its texture, its size and the game's calls that led to it, and tinted in the colour of its number
   (red, green, blue, yellow, cyan, magenta, then again), so a light seen on screen can be found in the
   log.  The calls are the return addresses into the game found on the stack: each word in the game's
   code range just after a call instruction.  A table of draws per light follows every 30 s. */
#define LS_MAX 128

typedef struct {
    GLint tex, tw, th;
    GLsizei count;
    GLenum mode;
    uint8_t dmask;
    uint32_t chain[LS_CHAIN];
} ls_sig;

static struct {
    ls_sig sig[LS_MAX];
    unsigned draws[LS_MAX];
    int n, full;
    DWORD next_dump;
} lsv;

/* The bytes readable from p on without leaving its run of committed, readable pages; asked of
   VirtualQuery, since a probe that faults (IsBadReadPtr) goes to the host's crash handler first. */
static size_t readable_from(const void *p)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                         PAGE_EXECUTE_WRITECOPY)))
        return 0;
    return (size_t)((const char *)mbi.BaseAddress + mbi.RegionSize - (const char *)p);
}

/* Whether the game page holding a is readable (the first, the Mach-O header's, is not), asked once a page. */
static int game_page_ok(uint32_t a)
{
    static int8_t ok[0x63d000 >> 12];
    int8_t *p = &ok[a >> 12];
    if (!*p)
        *p = readable_from((const void *)(uintptr_t)(a & ~0xfffu)) >= 0x1000 ? 1 : -1;
    return *p > 0;
}

int game_calls(const uint32_t *sp, uint32_t *out, int max)
{
    int n = 0, words = (int)(readable_from(sp) / sizeof *sp);
    for (int k = 0; k < 768 && k < words && n < max; k++) {
        uint32_t v = sp[k];
        if (v < 0x1010 || v >= 0x63d000)                /* the game's __TEXT */
            continue;
        if (!game_page_ok(v - 6) || !game_page_ok(v - 1))
            continue;
        const unsigned char *c = (const unsigned char *)(uintptr_t)v;
        if (c[-5] == 0xE8 || (c[-2] == 0xFF && (c[-1] & 0x38) == 0x10) ||
            (c[-3] == 0xFF && (c[-2] & 0x38) == 0x10) || (c[-6] == 0xFF && (c[-5] & 0x38) == 0x10))
            out[n++] = v;
    }
    return n;
}

void light_survey(int elems, GLenum mode, GLint first, GLsizei count, GLenum type, const void *idx)
{
    static const GLfloat col[6][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0.3f, 1}, {1, 1, 0}, {0, 1, 1}, {1, 0, 1}};
    static const char *const cname[6] = {"red", "green", "blue", "yellow", "cyan", "magenta"};
    if (!bg.light || count <= 0)
        return;
    ls_sig s;
    memset(&s, 0, sizeof s);
    s.count = count;
    s.mode = mode;
    s.dmask = (uint8_t)g_ao_dmask;
    game_calls((const uint32_t *)__builtin_frame_address(0), s.chain, LS_CHAIN);
    void(APIENTRY * act)(GLenum) = tf_active_real();
    const unsigned char *m = NULL;
    if (act) {
        act(0x84C0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &s.tw);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &s.th);
    }
    int id = 0;
    while (id < lsv.n && memcmp(&lsv.sig[id], &s, sizeof s))
        id++;
    if (id == lsv.n && lsv.n < LS_MAX) {
        memcpy(&lsv.sig[lsv.n++], &s, sizeof s);
        if (act && s.tex)
            m = bs_tex_mean(s.tex);
        char b[LS_CHAIN * 12 + 1] = "";
        for (int i = 0, l = 0; i < LS_CHAIN && s.chain[i]; i++)
            l += snprintf(b + l, sizeof b - l, " 0x%06x", s.chain[i]);
        gmv_log("[bloomsurvey] light %d (%s): texture %d %dx%d average %u,%u,%u,%u, blend 0x%x/ONE, depth write %d, "
                "%s of %d, the game's calls (innermost first):%s\n",
                id, cname[id % 6], s.tex, s.tw, s.th, m ? m[0] : 0, m ? m[1] : 0, m ? m[2] : 0, m ? m[3] : 0,
                g_blend_src, s.dmask, elems ? "indices" : "vertices", (int)count, b);
    } else if (id == lsv.n && !lsv.full) {
        lsv.full = 1;
        gmv_log("[bloomsurvey] %d lights: table full, later ones are not logged\n", LS_MAX);
    }
    if (act)
        act(0x84C0 + g_tf_active);
    if (id < lsv.n) {
        lsv.draws[id]++;
        bs_tint(1, col[id % 6], elems, mode, first, count, type, idx);
    }
    DWORD now = GetTickCount();
    if (!lsv.next_dump)
        lsv.next_dump = now + 30000;
    else if ((int)(now - lsv.next_dump) >= 0) {
        lsv.next_dump = now + 30000;
        char b[1024];
        size_t l = 0;
        for (int i = 0; i < lsv.n && l < sizeof b - 32; i++)
            if (lsv.draws[i])
                l += snprintf(b + l, sizeof b - l, " %d:%u", i, lsv.draws[i]);
        int mn = g_tod_ms < 0 ? 0 : g_tod_ms / 60000;
        gmv_log("[bloomsurvey] draws per light over the last 30 s, to the game's %02d:%02d (light:draws):%s\n",
                mn / 60, mn % 60, b);
        memset(lsv.draws, 0, sizeof lsv.draws);
    }
}
