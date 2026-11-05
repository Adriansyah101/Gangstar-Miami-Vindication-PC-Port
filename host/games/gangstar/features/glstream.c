/* glstream.c -- VBO_STREAM, an experiment, OFF (dev build only: GMV_VBO_STREAM=1).
   A/B run 2026-10-30 (cache\vbo_ab.log): in game 104 fps off vs 66 fps on; ~9k draws/s, ~51 KB of
   vertices a draw, 39 us a draw through glBufferSubData vs 11.5 us through the driver's own
   client-array path.  Kept for reference; the real gain would need the static meshes uploaded once.

   Most of the game's draws take their vertices and indices from client arrays (plain memory), as
   OpenGL ES 1 code does.  The driver then copies them on the CPU inside every glDrawElements, which
   showed up as GL driver time in the PERF_PROFILE runs.  Here such a draw is fed from two stream
   buffer objects instead: the vertices it uses (lowest to highest index) and its indices are copied
   in with glBufferSubData, the array pointers aimed at them for the one draw, and everything put back
   as it was right after, so the rest of the port (fog, bloom, reflections, which read the game's
   pointers) sees the same state.  The picture is the same; only where the GPU reads the data from
   changes.  A buffer that fills up is orphaned (glBufferData with no data) and refilled from its
   start.  Only plain GL 1.5 buffer calls are used, which every GPU of this port's era has.

   The game's array state is shadowed from its own calls (pointers, buffer bindings, enabled arrays);
   a draw with any of its arrays or its indices in the game's own buffers, or with an array kind the
   shadow does not cover, is passed on unchanged.

   PERF_PROFILE adds a line every 5 s (draws, how many were streamed, time inside the draws);
   GMV_VBO_AB=1 (dev) switches streaming on and off every 5 s window to compare them in one run. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"
#include <GL/gl.h>

#include "log.h"
#include "shims.h"
#include "gangstar.h"

#if GMV_DEV
#define GS_ARRAY 0x8892                                 /* GL_ARRAY_BUFFER */
#define GS_ELEMENT 0x8893                               /* GL_ELEMENT_ARRAY_BUFFER */
#define GS_STREAM_DRAW 0x88E0
#define GS_VB_SIZE (8u << 20)
#define GS_IB_SIZE (2u << 20)
#define GS_UNKNOWN 0xFFFFFFFFu                          /* the array's buffer was deleted under it */

typedef void(APIENTRY *bind_fn)(GLenum, GLuint);
typedef void(APIENTRY *gen_fn)(GLsizei, GLuint *);
typedef void(APIENTRY *del_fn)(GLsizei, const GLuint *);
typedef void(APIENTRY *data_fn)(GLenum, ptrdiff_t, const void *, GLenum);
typedef void(APIENTRY *sub_fn)(GLenum, ptrdiff_t, ptrdiff_t, const void *);
typedef void(APIENTRY *unit_fn)(GLenum);
typedef void(APIENTRY *de_fn)(GLenum, GLsizei, GLenum, const void *);
typedef void(APIENTRY *da_fn)(GLenum, GLint, GLsizei);

static struct {
    bind_fn bind;
    gen_fn gen;
    del_fn del;
    data_fn data;
    sub_fn sub;
    unit_fn unit;
} gf;

static void *gs_real(const char *name, const char *alt)
{
    void *p = gl_real(name);
    if (!p && alt) p = gl_real(alt);
    return p;
}

/* ---- the game's array state ---- */
enum { GA_V, GA_C, GA_N, GA_T, GA_COUNT = GA_T + 8 };   /* texcoords: one per client unit */
static struct ga {
    GLint size;
    GLenum type;
    GLsizei stride;
    const char *ptr;
    GLuint buf;                                         /* the array buffer bound when it was set */
} g_ga[GA_COUNT];
static unsigned g_on_bits;                              /* enabled arrays (1 << GA_*) */
static unsigned g_other_on;                             /* other array kinds enabled (never streamed) */
static unsigned g_unit;                                 /* the game's client active texture unit */
static GLuint g_abuf, g_ebuf;                           /* the game's array / element buffer bindings */

