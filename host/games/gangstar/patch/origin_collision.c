/* origin_collision.c -- origin collision: buildings near the world origin */
#include "compat.h"
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* ---- origin collision: buildings near the world origin lose their collision (an original bug, mended
   only with DRAW_DISTANCE above 1; at 1 the game is left as on the Mac, the user's choice) ----
   updateCells(grid, pos, r1, r2) adds the objects of the cells that came within r1 since the last
   call, by comparing with a rectangle around the last position (+0x1a4), which a new or reset grid
   sets to (0,0,0).  The first call so treats every cell within r1 of the world origin as already
   added, and the buildings there get no collision until their cells leave range and come back: about
   +-225 m on the Mac, most of the city at 3x the draw distance.  gmv_update_cells moves that
   starting position far away, so the first call adds every cell in range. */
#define OC_CELLS_SITE  0x00135a5au   /* call CCustomBatchGridSceneNode::updateCells, its only caller */
#define OC_CELLS_FN    0x005ed73eu

#define OC_GRIDS 8
static struct { char *grid; float r1, r2; } oc_grids[OC_GRIDS];   /* each grid's radii at its last update */
float oc_seed_r1, oc_seed_r2;       /* the radii in force when this hook went into a running game */
int dd_unhook;                      /* ORIGINAL_MAC_EXPERIENCE: out after the update that follows */

/* The draw distance's hooks out.  The cell updates must first see the change of radius, or the cells
   between the two radii would stay. */
void dd_take_out(void)
{
    patch_undo_all(&dd_undo);
    dd_on = dd_live = dd_unhook = 0;
    gmv_log("[gamepatch] draw distance hooks out: the game's own again\n");
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_update_cells(char *grid, float *pos, float r1, float r2)
{
    float *last = (float *)(grid + 0x1a4);
    int fresh = last[0] == 0 && last[1] == 0 && last[2] == 0, i, slot = 0;
    if (fresh && dd_scale > 1)
        last[0] = last[1] = last[2] = -1e7f;       /* 100 km away: nothing counts as added */
    for (i = 0; i < OC_GRIDS && oc_grids[i].grid != grid; i++)
        if (!oc_grids[i].grid)
            slot = i;
    if (i < OC_GRIDS)
        slot = i;
    float o1 = r1, o2 = r2;                        /* a new grid: no change of radius */
    if (i < OC_GRIDS && !fresh)
        o1 = oc_grids[slot].r1, o2 = oc_grids[slot].r2;
    else if (!fresh && oc_seed_r1 > 0)             /* it was updated before this hook went in */
        o1 = oc_seed_r1, o2 = oc_seed_r2;
    if (o1 != r1)
        gmv_log("[gamepatch] city cells: radius %.0f -> %.0f m\n", o1 / 100, r1 / 100);
    oc_grids[slot].grid = grid, oc_grids[slot].r1 = r1, oc_grids[slot].r2 = r2;
    ((void (*)(char *, float *, float, float, float, float))(uintptr_t)OC_CELLS_FN)(grid, pos, r1, r2, o1, o1 + o2);
    if (dd_unhook) {
        int all = 1;
        for (i = 0; i < OC_GRIDS; i++)
            all &= !oc_grids[i].grid || oc_grids[i].r1 == r1;
        if (all)
            dd_take_out();                         /* (this call has returned: the bytes can change) */
    }
}

void apply_origin_collision_fix(void)
{
    if (!dd_on)
        return;
    if (!sm_is_call(OC_CELLS_SITE, OC_CELLS_FN)) {
        gmv_log("[gamepatch] cell update not the expected bytes; buildings near the world origin (most of "
                "the city with DRAW_DISTANCE) may have no collision\n");
        return;
    }
    int32_t r = (int32_t)((uint32_t)(uintptr_t)gmv_update_cells - (OC_CELLS_SITE + 5));
    if (!patch_bytes(OC_CELLS_SITE + 1, (const unsigned char *)&r, 4))
        return;
    if (dd_scale > 1)
        gmv_log("[gamepatch] buildings near the world origin keep their collision (needed by DRAW_DISTANCE)\n");
    if (!tab_live())
        return;
    if (dd_cells_aim()) {
        dd_live = 1;
        gmv_log("[gamepatch] draw distance ready for the options menu (%.2gx now)\n", dd_scale);
    } else
        gmv_log("[gamepatch] cell update's radii not the expected bytes; DRAW_DISTANCE takes effect at the next start\n");
}
