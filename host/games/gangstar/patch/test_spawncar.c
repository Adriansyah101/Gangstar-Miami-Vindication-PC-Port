/* test_spawncar.c -- test aid: the player starts at the wheel of a car (GMV_TEST_SPAWN_CAR) */
#include "compat.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

#if GMV_DEV
/* ---- test aid: put the player in a car of a named type once the save is in the city ----------------

   Asked for by the user on 2026-11-06 for comparing car reflections up close: TEST_SPAWN_CAR=Laufer (a vehicle
   type's own name in metatypes.gmap: Laufer, Lambo, Vamp, Tawny, Taxi, CopCar...).  Once the city is up, no
   menu shows, no mission is under way and the player has been on foot for 3 s, a car of that type is made where
   they stand, as the Crusher is (uc_make: 0x220d4, its world list, vfunc 0x44, the navmesh), and they are seated
   in it at once as the Crusher's swap does (Player vt+0x68, ActorGetIn's skip for a player).  Once a run. */
static int g_sc_type = -2;                      /* -2 = not looked up yet, -1 = off or not found */
static char g_sc_name[32];
static int g_sc_done;
static DWORD g_sc_since;

static int sc_find_type(uint32_t world, const char *name)
{
    int n = (int)ms_rd(world + 0x408);
    uint32_t tab = ms_rd(world + 0x40c);
    size_t nl = strlen(name);
    for (int t = 0; tab && t < n && t < 256; t++) {
        uint32_t e = ms_rd(tab + 4 * (uint32_t)t);
        if (e && ms_rd(e) == 3 && ms_rd(e + 4) == nl && !memcmp((const void *)(uintptr_t)(e + 8), name, nl))
            return t;
    }
    return -1;
}

/* The game's code keeps 16-byte stack alignment (movaps on locals in the seating, 0x6e262); Windows gives 4. */
__attribute__((cdecl, force_align_arg_pointer, noinline))
void gmv_test_spawn_car_tick(void)
{
    if (g_sc_type == -2) {
        const char *e = gmv_dbgenv("GMV_TEST_SPAWN_CAR");
        g_sc_type = -1;
        if (e && *e) {
            snprintf(g_sc_name, sizeof g_sc_name, "%s", e);
            g_sc_type = 0;                      /* looked up once the world is there */
            gmv_log("[spawncar] test aid: the player starts at the wheel of a %s\n", g_sc_name);
        }
    }
    if (g_sc_type < 0 || g_sc_done)
        return;
    uint32_t mgr = mr_mgr(), world = ms_rd(ms_rd(MR_WORLD_REF)), p = ms_rd(MR_PLAYER);
    if (!mgr || !world || !p || pointer_shown() || (int)ms_rd(mgr + 4) != -1 || ms_rd(p + 4 + 0x558)) {
        g_sc_since = 0;
        return;
    }
    DWORD now = GetTickCount();
    if (!g_sc_since) {
        g_sc_since = now ? now : 1;
        return;
    }
    if (now - g_sc_since < 3000)
        return;
    g_sc_done = 1;
    int type = sc_find_type(world, g_sc_name);
    if (type < 0) {
        gmv_log("[spawncar] no vehicle type called %s\n", g_sc_name);
        return;
    }
    uint32_t c = p + 4;
    uc_spot_t s = { { 0 }, 0, 0, 0, 1 };
    uc_pos(c, s.pos);
    /* the car's navmesh getters are the base object's; the character's are used only if they are the same */
    uint32_t cvt = ms_rd(c), car_vt = 0x00656068u;
    int nav = ms_rd(cvt + 0x54) == ms_rd(car_vt + 0x54) && ms_rd(cvt + 0x58) == ms_rd(car_vt + 0x58);
    if (!nav) {
        gmv_log("[spawncar] the character's navmesh getters are not the car's (%08x %08x); no car made\n",
                ms_rd(cvt + 0x54), ms_rd(cvt + 0x58));
        return;
    }
    s.cell = ((uint32_t (*)(uint32_t))ms_rd(cvt + 0x54))(c) & 0xffff;
    s.tri = ((uint32_t (*)(uint32_t))ms_rd(cvt + 0x58))(c) & 0xffff;
    uint32_t car = uc_make(world, type, &s);
    if (!car) {
        gmv_log("[spawncar] the game made no %s\n", g_sc_name);
        return;
    }
    *(uint32_t *)(uintptr_t)(c + 0x670) = 1;
    ((void (*)(uint32_t, uint32_t))ms_rd(ms_rd(p) + 0x68))(p, car);
    *(unsigned char *)(uintptr_t)(car + 5) |= 8;      /* let go of as ObjectRelease does: the game takes it away later */
    gmv_log("[spawncar] a %s (type %d) at %.0f %.0f %.0f, navmesh %u/%u: %08x; the player in it %d\n", g_sc_name, type,
            s.pos[0], s.pos[1], s.pos[2], s.cell, s.tri, car, ms_rd(c + 0x558) == car);
}
#else
void gmv_test_spawn_car_tick(void) {}
#endif
