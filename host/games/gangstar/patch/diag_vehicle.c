/* diag_vehicle.c -- diagnostics: vehicle speed and AI destinations */
#include "compat.h"
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* ---- diagnostic: trace the vehicle SPEED (GMV_CARSND_TRACE=1) ----

   The user reports the car engine sound is stuck at low RPM.  The audio backend is proven healthy
   (full-scale, zero underruns), so the pitch is the game's own, computed from the vehicle's speed.
   (The first guess, Vehicle::motionPitch @0x1084d0, turned out to be the vehicle's body-motion pitch
   and is never called -- ruled out.)  Vehicle::getSpeed @0x101f74 is the real speed accessor: its
   body is  return this->[+0x3d4] / 1000.0f * 3600.0f  (m/s field -> km/h), and it is called every
   frame for any vehicle (HUD/AI/sound).  This hook faithfully reproduces that body and logs the
   value, so we can see whether the player's car actually accelerates to a high speed: speed climbs
   with throttle => the engine-sound pitch mapping is the suspect; speed stuck low => it is the game's
   own physics/speed.  It fully REPLACES the trivial accessor (same formula, returns the float in
   st(0) as the i386 ABI expects) -- a trace, not a behavioural edit.  Off unless GMV_CARSND_TRACE. */
#define CARSND_GETSPEED 0x00101f74u

/* Latest vehicle speed (km/h) seen by getSpeed.  In a traffic-free area the only vehicle is the
   player's car, so the [engrev] trace can print the engine blend vs this speed inline. */
volatile float g_carsnd_last_speed;

__attribute__((cdecl, force_align_arg_pointer))
static float gmv_carsnd_getspeed(void *thisp)
{
    float raw = thisp ? *(const float *)((const char *)thisp + 0x3d4) : 0.0f;
    float k1  = *(const float *)(uintptr_t)0x005f1150u;     /* 1000.0f  */
    float k2  = *(const float *)(uintptr_t)0x005f1818u;     /* 3600.0f  */
    float spd = raw / k1 * k2;                              /* == getSpeed @0x101f74, in km/h */
    g_carsnd_last_speed = spd;                              /* stash for the [engrev] trace   */

    /* getSpeed is virtual, so it has NO static call sites -- we cannot find the inlined CarSounds
       engine-pitch update statically.  But this hook is entered via `jmp` from getSpeed's entry, so
       the return address on our own frame IS the game caller's return address.  Record each distinct
       caller once: one of them is the sound update (the one whose surrounding code maps speed to the
       engine voice's pitch).  Pure diagnostic, no behavioural change. */
    void *caller = __builtin_return_address(0);
    static void *seen[48];
    static unsigned nseen;
    int known = 0;
    for (unsigned i = 0; i < nseen; i++)
        if (seen[i] == caller) { known = 1; break; }
    if (!known && nseen < 48) {
        seen[nseen++] = caller;
        gmv_log("[carsnd] getSpeed caller #%u = %p  (speed=%.2f km/h, this=%p)\n",
                nseen, caller, (double)spd, thisp);
    }

    static unsigned n;
    static float smax = 0.0f;
    int newmax = spd > smax + 1.0f;
    if (newmax) smax = spd;
    if (newmax || (n++ % 90) == 0)
        gmv_log("[carsnd] getSpeed=%.2f km/h (max %.2f) rawField[+0x3d4]=%.4f this=%p\n",
                (double)spd, (double)smax, (double)raw, thisp);
    return spd;
}

void apply_carsnd_trace(void)
{
    const char *e  = gmv_dbgenv("GMV_CARSND_TRACE");
    const char *e2 = gmv_dbgenv("GMV_ENGSRC_TRACE");           /* engsrc trace also wants live speed */
    if (!((e && *e && *e != '0') || (e2 && *e2 && *e2 != '0')))
        return;
    /* Entry bytes: push ebp; mov ebp,esp; sub esp,4; call $+5; pop eax (PIC).  Replace the first 5
       with a jmp to the host hook (which reimplements the whole accessor), only on a match. */
    static const unsigned char expect[8] = { 0x55, 0x89, 0xe5, 0x83, 0xec, 0x04, 0xe8, 0x00 };
    if (memcmp((const void *)(uintptr_t)CARSND_GETSPEED, expect, sizeof expect) != 0) {
        gmv_log("[gamepatch] getSpeed 0x%08x not the expected bytes; car-sound trace skipped\n",
                CARSND_GETSPEED);
        return;
    }
    unsigned char site[5];
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)&gmv_carsnd_getspeed - (CARSND_GETSPEED + 5));
    site[0] = 0xe9;                                      /* jmp rel32 -> host hook (tail, returns to caller) */
    memcpy(site + 1, &rel, 4);
    if (!patch_bytes(CARSND_GETSPEED, site, sizeof site)) {
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); car-sound trace skipped\n",
                CARSND_GETSPEED, GetLastError());
        return;
    }
    gmv_log("[gamepatch] car-sound trace installed: Vehicle::getSpeed -> host hook %p "
            "([carsnd] logs vehicle speed while driving)\n", (void *)&gmv_carsnd_getspeed);
}

