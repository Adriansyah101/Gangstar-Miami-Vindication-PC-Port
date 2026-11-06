/* sync_physics.c -- PHYSICS_SYNC: one physics step per tick */
#include "compat.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* ---- physics sync: one physics step per tick, at the tick rate (GMV_PHYSICS_SYNC=1) -------------

   The level update (0x1a1752) runs Box2D -- vehicles and loose objects -- in a fixed-step loop with
   no interpolation:  acc += min(dt, [0x949a18]) / 1000  (0x1a1ef3..0x1a1f2c, acc at 0x949ab4), then
   while acc >= 1/60: each body's vtbl[0x174](1/60), b2World::Step(0x1c3620, 1/60, 10), each body's
   vtbl[0x128] (sync), acc -= 1/60.  The step constant is loaded from 0x5f6e98 (1/60) and 0x5f6e9c
   (-1/60) and pushed as the immediate 0x3c888889 (1/60) at 0x1a1f78 and 0x1a1faa; nothing else
   uses acc or those constants.  0x1a1f3f also adds the same 1/60 to a game clock once per update.
   Drawn every other tick, at 60 ticks/s each drawn frame gets two steps.  At any other tick rate
   (FRAME_MS=13.333 for a 75 Hz panel: 1.6 steps per drawn frame, so 2,2,1,2,1...) cars move
   unevenly while the screen updates evenly, which is the driving judder.  The whole-ms dt (13/13/14,
   16/17/17) also lets the accumulator's phase settle near the threshold, giving 0/2-step ticks.
   This makes the step the pacer period S and runs exactly one step per paced tick: the three loads
   are re-aimed at host floats, the two immediates rewritten, and the acc update at 0x1a1f24 sends
   dt to gmv_physsync, which sets acc to 1.5*S for a tick near S (one step, residual S/2) and keeps
   the game's own catch-up (acc += dt) for a hitch.  At the default 16.667 ms S is exactly 1/60.
   On by default, off under ORIGINAL_MAC_EXPERIENCE; needs the tick pacer (FRAME_MS > 0).  After a change of the options menu's FRAME
   RATE, S follows the new rate, or goes back to 1/60 with the game's own acc += dt outside 1.2-40 ms
   (physics_sync_set). */
#define PHYS_ACC       0x00949ab4u   /* float physics accumulator (s)                                */
#define PHYS_ACC_SITE  0x001a1f24u   /* addss xmm0,[esi+0x7a8354]; movss [esi+0x7a8354],xmm0 (16 B)  */
#define PHYS_ACC_CONT  0x001a1f34u   /* movss xmm1,[esi+0x455738]: load step, compare acc            */
#define PHYS_PIC_BASE  0x001a1760u   /* esi in 0x1a1752 (call/pop)                                   */
#define PHYS_STEP_LD1  0x001a1f34u   /* f3 0f 10 8e <disp32> : movss xmm1,[esi+step]                 */
#define PHYS_NEG_LD    0x001a1ffeu   /* f3 0f 58 86 <disp32> : addss xmm0,[esi+negstep]              */
#define PHYS_STEP_LD2  0x001a2014u   /* 0f 2e 86 <disp32>    : ucomiss xmm0,[esi+step]               */
#define PHYS_IMM1      0x001a1f78u   /* c7 44 24 04 <imm32>  : body vtbl[0x174] step argument        */
#define PHYS_IMM2      0x001a1faau   /* c7 44 24 04 <imm32>  : b2World::Step step argument           */

static float g_phys_step, g_phys_negstep;
int g_phys_on;

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_physsync(float dt)
{
    float *acc = (float *)(uintptr_t)PHYS_ACC;
    float s = g_phys_step;
    if (g_phys_on && dt > 0.5f * s && dt < 1.5f * s)
        *acc = 1.5f * s;
    else
        *acc += dt;
}

