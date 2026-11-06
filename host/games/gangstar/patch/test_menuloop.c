/* test_menuloop.c -- test aid: loop one main-menu scene */
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

#if GMV_DEV
/* ---- test aid: loop one main-menu scene, camera behind its walker (GMV_MENU_LOOP_SCENE) -------

   For watching the scripted walkers up close (the WALK_SYNC facing flicker).  The main menu's
   background is script 81 (kind 0, started by 0x1b5b52 and kept at 0x949ad8); it starts script 92,
   which starts the first scene.  Each scene is a script of its own that sets its cameras (CamSet,
   'SceneCamera_MML_*'), runs about 10 s and ends by starting the next scene with a ScriptUnlock
   (setup 0xd080e: 0x32d98 looks the script up, 0x3244c sets it running), in this fixed order:
   COCONUT 83, SWAMP 99, PORT 94, COUNTRYSIDE 84, DOWNTOWN 86 and 87, AIRPORT 82, LILHAVANA 90 and
   89, POORSUBURB 93, RICHRESID 97, 95 and 96, SOUTHBEACH 98, NORTHBEACH 91, DESIGND 85, then
   COCONUT again.  GMV_MENU_LOOP_SCENE=N swaps every scene start on the main menu for the Nth
   scene; when that scene ends by starting the next, it is set running again on the next tick
   instead, once its runner is done with the command.

   GMV_MENU_LOOP_CAM=1 also puts the view behind one of the walkers.  The characters the ActorGoTo
   commands (setup 0xc897a, every update) move are collected from the scene's start, with the time
   each was last moved; after 1 s the one on foot nearest the scene camera's look-at point (camera
   +0x14), within 30 m of it, is taken, and F9 steps to the next nearest.  The camera manager's update (0x44640, its one call
   at 0x1a2316) runs as before, and then the camera scene node (manager +0) is set to a chase view:
   behind the walker along his smoothed walking direction, so the view itself does not turn with
   his facing.  The node's setPosition / setTarget (vtable +0xa4 / +0x104) are the ones 0x44b88
   snaps with.  A walker not moved for 100 ms, or without his scene node (+0x10, which his position
   getter reads), is let go and the scene camera shows again, so a despawned one is never read.  The on-foot follow camera (manager mode 0) is no use here: it
   does not move on the menu.  Test-only; off unless set. */
#define ML_UNLOCK_SLOT 0x006553e8u   /* ScriptCommands::ScriptUnlock vtable, setup */
#define ML_UNLOCK_FN   0x000d080eu
#define ML_GOTO_SLOT   0x006527e8u   /* ScriptCommands::ActorGoTo vtable, setup (runs every update) */
#define ML_GOTO_FN     0x000c897au
#define ML_CAM_CALL    0x001a2316u   /* call 0x44640, the camera manager's update (mgr, dt ms) */
#define ML_CAM_UPDATE  0x00044640u
#define ML_MASTER      0x00949ad8u   /* the main menu's background script */
#define ML_SCRIPT_MGR  0x00035d08u
#define ML_SCRIPT_GET  0x00032d98u   /* (mgr, idx, kind) -> script */
#define ML_SCRIPT_RUN  0x0003244cu   /* (script): state 2, running */
#define ML_RESOLVE     0x00032838u   /* (script, name) -> object, as the commands resolve names */
#define ML_WALKERS     64
#define ML_RANGE       3000.0f       /* only walkers this near the scene camera's aim (30 m) */
#define ML_BACK       320.0f        /* chase view: units (about 1 cm) behind the walker ... */
#define ML_UP          190.0f        /* ... and above his origin, */
#define ML_AIM_UP      110.0f        /* looking at this height ... */
#define ML_AIM_AHEAD   150.0f        /* ... this far ahead of him */

static const int ml_scenes[] = { 83, 99, 94, 84, 86, 87, 82, 90, 89, 93, 97, 95, 96, 98, 91, 85 };

typedef void (*ml_cmd_fn)(uint32_t);
typedef struct { float x, y, z; } ml_v3;
typedef ml_v3 (__attribute__((callee_pop_aggregate_return(1))) *ml_pos_fn)(uint32_t);
typedef void (*ml_node_set_fn)(uint32_t, const float *);

