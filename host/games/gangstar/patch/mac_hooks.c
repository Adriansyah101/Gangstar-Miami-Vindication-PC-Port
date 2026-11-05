/* mac_hooks.c -- ORIGINAL_MAC_EXPERIENCE: the hooks that exist only for what it turns off */
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

/* ---- ORIGINAL_MAC_EXPERIENCE: the hooks that exist only for what it turns off ----
   They go in at the start without it and with its in-game switch going off; they come out with the
   switch going on, leaving the bytes the game shipped with, so nothing is needed from a new start:
   - every other enhancement's and the timing's at once (mac_features_in/out, at the end of this file);
   - the Crusher's once it is out of the city (gmv_uc_tick);
   - the densities and SPAWN_FADE at once, the spawn pool moved back and our copies in it freed (those out
     in the city as they come back, gmv_pool_tick);
   - the draw distance after the next cell update, which must see the radius go back;
   - the collision sizes and the proxy guard as soon as what is in use fits and no pairs are buffered: the
     tables shrink in the world in use (gmv_bp_shrink, from gmv_uc_tick, between steps). */
patch_undo pd_undo;
int dd_tables_back;                 /* to the Mac's: at the next tick, or the next world if none yet */

void dd_hooks_in(void)
{
    patch_undo_now = &dd_undo;
    apply_draw_distance();
    apply_origin_collision_fix();           /* after the draw distance, which needs it */
    patch_undo_now = NULL;
}

void crowd_hooks_in(void)
{
    patch_undo_now = &pd_undo;
    apply_spawn_hook();
    apply_spawn_fade();                     /* after the spawn hook, whose spawner hook it shares */
    patch_undo_now = NULL;
}

void mac_hooks_in(void)
{
    dd_unhook = dd_tables_back = 0;
    if (!dd_on) {
        const int *prof = (const int *)(uintptr_t)DD_PROFILE;
        if (prof[3])                        /* read before the hook went in: the Mac's */
            dd_mac_profile[3] = prof[3], dd_mac_profile[4] = prof[4];
        oc_seed_r1 = *(const uint8_t *)(uintptr_t)DD_RADIUS_SET ? *(const float *)(uintptr_t)DD_RADIUS : 0;
        oc_seed_r2 = *(const uint8_t *)(uintptr_t)DD_KEEP_SET ? *(const float *)(uintptr_t)DD_KEEP : 0;
        dd_hooks_in();
    }
    dd_tables_fit(dd_scale);
    mac_out_pending = 0;
    if (!mac_features_on)
        mac_features_in();
    if (gmv_uc_on())
        uc_hooks_in();
    if (!pd_on) {
        sf_menu = -1;
        crowd_hooks_in();
        pd_pool_top_up();
    }
}

void mac_hooks_out(void)
{
    if (pd_on || sf_hooked) {
        if (pd_pool_on)
            pd_unmove_pool();
        patch_undo_all(&pd_undo);
        pd_on = pd_centres_on = pd_pool_on = pd_racing = sf_hooked = sf_live = sf_n = 0;
        pd_trim_gen = pd_world_gen;
        if (sv_on) {                        /* SPAWN_SURVEY stays */
            sm_redirect(SV_SITE, (void *)gmv_can_spawn);
            sm_redirect(PD_MAKE_SITE, (void *)gmv_spawn_make);
        }
        gmv_log("[gamepatch] density and SPAWN_FADE hooks out, the spawn pool back in its 256 slots (%d of our "
                "copies still in the city)\n", pd_extra_n);
    }
    if (dd_on)
        dd_unhook = 1;
    dd_tables_back = dd_tables_in;
    mac_features_out();
}

static void dd_tables_mac(void)
{
    uint32_t *sizes = (uint32_t *)(uintptr_t)DD_MAX_PROXIES;
    patch_undo_all(&dd_tables_undo);
    sizes[0] = 0x4000, sizes[1] = 0x20000, sizes[2] = 0x20000, sizes[3] = 0x1ffff;
    dd_tables_in = dd_tables_back = 0;
    gmv_log("[gamepatch] collision tables the Mac's again: 16384 proxies, 131072 pairs\n");
}

/* A new b2World is about to be made (b2solve.c): the sizes it reads can be the Mac's now. */
void gmv_b2_world_new(void)
{
    pd_world_gen++;
    gmv_trace("[gamepatch] a new physics world (%d so far), the collision tables %s\n", pd_world_gen,
            dd_tables_back ? "go back to the Mac's" : "stay as they are");
    if (dd_unhook)
        dd_take_out();
    if (dd_tables_back)
        dd_tables_mac();
}

/* After a present, between steps: the world in use down to the Mac's sizes, once it fits.  Not before the
   city's radius is back, which is what fills them (a run of 2026-11-02: 15837 of the 16384 in use until
   then), and with room to spare, since the proxy guard goes with the tables. */
void dd_tables_tick(void)
{
    static int told_wait, told_no;
    if (!dd_tables_back || dd_unhook)
        return;
    uint32_t world = *(const uint32_t *)(uintptr_t)DD_WORLD;
    void *bp = world ? *(void **)(uintptr_t)(world + DD_WORLD_BP) : NULL;
    if (!bp || !((const int *)(uintptr_t)DD_PROFILE)[3])
        return;                                 /* no world yet: gmv_b2_world_new sees to it */
    int r = *(int *)((char *)bp + 0x50) > 0x4000 - 0x400 ? 0 : gmv_bp_shrink(bp, 0x4000, 0x20000);
    if (r > 0) {
        told_wait = told_no = 0;
        dd_tables_mac();
    } else if (r == 0 && !told_wait++) {
        gmv_log("[gamepatch] collision tables: waiting for what is in use to fit the Mac's (%d proxies, %d pairs, "
                "%d buffered)\n", *(int *)((char *)bp + 0x50), *(int *)((char *)bp + 0x10), *(int *)((char *)bp + 0x18));
    } else if (r < 0 && !told_no++) {
        gmv_log("[gamepatch] collision tables: no memory or not what was expected; the Mac's sizes wait for the "
                "next start\n");
    }
}
