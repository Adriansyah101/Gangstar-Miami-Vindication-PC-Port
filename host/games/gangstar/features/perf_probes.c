/* perf_probes.c -- PERF_PROFILE (diagnostic only): timing of three of the game's own routines, and the summary. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

#include "game.h"
#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"

#if GMV_DEV
double ms_of(long long q);                      /* loadprof.c */
#else                                           /* release, no loadprof.c: PERF_PROFILE is off (gmv_perf_on is 0) */
static double ms_of(long long q) { (void)q; return 0; }
#endif

/* PERF_PROFILE probe of the game's UI name lookup.  Measures only; the game's search still runs.
   0x2fd160 find(ui, "a.b.c") runs the breadth-first search 0x2fcf60(ui, path, root) from ui+0x40,
   and from (ui+0x3c)->+0x10 when that finds nothing.  Each path segment may match at any depth, so
   a lookup can walk the whole UI tree.  The search's queue count (ui+0x2c) afterwards is the number
   of tree entries it queued.  Both calls are re-aimed at uf_search, which adds up per path and
   caller: lookups, entries queued, misses. */
#define UF_SEARCH 0x002fcf60u
static const uint32_t uf_sites[2] = { 0x002fd183u, 0x002fd1b1u };
typedef void *(*uf_fn)(void *ui, const char *path, void *root);
#define UF_MAX 160
static struct uf_ent {
    char path[48];
    uint32_t caller;
    int second;                                  /* the fallback root */
    unsigned calls, misses;
    unsigned long long queued;
} g_uf[UF_MAX];
static unsigned g_uf_n, g_uf_calls, g_uf_misses, g_uf_lost;
static unsigned long long g_uf_queued;
static volatile LONG g_uf_tid;

__attribute__((cdecl, force_align_arg_pointer))
static void *uf_search(void *ui, const char *path, void *root)
{
    void *r = ((uf_fn)(uintptr_t)UF_SEARCH)(ui, path, root);
    LONG me = (LONG)GetCurrentThreadId();
    if ((LONG)g_uf_tid != me && InterlockedCompareExchange(&g_uf_tid, me, 0) != 0)
        return r;
    unsigned queued = *(const unsigned *)((const char *)ui + 0x2c);
    int second = (uint32_t)(uintptr_t)__builtin_return_address(0) == uf_sites[1] + 5;
    /* 0x2fd160 keeps an ebp frame: its return address is the lookup's caller. */
    void **fp = (void **)__builtin_frame_address(0);
    uint32_t caller = (uint32_t)(uintptr_t)((void **)fp[0])[1];
    g_uf_calls++;
    g_uf_queued += queued;
    if (!r)
        g_uf_misses++;
    char key[48];
    size_t len = 0;
    if (path)
        while (len < sizeof key - 1 && path[len]) {
            key[len] = path[len];
            len++;
        }
    key[len] = 0;
    struct uf_ent *e = NULL;
    for (unsigned i = 0; i < g_uf_n; i++)
        if (g_uf[i].caller == caller && g_uf[i].second == second && !strcmp(g_uf[i].path, key)) {
            e = &g_uf[i];
            break;
        }
    if (!e && g_uf_n < UF_MAX) {
        e = &g_uf[g_uf_n++];
        memcpy(e->path, key, len + 1);
        e->caller = caller;
        e->second = second;
    }
    if (!e) {
        g_uf_lost++;
        return r;
    }
    e->calls++;
    e->queued += queued;
    if (!r)
        e->misses++;
    return r;
}

static void uf_report(double seconds)
{
    if (!g_uf_calls)
        return;
    gmv_logfile("[perf]   ui find: %.0f searches a second, %.0f entries queued a search, %.0f%% found "
                "nothing; %u paths%s\n", g_uf_calls / seconds, (double)g_uf_queued / g_uf_calls,
                g_uf_misses * 100.0 / g_uf_calls, g_uf_n, g_uf_lost ? " (table full)" : "");
    for (int k = 0; k < 12; k++) {
        int best = -1;
        for (unsigned i = 0; i < g_uf_n; i++)
            if (g_uf[i].calls && (best < 0 || g_uf[i].queued > g_uf[best].queued))
                best = (int)i;
        if (best < 0)
            break;
        struct uf_ent *e = &g_uf[best];
        gmv_logfile("[perf]     %4.1f%% of the entries: \"%s\"%s <- game 0x%06x, %.0f a second, %.0f "
                    "queued each, %.0f%% nothing\n", e->queued * 100.0 / (g_uf_queued ? g_uf_queued : 1),
                    e->path, e->second ? " (2nd root)" : "", e->caller, e->calls / seconds,
                    (double)e->queued / e->calls, e->misses * 100.0 / e->calls);
        e->calls = 0;
    }
    memset(g_uf, 0, sizeof g_uf);
    g_uf_n = g_uf_calls = g_uf_misses = g_uf_lost = 0;
    g_uf_queued = 0;
}