static int g_ml_idx = -1, g_ml_n, g_ml_n_scene, g_ml_cam, g_ml_restart, g_ml_pick;
static uint32_t g_ml_walker[ML_WALKERS];
static DWORD g_ml_seen[ML_WALKERS];
static int g_ml_target = -1;        /* index into g_ml_walker, -1 = the scene camera shows */
static int g_ml_fresh;              /* the chase view starts from the scene camera's view */
static float g_ml_dir[2], g_ml_prev[3], g_ml_eye[3], g_ml_at[3];
static DWORD g_ml_t0;               /* the looped scene's start (GetTickCount | 1), 0 when not in it */
static uint32_t ml_script(void)
{
    uint32_t mgr = ((uint32_t (*)(void))(uintptr_t)ML_SCRIPT_MGR)();
    return ((uint32_t (*)(uint32_t, int, int))(uintptr_t)ML_SCRIPT_GET)(mgr, g_ml_idx, 0);
}

static void ml_scene_start(void)
{
    g_ml_t0 = GetTickCount() | 1;
    g_ml_n = 0;
    g_ml_target = -1;
    gmv_log("[menuloop] scene %d (script %d) starts\n", g_ml_n_scene, g_ml_idx);
}

/* Moved by its walk command within 100 ms, and still has its scene node (+0x10, which the position
   getter 0x23d16 reads; a character far from the camera can be without one). */
static int ml_alive(int i)
{
    return GetTickCount() - g_ml_seen[i] < 100 && *(const uint32_t *)(uintptr_t)(g_ml_walker[i] + 0x10);
}

/* In a vehicle, as 0xc897a tests it: the flag at +0x585 and a vehicle from vtable +0x114. */
static int ml_driving(uint32_t ch)
{
    return (*(const unsigned char *)(uintptr_t)(ch + 0x585) & 4) &&
           ((uint32_t (*)(uint32_t))(uintptr_t)*(uint32_t *)(uintptr_t)(*(uint32_t *)(uintptr_t)ch + 0x114))(ch);
}

static ml_v3 ml_pos(uint32_t ch)
{
    return (*(ml_pos_fn *)(uintptr_t)(*(uint32_t *)(uintptr_t)ch + 0x5c))(ch);
}

/* Take the walker g_ml_pick places from the nearest to the scene camera's look-at point. */
static void ml_pick(uint32_t mgr)
{
    const float *at = (const float *)(uintptr_t)(*(uint32_t *)(uintptr_t)(mgr + 8) + 0x14);
    float d[ML_WALKERS];
    int order[ML_WALKERS], n = 0, i, j;
    for (i = 0; i < g_ml_n; i++) {
        if (!ml_alive(i) || ml_driving(g_ml_walker[i]))
            continue;
        ml_v3 p = ml_pos(g_ml_walker[i]);
        d[i] = (p.x - at[0]) * (p.x - at[0]) + (p.y - at[1]) * (p.y - at[1]) + (p.z - at[2]) * (p.z - at[2]);
        if (d[i] > ML_RANGE * ML_RANGE)
            continue;
        for (j = n++; j > 0 && d[order[j - 1]] > d[i]; j--)
            order[j] = order[j - 1];
        order[j] = i;
    }
    if (!n) {
        static DWORD said;
        if (said != g_ml_t0 && GetTickCount() - g_ml_t0 >= 3000) {
            said = g_ml_t0;
            gmv_log("[menuloop] no walker on foot within 30 m of the scene camera's aim yet (%d moved this scene)\n",
                    g_ml_n);
        }
        return;
    }
    g_ml_target = order[g_ml_pick % n];
    g_ml_fresh = 1;
    gmv_log("[menuloop] camera behind walker %d of %d (0x%08x, %.0f units from the scene camera's aim; F9 = next)\n",
            g_ml_pick % n + 1, n, g_ml_walker[g_ml_target], sqrtf(d[g_ml_target]));
}

