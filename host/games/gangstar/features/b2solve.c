/* b2solve.c -- b2World::Solve without walking every body in the world, with identical results.

   PERF_PROFILE runs (2026-10-30) showed Box2D's Solve (0x1c1a60) taking 8-9% of the CPU in the
   city: the world holds ~15,000 bodies, all but ~50-90 of them static (the city's collision), and
   Solve walks the whole body list three times a step (clear the island flag, find island seeds,
   sync the shapes of moved bodies), ~0.31 ms of its ~0.37 ms, at up to 240 steps a second with
   PHYSICS_SYNC.  The islands themselves cost ~0.02 ms.

   Solve, as read (Gameloft's build of Box2D 2.0; world fields at the offsets below):
     positionIterationCount = 0; b2Island island(bodyCount, contactCount, jointCount, &stack, listener)
     every body:    flags &= ~island(4)
     every contact: flags &= ~island(4);   every joint: islandFlag = 0
     stack = alloc(bodyCount * 4)
     every body in list order, unless flags & (island|sleep|frozen) or static: depth-first island
       through contact edges (contact not nonSolid/island and touching) and joint edges, statics
       added but not crossed; island.Solve(step, gravity, correctPositions, allowSleep);
       positionIterationCount = max(.., island's); statics of the island lose the island flag
     free(stack)
     every body in list order, unless sleeping/frozen or static: SynchronizeShapes, and the
       boundary listener's Violation(body) when it returns false
     broadphase Commit; ~b2Island

   For a static body the three body loops do nothing at all as long as its island flag is clear:
   the seed and sync loops skip statics, and the clear loop only clears that flag.  A static only
   gets the flag inside Solve's own island search, which always clears it again (every body it
   flags lands in the island, and the island's statics are cleared after its solve).  So this
   copy of Solve runs the same steps with the three body loops over a short list instead: every
   body that is not static or has the island flag, in the world list's order.  Same calls in the
   same order with the same arguments, same writes.  The contact and joint loops are kept as they
   are (a few dozen entries).

   The short list follows the only code that changes the body list or a body's type (the writes
   of world+0x19230, body+0x5c/+0x60 and body+0x2 in the binary):
     b2World::CreateBody 0x1c1ed0 (new body at the list head),  b2World::DestroyBody 0x1c3960,
     b2Body::SetMass 0x1bfd30 and SetMassFromShapes 0x1c01f0 (static <-> dynamic),
     b2World::b2World 0x1c3710 (a new world: list rebuilt by one walk at its first step).
   Each is hooked at its entry (its first instructions run from a trampoline) and adjusts the list
   after the original returns.  A body made non-static later is put in its list position by a walk
   from the list head (it is a new body, near the head).

   Dev: GMV_B2_VERIFY=1 (build\b2_verify.bat) compares the short list with a full walk at every
   step, logs any difference and then uses the full walk's.  GMV_FAST_B2SOLVE=0 keeps the original. */

#include "compat.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "shims.h"
#include "gangstar.h"

#define BS_CALLSITE  0x001c36abu   /* in b2World::Step (0x1c3620): call 0x1c1a60 Solve            */
#define BS_SOLVE     0x001c1a60u   /* b2World::Solve(world, step)                                 */
#define BS_SOLVE_END 0x001c1d55u
#define BS_SOLVE_FNV 0x7fd6d454u   /* FNV-1a of Solve's bytes, as analyzed                         */
#define BS_CREATE    0x001c1ed0u   /* b2World::CreateBody(world, def) -> body                     */
#define BS_DESTROY   0x001c3960u   /* b2World::DestroyBody(world, body)                           */
#define BS_SETMASS   0x001bfd30u   /* b2Body::SetMass(body, massData)                             */
#define BS_SETMASSFS 0x001c01f0u   /* b2Body::SetMassFromShapes(body)                             */
#define BS_WORLDCTOR 0x001c3710u   /* b2World::b2World(world, aabb, gravity, doSleep)             */
#define BS_ISL_CTOR  0x001c0e10u   /* b2Island::b2Island(isl, bodyCap, contactCap, jointCap, alloc, listener) */
#define BS_ISL_DTOR  0x001c0d70u   /* b2Island::~b2Island(isl)                                    */
#define BS_ISL_SOLVE 0x001c0f50u   /* b2Island::Solve(isl, step, gravity, correctPositions, allowSleep) */
#define BS_SYNC      0x001c0420u   /* bool b2Body::SynchronizeShapes(body)                        */
#define BS_ALLOC     0x001bfc60u   /* b2StackAllocator::Allocate(alloc, size)                     */
#define BS_FREE      0x001bfc00u   /* b2StackAllocator::Free(alloc, p)                            */
#define BS_COMMIT    0x001b8f20u   /* b2BroadPhase::Commit(bp)                                    */

