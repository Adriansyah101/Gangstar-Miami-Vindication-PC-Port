/* fog_distance.c -- the fog's distances follow DRAW_DISTANCE */
#include "compat.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* ---- The fog's distances follow DRAW_DISTANCE (an enhancement; the game's own under
   ORIGINAL_MAC_EXPERIENCE) ----
   The time-of-day update 0x142892 sets the linear fog every tick: start = profile +0x0c (the end of
   the full detail, 450 m on the Mac), end = the low-detail far distance (0xb5a00) plus 2/3 of the
   low-detail band (1200 m on the Mac).  It hands the pair {start, end} to the shader parameter
   setter 0x54cec0 at 0x142d5a; that call goes to gmv_set_fog, which swaps in ours (the user's
   values): FOG_MODE 1/2 from 100 m to 800 m at 1x, FOG_MODE 0 from 450 m to 900 m at 1x, the end
   50 m further for each 0.5x more.  One world unit is about 1 cm. */
#define FOG_SITE 0x00142d5au
#define FOG_FN   0x0054cec0u
#define FOG_START_M      100.0f     /* FOG_MODE 1/2 */
#define FOG_END_M        800.0f     /* at DRAW_DISTANCE 1 */
#define FOG_GAME_START_M 450.0f     /* FOG_MODE 0, the game's own fog */
#define FOG_GAME_END_M   900.0f
#define FOG_END_PER_X    100.0f     /* more for each 1x of DRAW_DISTANCE */

#define SKY_RADIUS 60000.0f         /* the skybox's scale across, set by its render at 0xbe7d0 */

float g_fog_world_far;             /* the far end of the drawn city (the low-detail far), for the skyline's fog */
float g_fog_clip;                   /* where the world is seen to end: the fog's end or the city's far end */
float sky_k = 1;             /* the skybox's height over the camera's (apply_sky_horizon) */
float g_cam_z;                      /* the camera's height over the sea, as the skybox render read it */

int g_fog_all, g_sky_draw;
static int fog_mode = -1;
int fog_sky_wrapped;         /* the skybox render goes through gmv_skybox_render */

/* FOG_MODE: 0 = the game's own fog (blank), 1 = linear on the whole world, 2 = the hybrid (the GL shim). */
int gmv_fog_mode(void)
{
    if (fog_mode < 0) {
        const char *m = gmv_getenv("GMV_FOG_MODE");
        fog_mode = m && *m ? atoi(m) : 0;
        fog_mode = fog_mode < 0 ? 0 : fog_mode > 2 ? 2 : fog_mode;
    }
    return fog_mode;
}

/* The options menu's FOG_MODE (the GL shim, after a present).  Without the skybox wrapped, the whole-world
   fog cannot come on; mode 0 puts the skybox back at sea level and the game's distances. */
int gmv_fog_mode_set(int mode)
{
    gmv_fog_mode();
    fog_mode = mode < 0 ? 0 : mode > 2 ? 2 : mode;
    if (fog_mode && !fog_sky_wrapped)
        fog_mode = 0;
    g_fog_all = fog_mode > 0;
    if (!g_fog_all)
        sky_k = 0;
    gmv_log("[gamepatch] fog mode %d%s\n", fog_mode, g_fog_all ? ": fog on the whole world" : ": the game's own fog");
    return fog_mode;
}

