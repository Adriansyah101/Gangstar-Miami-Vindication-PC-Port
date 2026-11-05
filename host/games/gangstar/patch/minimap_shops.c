/* minimap_shops.c -- MINIMAP_SHOPS */
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

/* ---- MINIMAP_SHOPS: the shops on the minimap (an enhancement, off by default and under ORIGINAL_MAC_EXPERIENCE) ----

   The minimap (MiniMap.cpp, world +0x9c) keeps a list at +0x5c of the objects it has icons for,
   each with its sprite made at the object's set-up (0x46eda, icon 0x239ce).  The checkpoints of
   category 0x15 (CheckpointShop, ShopCarRental, ShopRepair, ShopWeapon) and 0x16 (CheckpointTeleport)
   are in it, but the draw loop of 0x4623a skips both (0x4679d: cat - 0x15 < 2 -> 0x468c4), so the
   shops only show on the pause map.  With the setting, the same 17 bytes skip only 0x16 and send
   0x15 to 0x4681f, which sets the draw's "inside only" flag ([ebp-0x8c] = 1, the last argument of
   0x45a00) as the game does for its own badges: a shop shows while it is inside the minimap and
   is not pinned to the edge when it is not.
     8a 04 01            mov al,[ecx+eax]        (as before: the category)
     3c 16 / 0f 84 ..    cmp al,0x16 / je 0x468c4
     3c 15 / 74 73       cmp al,0x15 / je 0x4681f
     90 90
   GMV_MINIMAP_SHOPS=1 turns it on; the options tab switches it in game. */
#define MS_SITE 0x0004679du

static const unsigned char ms_orig[17] = {
    0x8a, 0x04, 0x01, 0x88, 0xc1, 0x80, 0xc1, 0xeb, 0x80, 0xf9, 0x02, 0x0f, 0x82, 0x16, 0x01, 0x00, 0x00 };
static const unsigned char ms_new[17] = {
    0x8a, 0x04, 0x01, 0x3c, 0x16, 0x0f, 0x84, 0x1c, 0x01, 0x00, 0x00, 0x3c, 0x15, 0x74, 0x73, 0x90, 0x90 };

/* 1 if the minimap now draws the shops (on) or not (off), 0 if the bytes are not the expected ones. */
int minimap_shops_set(int on)
{
    const void *p = (const void *)(uintptr_t)MS_SITE;
    if (memcmp(p, ms_orig, sizeof ms_orig) && memcmp(p, ms_new, sizeof ms_new))
        return 0;
    return patch_bytes(MS_SITE, on ? ms_new : ms_orig, sizeof ms_orig);
}

void apply_minimap_shops(void)
{
    const char *e = gmv_getenv("GMV_MINIMAP_SHOPS");
    if (!(e && *e && *e != '0'))
        return;
    if (minimap_shops_set(1))
        gmv_log("[gamepatch] the minimap shows the shops near you (MINIMAP_SHOPS=0 for the Mac's minimap)\n");
    else
        gmv_log("[gamepatch] minimap draw loop not the expected bytes (or write failed, %lu); no shops on the minimap\n",
                GetLastError());
}