/* b2World */
#define W_ALLOC      0x44          /* b2StackAllocator                                            */
#define W_LOCK       0x191d4       /* bool m_lock (set during Step)                               */
#define W_BP         0x191d8       /* b2BroadPhase*                                               */
#define W_BODIES     0x19230
#define W_JOINTS     0x19234
#define W_CONTACTS   0x19238
#define W_NBODY      0x1923c
#define W_NCONTACT   0x19240
#define W_NJOINT     0x19244
#define W_GRAVITY    0x19248
#define W_ALLOWSLEEP 0x19250       /* bool                                                        */
#define W_BOUNDARY   0x1925c       /* b2BoundaryListener*                                         */
#define W_LISTENER   0x19264       /* b2ContactListener*                                          */
#define W_POSITER    0x19270       /* int32 m_positionIterationCount                              */
#define W_CORRECT    0x19274       /* bool                                                        */
/* b2Body: u16 flags +0 (frozen 2, island 4, sleep 8), u16 type +2 (0 = static), world +0x58,
   prev +0x5c, next +0x60, jointList +0x6c, contactList +0x70.  Contact: u32 flags +4 (nonSolid 1,
   island 4), manifoldCount +8, next +0x10.  Joint: next +0xc, bool islandFlag +0x3c.  Edges:
   other +0, contact/joint +4, next +0xc.  b2Island (0x30): bodies +8, contacts +0xc, joints
   +0x10, bodyCount +0x14, jointCount +0x18, contactCount +0x1c, positionIterationCount +0x2c. */

#define AT(p, o, T) (*(T *)((uint8_t *)(p) + (o)))
#define U8(p, o)  AT(p, o, uint8_t)
#define U16(p, o) AT(p, o, uint16_t)
#define I32(p, o) AT(p, o, int32_t)
#define U32(p, o) AT(p, o, uint32_t)
#define PTR(p, o) AT(p, o, uint8_t *)

typedef void (*bs_solve_fn)(void *w, void *step);
typedef void *(*bs_create_fn)(void *w, const void *def);
typedef void (*bs_destroy_fn)(void *w, void *b);
typedef void (*bs_setmass_fn)(void *b, const void *md);
typedef void (*bs_setmassfs_fn)(void *b);
typedef uint32_t (*bs_wctor_fn)(void *w, void *a, void *b, uint32_t c);
typedef void (*bs_isl_ctor_fn)(void *isl, int32_t nb, int32_t nc, int32_t nj, void *alloc, void *listener);
typedef void (*bs_isl_fn)(void *isl);
typedef void (*bs_isl_solve_fn)(void *isl, void *step, void *grav, uint32_t correct, uint32_t allowSleep);
typedef uint32_t (*bs_sync_fn)(void *b);
typedef void *(*bs_alloc_fn)(void *alloc, int32_t size);
typedef void (*bs_free_fn)(void *alloc, void *p);
typedef void (*bs_commit_fn)(void *bp);
typedef void (*bs_violation_fn)(void *listener, void *b);

static bs_create_fn g_create;            /* trampolines: the hooked functions' originals */
static bs_destroy_fn g_destroy;
static bs_setmass_fn g_setmass;
static bs_setmassfs_fn g_setmassfs;
static bs_wctor_fn g_wctor;

static volatile LONG g_lockv;
static uint8_t *g_world;                 /* the world the list belongs to (NULL: rebuild)      */
static int g_dirty = 1;
static uint8_t **g_a, **g_snap, **g_chk; /* the list, the step's copy, the verify walk's       */
static int g_n, g_cap, g_snapcap, g_chkcap;
static int g_verify, g_on;