/* ---- switches and stream buffers ---- */
static int g_want = -1;                                 /* the wrappers wanted, read on first use */
static int g_stream;                                    /* GMV_VBO_STREAM */
static unsigned g_hooked;                               /* which state wrappers the game got */
#define GS_ALL_HOOKS 0x3Fu
static int g_ready;                                     /* 1 = buffers made, -1 = failed (off for good) */
static int g_on;                                        /* streaming now */
static int g_ab;                                        /* GMV_VBO_AB: flip every PERF_PROFILE window */
static GLuint g_vb, g_ib;
static size_t g_vb_off, g_ib_off;
static unsigned char *g_tmp;                            /* rebased indices */
static size_t g_tmp_cap;

/* ---- PERF_PROFILE stats ---- */
enum { GS_DONE, GS_OFF, GS_VBO, GS_EBO, GS_OTHER, GS_BIG, GS_NR };
static int g_stats = -1;
static struct {
    unsigned draws, why[GS_NR], orphans;
    unsigned long long bytes;
    long long qpc;
} g_st;

int gmv_gls_want(void)
{
    if (g_want < 0) {
        const char *e = gmv_dbgenv("GMV_VBO_STREAM");   /* off: slower than the driver's own path (A/B run) */
        g_stream = e && *e && *e != '0';
#if GMV_DEV
        const char *ab = gmv_dbgenv("GMV_VBO_AB");
        g_ab = g_stream && ab && *ab && *ab != '0';
#endif
        g_want = g_stream || gmv_perf_on();            /* PERF_PROFILE: the wrappers count the draws' kinds */
    }
    return g_want;
}

static void gs_note(int a, GLint size, GLenum type, GLsizei stride, const void *ptr)
{
    g_ga[a].size = size;
    g_ga[a].type = type;
    g_ga[a].stride = stride;
    g_ga[a].ptr = ptr;
    g_ga[a].buf = g_abuf;
}

/* The game's calls (cdecl), passed on to opengl32 after noting them. */
static void gs_VertexPointer(GLint size, GLenum type, GLsizei stride, const void *ptr)
{
    gs_note(GA_V, size, type, stride, ptr);
    glVertexPointer(size, type, stride, ptr);
}
static void gs_ColorPointer(GLint size, GLenum type, GLsizei stride, const void *ptr)
{
    gs_note(GA_C, size, type, stride, ptr);
    glColorPointer(size, type, stride, ptr);
}
static void gs_TexCoordPointer(GLint size, GLenum type, GLsizei stride, const void *ptr)
{
    if (g_unit < 8)
        gs_note(GA_T + (int)g_unit, size, type, stride, ptr);
    glTexCoordPointer(size, type, stride, ptr);
}
static void gs_NormalPointer(GLenum type, GLsizei stride, const void *ptr)
{
    gs_note(GA_N, 3, type, stride, ptr);
    glNormalPointer(type, stride, ptr);
}
static void gs_BindBuffer(GLenum target, GLuint b)
{
    if (!gf.bind) gf.bind = (bind_fn)gs_real("glBindBuffer", "glBindBufferARB");
    if (target == GS_ARRAY) g_abuf = b;
    else if (target == GS_ELEMENT) g_ebuf = b;
    if (gf.bind) gf.bind(target, b);
}
static void gs_DeleteBuffers(GLsizei n, const GLuint *ids)
{
    if (!gf.del) gf.del = (del_fn)gs_real("glDeleteBuffers", "glDeleteBuffersARB");
    for (GLsizei i = 0; ids && i < n; i++) {
        if (!ids[i]) continue;
        if (g_abuf == ids[i]) g_abuf = 0;               /* GL drops the bindings of a deleted buffer */
        if (g_ebuf == ids[i]) g_ebuf = 0;
        for (int a = 0; a < GA_COUNT; a++)
            if (g_ga[a].buf == ids[i]) g_ga[a].buf = GS_UNKNOWN;
    }
    if (gf.del) gf.del(n, ids);
}

