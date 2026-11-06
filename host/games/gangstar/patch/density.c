/* density.c -- TRAFFIC_DENSITY, PEDESTRIAN_DENSITY and PARKED_CAR_DENSITY */
#include "compat.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

#define SV_FN        0x00080272u

/* ---- TRAFFIC_DENSITY / PEDESTRIAN_DENSITY / PARKED_CAR_DENSITY: more (or fewer) of each in the city
   (enhancements, 1 = as on the Mac, forced to 1 by ORIGINAL_MAC_EXPERIENCE) ----
   The world's counts are kept by spawn point kind: the spawner 0x082504 adds 1 to counts[kind] at
   0x0828f8 and gives the object that kind (vfunc 0x4c, Vehicle +0x2a8), which takes it off again
   when the object goes.  Kind 3 points put a car on a road (0x07fcea checks the lane) and give it a
   driver and its drive (0x10dc3e, 0x022238): the moving traffic.  Kind 17 points put a car down
   without that step: the parked cars.  Kind 8 points put down NPCs (vtable 0x64ef88, the Character
   methods): the pedestrians.  The check 0x080272 takes, for these kinds, the point's own cap (+0x74 by
   day, +0x78 at night, when it is not negative; the points have none by day and 2 or 3 at night) or a
   built-in 18 / 10 / 14 (imm32 at 0x0802ec, 0x080309, 0x080326), then the performance profile's +0x1c
   / +0x24 / +0x20 if lower (40 / 20 / 40 on the Mac), then all three kinds together (and kind 16)
   under 48 (imm8 at 0x0803f3).  A point waits a random d0..d1 ms between spawns (its record +8, +0xc:
   0x6426e0, 0x642760, 0x642820, by the table 0x6888c0).
   Each density multiplies its kind's built-in cap and divides its wait once, and multiplies the point's
   own caps and the profile's for the length of each check.  The total of 48 grows by what the built-in
   caps grew, up to 127 (the most its imm8 holds).  More objects also fill the Box2D broadphase
   (16384 proxies), so its CreateProxy guard from DRAW_DISTANCE goes in as well.
   The spawner makes nothing: it takes an object of one of its group's types from a pool of 256
   (0x689180; the takers 0x081bc1, 0x081e6b, 0x082854, 0x083242, 0x083939, 0x08415e) and gives up
   when none is there.  An object that goes (0x020ed0, more than 14000 from centre B in 0x1a8f6a) is
   put back (0x08195c).  The pool is built once per level (0x0819e8 from 0x1ad591, cleared by
   0x07f9c4): one object of each type the spawn groups name, 14 of type 4, 11 of type 61, 3 of types
   60, 90, 91, 190 and 191.  That is what held the counts at the Mac's whatever the caps.  So the
   densities also move the pool to PD_POOL_N slots and, after the build, add copies of each car and
   motorbike (the larger of the traffic and parked densities) and each NPC (the pedestrian density).
   The caps alone barely help: at 4x the counts stayed where the Mac's are, every check allowed and
   few spawns made.  A point spawns only at its locations that lie outside one circle and inside a
   second, both 14000 across and centred ahead of the camera (0x0805e2, again in 0x07fcea; the table
   0x642660 {radius A, radius B, -> centre A, -> centre B} by the point's band +0x48).  0x082c12, each
   frame from 0x1a2396, puts the centres at the camera plus its look direction times 3039 / 3900 for
   the vehicles (0x688cc0 / 0x688cf0, copied to 0x689120 / 0x689130) and 1000 / 1700 for the people
   (0x688ce0 / 0x688d10): a strip 8.6 m (vehicles) or 7 m (people) deep, about 140 m out.  So each
   density above 1 also moves centre A back towards the camera, which deepens the strip inwards by the
   same factor (at 4x: 144..179 m ahead for vehicles, 129..157 m for people; the fog starts far beyond
   either).  Each check (gmv_can_spawn, ahead of the point's own search) sets its own kind's.  Centre B
   stays: 0x1a8f6a takes an object away by its distance from centre B, so moving B outwards had the
   new objects taken away as soon as they were made.  That check is also why the Mac churns: it takes
   away at the very radius the strip lies on, so the slightest turn of the camera takes away what was
   just made.  The densities take away at PD_KEEP times the radius instead (its ucomiss at 0x1a93aa
   pointed at pd_keep2).
   During a race the city goes back to the Mac's numbers and comes back afterwards.  The race is the
   object at *(*0x689e98 + 8) (as the script commands get it, 0xf2ed0); its state +0x14 is 1 for the
   countdown (RaceStart, 0xb16bc), 2 while racing (0xb16cc), 3 once over, and 0 again when
   RaceWaitEnded or the RaceStart's abort stops it (0xb1740).  pd_follow_race, from each spawn check
   and each frame's centre update, rewrites the caps, waits, total and despawn distance when that
   changes. */