/* Tyre grip under physics sync.  Each vehicle's vtbl[0x174] step (Car 0xfef68, Moto 0x10500a, Boat
   0x120f7e) ends by applying two lateral-grip impulses straight to its b2Body (the inlined
   ApplyImpulse: v += invMass*I at +0x40/+0x44, w += invI*cross(r,I) at +0x48), I = -k*m*v_lat at
   the front and rear wheels (k = 1.5*0.15, or 0.375*0.15 at the front in the other branch).  Unlike the drive force (b2Body force, integrated by Step) and the
   damping (scaled by the step argument), these are per step with no dt, so a shorter step grips
   proportionally harder per second: at FRAME_MS=6.667 (150 steps/s) 2.5x, and the car snaps into
   turns.  Read as force*(1/60), the impulse for a step S is I*S*60, so each of the four loads of an
   impulse component (8-byte movss xmmN,[ebp+disp]) is sent through a stub that multiplies the
   register by S*60 -- the stack slots are left alone, as Moto reuses them after the block.  The skid
   tests before the block still see the unscaled impulse, which is what keeps them on the original
   thresholds.  Helicopters have no grip impulses (only a speed cap). */
static const uint32_t grip_sites[] = {
    0x000ffff0u, 0x00100009u, 0x00100080u, 0x00100099u,     /* Car  [ebp-0x9c/-0xa0], [ebp-0xb0/-0xac]   */
    0x001065e9u, 0x00106602u, 0x00106679u, 0x00106692u,     /* Moto [ebp-0x118/-0x11c], [-0x12c/-0x128] */
    0x00121cb2u, 0x00121ccbu, 0x00121d32u, 0x00121d4bu,     /* Boat [ebp-0xb8/-0xb4], [ebp-0xac/-0xb0]   */
};
static const unsigned char grip_expect[][8] = {
    { 0xf3, 0x0f, 0x10, 0x8d, 0x64, 0xff, 0xff, 0xff }, { 0xf3, 0x0f, 0x10, 0x95, 0x60, 0xff, 0xff, 0xff },
    { 0xf3, 0x0f, 0x10, 0x95, 0x50, 0xff, 0xff, 0xff }, { 0xf3, 0x0f, 0x10, 0x8d, 0x54, 0xff, 0xff, 0xff },
    { 0xf3, 0x0f, 0x10, 0x8d, 0xe8, 0xfe, 0xff, 0xff }, { 0xf3, 0x0f, 0x10, 0x95, 0xe4, 0xfe, 0xff, 0xff },
    { 0xf3, 0x0f, 0x10, 0x95, 0xd4, 0xfe, 0xff, 0xff }, { 0xf3, 0x0f, 0x10, 0x8d, 0xd8, 0xfe, 0xff, 0xff },
    { 0xf3, 0x0f, 0x10, 0x9d, 0x48, 0xff, 0xff, 0xff }, { 0xf3, 0x0f, 0x10, 0xb5, 0x4c, 0xff, 0xff, 0xff },
    { 0xf3, 0x0f, 0x10, 0xad, 0x54, 0xff, 0xff, 0xff }, { 0xf3, 0x0f, 0x10, 0x95, 0x50, 0xff, 0xff, 0xff },
};
#define NGRIP (sizeof grip_sites / sizeof *grip_sites)

static float g_grip_scale;

/* Bike wheelie/stoppie under physics sync.  Moto's step (0x10500a) turns the change of the rider's
   lean since its last step into a pitch rate for that one step: rate = (lean - last lean) * 50000
   (0x106d36, the constant at 0x5f3ca8), which the wheel loop then applies as rate * step.  The lean
   changes by the same amount whatever the step, so the lift is proportional to the step: at
   FRAME_MS=1.25 (800 steps/s) 1/13 of the Mac's, and the rider leans while the bike stays down.  The
   multiply is sent through a stub that also multiplies by 1/(S*60), so the lift is the 1/60 step's. */
#define KICK_SITE 0x00106d36u   /* f3 0f 59 8a <disp32> : mulss xmm1,[edx+0x4eec8d] */
static float g_kick_scale = 1.0f;

int kick_tried, grip_tried;