void *gmv_gls_intercept(const char *name)
{
    static const struct { const char *n; void *f; unsigned bit; } t[] = {
        {"_glVertexPointer", (void *)gs_VertexPointer, 1},
        {"_glColorPointer", (void *)gs_ColorPointer, 2},
        {"_glTexCoordPointer", (void *)gs_TexCoordPointer, 4},
        {"_glNormalPointer", (void *)gs_NormalPointer, 8},
        {"_glBindBuffer", (void *)gs_BindBuffer, 16},
        {"_glBindBufferARB", (void *)gs_BindBuffer, 16},
        {"_glDeleteBuffers", (void *)gs_DeleteBuffers, 32},
        {"_glDeleteBuffersARB", (void *)gs_DeleteBuffers, 32},
        {NULL, NULL, 0},
    };
    if (!gmv_gls_want())
        return NULL;
    for (int i = 0; t[i].n; i++)
        if (!strcmp(t[i].n, name)) {
            g_hooked |= t[i].bit;
            return t[i].f;
        }
    return NULL;
}

/* From the GL shim's wrappers of glClientActiveTexture and gl{En,Dis}ableClientState. */
void gmv_gls_client_unit(unsigned u)
{
    g_unit = u;
}
void gmv_gls_client_state(GLenum a, int on)
{
    unsigned bit;
    switch (a) {
    case GL_VERTEX_ARRAY: bit = 1u << GA_V; break;
    case GL_COLOR_ARRAY: bit = 1u << GA_C; break;
    case GL_NORMAL_ARRAY: bit = 1u << GA_N; break;
    case GL_TEXTURE_COORD_ARRAY:
        if (g_unit < 8) { bit = 1u << (GA_T + g_unit); break; }
        /* fall through */
    default:                                            /* edge flag, index, secondary colour, fog coord */
        bit = a == 0x8077 ? 1u : a == 0x8079 ? 2u : a == 0x845E ? 4u : a == 0x8457 ? 8u : 16u << (g_unit & 7);
        if (on) g_other_on |= bit; else g_other_on &= ~bit;
        return;
    }
    if (on) g_on_bits |= bit; else g_on_bits &= ~bit;
}

static int gs_init(void)
{
    if (g_ready)
        return g_ready > 0;
    g_ready = -1;
    if (g_hooked != GS_ALL_HOOKS) {
        gmv_log("[vbo] the game's array calls are not all wrapped (0x%x); the draws are passed on as they are\n", g_hooked);
        return 0;
    }
    if (!gf.bind) gf.bind = (bind_fn)gs_real("glBindBuffer", "glBindBufferARB");
    gf.gen = (gen_fn)gs_real("glGenBuffers", "glGenBuffersARB");
    gf.data = (data_fn)gs_real("glBufferData", "glBufferDataARB");
    gf.sub = (sub_fn)gs_real("glBufferSubData", "glBufferSubDataARB");
    gf.unit = (unit_fn)gs_real("glClientActiveTexture", "glClientActiveTextureARB");
    if (!gf.bind || !gf.gen || !gf.data || !gf.sub || !gf.unit) {
        gmv_log("[vbo] no buffer objects in this GL; the draws are passed on as they are\n");
        return 0;
    }
    GLuint b[2] = {0, 0};
    while (glGetError() != GL_NO_ERROR) {}
    gf.gen(2, b);
    gf.bind(GS_ARRAY, b[0]);
    gf.data(GS_ARRAY, GS_VB_SIZE, NULL, GS_STREAM_DRAW);
    gf.bind(GS_ELEMENT, b[1]);
    gf.data(GS_ELEMENT, GS_IB_SIZE, NULL, GS_STREAM_DRAW);
    gf.bind(GS_ARRAY, g_abuf);
    gf.bind(GS_ELEMENT, g_ebuf);
    GLenum err = glGetError();
    if (!b[0] || !b[1] || err != GL_NO_ERROR) {
        gmv_log("[vbo] stream buffers not made (GL error 0x%x); the draws are passed on as they are\n", err);
        return 0;
    }
    g_vb = b[0];
    g_ib = b[1];
    g_ready = 1;
    gmv_log("[vbo] VBO_STREAM: client-array draws fed from stream buffers (%u MB vertices, %u MB indices)%s\n",
            GS_VB_SIZE >> 20, GS_IB_SIZE >> 20, g_ab ? "; GMV_VBO_AB: on and off by turns every 5 s" : "");
    return 1;
}

