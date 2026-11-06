/* bbfast.c -- the billboard turn 0x131f1e with its range tests before the mesh lookup, same results.

   0x131f1e(node) turns flat scene elements to face the camera once a frame (called only from the
   scene node method 0x1315bc, vtable 0x656b78 slot 4, while the byte at 0x641111 is set).  PERF_PROFILE
   runs (2026-10-30) put it at ~3.6% of a tick, most of it in the element loop: for every element
   (820-2256 a frame) it first follows ~7 dependent loads to the element's mesh and takes a reference,
   and only then tests the element's box against the camera; ~60% fail and just drop the reference.

   The function, as read (camera = *(*(*(*0x6853a0 + 0x17aa4) + 0x1c) + 0xe4), fetched again before
   each use; nothing at all without a camera or without a group):
     M = node+0x118 (reference taken); P = camera vt+0xa0(), T = vt+0x108() (float* x, y);
     d = T - P (x, y; z = 0), normalized unless zero; F = vt+0x120() (float); r2 = 1.5 * F * F
     groups: node+0x1c4..+0x1c8, 12-byte {begin, end, cap} vectors of element pointers; per element:
       mesh = lookup(M, element+4), reference taken
       box = *(element+0xc) (min xyz, max xyz), c = (min + max) * 0.5
       |c - P|^2 (x, y) >= r2, or (c - P) . d < 0: skip
       data = map 0x517b10(mesh+8 -> +0x14 vertex buffer, 5); vertices at data + mesh+8 -> +0x18,
       stride u16 at +0x22, element's range u16 +0x24 (first) .. +0x26 (end); face normal from the
       first three; angle = acosf(clamp(normal . d, -1, 1)); |angle| >= 0.01: signed by the cross
       product's sign, sinf/cosf, every vertex of the range turned about c in x/y
       unmap (+0x13 map count, vt+0x18 when GL-mapped); mesh reference dropped
     M reference dropped
   The mesh lookup only reads, and its result is used only by elements that pass the tests; the
   reference it takes is dropped again at once (the mesh is held by M, so it never reaches zero).  So
   this copy runs the tests first and looks the mesh up only for the elements that pass: the same
   calls (camera, map/unmap, acosf/sinf/cosf, the game's own stubs) in the same order with the same
   arguments, and the same writes.  The float work is the original's, operation for operation, in SSE
   single precision (the file is built with fpmath=sse; max/min/sqrt by their SSE instructions).

   Dev: GMV_BB_VERIFY=1 in the environment (with GMV_PERF_PROFILE=1) runs this copy, notes what it wrote, puts the old values
   back and runs the original on the same frame, with its map and sinf calls (0x13223a, 0x1323eb)
   recorded; the buffers mapped, the angles and every value written are compared bit for bit.
   GMV_FAST_BILLBOARD=0 keeps the original. */

#include "compat.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"

#ifndef __clang__                       /* clang takes SSE for floats when -msse2 is on, and has no such pragma */
#pragma GCC target("sse2,fpmath=sse")
#endif

#define BB_FN       0x00131f1eu   /* the billboard turn(node)                                     */
#define BB_FN_END   0x001325d0u
#define BB_FN_FNV   0xa43f962au   /* FNV-1a of its bytes, the six call fields below left out       */
#define BB_CALLER   0x001315dau   /* its only call (in 0x1315bc)                                  */
#define BB_SITE_MAP 0x0013223au   /* call 0x517b10 map                                            */
#define BB_SITE_SIN 0x001323ebu   /* call 0x5eeeae sinf stub                                      */
#define BB_MAP      0x00517b10u   /* IBuffer::map(buffer, access) -> data                         */
#define BB_ACOSF    0x005ee986u   /* the game's stubs                                             */
#define BB_SINF     0x005eeeaeu
#define BB_COSF     0x005ee9dau
#define BB_APP      0x006853a0u   /* -> the object holding the camera chain                        */
#define BB_GETAPP   0x000031b0u   /* returns *BB_APP; low code, so the image loader leads its four  */
static const uint32_t bb_getapp_sites[4] = { 0x131f30u, 0x131f76u, 0x131fa8u, 0x13203du };
                                  /* calls here to its copy (+GMV_LOW_DELTA, gmv_patch.bin)         */