static struct {
    unsigned steps, fallback, rebuilds, created, massed, removed, mismatch;
    unsigned long long listed, bodies, walked;
} g_st;
static unsigned g_mis_logged;

static void bs_lock(void)
{
    while (InterlockedCompareExchange(&g_lockv, 1, 0))
        YieldProcessor();
}

static void bs_unlock(void)
{
    InterlockedExchange(&g_lockv, 0);
}

static int bs_keep(const uint8_t *b)
{
    return U16(b, 2) != 0 || (U16(b, 0) & 4);
}

static int bs_grow(uint8_t ***a, int *cap, int need)
{
    if (need <= *cap)
        return 1;
    int c = *cap ? *cap : 256;
    while (c < need)
        c *= 2;
    uint8_t **p = realloc(*a, (size_t)c * sizeof *p);
    if (!p)
        return 0;
    *a = p;
    *cap = c;
    return 1;
}

static int bs_find(const uint8_t *b)
{
    for (int i = 0; i < g_n; i++)
        if (g_a[i] == b)
            return i;
    return -1;
}

static void bs_insert(int at, uint8_t *b)
{
    if (!bs_grow(&g_a, &g_cap, g_n + 1)) {
        g_dirty = 1;
        return;
    }
    memmove(g_a + at + 1, g_a + at, (size_t)(g_n - at) * sizeof *g_a);
    g_a[at] = b;
    g_n++;
}

/* Every kept body of w in list order into *out; -1 when out of memory. */
static int bs_walk(uint8_t *w, uint8_t ***out, int *cap)
{
    int n = 0;
    for (uint8_t *b = PTR(w, W_BODIES); b; b = PTR(b, 0x60))
        if (bs_keep(b)) {
            if (!bs_grow(out, cap, n + 1))
                return -1;
            (*out)[n++] = b;
        }
    return n;
}

static int bs_rebuild(uint8_t *w)
{
    int n = bs_walk(w, &g_a, &g_cap);
    g_st.rebuilds++;
    if (n < 0) {
        g_world = NULL;
        g_dirty = 1;
        return 0;
    }
    g_n = n;
    g_world = w;
    g_dirty = 0;
    return 1;
}

/* GMV_B2_VERIFY: the list against a full walk; on a difference the walk's list is used. */
static void bs_check(uint8_t *w)
{
    int n = bs_walk(w, &g_chk, &g_chkcap);
    if (n < 0)
        return;
    if (n == g_n && !memcmp(g_chk, g_a, (size_t)n * sizeof *g_a))
        return;
    g_st.mismatch++;
    if (g_mis_logged < 8) {
        g_mis_logged++;
        int i = 0;
        while (i < n && i < g_n && g_chk[i] == g_a[i])
            i++;
        const uint8_t *x = i < n ? g_chk[i] : NULL, *y = i < g_n ? g_a[i] : NULL;
        gmv_logfile("[b2solve] MISMATCH: list %d bodies, full walk %d; first difference at %d: walk "
                    "%p (flags 0x%x type %u), list %p (flags 0x%x type %u)\n", g_n, n, i, (void *)x,
                    x ? U16(x, 0) : 0, x ? U16(x, 2) : 0, (void *)y, y ? U16(y, 0) : 0,
                    y ? U16(y, 2) : 0);
    }
    if (bs_grow(&g_a, &g_cap, n)) {
        memcpy(g_a, g_chk, (size_t)n * sizeof *g_a);
        g_n = n;
    }
}