static void kick_install(void)
{
    static const unsigned char expect[8] = { 0xf3, 0x0f, 0x59, 0x8a, 0x8d, 0xec, 0x4e, 0x00 };
    if (kick_tried)
        return;
    kick_tried = 1;
    if (memcmp((const void *)(uintptr_t)KICK_SITE, expect, 8)) {
        gmv_log("[gamepatch] bike lean kick at 0x%08x not the expected bytes; wheelies stay weak under physics sync\n",
                KICK_SITE);
        return;
    }
    unsigned char *st = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!st) {
        gmv_log("[gamepatch] VirtualAlloc for the bike lean stub failed (%lu)\n", GetLastError());
        return;
    }
    size_t o = 0;
    memcpy(st, expect, 8);                                          /* mulss xmm1,[edx+K]        */
    o = 8;
    st[o++] = 0xf3; st[o++] = 0x0f; st[o++] = 0x59; st[o++] = 0x0d; /* mulss xmm1,[g_kick_scale] */
    { uint32_t a = (uint32_t)(uintptr_t)&g_kick_scale; memcpy(st + o, &a, 4); o += 4; }
    emit_jmp(st, &o, KICK_SITE + 8);
    FlushInstructionCache(GetCurrentProcess(), st, o);
    unsigned char site[8] = { 0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)st - (KICK_SITE + 5));
    memcpy(site + 1, &rel, 4);
    if (!patch_bytes(KICK_SITE, site, sizeof site))
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); wheelies stay weak under physics sync\n", KICK_SITE,
                GetLastError());
    else
        gmv_log("[gamepatch] bike lean kick scaled by %.3f for the step (wheelie/stoppie as at 1/60 s)\n",
                g_kick_scale);
}