__attribute__((cdecl, force_align_arg_pointer))
static int gmv_set_fog(void *params, int id, int index, const float *range)
{
    g_fog_world_far = (3 * range[1] + 2 * range[0]) / 5;     /* the game's end is far + 2/3 of (far - start) */
    int mac = gmv_config_mac();
    static float logged = -1;                       /* the end logged: at the first tick and after each change */
    if (mac && !g_fog_all) {                        /* ORIGINAL_MAC_EXPERIENCE: the game's own distances */
        if (logged != 0) {
            logged = 0;
            gmv_log("[gamepatch] fog %.0f..%.0f m, the game's own (ORIGINAL_MAC_EXPERIENCE)\n", range[0] / 100, range[1] / 100);
        }
        return ((int (*)(void *, int, int, const float *))(uintptr_t)FOG_FN)(params, id, index, range);
    }
    double dd = dd_on && dd_scale > 1 ? dd_scale : 1;   /* the draw distance in force */
    float extra = (float)(FOG_END_PER_X * (dd - 1));
    float r[2] = { (g_fog_all ? FOG_START_M : FOG_GAME_START_M) * 100, ((g_fog_all ? FOG_END_M : FOG_GAME_END_M) + extra) * 100 };
    if (g_fog_all) {
        g_fog_clip = r[1] < g_fog_world_far ? r[1] : g_fog_world_far;
        sky_k = 1 - SKY_RADIUS / g_fog_clip;
    }
    if (logged != r[1] + g_fog_all) {               /* (the mode counted, for the same end in both) */
        logged = r[1] + g_fog_all;
        if (g_fog_all)
            gmv_log("[gamepatch] fog %.0f..%.0f m at draw distance %.2gx (the game's %.0f..%.0f m); the world seen to %.0f m, "
                    "the skyline's foot there\n", r[0] / 100, r[1] / 100, dd, range[0] / 100, range[1] / 100, g_fog_clip / 100);
        else
            gmv_log("[gamepatch] the game's own fog at %.0f..%.0f m at draw distance %.2gx (the game's %.0f..%.0f m)\n",
                    r[0] / 100, r[1] / 100, dd, range[0] / 100, range[1] / 100);
    }
    return ((int (*)(void *, int, int, const float *))(uintptr_t)FOG_FN)(params, id, index, r);
}

/* Most of the city's materials have no fog (see fog_force_begin in the GL shim), which fogs them while
   leaving the sky alone: the skybox render 0xbe468, called only through its vtable entry 0x6522d8, is
   wrapped to set g_sky_draw.  render(node, arg) reads its second argument, so both are passed on. */
#define FOG_SKY_VT     0x006522d8u
#define FOG_SKY_RENDER 0x000be468u

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_skybox_render(void *n, int arg)
{
    g_sky_draw = 1;
    ((void (*)(void *, int))(uintptr_t)FOG_SKY_RENDER)(n, arg);
    g_sky_draw = 0;
}

/* The skybox wrap for FOG_MODE 1/2 (or the options menu's), and the distances' setter; neither under
   ORIGINAL_MAC_EXPERIENCE. */
void apply_fog(void)
{
    if (gmv_config_mac())
        return;                                 /* the Mac's fog: the hooks come with the switch */
    if (gmv_fog_mode() || tab_live()) {
        uint32_t w = (uint32_t)(uintptr_t)gmv_skybox_render;
        if (*(uint32_t *)(uintptr_t)FOG_SKY_VT == FOG_SKY_RENDER && patch_bytes(FOG_SKY_VT, (const unsigned char *)&w, 4)) {
            fog_sky_wrapped = 1;
            g_fog_all = gmv_fog_mode() > 0;
            sky_k = g_fog_all ? 1.0f : 0.0f;
            gmv_log("[gamepatch] %s\n", g_fog_all ? "fog on the whole world (the sky left clear, the skyline hazed)"
                                                  : "the game's own fog, the whole world's ready for the options menu");
        } else {
            fog_mode = 0;
            gmv_log("[gamepatch] skybox render not the expected entry; fog stays on the game's own materials\n");
        }
    }
    if (!sm_is_call(FOG_SITE, FOG_FN)) {
        gmv_log("[gamepatch] fog setter not the expected bytes; the fog keeps the game's distances\n");
        return;
    }
    int32_t r = (int32_t)((uint32_t)(uintptr_t)gmv_set_fog - (FOG_SITE + 5));
    if (!patch_bytes(FOG_SITE + 1, (const unsigned char *)&r, 4))
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); the fog keeps the game's distances\n", FOG_SITE + 1, GetLastError());
}