__attribute__((cdecl, force_align_arg_pointer))
static void bs_solve(uint8_t *w, void *step)
{
    bs_lock();
    int ok = (w == g_world && !g_dirty) || bs_rebuild(w);
    if (ok && g_verify)
        bs_check(w);
    int n = g_n;
    if (ok && !bs_grow(&g_snap, &g_snapcap, n))
        ok = 0;
    if (ok)
        memcpy(g_snap, g_a, (size_t)n * sizeof *g_a);
    bs_unlock();
    if (!ok) {
        g_st.fallback++;
        ((bs_solve_fn)(uintptr_t)BS_SOLVE)(w, step);
        return;
    }
    uint8_t **a = g_snap;
    g_st.steps++;
    g_st.listed += (unsigned)n;
    g_st.bodies += U32(w, W_NBODY);

    uint8_t isl[0x30] __attribute__((aligned(16)));
    I32(w, W_POSITER) = 0;
    ((bs_isl_ctor_fn)(uintptr_t)BS_ISL_CTOR)(isl, I32(w, W_NBODY), I32(w, W_NCONTACT), I32(w, W_NJOINT),
                                            w + W_ALLOC, PTR(w, W_LISTENER));
    for (int i = 0; i < n; i++)
        U16(a[i], 0) &= (uint16_t)~4u;
    for (uint8_t *c = PTR(w, W_CONTACTS); c; c = PTR(c, 0x10))
        U32(c, 4) &= ~4u;
    for (uint8_t *j = PTR(w, W_JOINTS); j; j = PTR(j, 0xc))
        U8(j, 0x3c) = 0;
    uint8_t **stack = ((bs_alloc_fn)(uintptr_t)BS_ALLOC)(w + W_ALLOC, I32(w, W_NBODY) << 2);

    for (int i = 0; i < n; i++) {
        uint8_t *seed = a[i];
        if (U16(seed, 0) & 0xe)
            continue;
        if (U16(seed, 2) == 0)
            continue;
        I32(isl, 0x14) = 0;
        I32(isl, 0x1c) = 0;
        I32(isl, 0x18) = 0;
        int sc = 0;
        stack[sc++] = seed;
        U16(seed, 0) |= 4;
        while (sc > 0) {
            uint8_t *b = stack[--sc];
            ((uint8_t **)PTR(isl, 8))[I32(isl, 0x14)++] = b;
            U16(b, 0) &= (uint16_t)~8u;
            if (U16(b, 2) == 0)
                continue;
            for (uint8_t *cn = PTR(b, 0x70); cn; cn = PTR(cn, 0xc)) {
                uint8_t *c = PTR(cn, 4);
                if (U8(c, 4) & 5)
                    continue;
                if (I32(c, 8) == 0)
                    continue;
                ((uint8_t **)PTR(isl, 0xc))[I32(isl, 0x1c)++] = c;
                U32(PTR(cn, 4), 4) |= 4;
                uint8_t *o = PTR(cn, 0);
                if (U8(o, 0) & 4)
                    continue;
                stack[sc++] = o;
                U16(o, 0) |= 4;
            }
            for (uint8_t *jn = PTR(b, 0x6c); jn; jn = PTR(jn, 0xc)) {
                uint8_t *j = PTR(jn, 4);
                if (U8(j, 0x3c))
                    continue;
                ((uint8_t **)PTR(isl, 0x10))[I32(isl, 0x18)++] = j;
                U8(PTR(jn, 4), 0x3c) = 1;
                uint8_t *o = PTR(jn, 0);
                if (U8(o, 0) & 4)
                    continue;
                stack[sc++] = o;
                U16(o, 0) |= 4;
            }
        }
        ((bs_isl_solve_fn)(uintptr_t)BS_ISL_SOLVE)(isl, step, w + W_GRAVITY, U8(w, W_CORRECT),
                                                  U8(w, W_ALLOWSLEEP));
        int32_t pi = I32(isl, 0x2c), wp = I32(w, W_POSITER);
        I32(w, W_POSITER) = wp >= pi ? wp : pi;
        for (int k = 0; k < I32(isl, 0x14); k++) {
            uint8_t *b = ((uint8_t **)PTR(isl, 8))[k];
            if (U16(b, 2) == 0)
                U16(b, 0) &= (uint16_t)~4u;
        }
    }
    ((bs_free_fn)(uintptr_t)BS_FREE)(w + W_ALLOC, stack);

    for (int i = 0; i < n; i++) {
        uint8_t *b = a[i];
        if (U16(b, 0) & 0xa)
            continue;
        if (U16(b, 2) == 0)
            continue;
        if ((uint8_t)((bs_sync_fn)(uintptr_t)BS_SYNC)(b))
            continue;
        uint8_t *l = PTR(w, W_BOUNDARY);
        if (!l)
            continue;
        ((bs_violation_fn)(*(uintptr_t **)l)[2])(l, b);
    }
    ((bs_commit_fn)(uintptr_t)BS_COMMIT)(PTR(w, W_BP));
    ((bs_isl_fn)(uintptr_t)BS_ISL_DTOR)(isl);
}