/* The stubs read g_grip_scale, so once in they follow a new step. */
static void grip_scale_in(double ms)
{
    g_grip_scale = (float)(ms * 0.06);
    g_kick_scale = g_grip_scale > 0.0f ? 1.0f / g_grip_scale : 1.0f;
    if (!(g_kick_scale > 0.999f && g_kick_scale < 1.001f))
        kick_install();
    if (grip_tried || (g_grip_scale > 0.999f && g_grip_scale < 1.001f))
        return;                                         /* S is 1/60: the impulses are already right */
    grip_tried = 1;
    for (size_t i = 0; i < NGRIP; i++)
        if (memcmp((const void *)(uintptr_t)grip_sites[i], grip_expect[i], 8)) {
            gmv_log("[gamepatch] grip impulse load at 0x%08x not the expected bytes; grip scaling skipped "
                    "(steering will be sharper than the original)\n", grip_sites[i]);
            return;
        }
    unsigned char *st = VirtualAlloc(NULL, NGRIP * 24, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!st) {
        gmv_log("[gamepatch] VirtualAlloc for grip stubs failed (%lu); grip scaling skipped\n", GetLastError());
        return;
    }
    size_t o = 0;
    for (size_t i = 0; i < NGRIP; i++) {
        unsigned char *stub = st + o;
        memcpy(st + o, grip_expect[i], 8);                              /* movss xmmN,[ebp+disp]   */
        o += 8;
        st[o++] = 0xf3; st[o++] = 0x0f; st[o++] = 0x59;                 /* mulss xmmN,[g_grip_scale] */
        st[o++] = (unsigned char)((grip_expect[i][3] & 0x38) | 0x05);
        { uint32_t a = (uint32_t)(uintptr_t)&g_grip_scale; memcpy(st + o, &a, 4); o += 4; }
        emit_jmp(st, &o, grip_sites[i] + 8);

        unsigned char site[8] = { 0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
        int32_t rel = (int32_t)((uint32_t)(uintptr_t)stub - (grip_sites[i] + 5));
        memcpy(site + 1, &rel, 4);
        if (!patch_bytes(grip_sites[i], site, sizeof site)) {
            gmv_log("[gamepatch] write at 0x%08x failed (%lu); grip scaling partial\n", grip_sites[i],
                    GetLastError());
            return;
        }
    }
    FlushInstructionCache(GetCurrentProcess(), st, o);
    gmv_log("[gamepatch] grip impulses (car/bike/boat, %u loads) scaled by %.3f for the %.3f ms step\n",
            (unsigned)NGRIP, g_grip_scale, ms);
}

static void grip_scale_set(double ms)
{
    patch_undo *keep = patch_undo_now;
    patch_undo_now = &mac_undo;
    grip_scale_in(ms);
    patch_undo_now = keep;
}

static int phys_install(double ms)
{
    static const unsigned char acc_expect[16] = { 0xf3, 0x0f, 0x58, 0x86, 0x54, 0x83, 0x7a, 0x00,
                                                  0xf3, 0x0f, 0x11, 0x86, 0x54, 0x83, 0x7a, 0x00 };
    static const unsigned char ld1_expect[8] = { 0xf3, 0x0f, 0x10, 0x8e, 0x38, 0x57, 0x45, 0x00 };
    static const unsigned char neg_expect[8] = { 0xf3, 0x0f, 0x58, 0x86, 0x3c, 0x57, 0x45, 0x00 };
    static const unsigned char ld2_expect[7] = { 0x0f, 0x2e, 0x86, 0x38, 0x57, 0x45, 0x00 };
    static const unsigned char imm_expect[8] = { 0xc7, 0x44, 0x24, 0x04, 0x89, 0x88, 0x88, 0x3c };
    if (memcmp((const void *)(uintptr_t)PHYS_ACC_SITE, acc_expect, sizeof acc_expect) ||
        memcmp((const void *)(uintptr_t)PHYS_STEP_LD1, ld1_expect, sizeof ld1_expect) ||
        memcmp((const void *)(uintptr_t)PHYS_NEG_LD, neg_expect, sizeof neg_expect) ||
        memcmp((const void *)(uintptr_t)PHYS_STEP_LD2, ld2_expect, sizeof ld2_expect) ||
        memcmp((const void *)(uintptr_t)PHYS_IMM1, imm_expect, sizeof imm_expect) ||
        memcmp((const void *)(uintptr_t)PHYS_IMM2, imm_expect, sizeof imm_expect)) {
        gmv_log("[gamepatch] physics step loop (0x%08x..0x%08x) not the expected bytes; physics sync skipped\n",
                PHYS_ACC_SITE, PHYS_STEP_LD2 + 7);
        return 0;
    }

    unsigned char *tr = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) {
        gmv_log("[gamepatch] VirtualAlloc for trampoline failed (%lu); physics sync skipped\n", GetLastError());
        return 0;
    }
    g_phys_step = (float)(ms / 1000.0);
    g_phys_negstep = -g_phys_step;

    /* xmm0 = min(dt, cap) / 1000 here; eax/ecx/edx/xmm are reloaded before their next use. */
    size_t o = 0;
    tr[o++] = 0x83; tr[o++] = 0xec; tr[o++] = 0x04;                     /* sub esp,4               */
    tr[o++] = 0xf3; tr[o++] = 0x0f; tr[o++] = 0x11; tr[o++] = 0x04; tr[o++] = 0x24; /* movss [esp],xmm0 */
    tr[o++] = 0xb8;                                                     /* mov eax,&gmv_physsync   */
    { uint32_t fnp = (uint32_t)(uintptr_t)&gmv_physsync; memcpy(tr + o, &fnp, 4); o += 4; }
    tr[o++] = 0xff; tr[o++] = 0xd0;                                     /* call eax                */
    tr[o++] = 0x83; tr[o++] = 0xc4; tr[o++] = 0x04;                     /* add esp,4               */
    tr[o++] = 0xf3; tr[o++] = 0x0f; tr[o++] = 0x10; tr[o++] = 0x05;     /* movss xmm0,[PHYS_ACC]   */
    { uint32_t a = PHYS_ACC; memcpy(tr + o, &a, 4); o += 4; }
    emit_jmp(tr, &o, PHYS_ACC_CONT);
    FlushInstructionCache(GetCurrentProcess(), tr, o);

    unsigned char site[16];
    memset(site, 0x90, sizeof site);
    site[0] = 0xe9;
    { int32_t rel = (int32_t)((uint32_t)(uintptr_t)tr - (PHYS_ACC_SITE + 5)); memcpy(site + 1, &rel, 4); }

    uint32_t d_step = (uint32_t)(uintptr_t)&g_phys_step - PHYS_PIC_BASE;
    uint32_t d_neg = (uint32_t)(uintptr_t)&g_phys_negstep - PHYS_PIC_BASE;
    uint32_t bits;
    memcpy(&bits, &g_phys_step, 4);
    if (!patch_bytes(PHYS_STEP_LD1 + 4, (const unsigned char *)&d_step, 4) ||
        !patch_bytes(PHYS_NEG_LD + 4, (const unsigned char *)&d_neg, 4) ||
        !patch_bytes(PHYS_STEP_LD2 + 3, (const unsigned char *)&d_step, 4) ||
        !patch_bytes(PHYS_IMM1 + 4, (const unsigned char *)&bits, 4) ||
        !patch_bytes(PHYS_IMM2 + 4, (const unsigned char *)&bits, 4) ||
        !patch_bytes(PHYS_ACC_SITE, site, sizeof site)) {
        gmv_log("[gamepatch] write in the physics step loop failed (%lu); physics sync may be partial\n",
                GetLastError());
        return 0;
    }
    gmv_log("[gamepatch] physics sync installed at 0x%08x (trampoline %p): one Box2D step per tick, "
            "step %.3f ms (%.2f Hz) instead of 1/60 s\n", PHYS_ACC_SITE, (void *)tr, ms, 1000.0 / ms);
    return 1;
}