#define AT(p, o, T) (*(T *)((uint8_t *)(p) + (o)))
#define U16(p, o) AT(p, o, uint16_t)
#define I32(p, o) AT(p, o, int32_t)
#define U32(p, o) AT(p, o, uint32_t)
#define PTR(p, o) AT(p, o, uint8_t *)
#define VT(o, off) (*(void **)(*(uint8_t **)(o) + (off)))

typedef const float *(*bb_vecfn)(void *);
typedef float (*bb_ffn)(void *);
typedef uint8_t *(*bb_mapfn)(void *, int);
typedef void (*bb_vfn)(void *);
typedef float (*bb_mathfn)(float);

static void (*g_orig)(void *);           /* trampoline: the original's first instructions, then back */
static bb_mapfn g_fwd_map;               /* what the original's map / sinf calls led to (verify) */
static bb_mathfn g_fwd_sin;
static int g_on, g_verify;

static struct {
    unsigned calls, fallback, checked, bad, rewritten;
    unsigned long long elems, passed, turned, verts;
} g_st;
static unsigned g_checked_all, g_bad_all, g_logged;   /* since the start */

/* GMV_BB_VERIFY records */
typedef struct { float *p; uint32_t ox, oy, nx, ny; } bb_write;
typedef struct { void **a; int n, cap; } bb_plist;
typedef struct { uint32_t *a; int n, cap; } bb_ulist;
static bb_write *g_w;
static int g_nw, g_wcap, g_wfull;
static uint32_t *g_seen;                 /* open-addressing set of vertex addresses (last write wins) */
static int g_seencap;
static bb_plist g_maps_mine, g_maps_orig, g_data_mine, g_data_orig;
static bb_ulist g_ang_mine, g_ang_orig;
static int g_rec_orig;                   /* the original is running under the check */

static int bb_grow(void **a, int *cap, int need, size_t sz)
{
    if (need <= *cap)
        return 1;
    int c = *cap ? *cap * 2 : 1024;
    while (c < need)
        c *= 2;
    void *n = realloc(*a, (size_t)c * sz);
    if (!n)
        return 0;
    *a = n;
    *cap = c;
    return 1;
}

static void bb_padd(bb_plist *l, void *p)
{
    if (bb_grow((void **)&l->a, &l->cap, l->n + 1, sizeof *l->a))
        l->a[l->n] = p;
    l->n++;                              /* counted even when the record could not grow */
}

static void bb_uadd(bb_ulist *l, uint32_t v)
{
    if (bb_grow((void **)&l->a, &l->cap, l->n + 1, sizeof *l->a))
        l->a[l->n] = v;
    l->n++;
}

static uint32_t bb_bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* The original's sqrtss, and its maxss/minss: a > b ? a : b and a < b ? a : b (NaN: b). */
static inline float bb_sqrt(float x)
{
    float r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}

static inline float bb_max(float a, float b)
{
    __asm__("maxss %1, %0" : "+x"(a) : "x"(b));
    return a;
}

static inline float bb_min(float a, float b)
{
    __asm__("minss %1, %0" : "+x"(a) : "x"(b));
    return a;
}

static uint8_t *bb_cam(void)
{
    uint8_t *s = *(uint8_t **)(uintptr_t)BB_APP;
    return PTR(PTR(PTR(s, 0x17aa4), 0x1c), 0xe4);
}

static void bb_release(uint8_t *o)
{
    if (--I32(o, 4) == 0) {
        ((bb_vfn)VT(o, 8))(o);
        ((bb_vfn)VT(o, 4))(o);
    }
}