void gmv_uifind_probe(void)
{
    if (!gmv_perf_on())
        return;
    for (int i = 0; i < 2; i++) {
        unsigned char *p = (unsigned char *)(uintptr_t)uf_sites[i];
        int32_t rel;
        memcpy(&rel, p + 1, 4);
        if (p[0] != 0xe8 || uf_sites[i] + 5 + (uint32_t)rel != UF_SEARCH) {
            gmv_log("[perf] ui find probe: 0x%06x is not `call 0x%06x`; skipped\n", uf_sites[i], UF_SEARCH);
            return;
        }
    }
    for (int i = 0; i < 2; i++) {
        unsigned char *p = (unsigned char *)(uintptr_t)uf_sites[i];
        int32_t rel = (int32_t)((uint32_t)(uintptr_t)&uf_search - (uf_sites[i] + 5));
        DWORD old;
        if (VirtualProtect(p + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
            memcpy(p + 1, &rel, 4);
            VirtualProtect(p + 1, 4, old, &old);
        }
    }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    gmv_log("[perf] ui find probe on (0x2fd183, 0x2fd1b1)\n");
}

/* PERF_PROFILE probe of Box2D's world step b2World::Step (0x1c3620).  Its calls to
   b2ContactManager::Collide (0x1c368f), b2World::Solve (0x1c36ab) and SolveTOI (0x1c36d0) are
   timed, and inside Solve the island solves (0x1c1c6c -> b2Island::Solve 0x1c0f50), the shape
   syncs (0x1c1d08 -> b2Body::SynchronizeShapes 0x1c0420) and the broadphase commit (0x1c1d3d ->
   0x1b8f20).  Solve's time minus those three is its own walks over the body, contact and joint
   lists.  Every 16th step the world's lists are counted (body flags u16 +0, type u16 +2 with 0 =
   static, next +0x60; contact next +0x10, manifold count +8; joint next +0xc).  Measures only.
   Each wrapper calls what its site called before (Solve's site may already lead to b2solve.c's
   copy, which makes no calls from the three sites inside the original Solve). */
enum { B2P_COLLIDE, B2P_SOLVE, B2P_TOI, B2P_ISLAND, B2P_SYNC, B2P_COMMIT, B2P_N };
static const struct { uint32_t site, target; } b2p_sites[B2P_N] = {
    { 0x001c368fu, 0x001c0850u }, { 0x001c36abu, 0x001c1a60u }, { 0x001c36d0u, 0x001c2e90u },
    { 0x001c1c6cu, 0x001c0f50u }, { 0x001c1d08u, 0x001c0420u }, { 0x001c1d3du, 0x001b8f20u },
};
static struct {
    unsigned n[B2P_N];
    long long t[B2P_N], walk_t;
    unsigned long long isl_bodies, isl_contacts;
    unsigned counts;
    unsigned long long bodies, statics, frozen, sleeping, awake, contacts, touching, joints;
} g_b2p;
static volatile LONG g_b2p_tid;
static uint32_t b2p_fwd[B2P_N];          /* what each site called before the probe */

typedef uint32_t (*b2p_fn1)(void *);
typedef uint32_t (*b2p_fn2)(void *, void *);
typedef uint32_t (*b2p_fn5)(void *, void *, void *, uint32_t, uint32_t);
#define B2P_FN(k) ((uintptr_t)b2p_fwd[k])

static int b2p_mine(void)
{
    LONG me = (LONG)GetCurrentThreadId();
    return g_b2p_tid == me || InterlockedCompareExchange(&g_b2p_tid, me, 0) == 0;
}

static void b2p_count(const uint8_t *w)
{
    long long t0 = gmv_prof_now();
    for (const uint8_t *b = *(uint8_t *const *)(w + 0x19230); b; b = *(uint8_t *const *)(b + 0x60)) {
        uint16_t f = *(const uint16_t *)b, ty = *(const uint16_t *)(b + 2);
        g_b2p.bodies++;
        if (!ty)
            g_b2p.statics++;
        else if (f & 2)
            g_b2p.frozen++;
        else if (f & 8)
            g_b2p.sleeping++;
        else
            g_b2p.awake++;
    }
    g_b2p.walk_t += gmv_prof_now() - t0;
    for (const uint8_t *c = *(uint8_t *const *)(w + 0x19238); c; c = *(uint8_t *const *)(c + 0x10)) {
        g_b2p.contacts++;
        if (*(const int32_t *)(c + 8) > 0)
            g_b2p.touching++;
    }
    for (const uint8_t *j = *(uint8_t *const *)(w + 0x19234); j; j = *(uint8_t *const *)(j + 0xc))
        g_b2p.joints++;
    g_b2p.counts++;
}

__attribute__((cdecl, force_align_arg_pointer))
static void b2p_collide(void *cm)
{
    if (!b2p_mine()) {
        ((b2p_fn1)B2P_FN(B2P_COLLIDE))(cm);
        return;
    }
    long long t0 = gmv_prof_now();
    ((b2p_fn1)B2P_FN(B2P_COLLIDE))(cm);
    g_b2p.t[B2P_COLLIDE] += gmv_prof_now() - t0;
    g_b2p.n[B2P_COLLIDE]++;
}

__attribute__((cdecl, force_align_arg_pointer))
static void b2p_solve(void *world, void *step)
{
    if (!b2p_mine()) {
        ((b2p_fn2)B2P_FN(B2P_SOLVE))(world, step);
        return;
    }
    if ((g_b2p.n[B2P_SOLVE] & 15) == 0)
        b2p_count(world);
    long long t0 = gmv_prof_now();
    ((b2p_fn2)B2P_FN(B2P_SOLVE))(world, step);
    g_b2p.t[B2P_SOLVE] += gmv_prof_now() - t0;
    g_b2p.n[B2P_SOLVE]++;
}

__attribute__((cdecl, force_align_arg_pointer))
static void b2p_toi(void *world, void *step)
{
    if (!b2p_mine()) {
        ((b2p_fn2)B2P_FN(B2P_TOI))(world, step);
        return;
    }
    long long t0 = gmv_prof_now();
    ((b2p_fn2)B2P_FN(B2P_TOI))(world, step);
    g_b2p.t[B2P_TOI] += gmv_prof_now() - t0;
    g_b2p.n[B2P_TOI]++;
}

/* b2Island::Solve(island, step, gravity, a, b): island body count +0x14, contact count +0x1c. */
__attribute__((cdecl, force_align_arg_pointer))
static void b2p_island(void *isl, void *step, void *grav, uint32_t a, uint32_t b)
{
    if (!b2p_mine()) {
        ((b2p_fn5)B2P_FN(B2P_ISLAND))(isl, step, grav, a, b);
        return;
    }
    g_b2p.isl_bodies += *(const int32_t *)((const uint8_t *)isl + 0x14);
    g_b2p.isl_contacts += *(const int32_t *)((const uint8_t *)isl + 0x1c);
    long long t0 = gmv_prof_now();
    ((b2p_fn5)B2P_FN(B2P_ISLAND))(isl, step, grav, a, b);
    g_b2p.t[B2P_ISLAND] += gmv_prof_now() - t0;
    g_b2p.n[B2P_ISLAND]++;
}

/* b2Body::SynchronizeShapes returns a bool in al; the whole eax is passed back. */
__attribute__((cdecl, force_align_arg_pointer))
static uint32_t b2p_sync(void *body)
{
    if (!b2p_mine())
        return ((b2p_fn1)B2P_FN(B2P_SYNC))(body);
    long long t0 = gmv_prof_now();
    uint32_t r = ((b2p_fn1)B2P_FN(B2P_SYNC))(body);
    g_b2p.t[B2P_SYNC] += gmv_prof_now() - t0;
    g_b2p.n[B2P_SYNC]++;
    return r;
}

__attribute__((cdecl, force_align_arg_pointer))
static void b2p_commit(void *bp)
{
    if (!b2p_mine()) {
        ((b2p_fn1)B2P_FN(B2P_COMMIT))(bp);
        return;
    }
    long long t0 = gmv_prof_now();
    ((b2p_fn1)B2P_FN(B2P_COMMIT))(bp);
    g_b2p.t[B2P_COMMIT] += gmv_prof_now() - t0;
    g_b2p.n[B2P_COMMIT]++;
}

static void b2p_report(double seconds)
{
    unsigned steps = g_b2p.n[B2P_SOLVE];
    if (!steps)
        return;
    double s = steps, own = ms_of(g_b2p.t[B2P_SOLVE] - g_b2p.t[B2P_ISLAND] - g_b2p.t[B2P_SYNC] -
                                  g_b2p.t[B2P_COMMIT]);
    unsigned ni = g_b2p.n[B2P_ISLAND] ? g_b2p.n[B2P_ISLAND] : 1;
    gmv_logfile("[perf]   box2d: %.0f steps a second; a step: collide %.3f ms, solve %.3f ms (islands "
                "%.3f ms: %.1f islands of %.1f bodies/%.1f contacts; shape syncs %.3f ms x%.1f; commit "
                "%.3f ms; Solve's own list walks %.3f ms), TOI %.3f ms (%u runs)\n", s / seconds,
                ms_of(g_b2p.t[B2P_COLLIDE]) / s, ms_of(g_b2p.t[B2P_SOLVE]) / s,
                ms_of(g_b2p.t[B2P_ISLAND]) / s, g_b2p.n[B2P_ISLAND] / s,
                (double)g_b2p.isl_bodies / ni, (double)g_b2p.isl_contacts / ni,
                ms_of(g_b2p.t[B2P_SYNC]) / s, g_b2p.n[B2P_SYNC] / s, ms_of(g_b2p.t[B2P_COMMIT]) / s,
                own / s, ms_of(g_b2p.t[B2P_TOI]) / s, g_b2p.n[B2P_TOI]);
    if (g_b2p.counts) {
        double c = g_b2p.counts;
        gmv_logfile("[perf]   box2d world: %.0f bodies (%.0f static, %.0f frozen, %.0f sleeping, %.0f "
                    "awake), %.0f contacts (%.0f touching), %.0f joints; one walk over the bodies "
                    "%.3f ms\n", g_b2p.bodies / c, g_b2p.statics / c, g_b2p.frozen / c,
                    g_b2p.sleeping / c, g_b2p.awake / c, g_b2p.contacts / c, g_b2p.touching / c,
                    g_b2p.joints / c, ms_of(g_b2p.walk_t) / c);
    }
    memset(&g_b2p, 0, sizeof g_b2p);
}

void gmv_b2step_probe(void)
{
    if (!gmv_perf_on())
        return;
    static void *const fns[B2P_N] = { (void *)b2p_collide, (void *)b2p_solve, (void *)b2p_toi,
                                      (void *)b2p_island, (void *)b2p_sync, (void *)b2p_commit };
    for (int i = 0; i < B2P_N; i++) {
        const unsigned char *p = (const unsigned char *)(uintptr_t)b2p_sites[i].site;
        int32_t rel;
        memcpy(&rel, p + 1, 4);
        b2p_fwd[i] = b2p_sites[i].site + 5 + (uint32_t)rel;
        if (p[0] != 0xe8 || (b2p_fwd[i] != b2p_sites[i].target && i != B2P_SOLVE)) {
            gmv_log("[perf] box2d step probe: 0x%06x is not `call 0x%06x`; skipped\n", b2p_sites[i].site,
                    b2p_sites[i].target);
            return;
        }
    }
    for (int i = 0; i < B2P_N; i++) {
        unsigned char *p = (unsigned char *)(uintptr_t)b2p_sites[i].site;
        int32_t rel = (int32_t)((uint32_t)(uintptr_t)fns[i] - (b2p_sites[i].site + 5));
        DWORD old;
        if (VirtualProtect(p + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
            memcpy(p + 1, &rel, 4);
            VirtualProtect(p + 1, 4, old, &old);
        }
    }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    gmv_log("[perf] box2d step probe on (collide, solve, islands, shape syncs, commit, TOI)\n");
}

/* PERF_PROFILE probe of the billboard turn 0x131f1e(node) (called from the scene node method
   0x1315bc, vtable 0x656b78 slot 4, when the byte at 0x641111 is set).  It walks a vector of
   vectors at node+0x1c4 (12-byte {begin, end, cap}) of element pointers; each element in range
   and in front of the camera has its vertex data locked (call 0x13223a -> 0x517b10) and, when its
   angle to the camera is large enough, is turned (call 0x1323eb -> sin stub 0x5eeeae).  Timed and
   counted here; measures only. */
enum { BBP_FN, BBP_LOCK, BBP_TURN, BBP_N };
static const struct { uint32_t site, target; } bbp_sites[BBP_N] = {
    { 0x001315dau, 0x00131f1eu }, { 0x0013223au, 0x00517b10u }, { 0x001323ebu, 0x005eeeaeu },
};
static uint32_t bbp_fwd[BBP_N];
static struct {
    unsigned calls, lists_max;
    unsigned long long elems, locks, turns;
    long long t;
    double t_max;
} g_bbp;
static volatile LONG g_bbp_tid;

static int bbp_mine(void)
{
    LONG me = (LONG)GetCurrentThreadId();
    return g_bbp_tid == me || InterlockedCompareExchange(&g_bbp_tid, me, 0) == 0;
}

__attribute__((cdecl, force_align_arg_pointer))
static void bbp_fn(void *node)
{
    if (!bbp_mine()) {
        ((b2p_fn1)(uintptr_t)bbp_fwd[BBP_FN])(node);
        return;
    }
    const uint8_t *n = (const uint8_t *)node;
    const uint32_t *v = *(uint32_t *const *)(n + 0x1c4), *ve = *(uint32_t *const *)(n + 0x1c8);
    unsigned lists = 0;
    for (; v && v + 3 <= ve; v += 3, lists++)
        g_bbp.elems += (v[1] - v[0]) / 4;
    if (lists > g_bbp.lists_max)
        g_bbp.lists_max = lists;
    long long t0 = gmv_prof_now();
    ((b2p_fn1)(uintptr_t)bbp_fwd[BBP_FN])(node);
    long long dt = gmv_prof_now() - t0;
    g_bbp.t += dt;
    if (ms_of(dt) > g_bbp.t_max)
        g_bbp.t_max = ms_of(dt);
    g_bbp.calls++;
}

__attribute__((cdecl, force_align_arg_pointer))
static uint32_t bbp_lock(void *a, uint32_t b)
{
    if (g_bbp_tid == (LONG)GetCurrentThreadId())
        g_bbp.locks++;
    return ((b2p_fn2)(uintptr_t)bbp_fwd[BBP_LOCK])(a, (void *)(uintptr_t)b);
}

typedef float (*bbp_ffn)(float);
__attribute__((cdecl, force_align_arg_pointer))
static float bbp_turn(float x)
{
    if (g_bbp_tid == (LONG)GetCurrentThreadId())
        g_bbp.turns++;
    return ((bbp_ffn)(uintptr_t)bbp_fwd[BBP_TURN])(x);
}

static void bbp_report(double seconds)
{
    if (!g_bbp.calls)
        return;
    double c = g_bbp.calls;
    gmv_logfile("[perf]   billboard turn: %.0f calls a second, %.3f ms a call (max %.2f); %.0f elements "
                "a call in up to %u lists, %.1f in range and in front, %.1f turned\n", c / seconds,
                ms_of(g_bbp.t) / c, g_bbp.t_max, g_bbp.elems / c, g_bbp.lists_max, g_bbp.locks / c,
                g_bbp.turns / c);
    memset(&g_bbp, 0, sizeof g_bbp);
}

void gmv_billboard_probe(void)
{
    if (!gmv_perf_on())
        return;
    static void *const fns[BBP_N] = { (void *)bbp_fn, (void *)bbp_lock, (void *)bbp_turn };
    for (int i = 0; i < BBP_N; i++) {
        const unsigned char *p = (const unsigned char *)(uintptr_t)bbp_sites[i].site;
        int32_t rel;
        memcpy(&rel, p + 1, 4);
        bbp_fwd[i] = bbp_sites[i].site + 5 + (uint32_t)rel;
        if (p[0] != 0xe8 || bbp_fwd[i] != bbp_sites[i].target) {
            gmv_log("[perf] billboard probe: 0x%06x is not `call 0x%06x`; skipped\n", bbp_sites[i].site,
                    bbp_sites[i].target);
            return;
        }
    }
    for (int i = 0; i < BBP_N; i++) {
        unsigned char *p = (unsigned char *)(uintptr_t)bbp_sites[i].site;
        int32_t rel = (int32_t)((uint32_t)(uintptr_t)fns[i] - (bbp_sites[i].site + 5));
        DWORD old;
        if (VirtualProtect(p + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
            memcpy(p + 1, &rel, 4);
            VirtualProtect(p + 1, 4, old, &old);
        }
    }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    gmv_log("[perf] billboard probe on (0x1315da, 0x13223a, 0x1323eb); flag 0x641111 = %u\n",
            *(const unsigned char *)(uintptr_t)0x641111u);
}

void gmv_game_perf_report(double seconds)
{
    gmv_broadphase_report(seconds);
    gmv_b2solve_report(seconds);
    gmv_bbfast_report(seconds);
    gmv_hudbars_report(seconds);
    gmv_glstream_report(seconds);
    gmv_vbostatic_report(seconds);
    uf_report(seconds);
    b2p_report(seconds);
    bbp_report(seconds);
}

void gmv_game_perf_counts(unsigned *made, unsigned *gone, int *bounds)
{
    gmv_broadphase_tick(made, gone, bounds);
}