#define PD_MAX       4.0
#define PD_TOTAL_IMM 0x000803f3u
#define PD_CENTRE_SITE 0x001a2396u   /* call 0x082c12, the spawn centres' update */
#define PD_CENTRE_FN   0x00082c12u
static const uint32_t pd_centres[3][2] = {   /* {centre A, centre B} */
    {0x00688cc0u, 0x00688cf0u}, {0x00689120u, 0x00689130u}, {0x00688ce0u, 0x00688d10u},
};
static float pd_centre_a[3][2];              /* each centre A as the game put it, x and y */
#define PD_KEEP_SITE 0x001a93adu   /* disp32 of 0x1a8f6a's ucomiss against 14000^2 (0x5f6ea0), PIC base 0x1a8f7b */
#define PD_KEEP_BASE 0x001a8f7bu
#define PD_KEEP_OLD  0x005f6ea0u
#define PD_KEEP      1.2           /* how much farther from centre B than the spawns an object goes */
static float pd_keep2;

const pd_kinds_t pd_kinds[3] = {
    {"GMV_TRAFFIC_DENSITY",    3, 18, 0x1c, 0x000802ecu, 0x006426e0u, 400, 800},
    {"GMV_PEDESTRIAN_DENSITY", 8, 14, 0x20, 0x00080326u, 0x00642820u, 500, 1000},
    {"GMV_PARKED_CAR_DENSITY", 17, 10, 0x24, 0x00080309u, 0x00642760u, 400, 800},
};
double pd_scale[3] = {1, 1, 1};      /* in force now: the ini's, or 1 during a race */
double pd_user[3] = {1, 1, 1};       /* the ini's */
int pd_on, pd_racing;
#define PD_RACE_MGR  0x00689e98u   /* the race's owner (0xf2ed0); the race is at +8 */

/* A word of the game's memory, 0 if it cannot be read. */
static uint32_t pd_rd(uint32_t a)
{
    return (a > 0x1000 && !IsBadReadPtr((void *)(uintptr_t)a, 4)) ? *(const uint32_t *)(uintptr_t)a : 0;
}

static int pd_race_on(void)
{
    uint32_t mgr = *(const uint32_t *)(uintptr_t)PD_RACE_MGR, race = mgr ? *(const uint32_t *)(uintptr_t)(mgr + 8) : 0;
    int st = race ? *(const int *)(uintptr_t)(race + 0x14) : 0;
    return st == 1 || st == 2;
}

/* The caps, waits and total for pd_scale; the total as written */
int pd_write(void)
{
    int total = 48;
    for (int i = 0; i < 3; i++) {
        double s = pd_scale[i];
        int cap = (int)(pd_kinds[i].cap * s + 0.5);
        total += cap - pd_kinds[i].cap;
        uint32_t d[2] = { pd_kinds[i].d0, pd_kinds[i].d1 };
        if (s > 0)
            for (int j = 0; j < 2; j++)
                d[j] = (uint32_t)(d[j] / s + 0.5) < 1 ? 1 : (uint32_t)(d[j] / s + 0.5);
        if (!patch_bytes(pd_kinds[i].cap_imm, (const unsigned char *)&cap, 4) ||
            !patch_bytes(pd_kinds[i].rec + 8, (const unsigned char *)d, 8))
            return -1;
    }
    unsigned char t = (unsigned char)(total < 0 ? 0 : total > 127 ? 127 : total);
    patch_bytes(PD_TOTAL_IMM, &t, 1);
    int more = 0;
    for (int i = 0; i < 3; i++)
        more |= pd_scale[i] > 1;
    double keep = pd_racing || !more ? 1 : PD_KEEP;   /* the Mac's when no density is above it */
    pd_keep2 = (float)(14000 * keep * 14000 * keep);
    return t;
}