/* One element that passed the tests: c = its box center, d = the camera direction. */
static void bb_element(uint8_t *m, uint8_t *e, const float *b, float cx, float cy, float dx, float dy,
                       float dz, float dz0, int rec)
{
    float cz = (b[2] + b[5]) * 0.5f;
    uint32_t lim = U16(e, 0x26), first = U16(e, 0x24);

    /* the mesh, as the original looks it up */
    uint32_t idx = U32(e, 4);
    uint8_t *tbl = PTR(m, 8), *ent = PTR(m, 0x14), *arr = PTR(m, 0x20);
    uint32_t k = U16(arr, U32(ent, idx * 8) * 0x14 + 0xc) + U32(ent, idx * 8 + 4);
    k *= U32(m, 0x70);
    uint8_t *q = PTR(tbl, k);
    uint32_t j = U32(tbl, k + 4);
    q = *(uint8_t **)q;
    uint8_t *mesh = PTR(arr, U32(q, j * 8) * 0x14);
    if (mesh)
        I32(mesh, 4)++;

    uint8_t *mb = PTR(mesh, 8);
    void *vb = PTR(mb, 0x14);
    if (rec)
        bb_padd(&g_maps_mine, vb);
    uint8_t *data = ((bb_mapfn)(uintptr_t)BB_MAP)(vb, 5);
    if (rec)
        bb_padd(&g_data_mine, data);
    uint32_t stride = U16(mb, 0x22), off0 = U32(mb, 0x18);
    const float *v0 = (const float *)(data + (stride * first + off0));
    const float *v1 = (const float *)(data + (stride * (first + 1) + off0));
    const float *v2 = (const float *)(data + (stride * (first + 2) + off0));

    float a = v0[2] - v1[2];
    float bz = v2[2] - v1[2];
    float n1 = (v0[0] - v1[0]) * bz - (v2[0] - v1[0]) * a;
    float n0 = (v2[1] - v1[1]) * a - (v0[1] - v1[1]) * bz;
    float l = n0 * n0 + n1 * n1;
    l = l + 0.0f;
    float nx = -n0, ny = -n1, nz;
    if (l != 0.0f) {
        float inv = 1.0f / bb_sqrt(l);
        nz = inv * 0.0f;
        ny = ny * inv;
        nx = nx * inv;
    } else
        nz = 0.0f;
    float dot = nz * dz + (dx * nx + dy * ny);
    float ang = ((bb_mathfn)(uintptr_t)BB_ACOSF)(bb_min(bb_max(-1.0f, dot), 1.0f));

    if (!(0.01f > __builtin_fabsf(ang))) {
        float cr = (nx * dy - ny * dx) + dz0;
        if (0.0f > cr)
            ang = -ang;
        if (rec)
            bb_uadd(&g_ang_mine, bb_bits(ang));
        float s = ((bb_mathfn)(uintptr_t)BB_SINF)(ang);
        float c = ((bb_mathfn)(uintptr_t)BB_COSF)(ang);
        g_st.turned++;
        if (lim > first) {
            float kz = cz * -0.0f;
            float ty = ((cy - cx * s) - cy * c) + kz;
            float tx = ((cx - cx * c) + cy * s) + kz;
            float ms = -s;
            uint32_t end = (first + 1 > lim ? first + 1 : lim) - 1;
            for (uint32_t v = first;; v++) {
                float *w = (float *)(data + ((uint32_t)U16(mb, 0x22) * v + off0));
                float x = w[0], y = w[1];
                float nxv = (x * c + y * ms) + tx;
                float nyv = (x * s + y * c) + ty;
                if (rec) {
                    if (bb_grow((void **)&g_w, &g_wcap, g_nw + 1, sizeof *g_w)) {
                        bb_write *r = &g_w[g_nw++];
                        r->p = w;
                        memcpy(&r->ox, &w[0], 4);
                        memcpy(&r->oy, &w[1], 4);
                        r->nx = bb_bits(nxv);
                        r->ny = bb_bits(nyv);
                    } else
                        g_wfull = 1;
                }
                w[0] = nxv;
                w[1] = nyv;
                g_st.verts++;
                if (v == end)
                    break;
            }
        }
    }

    if ((uint32_t)(uintptr_t)data + off0) {      /* unmap */
        uint8_t *bf = PTR(mb, 0x14);
        uint8_t st = bf[0x13], n = st & 0x1f;
        if (n >= 2)
            bf[0x13] = (uint8_t)((st & 0xe0) | (n - 1));
        else {
            if (bf[0x12] & 0x20)
                ((bb_vfn)VT(bf, 0x18))(bf);
            bf[0x13] = 0;
        }
    }
    if (mesh)
        bb_release(mesh);
}