/* Physics sync for a tick of ms: installed the first time the pacer runs at 1.2-40 ms, and from
   then on a step of ms there and the game's own 1/60 s outside it (the options menu's FRAME RATE). */
int phys_installed;

static int phys_install_undoable(double ms)
{
    patch_undo *keep = patch_undo_now;
    patch_undo_now = &mac_undo;
    int ok = phys_install(ms);
    patch_undo_now = keep;
    return ok;
}

void physics_sync_set(double ms)
{
    static int failed;
    const char *e = gmv_getenv("GMV_PHYSICS_SYNC");
    /* Below ~1.2 ms the whole-ms dt is mostly 1 and the 0.5..1.5*S window stops matching it. */
    int want = ms >= 1.2 && ms <= 40.0, on = e && *e && *e != '0';
    if (failed || (!on && !phys_installed))
        return;
    if (!phys_installed) {
        if (!want)
            gmv_log("[gamepatch] physics sync needs the tick pacer at 1.2-40 ms (FRAME_MS=%.3f); not in force\n",
                    ms);
        else if (!phys_install_undoable(ms))
            failed = 1;
        else {
            phys_installed = g_phys_on = 1;
            grip_scale_set(ms);
        }
        return;
    }
    if (!on)                                    /* off from ORIGINAL_MAC_EXPERIENCE: the game's own step */
        want = 0;
    float s = want ? (float)(ms / 1000.0) : 1.0f / 60.0f;
    uint32_t bits;
    memcpy(&bits, &s, 4);
    g_phys_step = s;
    g_phys_negstep = -s;
    g_phys_on = want;
    if (!patch_bytes(PHYS_IMM1 + 4, (const unsigned char *)&bits, 4) ||
        !patch_bytes(PHYS_IMM2 + 4, (const unsigned char *)&bits, 4)) {
        gmv_log("[gamepatch] write in the physics step loop failed (%lu); physics sync may be partial\n",
                GetLastError());
        failed = 1;
        return;
    }
    grip_scale_set(want ? ms : 1000.0 / 60.0);
    if (want)
        gmv_log("[gamepatch] physics sync now step %.3f ms (%.2f Hz), grip x%.3f\n", ms, 1000.0 / ms,
                g_grip_scale);
    else
        gmv_log("[gamepatch] physics sync off (FRAME_MS=%.3f%s): the game's own 1/60 s step\n", ms,
                on ? "" : ", PHYSICS_SYNC off");
}

void apply_physics_sync(void)
{
    physics_sync_set(gmv_pace_frame_ms());
}