/* Ease v toward w by the share k. */
static void ml_ease(float *v, const float *w, float k)
{
    for (int i = 0; i < 3; i++)
        v[i] += (w[i] - v[i]) * k;
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_ml_cam_update(uint32_t mgr, int dt)
{
    ((void (*)(uint32_t, int))(uintptr_t)ML_CAM_UPDATE)(mgr, dt);
    if (!g_ml_t0 || gmv_top_state_vtable() != MAIN_MENU_VT)
        return;
    if (g_ml_target < 0 || !ml_alive(g_ml_target)) {
        g_ml_target = -1;
        if (g_ml_n && GetTickCount() - g_ml_t0 >= 1000)
            ml_pick(mgr);
        if (g_ml_target < 0)
            return;
    }
    uint32_t node = *(uint32_t *)(uintptr_t)mgr, cam = *(uint32_t *)(uintptr_t)(mgr + 8);
    ml_v3 p = ml_pos(g_ml_walker[g_ml_target]);
    float t = dt > 0 && dt < 200 ? (float)dt : 16.0f, mx, my, m;
    if (g_ml_fresh) {               /* start out looking at him from the scene camera's side */
        const float *eye = (const float *)(uintptr_t)(cam + 8);
        mx = p.x - eye[0]; my = p.y - eye[1];
        m = sqrtf(mx * mx + my * my);
        g_ml_dir[0] = m > 1.0f ? mx / m : 1.0f;
        g_ml_dir[1] = m > 1.0f ? my / m : 0.0f;
        memcpy(g_ml_eye, eye, sizeof g_ml_eye);
        memcpy(g_ml_at, (const float *)(uintptr_t)(cam + 0x14), sizeof g_ml_at);
    } else {                        /* follow the walking direction, eased over ~0.4 s */
        mx = p.x - g_ml_prev[0]; my = p.y - g_ml_prev[1];
        m = sqrtf(mx * mx + my * my);
        if (m > 0.05f * t) {        /* moving faster than 0.5 m/s */
            float k = 1.0f - expf(-t / 400.0f);
            g_ml_dir[0] += (mx / m - g_ml_dir[0]) * k;
            g_ml_dir[1] += (my / m - g_ml_dir[1]) * k;
            m = sqrtf(g_ml_dir[0] * g_ml_dir[0] + g_ml_dir[1] * g_ml_dir[1]);
            if (m > 1e-3f) { g_ml_dir[0] /= m; g_ml_dir[1] /= m; }
        }
    }
    g_ml_prev[0] = p.x; g_ml_prev[1] = p.y; g_ml_prev[2] = p.z;
    float eye[3] = { p.x - g_ml_dir[0] * ML_BACK, p.y - g_ml_dir[1] * ML_BACK, p.z + ML_UP };
    float at[3] = { p.x + g_ml_dir[0] * ML_AIM_AHEAD, p.y + g_ml_dir[1] * ML_AIM_AHEAD, p.z + ML_AIM_UP };
    /* glide in from the scene camera's view (~0.3 s), then hold the chase view with a light ease */
    float k = g_ml_fresh ? 0.0f : 1.0f - expf(-t / 120.0f);
    g_ml_fresh = 0;
    ml_ease(g_ml_eye, eye, k);
    ml_ease(g_ml_at, at, k);
    uint32_t vt = *(uint32_t *)(uintptr_t)node;
    (*(ml_node_set_fn *)(uintptr_t)(vt + 0xa4))(node, g_ml_eye);
    (*(ml_node_set_fn *)(uintptr_t)(vt + 0x104))(node, g_ml_at);
}

__attribute__((cdecl, force_align_arg_pointer))
void gmv_ml_unlock(uint32_t c)
{
    uint32_t own = *(uint32_t *)(uintptr_t)(c + 4);
    if (gmv_top_state_vtable() == MAIN_MENU_VT && own && own != *(uint32_t *)(uintptr_t)ML_MASTER) {
        if (own == ml_script()) {   /* the looped scene is over: run it again on the next tick */
            g_ml_target = -1;
            g_ml_t0 = 0;
            g_ml_restart = 1;
            return;
        }
        int *idx = (int *)(uintptr_t)(c + 0x10), keep = *idx;
        *idx = g_ml_idx;
        ((ml_cmd_fn)(uintptr_t)ML_UNLOCK_FN)(c);
        *idx = keep;
        ml_scene_start();
        return;
    }
    ((ml_cmd_fn)(uintptr_t)ML_UNLOCK_FN)(c);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_ml_goto(uint32_t c)
{
    if (g_ml_t0) {
        uint32_t ch = ((uint32_t (*)(uint32_t, uint32_t))(uintptr_t)ML_RESOLVE)(*(uint32_t *)(uintptr_t)(c + 4),
                                                                              *(uint32_t *)(uintptr_t)(c + 0x14));
        int i, old = -1;
        for (i = 0; i < g_ml_n && g_ml_walker[i] != ch; i++)
            if (old < 0 && i != g_ml_target && GetTickCount() - g_ml_seen[i] > 1000)
                old = i;
        if (ch && i == g_ml_n) {    /* new: a free slot, else one not moved for a second */
            if (g_ml_n < ML_WALKERS)
                g_ml_n++;
            else if (old >= 0)
                i = old;
            if (i < g_ml_n)
                g_ml_walker[i] = ch;
        }
        if (i < g_ml_n)
            g_ml_seen[i] = GetTickCount();
    }
    ((ml_cmd_fn)(uintptr_t)ML_GOTO_FN)(c);
}

/* Once per present, on the game thread after the tick's update. */
void gmv_menu_loop_tick(void)
{
    static int f9;
    if (g_ml_idx < 0)
        return;
    if (gmv_top_state_vtable() != MAIN_MENU_VT) {
        g_ml_t0 = 0;
        g_ml_target = -1;
        g_ml_restart = 0;
        return;
    }
    if (g_ml_restart) {
        g_ml_restart = 0;
        ((void (*)(uint32_t))(uintptr_t)ML_SCRIPT_RUN)(ml_script());
        ml_scene_start();
    }
    if (!g_ml_cam)
        return;
    int k = gmv_input_scancode_held(66);        /* SDL's F9 */
    if (k && !f9 && g_ml_target >= 0) {
        g_ml_pick++;
        g_ml_target = -1;           /* the camera update picks the next one */
    }
    f9 = k;
}

static int ml_slot_is(uint32_t slot, uint32_t fn)
{
    return *(const uint32_t *)(uintptr_t)slot == fn;
}

static void ml_slot_set(uint32_t slot, void *fn)
{
    uint32_t v = (uint32_t)(uintptr_t)fn;
    patch_bytes(slot, (const unsigned char *)&v, 4);
}

void apply_menu_loop(void)
{
    const char *e = gmv_dbgenv("GMV_MENU_LOOP_SCENE");
    int n = e && *e ? atoi(e) : 0;
    if (n < 1)
        return;
    if (n > (int)(sizeof ml_scenes / sizeof *ml_scenes)) {
        gmv_log("[menuloop] MENU_LOOP_SCENE=%d: the menu has %u scenes; ignored\n", n,
                (unsigned)(sizeof ml_scenes / sizeof *ml_scenes));
        return;
    }
    e = gmv_dbgenv("GMV_MENU_LOOP_CAM");
    g_ml_cam = e && *e == '1';
    if (!ml_slot_is(ML_UNLOCK_SLOT, ML_UNLOCK_FN) || !ml_slot_is(ML_GOTO_SLOT, ML_GOTO_FN) ||
        (g_ml_cam && !sm_is_call(ML_CAM_CALL, ML_CAM_UPDATE))) {
        gmv_log("[menuloop] script command tables or camera update call not the expected ones; menu scene loop skipped\n");
        return;
    }
    g_ml_n_scene = n;
    g_ml_idx = ml_scenes[n - 1];
    ml_slot_set(ML_UNLOCK_SLOT, (void *)gmv_ml_unlock);
    if (g_ml_cam) {
        ml_slot_set(ML_GOTO_SLOT, (void *)gmv_ml_goto);
        int32_t r = (int32_t)((uint32_t)(uintptr_t)gmv_ml_cam_update - (ML_CAM_CALL + 5));
        patch_bytes(ML_CAM_CALL + 1, (const unsigned char *)&r, 4);
    }
    gmv_log("[menuloop] test aid: the main menu shows only its scene %d (script %d)%s\n", n, g_ml_idx,
            g_ml_cam ? ", with the camera behind a walker (F9 = next walker)" : "");
}
#else
void gmv_menu_loop_tick(void) {}
void apply_menu_loop(void) {}
#endif
