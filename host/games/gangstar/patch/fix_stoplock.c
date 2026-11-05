/* fix_stoplock.c -- the vehicle full-stop lock timer */
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

/* ---- correctness: the vehicle full-stop lock timer reads the frame DT, not the clock ----

   Symptom: a car that comes to a complete stop can get permanently stuck -- gas and brake dead,
   steering still turning the wheels, and the dead state staying with that car across exit/re-enter.

   Mechanism (Vehicle update, vtbl+0xa0 @0x112bc4; Ghidra decompile + capstone):
     stamp = app->[0x17ad8]                      @0x1132b2  (FUN_000031b0() = app singleton)
     moving forward -> stopped, all wheels down:  flags(+0x180) |= 0x60;  this->[0x184] = stamp
     if ((flags & 0x20) && (unsigned)(stamp - this->[0x184]) > 199)  flags &= ~0x60
   While 0x20 is set Car's force routine (vtbl+0x174 @0xfef68, the test at 0xff9e3) skips the whole
   drive-force block, and 0x40 makes brake/reverse (@0x10d26e) return early.  The intent is a 200 ms
   settle timer, but app->[0x17ad8] is the FRAME DT cell (the tick copies *[0x63e018] = 0x949ae0, the
   dt written by 0x1b8aa0), not a time.  So the lock releases only on a frame whose dt is SMALLER than
   the dt recorded at the stop (the unsigned subtraction wraps).  On a Mac, vsync jitter produces such
   a frame within a few ticks, which hides the bug; this host's steady dt (16/17/17 ms at the 60 Hz
   pacer, 0/14 ms under the old panel-rate vsync) never goes below a recorded 16 (or 0), so the car
   stays locked forever.  Fix: make the stamp the tick's own clock, now_ms at 0x949adc (written by
   0x1b8aa0 next to dt, once per tick), so the timer is the intended 200 ms.  The stamp is used only
   for that store and compare (0x1133a1, 0x1133b7) before the slot is reused at 0x11359b.  Default
   on; GMV_STOPLOCK_FIX=0 restores the original load, as does the options menu (stoplock_set). */
#define STOPLOCK_LOAD  0x001132b2u   /* mov eax,[eax+0x17ad8]  (8b 80 d8 7a 01 00)                  */
#define TICK_NOW_MS    0x00949adcu   /* now_ms cell written by 0x1b8aa0 each tick (dt is at +4)     */

/* 1 = the load is now_ms, 0 = the original; -1 if the site is neither, -2 if the write failed. */
int stoplock_set(int on)
{
    static const unsigned char orig[6] = { 0x8b, 0x80, 0xd8, 0x7a, 0x01, 0x00 };
    unsigned char site[6] = { 0xa1 };                    /* mov eax,[moffs32] ; nop */
    uint32_t now = TICK_NOW_MS;
    memcpy(site + 1, &now, 4);
    site[5] = 0x90;
    const void *p = (const void *)(uintptr_t)STOPLOCK_LOAD;
    if (memcmp(p, orig, sizeof orig) && memcmp(p, site, sizeof site))
        return -1;
    return patch_bytes(STOPLOCK_LOAD, on ? site : orig, sizeof site) ? on : -2;
}

void apply_stoplock_fix(void)
{
    const char *e = gmv_getenv("GMV_STOPLOCK_FIX");
    if (e && *e == '0')
        return;
    int r = stoplock_set(1);
    if (r == -1) {
        gmv_log("[gamepatch] stop-lock load 0x%08x not the expected bytes; fix skipped\n", STOPLOCK_LOAD);
        return;
    }
    if (r == -2) {
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); stop-lock fix skipped\n", STOPLOCK_LOAD, GetLastError());
        return;
    }
    gmv_log("[gamepatch] vehicle stop-lock timer fixed at 0x%08x: stamp = now_ms [0x%08x] instead of frame dt "
            "(STOPLOCK_FIX=0 to disable)\n", STOPLOCK_LOAD, TICK_NOW_MS);
}