/* ---- the hooks that keep the list ---- */

__attribute__((cdecl, force_align_arg_pointer))
static void *bs_create(uint8_t *w, const void *def)
{
    uint8_t *b = g_create(w, def);
    if (!b)
        return b;
    bs_lock();
    g_st.created++;
    if (w != g_world)
        g_dirty = 1;
    else if (bs_keep(b))
        bs_insert(0, b);                 /* the new body is the list head */
    bs_unlock();
    return b;
}

__attribute__((cdecl, force_align_arg_pointer))
static void bs_destroy(uint8_t *w, uint8_t *b)
{
    int locked = U8(w, W_LOCK);          /* a locked world keeps the body */
    g_destroy(w, b);
    if (locked)
        return;
    bs_lock();
    int i = w == g_world ? bs_find(b) : -1;
    if (i >= 0) {
        memmove(g_a + i, g_a + i + 1, (size_t)(g_n - i - 1) * sizeof *g_a);
        g_n--;
        g_st.removed++;
    }
    bs_unlock();
}

/* After a mass change: a body that is now kept goes to its list position. */
static void bs_massed(uint8_t *b)
{
    uint8_t *w = PTR(b, 0x58);
    bs_lock();
    if (w == g_world && !g_dirty && bs_keep(b) && bs_find(b) < 0) {
        int idx = 0;
        uint8_t *x = PTR(w, W_BODIES);
        for (; x && x != b; x = PTR(x, 0x60)) {
            g_st.walked++;
            if (idx < g_n && x == g_a[idx])
                idx++;
        }
        if (x)
            bs_insert(idx, b);
        else
            g_dirty = 1;
        g_st.massed++;
    }
    bs_unlock();
}

__attribute__((cdecl, force_align_arg_pointer))
static void bs_setmass(uint8_t *b, const void *md)
{
    g_setmass(b, md);
    bs_massed(b);
}

__attribute__((cdecl, force_align_arg_pointer))
static void bs_setmassfs(uint8_t *b)
{
    g_setmassfs(b);
    bs_massed(b);
}

__attribute__((cdecl, force_align_arg_pointer))
static uint32_t bs_wctor(uint8_t *w, void *aabb, void *grav, uint32_t sleep)
{
    bs_lock();
    g_world = NULL;
    g_dirty = 1;
    bs_unlock();
    gmv_b2_world_new();
    return g_wctor(w, aabb, grav, sleep);
}

#if GMV_DEV
void gmv_b2solve_report(double seconds)
{
    if (!g_on || (!g_st.steps && !g_st.fallback))
        return;
    unsigned s = g_st.steps ? g_st.steps : 1;
    gmv_logfile("[perf]   box2d solve: %.0f steps a second by the short list (%.0f of %.0f bodies on "
                "it), %u by the original; list rebuilt %u, bodies added %u created / %u made "
                "non-static (%.0f list entries walked each), %u removed%s%s\n",
                g_st.steps / seconds, (double)g_st.listed / s, (double)g_st.bodies / s, g_st.fallback,
                g_st.rebuilds, g_st.created, g_st.massed,
                g_st.massed ? (double)g_st.walked / g_st.massed : 0.0, g_st.removed,
                g_verify ? ", checked against a full walk every step: " : "",
                g_verify ? (g_st.mismatch ? "MISMATCHES (see above)" : "all identical") : "");
    memset(&g_st, 0, sizeof g_st);
}
#endif

/* ---- install ---- */