static void bb_turn(uint8_t *n, int rec)
{
    if (!bb_cam())
        return;
    if (U32(n, 0x1c8) + 0xb - U32(n, 0x1c4) < 0x17)
        return;
    uint8_t *m = PTR(n, 0x118);
    if (m)
        I32(m, 4)++;

    uint8_t *cam = bb_cam();
    const float *p = ((bb_vecfn)VT(cam, 0xa0))(cam);
    float px = p[0], py = p[1];
    cam = bb_cam();
    const float *t = ((bb_vecfn)VT(cam, 0x108))(cam);
    float dy = t[1] - py, dx = t[0] - px;
    float l = dx * dx + dy * dy;
    l = l + 0.0f;
    float dz = 0.0f;
    if (l != 0.0f) {
        float inv = 1.0f / bb_sqrt(l);
        dx = dx * inv;
        dy = dy * inv;
        dz = 0.0f * inv;
    }
    cam = bb_cam();
    float f = ((bb_ffn)VT(cam, 0x120))(cam);

    g_st.calls++;
    if (U32(n, 0x1c8) - U32(n, 0x1c4) + 0xb > 0x16) {
        float r2 = (1.5f * f) * f;
        float dz0 = dz * 0.0f;
        uint32_t count = 0, off = 4;
        for (;;) {
            uint32_t base = U32(n, 0x1c4);
            uint8_t **gb = *(uint8_t ***)(uintptr_t)(base + off - 4);
            uint8_t **ge = *(uint8_t ***)(uintptr_t)(base + off);
            count++;
            if ((uint32_t)((uint8_t *)ge - (uint8_t *)gb) > 3) {
                uint32_t i = 0;
                do {
                    uint8_t *e = gb[i];
                    i++;
                    g_st.elems++;
                    const float *b = *(const float **)(e + 0xc);
                    float cy = (b[1] + b[4]) * 0.5f;
                    float ey = cy - py;
                    float cx = (b[0] + b[3]) * 0.5f;
                    float ex = cx - px;
                    float d2 = ex * ex + ey * ey;
                    if (!(d2 >= r2) && !(0.0f > ex * dx + ey * dy)) {
                        g_st.passed++;
                        bb_element(m, e, b, cx, cy, dx, dy, dz, dz0, rec);
                    }
                    base = U32(n, 0x1c4);
                    gb = *(uint8_t ***)(uintptr_t)(base + off - 4);
                    ge = *(uint8_t ***)(uintptr_t)(base + off);
                } while ((uint32_t)((int32_t)((uint8_t *)ge - (uint8_t *)gb) >> 2) > i);
            }
            int32_t ng = (int32_t)(U32(n, 0x1c8) - U32(n, 0x1c4)) / 12;
            off += 12;
            if (!((uint32_t)ng > count))
                break;
        }
    }
    if (m)
        bb_release(m);
}

/* ---- GMV_BB_VERIFY ---- */

__attribute__((cdecl, force_align_arg_pointer))
static uint8_t *bbv_map(void *vb, int access)
{
    if (!g_rec_orig)
        return g_fwd_map(vb, access);
    bb_padd(&g_maps_orig, vb);
    uint8_t *d = g_fwd_map(vb, access);
    bb_padd(&g_data_orig, d);
    return d;
}