static int gs_esize(const struct ga *g)
{
    int ts;
    switch (g->type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: ts = 1; break;
    case GL_SHORT: case GL_UNSIGNED_SHORT: case 0x140B: ts = 2; break;      /* HALF_FLOAT */
    case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: case 0x140C: ts = 4; break;   /* FIXED */
    case GL_DOUBLE: ts = 8; break;
    default: return 0;
    }
    int n = g->size == 0x80E1 ? 4 : g->size;            /* GL_BGRA */
    return n >= 1 && n <= 4 ? n * ts : 0;
}

/* Aim array a at p (a client pointer or an offset into the bound array buffer). */
static void gs_point(int a, const void *p, int *unit_moved)
{
    const struct ga *g = &g_ga[a];
    if (a == GA_V) glVertexPointer(g->size, g->type, g->stride, p);
    else if (a == GA_C) glColorPointer(g->size, g->type, g->stride, p);
    else if (a == GA_N) glNormalPointer(g->type, g->stride, p);
    else {
        gf.unit(0x84C0 + (GLenum)(a - GA_T));
        *unit_moved = 1;
        glTexCoordPointer(g->size, g->type, g->stride, p);
    }
}

static int gs_stream(void *real, int elems, GLenum mode, GLint first, GLsizei count, GLenum type, const void *idx)
{
    unsigned on = g_on_bits;
    if (!(on & 1u << GA_V) || g_other_on)
        return GS_OTHER;
    if (elems && g_ebuf)
        return GS_EBO;
    for (int a = 0; a < GA_COUNT; a++)
        if ((on & 1u << a) && g_ga[a].buf)
            return GS_VBO;
    if (!g_on)
        return GS_OFF;                                  /* client arrays, not streamed */
    if (!gs_init())
        return GS_OFF;

    /* the vertices the draw uses */
    unsigned lo = 0xFFFFFFFFu, hi = 0;
    int ts = 0;
    if (elems) {
        if (!idx)
            return GS_OTHER;
        if (type == GL_UNSIGNED_SHORT) {
            const uint16_t *ix = idx;
            ts = 2;
            for (GLsizei k = 0; k < count; k++) {
                unsigned v = ix[k];
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
        } else if (type == GL_UNSIGNED_BYTE) {
            const uint8_t *ix = idx;
            ts = 1;
            for (GLsizei k = 0; k < count; k++) {
                unsigned v = ix[k];
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
        } else if (type == GL_UNSIGNED_INT) {
            const uint32_t *ix = idx;
            ts = 4;
            for (GLsizei k = 0; k < count; k++) {
                unsigned v = ix[k];
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
        } else
            return GS_OTHER;
        if ((size_t)count * ts > GS_IB_SIZE / 4)
            return GS_BIG;
    } else {
        if (first < 0)
            return GS_OTHER;
        lo = (unsigned)first;
        hi = (unsigned)first + (unsigned)count - 1;
    }
    if (hi - lo > (1u << 20))
        return GS_BIG;

    /* each array's bytes, gathered into runs of memory copied once (interleaved arrays share one) */
    struct { uintptr_t lo, hi; int a; } r[GA_COUNT];
    int n = 0;
    for (int a = 0; a < GA_COUNT; a++) {
        if (!(on & 1u << a))
            continue;
        const struct ga *g = &g_ga[a];
        int es = gs_esize(g);
        if (!es || !g->ptr)
            return GS_OTHER;
        size_t st = g->stride ? (size_t)g->stride : (size_t)es;
        uintptr_t b = (uintptr_t)g->ptr + lo * st;
        int k = n++;
        while (k > 0 && r[k - 1].lo > b) { r[k] = r[k - 1]; k--; }     /* sorted by start */
        r[k].lo = b;
        r[k].hi = (uintptr_t)g->ptr + hi * st + (size_t)es;
        r[k].a = a;
    }
    struct { uintptr_t lo, hi; size_t off; } c[GA_COUNT];
    int nc = 0;
    int in_c[GA_COUNT];
    for (int i = 0; i < n; i++) {
        if (nc && r[i].lo <= c[nc - 1].hi + 64) {
            if (r[i].hi > c[nc - 1].hi) c[nc - 1].hi = r[i].hi;
        } else {
            c[nc].lo = r[i].lo;
            c[nc].hi = r[i].hi;
            nc++;
        }
        in_c[i] = nc - 1;
    }
    size_t need = 0;
    for (int k = 0; k < nc; k++) {
        c[k].lo &= ~(uintptr_t)15;                      /* 16-byte steps never leave the page */
        c[k].hi = (c[k].hi + 15) & ~(uintptr_t)15;
        need += c[k].hi - c[k].lo;
    }
    if (need > GS_VB_SIZE / 4)
        return GS_BIG;

    /* copy the vertices in and aim the arrays at them */
    gf.bind(GS_ARRAY, g_vb);
    if (g_vb_off + need > GS_VB_SIZE) {
        gf.data(GS_ARRAY, GS_VB_SIZE, NULL, GS_STREAM_DRAW);
        g_vb_off = 0;
        g_st.orphans++;
    }
    for (int k = 0; k < nc; k++) {
        c[k].off = g_vb_off;
        gf.sub(GS_ARRAY, (ptrdiff_t)g_vb_off, (ptrdiff_t)(c[k].hi - c[k].lo), (const void *)c[k].lo);
        g_vb_off += c[k].hi - c[k].lo;
    }
    int moved = 0;
    for (int i = 0; i < n; i++) {
        size_t off = c[in_c[i]].off + (r[i].lo - c[in_c[i]].lo);
        gs_point(r[i].a, (const void *)off, &moved);
    }
    g_st.bytes += need;

    if (elems) {
        size_t isz = (size_t)count * ts;
        g_ib_off = (g_ib_off + 3) & ~(size_t)3;
        gf.bind(GS_ELEMENT, g_ib);
        if (g_ib_off + isz > GS_IB_SIZE) {
            gf.data(GS_ELEMENT, GS_IB_SIZE, NULL, GS_STREAM_DRAW);
            g_ib_off = 0;
            g_st.orphans++;
        }
        const void *src = idx;
        if (lo) {                                       /* indices from the first vertex copied */
            if (g_tmp_cap < isz) {
                unsigned char *t = realloc(g_tmp, isz);
                if (t) { g_tmp = t; g_tmp_cap = isz; }
            }
            if (g_tmp_cap >= isz) {
                if (ts == 2) {
                    const uint16_t *s = idx; uint16_t *d = (uint16_t *)g_tmp;
                    for (GLsizei k = 0; k < count; k++) d[k] = (uint16_t)(s[k] - lo);
                } else if (ts == 1) {
                    const uint8_t *s = idx; uint8_t *d = g_tmp;
                    for (GLsizei k = 0; k < count; k++) d[k] = (uint8_t)(s[k] - lo);
                } else {
                    const uint32_t *s = idx; uint32_t *d = (uint32_t *)g_tmp;
                    for (GLsizei k = 0; k < count; k++) d[k] = s[k] - lo;
                }
                src = g_tmp;
            } else
                src = NULL;
        }
        if (src) {
            gf.sub(GS_ELEMENT, (ptrdiff_t)g_ib_off, (ptrdiff_t)isz, src);
            ((de_fn)real)(mode, count, type, (const void *)g_ib_off);
            g_ib_off += isz;
            g_st.bytes += isz;
        }
        gf.bind(GS_ELEMENT, g_ebuf);
        if (!src) {                                     /* out of memory: draw it the game's way */
            gf.bind(GS_ARRAY, 0);
            for (int i = 0; i < n; i++)
                gs_point(r[i].a, g_ga[r[i].a].ptr, &moved);
            ((de_fn)real)(mode, count, type, idx);
            goto done;
        }
    } else
        ((da_fn)real)(mode, 0, count);

    /* put the game's pointers back */
    gf.bind(GS_ARRAY, 0);
    for (int i = 0; i < n; i++)
        gs_point(r[i].a, g_ga[r[i].a].ptr, &moved);
done:
    if (moved)
        gf.unit(0x84C0 + g_unit);
    if (g_abuf)
        gf.bind(GS_ARRAY, g_abuf);
    return GS_DONE;
}

/* Draw as the game asked (real = opengl32's glDrawElements when elems, else glDrawArrays), from the
   stream buffers when the draw can be; allow = 0 when the caller has changed the arrays itself. */
void gmv_gls_draw(void *real, int elems, GLenum mode, GLint first, GLsizei count, GLenum type, const void *idx,
                  int allow)
{
    if (g_stats < 0) {                                  /* the first draw */
        g_stats = gmv_perf_on() ? 1 : 0;
        gmv_gls_want();
        g_on = g_stream && !g_ab;                       /* A/B starts with a window off */
    }
    long long t0 = g_stats ? gmv_prof_now() : 0;
    int why = !allow || count <= 0 ? GS_OTHER
              : !g_on && !g_stats ? GS_OFF : gs_stream(real, elems, mode, first, count, type, idx);
    if (why != GS_DONE) {
        if (elems)
            ((de_fn)real)(mode, count, type, idx);
        else
            ((da_fn)real)(mode, first, count);
    }
    if (g_stats) {
        g_st.draws++;
        g_st.why[why]++;
        g_st.qpc += gmv_prof_now() - t0;
    }
}

/* PERF_PROFILE, every 5 s (loadprof.c's perf_tick). */
void gmv_glstream_report(double seconds)
{
    if (g_stats <= 0 || seconds <= 0)
        return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    double ms = (double)g_st.qpc * 1000.0 / (double)f.QuadPart;
    gmv_log("[perf]   vbo stream %s: %.0f draws a second, %.0f streamed (%.0f KB a second), passed on: client arrays %.0f, "
            "game buffers %.0f, game index buffer %.0f, other %.0f, too big %.0f; %.1f ms a second inside the draws "
            "(%.2f us a draw); %u orphaned\n",
            g_on && g_ready > 0 ? "ON" : "off", g_st.draws / seconds, g_st.why[GS_DONE] / seconds,
            (double)g_st.bytes / 1024.0 / seconds, g_st.why[GS_OFF] / seconds, g_st.why[GS_VBO] / seconds,
            g_st.why[GS_EBO] / seconds, g_st.why[GS_OTHER] / seconds, g_st.why[GS_BIG] / seconds, ms / seconds,
            g_st.draws ? ms * 1000.0 / g_st.draws : 0.0, g_st.orphans);
    memset(&g_st, 0, sizeof g_st);
    if (g_ab && g_ready >= 0)
        g_on = !g_on;
}
#else
/* A release build: no experiment, the draws pass straight on. */
typedef void(APIENTRY *de_fn)(GLenum, GLsizei, GLenum, const void *);
typedef void(APIENTRY *da_fn)(GLenum, GLint, GLsizei);

int gmv_gls_want(void)
{
    return 0;
}
void *gmv_gls_intercept(const char *name)
{
    (void)name;
    return NULL;
}
void gmv_gls_client_unit(unsigned u)
{
    (void)u;
}
void gmv_gls_client_state(GLenum a, int on)
{
    (void)a;
    (void)on;
}
void gmv_gls_draw(void *real, int elems, GLenum mode, GLint first, GLsizei count, GLenum type, const void *idx,
                  int allow)
{
    (void)allow;
    if (elems)
        ((de_fn)real)(mode, count, type, idx);
    else
        ((da_fn)real)(mode, first, count);
}
#endif