static void pd_follow_race(void)
{
    if (!pd_on || pd_race_on() == pd_racing)
        return;
    pd_racing = !pd_racing;
    for (int i = 0; i < 3; i++)
        pd_scale[i] = pd_racing ? 1 : pd_user[i];
    if (pd_write() < 0)
        gmv_log("[gamepatch] population density write failed (%lu)\n", GetLastError());
    else
        gmv_log("[gamepatch] population density: %s\n", pd_racing ? "race on, the Mac's city until it ends"
                                                                  : "race over, the densities are back");
}

static int pd_cap(int v, int k)
{
    return v < 0 ? v : (int)(v * pd_scale[k] + 0.5);
}

/* Centre A back towards the camera, s times as far from centre B as the game put it (never nearer) */
static void pd_set_band(double s)
{
    if (s < 1)
        s = 1;
    for (int i = 0; i < 3; i++) {
        float *a = (float *)(uintptr_t)pd_centres[i][0];
        const float *b = (const float *)(uintptr_t)pd_centres[i][1];
        for (int j = 0; j < 2; j++)
            a[j] = (float)(b[j] + (pd_centre_a[i][j] - b[j]) * s);
    }
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_spawn_centres(uint32_t arg)
{
    ((void (*)(uint32_t))(uintptr_t)PD_CENTRE_FN)(arg);
    pd_follow_race();
    for (int i = 0; i < 3; i++) {
        const float *a = (const float *)(uintptr_t)pd_centres[i][0];
        pd_centre_a[i][0] = a[0], pd_centre_a[i][1] = a[1];
    }
    double s = 1;
    for (int k = 0; k < 3; k++)
        if (pd_scale[k] > s)
            s = pd_scale[k];
    pd_set_band(s);
}

/* The spawner, for SPAWN_FADE (fades in what it took from the pool) */
int sf_hooked;
__attribute__((cdecl, force_align_arg_pointer, noinline))
int gmv_spawn_make(uint32_t sp, uint32_t loc)
{
    if (sf_hooked)
        sf_before_make();
    int r = ((int (*)(uint32_t, uint32_t))(uintptr_t)PD_MAKE_FN)(sp, loc);
    if (sf_hooked)
        sf_after_make();
    return r;
}

#define PD_BUILD_SITE 0x001ad591u   /* call 0x0819e8, the pool's build at level load */
#define PD_BUILD_FN   0x000819e8u
#define PD_NEW_FN     0x000220d4u   /* the object factory (world, type) */
#define PD_TYPE_FN    0x00026e1eu   /* an object's type */
#define PD_FREE_FN    0x00090430u
#define PD_CLEAR_SIZE 0x0007fb31u   /* imm32 0x400, the clear's memset of the pool */
static const uint32_t pd_pool_refs[10][2] = {   /* {disp32 to the pool, its PIC base} */
    {0x0007f9d5u, 0x0007f9d2u}, {0x0007fb26u, 0x0007f9d2u}, {0x0008199au, 0x00081969u},
    {0x00081b0bu, 0x000819f9u}, {0x00081bc3u, 0x00081bb0u}, {0x00081e6du, 0x00081c49u},
    {0x00082856u, 0x00082515u}, {0x00083244u, 0x00083017u}, {0x0008393bu, 0x00083017u},
    {0x00084160u, 0x0008407eu},
};
static const uint32_t pd_pool_bounds[8] = {     /* imm32 0x100 of each walk over the pool */
    0x0007f9dau, 0x000819dau, 0x00081bd6u, 0x00081e86u, 0x00082866u, 0x00083256u, 0x00083956u, 0x00084176u,
};
uint32_t pd_pool[PD_POOL_N];
int pd_pool_on;

double pd_built[3];                  /* the densities the pool was built for (0 before the first build) */
static uint32_t pd_extra[PD_POOL_N];        /* the copies we added, in the pool or out in the city */
int pd_extra_n;
static int pd_trim;                         /* ORIGINAL_MAC_EXPERIENCE: those out in the city go as they come back */

/* A copy the Mac's pool has no slot for is let go: out of every slot, asleep as the pool keeps it, and not freed.
   The Mac frees nothing of its pool before a load, so its objects may keep pointers to one that went home; a
   running game must not lose the memory under them (suspected in a fault of 2026-11-03, replaying a mission after
   the switch).  So each round of the switch costs one set of copies until the game ends; a load does not get them
   back either, as nothing after one can tell a stale one from a good one. */
#if GMV_DEV
#define PD_LEFT_N 8192
static uint32_t pd_left[PD_LEFT_N];
static int pd_left_n;
#endif

static void pd_let_go(uint32_t o)
{
#if GMV_DEV
    if (pd_left_n < PD_LEFT_N)
        pd_left[pd_left_n++] = o;
#else
    (void)o;
#endif
}

/* How many copies of each of the build's objects: cars and motorbikes serve the traffic and the
   parked cars, NPCs the pedestrians */
static double pd_pool_scale(uint32_t vt)
{
    double s = vt == 0x0064ef88u ? pd_built[1]
             : vt == 0x00656068u || vt == 0x00656228u ? (pd_built[0] > pd_built[2] ? pd_built[0] : pd_built[2])
             : 1;
    return s < 1 ? 1 : s;
}

/* The densities in force, and with the options tab those of the file, so the pool has room for them */
static void pd_built_read(void)
{
    for (int i = 0; i < 3; i++) {
        const char *f = tab_live() ? gmv_config_file_value(pd_kinds[i].env + 4) : NULL;
        double v = f && *f ? atof(f) : 1;
        v = v > PD_MAX ? PD_MAX : v;
        pd_built[i] = v > pd_user[i] ? v : pd_user[i];
    }
}

/* The copies wanted of each type the build made (made[t] of them, have[t] copies already), into free
   slots; how many went in. */
static int pd_pool_add_more(const int *made, const int *have, const uint32_t *vt, int *full)
{
    uint32_t world = pd_rd(SV_WORLD);
    int slot = 0, added = 0;
    for (int t = 0; t < 256 && slot < PD_POOL_N; t++)
        for (int extra = (int)(made[t] * pd_pool_scale(vt[t]) + 0.5) - made[t] - have[t]; extra > 0; extra--) {
            while (slot < PD_POOL_N && pd_pool[slot])
                slot++;
            if (slot == PD_POOL_N || pd_extra_n == PD_POOL_N)
                break;
            /* as the build does it: poolable, its +0x1c freed, put to sleep (vfunc 0xb4) */
            uint32_t o = ((uint32_t (*)(uint32_t, int))(uintptr_t)PD_NEW_FN)(world, t);
            if (!o)
                break;
            *(uint8_t *)(uintptr_t)(o + 5) |= 1;
            ((int (*)(uint32_t))(uintptr_t)PD_TYPE_FN)(o);
            uint32_t *res = (uint32_t *)(uintptr_t)(o + 0x1c);
            if (*res)
                ((void (*)(uint32_t))(uintptr_t)PD_FREE_FN)(*res);
            *res = 0;
            ((void (*)(uint32_t))(uintptr_t)pd_rd(pd_rd(o) + 0xb4))(o);
            pd_pool[slot] = o, pd_extra[pd_extra_n++] = o, added++;
        }
    *full = slot == PD_POOL_N;
    return added;
}

static int pd_pool_add(const int *made, const uint32_t *vt, int *full)
{
    static const int none[256];
    return pd_pool_add_more(made, none, vt, full);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_pool_build(void)
{
    static uint32_t before[PD_POOL_N];
    pd_built_read();
    pd_extra_n = 0;                             /* the build's clear has freed them */
    memcpy(before, pd_pool, sizeof pd_pool);
    ((void (*)(void))(uintptr_t)PD_BUILD_FN)();
    int made[256] = {0}, n = 0, added = 0;
    uint32_t vt[256] = {0};
    for (int i = 0; i < PD_POOL_N; i++)
        if (pd_pool[i] && pd_pool[i] != before[i]) {
            int t = ((int (*)(uint32_t))(uintptr_t)PD_TYPE_FN)(pd_pool[i]);
            n++;
            if (t >= 0 && t < 256)
                made[t]++, vt[t] = pd_rd(pd_pool[i]);
        }
    int full = 0;
    added = pd_pool_add(made, vt, &full);
    gmv_log("[gamepatch] population density: the spawn pool holds %d objects, %d more for the densities "
            "(room for %d)%s\n", n, added, PD_POOL_N, full ? "; FULL" : "");
}

int pd_is_extra(uint32_t o)
{
    for (int i = 0; i < pd_extra_n; i++)
        if (pd_extra[i] == o)
            return 1;
    return 0;
}

/* Whether the world's lists or a pool hold this object, read by its address alone. */
int pd_object_known(uint32_t o)
{
    static const uint32_t lists[3] = {0x418, 0x424, 0x430};
    uint32_t world = pd_rd(SV_WORLD);
    for (int l = 0; world && l < 3; l++) {
        uint32_t x = *(const uint32_t *)(uintptr_t)(world + lists[l] + 4);
        for (int i = 0; x && i < 8192; i++, x = *(const uint32_t *)(uintptr_t)(x + 0xc))
            if (x == o)
                return 1;
    }
    for (int i = 0; i < PD_POOL_N; i++)
        if (pd_pool[i] == o)
            return 1;
    for (int i = 0; i < 0x100; i++)
        if (((const uint32_t *)(uintptr_t)PD_POOL_OLD)[i] == o)
            return 1;
    return 0;
}

/* ORIGINAL_MAC_EXPERIENCE going on: a car remembers its radio station at +0xc8 (0x10f0c0, read back at
   0x10a613), and the game's own pick and logo know of five. */
void cr_cars_off_sixth(void)
{
    static const uint32_t lists[3] = {0x418, 0x424, 0x430};
    uint32_t world = pd_rd(SV_WORLD);
    const uint32_t *pool = pd_pool_on ? pd_pool : (const uint32_t *)(uintptr_t)PD_POOL_OLD;
    int n = pd_pool_on ? PD_POOL_N : 0x100;
    char (*is_vehicle)(uint32_t) = (char (*)(uint32_t))(uintptr_t)0x00020c6au;
    for (int i = 0; i < n; i++)
        if (pool[i] && is_vehicle(pool[i]) && (int)pd_rd(pool[i] + 0xc8) >= CR_GAME_STATIONS)
            *(int *)(uintptr_t)(pool[i] + 0xc8) = 0;
    for (int l = 0; world && l < 3; l++) {
        uint32_t o = *(const uint32_t *)(uintptr_t)(world + lists[l] + 4);
        for (int i = 0; o && i < 8192; i++, o = *(const uint32_t *)(uintptr_t)(o + 0xc))
            if (is_vehicle(o) && (int)pd_rd(o + 0xc8) >= CR_GAME_STATIONS)
                *(int *)(uintptr_t)(o + 0xc8) = 0;
    }
}

/* The copies for the densities in a running game (the build's work, as ORIGINAL_MAC_EXPERIENCE goes off or a
   density goes up in the menu; the pool builds only once per level):
   what the build made is what is in the pool and what is out in the city and goes back to it. */
void pd_pool_top_up(void)
{
    static const uint32_t lists[3] = {0x418, 0x424, 0x430};
    int made[256] = {0}, have[256] = {0}, full = 0;
    uint32_t vt[256] = {0}, world = pd_rd(SV_WORLD);
    pd_built_read();
    pd_trim = 0;
    if (!world || !pd_pool_on)
        return;                                 /* (no world yet: the build sees to them) */
    for (int l = 0; l <= 3; l++) {
        uint32_t o = l == 3 ? 0 : *(const uint32_t *)(uintptr_t)(world + lists[l] + 4);
        for (int i = 0; i < (l == 3 ? PD_POOL_N : 8192); i++) {
            if (l == 3)
                o = pd_pool[i];
            else if (!o)
                break;
            if (o && (l == 3 || (*(const uint8_t *)(uintptr_t)(o + 5) & 1))) {
                int t = ((int (*)(uint32_t))(uintptr_t)PD_TYPE_FN)(o);
                if (t >= 0 && t < 256) {
                    if (pd_is_extra(o))
                        have[t]++;
                    else
                        made[t]++, vt[t] = pd_rd(o);
                }
            }
            if (l != 3)
                o = *(const uint32_t *)(uintptr_t)(o + 0xc);
        }
    }
    int added = pd_pool_add_more(made, have, vt, &full);
    gmv_log("[gamepatch] population density: %d more objects in the spawn pool for the densities (%d there "
            "already)%s\n", added, pd_extra_n - added, full ? "; FULL" : "");
}

/* Back to the Mac's pool: our copies let go, the rest in their order.  A copy out in the city goes when it comes
   back (gmv_pool_tick). */
void pd_unmove_pool(void)
{
    uint32_t *old = (uint32_t *)(uintptr_t)PD_POOL_OLD;
    int at = 0;
    memset(old, 0, 0x100 * sizeof *old);
    for (int i = 0; i < PD_POOL_N; i++) {
        uint32_t o = pd_pool[i];
        if (!o)
            continue;
        pd_pool[i] = 0;
        if (pd_is_extra(o) || at == 0x100) {
            for (int e = 0; e < pd_extra_n; e++)
                if (pd_extra[e] == o)
                    pd_extra[e] = pd_extra[--pd_extra_n];
            pd_let_go(o);
        } else
            old[at++] = o;
    }
    pd_trim = pd_extra_n > 0;
}

#if GMV_DEV
/* A copy we let go that the game still walks: what the fault of 2026-11-03 would be, had it been freed. */
void pd_ws_left(uint32_t ch)
{
    for (int i = 0; i < pd_left_n; i++)
        if (pd_left[i] == ch) {
            gmv_log("[pool] copy %08x was let go but is walking: vtable %08x, +4 %08x, the world's or a pool's %d\n",
                    ch, pd_rd(ch), pd_rd(ch + 4), pd_object_known(ch));
            pd_left[i] = pd_left[--pd_left_n];          /* once for each */
            return;
        }
}

/* A walker whose memory went while the ray ran: where the game still knows it from, just before the fault
   this leads to.  The [pool] lines above it say if it was one of our copies. */
void pd_ws_report(uint32_t ch, uint32_t vt_was)
{
    static const uint32_t lists[3] = {0x418, 0x424, 0x430};
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((const void *)(uintptr_t)(ch + 0x514), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT)
        return;
    uint32_t world = pd_rd(SV_WORLD);
    int in_list = -1, in_pool = 0, in_old = 0;
    for (int l = 0; world && l < 3 && in_list < 0; l++) {
        uint32_t o = *(const uint32_t *)(uintptr_t)(world + lists[l] + 4);
        for (int i = 0; o && i < 8192; i++, o = *(const uint32_t *)(uintptr_t)(o + 0xc))
            if (o == ch)
                in_list = l;
    }
    for (int i = 0; i < PD_POOL_N; i++)
        in_pool |= pd_pool[i] == ch;
    for (int i = 0; i < 0x100; i++)
        in_old |= ((const uint32_t *)(uintptr_t)PD_POOL_OLD)[i] == ch;
    gmv_log("[pool] walker %08x gone during its ray: vtable was %08x, now %08x; in the world's list %d, our pool %d, "
            "the Mac's pool %d, one of our copies now %d\n",
            ch, vt_was, pd_rd(ch), in_list, in_pool, in_old, pd_is_extra(ch));
}
#endif

/* Once per present.  Our copies that were out in the city as the pool went back are let go as they return to
   it; a new load has freed them all. */
int pd_world_gen, pd_trim_gen;

void gmv_pool_tick(void)
{
    if (pd_trim) {
        uint32_t *old = (uint32_t *)(uintptr_t)PD_POOL_OLD;
        if (pd_trim_gen != pd_world_gen)
            pd_extra_n = 0;
        for (int i = 0; i < 0x100 && pd_extra_n; i++)
            for (int e = 0; old[i] && e < pd_extra_n; e++)
                if (pd_extra[e] == old[i]) {
                    pd_let_go(old[i]);
                    pd_extra[e] = pd_extra[--pd_extra_n];
                    old[i] = 0;
                }
        pd_trim = pd_extra_n > 0;
    }
#if GMV_DEV
    /* How many cars and people the city has, read only: no hook, so it says the same with or without
       ORIGINAL_MAC_EXPERIENCE (the Mac's, by a spawn survey on 2026-10-27: traffic 15-17, people and drivers
       31-33, parked 10-11). */
    static int peak[3];
    static DWORD next;
    static const int kind[3] = {3, 8, 17};
    const int *cnt = (const int *)(uintptr_t)pd_rd(pd_rd(SV_WORLD) + 0x414);
    if (!cnt)
        return;
    for (int k = 0; k < 3; k++)
        if (cnt[kind[k]] > peak[k])
            peak[k] = cnt[kind[k]];
    DWORD now = GetTickCount();
    if ((int)(now - next) >= 0) {
        int free_n = 0, n = pd_pool_on ? PD_POOL_N : 0x100;
        const uint32_t *p = pd_pool_on ? pd_pool : (const uint32_t *)(uintptr_t)PD_POOL_OLD;
        for (int i = 0; i < n; i++)
            free_n += p[i] != 0;
        if (next && (peak[0] || peak[1] || peak[2]))
            gmv_log("[crowd] last 15 s, most at once: traffic %d, people and drivers %d, parked %d; now in the pool "
                    "%d (of %d slots), our copies %d, night cap %s\n", peak[0], peak[1], peak[2], free_n, n, pd_extra_n,
                    pd_rd(0x0068a104u) == 3 ? "on" : "off");
        peak[0] = peak[1] = peak[2] = 0;
        next = now + 15000;
    }
#endif
}

static int pd_move_pool(void)
{
    for (int i = 0; i < 10; i++)
        if (*(const uint32_t *)(uintptr_t)pd_pool_refs[i][0] + pd_pool_refs[i][1] != PD_POOL_OLD)
            return 0;
    for (int i = 0; i < 8; i++)
        if (*(const uint32_t *)(uintptr_t)pd_pool_bounds[i] != 0x100)
            return 0;
    if (*(const uint32_t *)(uintptr_t)PD_CLEAR_SIZE != 0x400 || !sm_is_call(PD_BUILD_SITE, PD_BUILD_FN))
        return 0;
    uint32_t n = PD_POOL_N, size = sizeof pd_pool;
    /* in a running game it holds the objects by now; they move with it */
    memcpy(pd_pool, (const void *)(uintptr_t)PD_POOL_OLD, 0x100 * sizeof *pd_pool);
    memset((void *)(uintptr_t)PD_POOL_OLD, 0, 0x100 * sizeof *pd_pool);
    for (int i = 0; i < 10; i++) {
        uint32_t d = (uint32_t)(uintptr_t)pd_pool - pd_pool_refs[i][1];
        if (!patch_bytes(pd_pool_refs[i][0], (const unsigned char *)&d, 4))
            return 0;
    }
    for (int i = 0; i < 8; i++)
        patch_bytes(pd_pool_bounds[i], (const unsigned char *)&n, 4);
    patch_bytes(PD_CLEAR_SIZE, (const unsigned char *)&size, 4);
    int32_t r = (int32_t)((uint32_t)(uintptr_t)gmv_pool_build - (PD_BUILD_SITE + 5));
    return patch_bytes(PD_BUILD_SITE + 1, (const unsigned char *)&r, 4);
}

int pd_centres_on;

__attribute__((cdecl, force_align_arg_pointer))
int gmv_can_spawn(uint32_t sp)
{
    int kind = *(const int *)(uintptr_t)(sp + 0x28), k = -1, r;
    pd_follow_race();
    for (int i = 0; i < 3; i++)
        if (pd_kinds[i].kind == kind && pd_scale[i] != 1)
            k = i;
    if (pd_centres_on)
        pd_set_band(k >= 0 ? pd_scale[k] : 1);
    int *prof = (int *)(uintptr_t)pd_rd(0x0063dfbcu);
    if (pd_on && k >= 0 && prof) {
        int *own = (int *)(uintptr_t)(sp + 0x74), day = own[0], night = own[1];
        int *pc = (int *)((char *)prof + pd_kinds[k].prof), pv = *pc;
        own[0] = pd_cap(day, k), own[1] = pd_cap(night, k), *pc = pd_cap(pv, k);
        r = ((int (*)(uint32_t))(uintptr_t)SV_FN)(sp);
        own[0] = day, own[1] = night, *pc = pv;
    } else
        r = ((int (*)(uint32_t))(uintptr_t)SV_FN)(sp);
    return r;
}

static void apply_population_density(void)
{
    static const unsigned char total_code[3] = { 0x83, 0xf9, 0x30 };   /* cmpl $48,%ecx */
    int any = 0;
    for (int i = 0; i < 3; i++) {
        const char *e = gmv_getenv(pd_kinds[i].env);
        double s = e && *e ? atof(e) : 1;
        pd_user[i] = pd_scale[i] = s < 0 ? 0 : s > PD_MAX ? PD_MAX : s;
        any |= pd_scale[i] != 1;
    }
    if (!any && !tables_live())
        return;
    int ok = (sm_is_call(SV_SITE, SV_FN) || sm_is_call(SV_SITE, (uint32_t)(uintptr_t)gmv_can_spawn)) &&
             !memcmp((const void *)(uintptr_t)(PD_TOTAL_IMM - 2), total_code, 3);
    for (int i = 0; i < 3 && ok; i++) {
        const uint32_t *rec = (const uint32_t *)(uintptr_t)pd_kinds[i].rec;
        ok = *(const uint8_t *)(uintptr_t)(pd_kinds[i].cap_imm - 1) == 0xbf &&
             *(const int *)(uintptr_t)pd_kinds[i].cap_imm == pd_kinds[i].cap &&
             rec[0] == (uint32_t)pd_kinds[i].kind && rec[2] == pd_kinds[i].d0 && rec[3] == pd_kinds[i].d1;
    }
    if (!ok) {
        gmv_log("[gamepatch] spawn check or spawn records not the expected bytes; population density skipped\n");
        return;
    }
    int t = pd_write();
    if (t < 0) {
        gmv_log("[gamepatch] population density write failed (%lu)\n", GetLastError());
        return;
    }
    int deeper = tables_live();                 /* the options tab can raise them later */
    for (int i = 0; i < 3; i++)
        deeper |= pd_scale[i] > 1;
    if (deeper && sm_is_call(PD_CENTRE_SITE, PD_CENTRE_FN)) {
        int32_t r = (int32_t)((uint32_t)(uintptr_t)gmv_spawn_centres - (PD_CENTRE_SITE + 5));
        pd_centres_on = patch_bytes(PD_CENTRE_SITE + 1, (const unsigned char *)&r, 4);
    } else if (deeper)
        gmv_log("[gamepatch] spawn centres' update not the expected bytes; the spawn strip stays as deep as the Mac's\n");
    int keep = 0;
    if (deeper && *(const uint32_t *)(uintptr_t)PD_KEEP_SITE + PD_KEEP_BASE == PD_KEEP_OLD &&
        *(const float *)(uintptr_t)PD_KEEP_OLD == 14000.0f * 14000.0f) {
        uint32_t d = (uint32_t)(uintptr_t)&pd_keep2 - PD_KEEP_BASE;
        keep = patch_bytes(PD_KEEP_SITE, (const unsigned char *)&d, 4);
    } else if (deeper)
        gmv_log("[gamepatch] despawn check not the expected bytes; cars and people go where the Mac's do\n");
    if (deeper && !(pd_pool_on = pd_move_pool()))
        gmv_log("[gamepatch] spawn pool code not the expected bytes; no more cars or people than the Mac's pool holds\n");
    if (!dd_on)
        for (int i = 0; i < 2; i++)
            if (sm_is_call(dd_proxy_sites[i], DD_PROXY_FN)) {
                int32_t r = (int32_t)((uint32_t)(uintptr_t)gmv_create_proxy - (dd_proxy_sites[i] + 5));
                patch_bytes(dd_proxy_sites[i] + 1, (const unsigned char *)&r, 4);
            }
    pd_on = 1;
    gmv_log("[gamepatch] population density: traffic %.2gx (cap 18 -> %d), pedestrians %.2gx (14 -> %d), "
            "parked cars %.2gx (10 -> %d), all together 48 -> %d%s%s%s; the Mac's during races\n",
            pd_scale[0], (int)(18 * pd_scale[0] + 0.5), pd_scale[1], (int)(14 * pd_scale[1] + 0.5),
            pd_scale[2], (int)(10 * pd_scale[2] + 0.5), t, pd_centres_on ? "; spawn strip deepened inwards to match" : "",
            pd_pool_on ? "; spawn pool moved to make room" : "", keep ? "; taken away farther out to match" : "");
}

/* The spawn check's hook, for the densities. */
void apply_spawn_hook(void)
{
    apply_population_density();
    if (!pd_on)
        return;
    if (!sm_is_call(SV_SITE, SV_FN) && !sm_is_call(SV_SITE, (uint32_t)(uintptr_t)gmv_can_spawn)) {
        gmv_log("[gamepatch] spawn check call not the expected bytes; population densities not applied\n");
        return;
    }
    int32_t r = (int32_t)((uint32_t)(uintptr_t)gmv_can_spawn - (SV_SITE + 5));
    patch_bytes(SV_SITE + 1, (const unsigned char *)&r, 4);
}
