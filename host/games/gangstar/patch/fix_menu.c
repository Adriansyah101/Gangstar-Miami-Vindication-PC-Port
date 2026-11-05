/* fix_menu.c -- the main menu taking clicks during its fade */
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

/* ---- original-game behaviour, changed: the main menu takes clicks while its background fades ----

   Symptom: on the main menu, clicks are ignored for a second or so at a time, most often while the
   background scene is still loading.

   Mechanism: the menu's 3D background is a scripted camera flight cut together with the cinematic
   fader, CineFadeIn 0xcd8be and CineFadeOut 0xcda6c.  On every tick a fade runs, both call 0x283b0,
   which sets the input-lock flag [[app+0x17ac8]+0x18] and then calls 0x19d72, the input reset that
   zeroes every key and mouse-button level of the input manager.  A click stores its level in
   -sendMouseEvent: (0x1816e) and the manager turns levels into press/release events once per tick
   (0x19b60), so a press made during a fade is wiped before it is ever seen.  The Mac runs the same
   code; this is the original behaviour, changed here at the user's request.

   Fix: the four calls to 0x283b0 in the two fader routines go to gmv_cine_fade_lock, which, while
   the main-menu state (vtable 0x659f28) is on top and the front end is not switching screens
   (0x32de0), only sets the lock flag and skips the reset.  Anywhere else (cutscenes, other menus,
   screen changes) it calls 0x283b0 as before.  The script's explicit lock (0xcd66c) is untouched.
   Default on; GMV_MENU_FADE_CLICKS=0 keeps the original.  ORIGINAL_MAC_EXPERIENCE=1 keeps the original
   (it kept the fix on from 2026-11-03 until the user turned it off there on 2026-11-05). */
#define CINE_LOCK_FN   0x000283b0u   /* input lock: [obj+0x18] = 1, then input reset 0x19d72 */

static const uint32_t cine_lock_sites[] = {
    0x000cd908u, 0x000cd97cu,        /* CineFadeIn 0xcd8be: fade start and each fading tick */
    0x000cdae1u, 0x000cdb7fu,        /* CineFadeOut 0xcda6c: the same two */
};

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_cine_fade_lock(unsigned char *lock)
{
    void *(*fe_get)(void)  = (void *(*)(void))(uintptr_t)0x35d08u;
    int   (*fe_tr)(void *) = (int (*)(void *))(uintptr_t)0x32de0u;
    if (gmv_top_state_vtable() == MAIN_MENU_VT) {
        void *fe = fe_get();
        if (fe && !(fe_tr(fe) & 0xff)) {
            lock[0x18] = 1;
            return;
        }
    }
    ((void (*)(unsigned char *))(uintptr_t)CINE_LOCK_FN)(lock);
}

/* 1 = the fader calls go to gmv_cine_fade_lock, 0 = to 0x283b0; -1 if a site is neither, -2 if a
   write failed (then some go one way and some the other). */
int menu_fade_clicks_set(int on)
{
    const uint32_t ours = (uint32_t)(uintptr_t)gmv_cine_fade_lock, to = on ? ours : CINE_LOCK_FN;
    for (size_t i = 0; i < sizeof cine_lock_sites / sizeof *cine_lock_sites; i++) {
        const unsigned char *p = (const unsigned char *)(uintptr_t)cine_lock_sites[i];
        int32_t rel;
        memcpy(&rel, p + 1, 4);
        uint32_t now = cine_lock_sites[i] + 5 + (uint32_t)rel;
        if (p[0] != 0xe8 || (now != CINE_LOCK_FN && now != ours))
            return -1;
    }
    for (size_t i = 0; i < sizeof cine_lock_sites / sizeof *cine_lock_sites; i++) {
        int32_t rel = (int32_t)(to - (cine_lock_sites[i] + 5));
        if (!patch_bytes(cine_lock_sites[i] + 1, (const unsigned char *)&rel, 4))
            return -2;
    }
    return on;
}

void apply_menu_fade_clicks(void)
{
    const char *e = gmv_getenv("GMV_MENU_FADE_CLICKS");
    if (e && *e == '0')
        return;
    int r = menu_fade_clicks_set(1);
    if (r == -1)
        gmv_log("[gamepatch] fader calls (0x%08x..) not the expected calls to 0x%08x; menu fade clicks skipped\n",
                cine_lock_sites[0], CINE_LOCK_FN);
    else if (r == -2)
        gmv_log("[gamepatch] a write failed (%lu); menu fade clicks partly applied\n", GetLastError());
    else
        gmv_log("[gamepatch] main menu takes clicks during its background fades (fader input reset skipped on the "
                "main menu; MENU_FADE_CLICKS=0 to disable)\n");
}