__attribute__((cdecl, force_align_arg_pointer))
static float bbv_sin(float a)
{
    if (g_rec_orig)
        bb_uadd(&g_ang_orig, bb_bits(a));
    return g_fwd_sin(a);
}

/* At most 8 lines a call and 80 in all, so later calls that differ still show; every one is counted. */
static int g_call_bad, g_call_lines;

static void bb_mismatch(const char *what, int i, uint32_t mine, uint32_t orig, uint32_t old)
{
    g_call_bad = 1;
    if (g_call_lines < 8 && g_logged < 80) {
        g_call_lines++;
        g_logged++;
        gmv_log("[bbfast] MISMATCH in call %u: %s [%d] mine 0x%08x original 0x%08x (before 0x%08x)\n",
                g_checked_all, what, i, mine, orig, old);
    }
}

/* 1 if p was already in the set (it is added otherwise); the set is sized for this call's writes */
static int bb_seen(const float *p)
{
    uint32_t v = (uint32_t)(uintptr_t)p, h = (v >> 2) * 0x9e3779b1u;
    for (uint32_t m = (uint32_t)g_seencap - 1, k = h & m;; k = (k + 1) & m) {
        if (g_seen[k] == v)
            return 1;
        if (!g_seen[k]) {
            g_seen[k] = v;
            return 0;
        }
    }
}

static void bb_check(uint8_t *n)
{
    g_nw = 0;
    g_wfull = 0;
    g_maps_mine.n = g_maps_orig.n = g_data_mine.n = g_data_orig.n = 0;
    g_ang_mine.n = g_ang_orig.n = 0;
    bb_turn(n, 1);
    for (int i = g_nw - 1; i >= 0; i--) {        /* old values back, newest first */
        memcpy(&g_w[i].p[0], &g_w[i].ox, 4);
        memcpy(&g_w[i].p[1], &g_w[i].oy, 4);
    }
    g_rec_orig = 1;
    g_orig(n);
    g_rec_orig = 0;
    g_st.checked++;
    g_checked_all++;
    g_call_bad = g_call_lines = 0;

    int need = 64;
    while (need < g_nw * 2)
        need *= 2;
    if (g_wfull || g_maps_mine.n > g_maps_mine.cap || g_maps_orig.n > g_maps_orig.cap ||
        g_data_mine.n > g_data_mine.cap || g_data_orig.n > g_data_orig.cap ||
        g_ang_mine.n > g_ang_mine.cap || g_ang_orig.n > g_ang_orig.cap ||
        !bb_grow((void **)&g_seen, &g_seencap, need, sizeof *g_seen)) {
        bb_mismatch("a record could not grow; not compared", 0, 0, 0, 0);
        goto done;
    }
    if (g_maps_mine.n != g_maps_orig.n)
        bb_mismatch("map count", 0, (uint32_t)g_maps_mine.n, (uint32_t)g_maps_orig.n, 0);
    else
        for (int i = 0; i < g_maps_mine.n; i++) {
            if (g_maps_mine.a[i] != g_maps_orig.a[i])
                bb_mismatch("map buffer", i, (uint32_t)(uintptr_t)g_maps_mine.a[i],
                            (uint32_t)(uintptr_t)g_maps_orig.a[i], 0);
            if (g_data_mine.a[i] != g_data_orig.a[i])
                bb_mismatch("map data", i, (uint32_t)(uintptr_t)g_data_mine.a[i],
                            (uint32_t)(uintptr_t)g_data_orig.a[i], 0);
        }
    if (g_ang_mine.n != g_ang_orig.n)
        bb_mismatch("turn count", 0, (uint32_t)g_ang_mine.n, (uint32_t)g_ang_orig.n, 0);
    else
        for (int i = 0; i < g_ang_mine.n; i++)
            if (g_ang_mine.a[i] != g_ang_orig.a[i])
                bb_mismatch("angle", i, g_ang_mine.a[i], g_ang_orig.a[i], 0);
    /* a vertex written more than once (elements sharing vertices): only its last value counts */
    memset(g_seen, 0, (size_t)g_seencap * sizeof *g_seen);
    for (int i = g_nw - 1; i >= 0; i--) {
        if (bb_seen(g_w[i].p)) {
            g_st.rewritten++;
            continue;
        }
        uint32_t x, y;
        memcpy(&x, &g_w[i].p[0], 4);
        memcpy(&y, &g_w[i].p[1], 4);
        if (x != g_w[i].nx)
            bb_mismatch("vertex x", i, g_w[i].nx, x, g_w[i].ox);
        if (y != g_w[i].ny)
            bb_mismatch("vertex y", i, g_w[i].ny, y, g_w[i].oy);
    }
done:
    if (g_call_bad) {
        g_st.bad++;
        g_bad_all++;
    }
}