/* The skybox (the sky gradient and the distant skyline) sits 600 m around the camera, but the skybox
   render puts it at the camera's x and y and at height 0, the sea: the skyline's foot and the sky's
   horizon band are at sea level.  Seen from a helicopter they sink well below the true horizon (about
   14 degrees at 150 m up), and the city drawn out past them -- to 900 m on the Mac, to 2.7 km at
   DRAW_DISTANCE=3 -- rises over the skyline's foot with its square edge; lifted to the camera's height
   instead, it floats over the sea, which fades out in the fog well below the horizon.  With the fog on
   the whole world the skyline stands where the world is seen to end, g_fog_clip (the fog's end, or the
   city's far end if that comes first): the skybox is put at the camera's height times
   sky_k = 1 - SKY_RADIUS / g_fog_clip, so its foot, SKY_RADIUS away, is seen at the same angle below
   the horizon as the sea g_fog_clip away, at any altitude.  the GL shim ends the world there, fogs the
   skyline's foot, fills the sky below eye level with the fog's colour and carries the sea on past the
   map's edge, so the fogged sea runs into the skyline's foot.  At 0xbe5c6 the render zeroes the height
   of the position it read from the camera (eax points at it); a trampoline now stores the camera's
   height times sky_k (xmm0 is reloaded before its next use), and keeps the height in g_cam_z for the
   hybrid fog's thinning with height.  The same vector's height also enters a
   length that only scales the direction handed to atan2. */
#define SKY_HEIGHT_SITE 0x000be5c6u   /* movl $0x0,-0x80(%ebp) */
#define SKY_HEIGHT_CONT 0x000be5cdu

void apply_sky_horizon(void)
{
    static const unsigned char orig[7] = { 0xc7, 0x45, 0x80, 0x00, 0x00, 0x00, 0x00 };
    if (!fog_sky_wrapped)
        return;
    if (memcmp((const void *)(uintptr_t)SKY_HEIGHT_SITE, orig, sizeof orig)) {
        gmv_log("[gamepatch] skybox height not the expected bytes; the skyline stays at sea level\n");
        return;
    }
    static unsigned char *tr;
    if (!tr)
        tr = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) {
        gmv_log("[gamepatch] VirtualAlloc for the skybox height failed (%lu); the skyline stays at sea level\n",
                GetLastError());
        return;
    }
    size_t o = 0;
    static const unsigned char load[5] = { 0xf3, 0x0f, 0x10, 0x40, 0x08 };     /* movss 8(%eax),%xmm0 */
    memcpy(tr + o, load, 5), o += 5;
    tr[o++] = 0xf3, tr[o++] = 0x0f, tr[o++] = 0x11, tr[o++] = 0x05;           /* movss %xmm0,g_cam_z */
    uint32_t cz = (uint32_t)(uintptr_t)&g_cam_z;
    memcpy(tr + o, &cz, 4), o += 4;
    tr[o++] = 0xf3, tr[o++] = 0x0f, tr[o++] = 0x59, tr[o++] = 0x05;           /* mulss sky_k,%xmm0 */
    uint32_t k = (uint32_t)(uintptr_t)&sky_k;
    memcpy(tr + o, &k, 4), o += 4;
    static const unsigned char store[5] = { 0xf3, 0x0f, 0x11, 0x45, 0x80 };    /* movss %xmm0,-0x80(%ebp) */
    memcpy(tr + o, store, 5), o += 5;
    emit_jmp(tr, &o, SKY_HEIGHT_CONT);
    unsigned char site[7] = { 0xe9, 0, 0, 0, 0, 0x90, 0x90 };
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)tr - (SKY_HEIGHT_SITE + 5));
    memcpy(site + 1, &rel, 4);
    if (patch_bytes(SKY_HEIGHT_SITE, site, sizeof site))
        gmv_log("[gamepatch] skybox raised with the camera (trampoline @ %p): the skyline's foot stays where the "
                "fogged sea ends\n", (void *)tr);
    else
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); the skyline stays at sea level\n", SKY_HEIGHT_SITE, GetLastError());
}
