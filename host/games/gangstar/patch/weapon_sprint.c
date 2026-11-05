/* weapon_sprint.c -- ALLOW_WEAPON_SPRINT */
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

/* ---- ALLOW_WEAPON_SPRINT: keep the weapon in hand while sprinting (an enhancement, off by default and under ORIGINAL_MAC_EXPERIENCE) ----

   The Sprint key's handler (0x11e002) runs on press: it marks the character as sprinting
   (+0x666 = 1) and calls Player vt[0x78](1), which selects slot 1, the fists.  On release it clears
   the mark and selects the slot it saved before the sprint (handler +0x48).  The iPhone keeps the
   weapon out.  With the setting, the press jumps over the fists call (0x11e269 -> 0x11e2cc) and the
   release over the restore (0x11e2ae -> 0x11e2cc), which would only undo a weapon change made during
   the sprint.  The sprinting mark still goes on and off as before.
   GMV_ALLOW_WEAPON_SPRINT=1 turns it on. */

void apply_weapon_sprint(void)
{
    const char *e = gmv_getenv("GMV_ALLOW_WEAPON_SPRINT");
    if (!(e && *e && *e != '0'))
        return;
    static const unsigned char press_expect[3] = { 0x8b, 0x75, 0xf0 }, release_expect[3] = { 0x8b, 0x46, 0x48 };
    if (memcmp((const void *)(uintptr_t)WS_PRESS_SITE, press_expect, 3) != 0 ||
        memcmp((const void *)(uintptr_t)WS_RELEASE_SITE, release_expect, 3) != 0) {
        gmv_log("[gamepatch] sprint handler not the expected bytes; sprinting still puts the weapon away\n");
        return;
    }
    static const unsigned char press_jmp[2] = { 0xeb, 0x61 }, release_jmp[2] = { 0xeb, 0x1c };
    if (!patch_bytes(WS_PRESS_SITE, press_jmp, 2) || !patch_bytes(WS_RELEASE_SITE, release_jmp, 2))
        gmv_log("[gamepatch] write failed (%lu); sprinting may still put the weapon away\n", GetLastError());
    else
        gmv_log("[gamepatch] the weapon stays in hand while sprinting (ALLOW_WEAPON_SPRINT=0 for the Mac's fists)\n");
}