/* ---- entry ---- */

__attribute__((cdecl, force_align_arg_pointer))
static void bb_entry(uint8_t *n)
{
    if (!PTR(n, 0x118)) {                        /* not seen; the original as it is */
        g_st.fallback++;
        g_orig(n);
        return;
    }
    if (g_verify)
        bb_check(n);
    else
        bb_turn(n, 0);
}

#if GMV_DEV
void gmv_bbfast_report(double seconds)
{
    if (!g_on || !g_st.calls)
        return;
    double c = g_st.calls;
    gmv_logfile("[perf]   billboard turn (bbfast): %.0f calls a second, %.0f elements a call, %.1f in range "
                "and in front, %.1f turned (%.1f vertices); original %u\n", c / seconds,
                (double)g_st.elems / c, (double)g_st.passed / c, (double)g_st.turned / c,
                (double)g_st.verts / c, g_st.fallback);
    if (g_verify)
        gmv_logfile("[perf]     checked against the original: %u calls, %s%u differed (since the start %u of %u); "
                    "%.1f vertex writes a call overwritten later in the same call\n", g_st.checked,
                    g_st.bad ? "" : "all identical, ", g_st.bad, g_bad_all, g_checked_all,
                    g_st.checked ? (double)g_st.rewritten / g_st.checked : 0.0);
    memset(&g_st, 0, sizeof g_st);
}
#endif

/* ---- install ---- */

static int bb_patch(uint32_t va, const void *buf, size_t n)
{
    DWORD old;
    if (!VirtualProtect((void *)(uintptr_t)va, n, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy((void *)(uintptr_t)va, buf, n);
    VirtualProtect((void *)(uintptr_t)va, n, old, &old);
    return 1;
}

static uint32_t bb_call_target(uint32_t site)
{
    const unsigned char *p = (const unsigned char *)(uintptr_t)site;
    int32_t rel;
    memcpy(&rel, p + 1, 4);
    return p[0] == 0xe8 ? site + 5 + (uint32_t)rel : 0;
}

static int bb_redirect(uint32_t site, void *to)
{
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)to - (site + 5));
    return bb_patch(site + 1, &rel, 4);
}