static int bs_patch(uint32_t va, const unsigned char *buf, size_t n)
{
    DWORD old;
    if (!VirtualProtect((void *)(uintptr_t)va, n, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy((void *)(uintptr_t)va, buf, n);
    VirtualProtect((void *)(uintptr_t)va, n, old, &old);
    return 1;
}

static void bs_rel(unsigned char *at, uint32_t from_end, uint32_t to)
{
    int32_t rel = (int32_t)(to - from_end);
    memcpy(at, &rel, 4);
}

static const struct bs_hook {
    uint32_t fn;
    const char *pro;                     /* whole instructions moved to the trampoline */
    unsigned len;
    void *to;
    void **orig;
} g_hooks[] = {
    { BS_CREATE,    "\x55\x89\xe5\x83\xec\x18", 6, (void *)bs_create,    (void **)&g_create },
    { BS_DESTROY,   "\x55\x89\xe5\x57\x56",     5, (void *)bs_destroy,   (void **)&g_destroy },
    { BS_SETMASS,   "\x55\x89\xe5\x57\x56",     5, (void *)bs_setmass,   (void **)&g_setmass },
    { BS_SETMASSFS, "\x55\x89\xe5\x57\x56",     5, (void *)bs_setmassfs, (void **)&g_setmassfs },
    { BS_WORLDCTOR, "\x55\x89\xe5\x57\x56",     5, (void *)bs_wctor,     (void **)&g_wctor },
};
#define BS_NHOOK (int)(sizeof g_hooks / sizeof g_hooks[0])

void gmv_apply_b2solve(void)
{
    const char *e = gmv_dbgenv("GMV_FAST_B2SOLVE");
    if (e && *e == '0') {
        gmv_log("[b2solve] FAST_B2SOLVE=0: the original b2World::Solve kept\n");
        return;
    }
    uint32_t h = 0x811c9dc5u;
    for (uint32_t a = BS_SOLVE; a < BS_SOLVE_END; a++)
        h = (h ^ *(const uint8_t *)(uintptr_t)a) * 0x01000193u;
    const unsigned char *site = (const unsigned char *)(uintptr_t)BS_CALLSITE;
    int32_t rel;
    memcpy(&rel, site + 1, 4);
    int ok = h == BS_SOLVE_FNV && site[0] == 0xe8 && BS_CALLSITE + 5 + (uint32_t)rel == BS_SOLVE;
    for (int i = 0; ok && i < BS_NHOOK; i++)
        ok = !memcmp((const void *)(uintptr_t)g_hooks[i].fn, g_hooks[i].pro, g_hooks[i].len);
    if (!ok) {
        gmv_log("[b2solve] b2World::Solve or the body functions are not as expected; skipped\n");
        return;
    }
    unsigned char *tr = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) {
        gmv_log("[b2solve] VirtualAlloc for the trampolines failed (%lu); skipped\n", GetLastError());
        return;
    }
    for (int i = 0; i < BS_NHOOK; i++) {     /* trampolines first: the original's first bytes, then back */
        unsigned char *t = tr + i * 16;
        memcpy(t, g_hooks[i].pro, g_hooks[i].len);
        t[g_hooks[i].len] = 0xe9;
        bs_rel(t + g_hooks[i].len + 1, (uint32_t)(uintptr_t)(t + g_hooks[i].len + 5),
               g_hooks[i].fn + g_hooks[i].len);
        *g_hooks[i].orig = t;
    }
    FlushInstructionCache(GetCurrentProcess(), tr, 4096);
    if (GMV_DEV) {
        const char *v = gmv_dbgenv("GMV_B2_VERIFY");
        g_verify = v && *v && *v != '0';
    }
    for (int i = 0; i < BS_NHOOK; i++) {
        unsigned char j[5] = { 0xe9 };
        bs_rel(j + 1, g_hooks[i].fn + 5, (uint32_t)(uintptr_t)g_hooks[i].to);
        if (!bs_patch(g_hooks[i].fn, j, 5)) {
            gmv_log("[b2solve] write at 0x%06x failed (%lu)\n", g_hooks[i].fn, GetLastError());
            /* the hooks already written only keep the list, which nothing reads without the call below */
            FlushInstructionCache(GetCurrentProcess(), NULL, 0);
            return;
        }
    }
    unsigned char c[4];
    bs_rel(c, BS_CALLSITE + 5, (uint32_t)(uintptr_t)bs_solve);
    if (!bs_patch(BS_CALLSITE + 1, c, 4)) {
        gmv_log("[b2solve] write at 0x%06x failed (%lu)\n", BS_CALLSITE, GetLastError());
        FlushInstructionCache(GetCurrentProcess(), NULL, 0);
        return;
    }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    g_on = 1;
    gmv_log("[b2solve] b2World::Solve walks only the non-static bodies (call 0x%06x; CreateBody, "
            "DestroyBody, SetMass, SetMassFromShapes, b2World hooked)%s\n", BS_CALLSITE,
            g_verify ? "; GMV_B2_VERIFY: checked against a full walk every step" : "");
}