/* ---- diagnostic: trace vehicle AI drive-to destination set/clear (GMV_VEH_TRACE=1) ----

   The user reports a MISSION car that cannot accelerate (gas/brake dead) after a cutscene with a
   passenger, while steering still works, OTHER cars drive fine, and the dead state persists on that
   car even after exiting and re-entering it -- i.e. a per-vehicle control state stuck on that car,
   NOT a global input problem.  Vehicle::aiSetDestination(this, dest) @0x101fee writes this->[0x220]
   (the AI "drive to this LevelObject" target; aiGetDestination reads it back) -- a mission script
   command (ActorDriveTo / VehicleDriveTo, used by cutscenes) sets it to make a car drive itself.
   Hypothesis: a passenger cutscene sets it on the mission car and never clears it (never
   aiSetDestination(..,NULL)), so the car stays under AI control (throttle 0 once arrived) while the
   player's steering still leaks through.  This hook REPLACES the trivial setter (identical write to
   [+0x220]) and logs each CHANGE of destination per vehicle (deduped, so no per-frame spam), with
   the caller, so the trace shows whether some vehicle is left with a dangling non-NULL destination
   across the cutscene.  Pure diagnostic; installed only when GMV_VEH_TRACE is set. */
#define VEH_AISETDEST 0x00101feeu

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_veh_aisetdest(void *thisp, void *dest)
{
    if (thisp)
        *(void **)((char *)thisp + 0x220) = dest;   /* == aiSetDestination: this->[0x220] = dest */

    /* Log only when a vehicle's destination actually CHANGES (per-frame re-sets of the same value
       are common and would flood the log). */
    static void *vt[64], *vd[64];
    static unsigned vn;
    int logit = 1;
    unsigned i;
    for (i = 0; i < vn; i++)
        if (vt[i] == thisp) break;
    if (i < vn) { logit = (vd[i] != dest); vd[i] = dest; }
    else if (vn < 64) { vt[vn] = thisp; vd[vn] = dest; vn++; }
    if (logit)
        gmv_log("[veh] aiSetDestination this=%p dest=%p %s caller=%p\n",
                thisp, dest, dest ? "SET" : "CLEAR", __builtin_return_address(0));
}

void apply_veh_trace(void)
{
    const char *e = gmv_dbgenv("GMV_VEH_TRACE");
    if (!(e && *e && *e != '0'))
        return;
    /* Entry: push ebp; mov ebp,esp; mov eax,[ebp+0xc]; mov ecx,[ebp+8]; mov [ecx+0x220],eax; ... */
    static const unsigned char expect[8] = { 0x55, 0x89, 0xe5, 0x8b, 0x45, 0x0c, 0x8b, 0x4d };
    if (memcmp((const void *)(uintptr_t)VEH_AISETDEST, expect, sizeof expect) != 0) {
        gmv_log("[gamepatch] aiSetDestination 0x%08x not the expected bytes; veh trace skipped\n",
                VEH_AISETDEST);
        return;
    }
    unsigned char site[5];
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)&gmv_veh_aisetdest - (VEH_AISETDEST + 5));
    site[0] = 0xe9;                                      /* jmp rel32 -> host hook (tail, returns) */
    memcpy(site + 1, &rel, 4);
    if (!patch_bytes(VEH_AISETDEST, site, sizeof site)) {
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); veh trace skipped\n",
                VEH_AISETDEST, GetLastError());
        return;
    }
    gmv_log("[gamepatch] veh trace installed: Vehicle::aiSetDestination -> host hook %p "
            "([veh] logs AI drive-to set/clear per vehicle)\n", (void *)&gmv_veh_aisetdest);
}