void gmv_apply_bbfast(void)
{
    const char *e = gmv_dbgenv("GMV_FAST_BILLBOARD");
    if (e && *e == '0') {
        gmv_log("[bbfast] FAST_BILLBOARD=0: the original billboard turn kept\n");
        return;
    }
    uint32_t h = 0x811c9dc5u;
    for (uint32_t a = BB_FN; a < BB_FN_END; a++) {
        if ((a > BB_SITE_MAP && a <= BB_SITE_MAP + 4) || (a > BB_SITE_SIN && a <= BB_SITE_SIN + 4))
            continue;                            /* PERF_PROFILE's probe may lead these elsewhere */
        int site = 0;
        for (int i = 0; i < 4; i++)
            site |= a > bb_getapp_sites[i] && a <= bb_getapp_sites[i] + 4;
        if (site)
            continue;                            /* checked below: 0x31b0 or its copy */
        h = (h ^ *(const uint8_t *)(uintptr_t)a) * 0x01000193u;
    }
    /* the constants it reads: 1.0, 1.5, 0.5, -1.0, 0.01, -0.0 (used here as literals) */
    static const struct { uint32_t va, bits; } k[] = {
        { 0x5f0680u, 0x3f800000u }, { 0x5f0964u, 0x3fc00000u }, { 0x5f096cu, 0x3f000000u },
        { 0x5f0688u, 0xbf800000u }, { 0x5f0968u, 0x3c23d70au }, { 0x5f0d28u, 0x80000000u },
        { 0x5f5540u, 0x80000000u }, { 0x5f5550u, 0x7fffffffu },
    };
    /* the caller only has to be a call: PERF_PROFILE's billboard probe leads it through its timer,
       which still calls 0x131f1e (and so lands in the jump written there) */
    int ok = h == BB_FN_FNV && bb_call_target(BB_CALLER) && bb_call_target(BB_SITE_MAP) &&
             bb_call_target(BB_SITE_SIN) && !memcmp((const void *)(uintptr_t)BB_FN, "\x55\x89\xe5\x53\x57", 5);
    for (size_t i = 0; ok && i < sizeof k / sizeof k[0]; i++)
        ok = *(const uint32_t *)(uintptr_t)k[i].va == k[i].bits;
    for (int i = 0; ok && i < 4; i++) {
        uint32_t t = bb_call_target(bb_getapp_sites[i]);
        ok = t == BB_GETAPP || t == BB_GETAPP + GMV_LOW_DELTA;
    }
    if (!ok) {
        gmv_log("[bbfast] the billboard turn 0x%06x is not as expected (bytes FNV %08x, want %08x); skipped\n",
                BB_FN, h, BB_FN_FNV);
        return;
    }
    unsigned char *tr = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) {
        gmv_log("[bbfast] VirtualAlloc for the trampoline failed (%lu); skipped\n", GetLastError());
        return;
    }
    memcpy(tr, (const void *)(uintptr_t)BB_FN, 5);   /* push ebp; mov ebp, esp; push ebx; push edi */
    tr[5] = 0xe9;
    int32_t rel = (int32_t)((BB_FN + 5) - (uint32_t)(uintptr_t)(tr + 10));
    memcpy(tr + 6, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), tr, 4096);
    g_orig = (void (*)(void *))(void *)tr;

    if (GMV_DEV) {
        const char *v = gmv_dbgenv("GMV_BB_VERIFY");
        if (v && *v && *v != '0') {
            g_fwd_map = (bb_mapfn)(uintptr_t)bb_call_target(BB_SITE_MAP);
            g_fwd_sin = (bb_mathfn)(uintptr_t)bb_call_target(BB_SITE_SIN);
            g_verify = bb_redirect(BB_SITE_MAP, (void *)bbv_map) && bb_redirect(BB_SITE_SIN, (void *)bbv_sin);
            if (!g_verify)
                gmv_log("[bbfast] BB_VERIFY: write at the original's calls failed (%lu)\n", GetLastError());
        }
    }
    unsigned char j[5] = { 0xe9 };
    rel = (int32_t)((uint32_t)(uintptr_t)bb_entry - (BB_FN + 5));
    memcpy(j + 1, &rel, 4);
    if (!bb_patch(BB_FN, j, 5)) {
        gmv_log("[bbfast] write at 0x%06x failed (%lu)\n", BB_FN, GetLastError());
        FlushInstructionCache(GetCurrentProcess(), NULL, 0);
        return;
    }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    g_on = 1;
    gmv_log("[bbfast] billboard turn 0x%06x: range tests before the mesh lookup%s\n", BB_FN,
            g_verify ? "; GMV_BB_VERIFY: checked against the original every call" : "");
}
