/* sync_walk.c -- WALK_SYNC: scripted walkers' steering at high tick rates */
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

/* ---- scripted walkers' steering at high tick rates (GMV_WALK_SYNC, on by default) -------------

   A script walk command (the main menu's background scenes, cutscenes) updates its character each
   game update through 0xc897a: 0x657c8 picks the point to head for, then 0x6341e turns the
   character straight to it (no turn rate).  0x657c8 casts a ray (0x2472c, called at 0x659c8) from
   the character's node at +0x2e8 -- a bone about one unit off the body's centre, so it swings with
   the facing -- toward the destination (+0x514): clear, it heads for the destination; blocked, for
   the side-step point the call returns in its last argument (+0x520 either way).  When the ray only
   grazes an obstacle, facing the side-step point swings the bone far enough to clear it, and facing
   the destination again blocks it, so the choice flips on every update while the character walks
   through that narrow band.  At the Mac's 30 updates/s a walker crosses the band in an update or
   two (one-frame turns now and then); at 240 ticks/s an update moves him a quarter as far, and the
   ambulance paramedic and the shotgun scene's walkers turned 50-75 degrees back and forth on every
   drawn frame for up to 20 frames (gmv_face.txt: 3.8 flips/s at 240 ticks/s, 0.5 at 60).  The fix
   makes the ray's answer last a Mac update: per character, the call is made at most once per 33.3
   ms and the result in between is the last one (clear, or blocked with its side-step point), so
   the walker steers as at 30 updates/s and still turns and moves at the full rate.  A new
   destination asks again at once.  Installed only when the tick pacer runs faster than 60
   ticks/s, and in force only while it does (walk_sync_set). */
#define WS_RAY_CALL   0x000659c8u   /* call 0x2472c in 0x657c8 */
#define WS_RAY_FN     0x0002472cu   /* (ch, &from, &dir, 0, 0, 1, &hit, 0, 0, &steer) -> clear */
#define WS_SLOTS      64

typedef uint32_t (*ws_ray_fn)(uint32_t, float *, float *, int, int, int, float *, int, int, float *);
static struct { uint32_t ch; uint32_t res; float dest[3], hit[3], steer[3]; double due, seen; } g_ws[WS_SLOTS];
double g_ws_tick;            /* one tick (ms): half a game update */
double g_ws_qpf;             /* QPC ticks per ms; the game's clock is QPC (mach_absolute_time) */
int g_ws_on, g_as_on;        /* walk / AI sync in force (installed and below 16.6 ms ticks) */

double ws_now_ms(void)
{
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return (double)n.QuadPart / g_ws_qpf;
}

#if GMV_DEV
#endif

__attribute__((cdecl, force_align_arg_pointer))
static uint32_t gmv_walk_ray(uint32_t ch, float *from, float *dir, int a4, int a5, int a6, float *hit,
                             int a8, int a9, float *steer)
{
#if GMV_DEV
    pd_ws_left(ch);
#endif
    if (!g_ws_on)
        return ((ws_ray_fn)(uintptr_t)WS_RAY_FN)(ch, from, dir, a4, a5, a6, hit, a8, a9, steer);
    const float *dest = (const float *)(uintptr_t)(ch + 0x514);
    double now = ws_now_ms();
    unsigned i, slot = WS_SLOTS, old = 0;
    for (i = 0; i < WS_SLOTS; i++) {
        if (g_ws[i].ch == ch) { slot = i; break; }
        if (g_ws[i].seen < g_ws[old].seen) old = i;
    }
    if (slot < WS_SLOTS && now - g_ws[slot].seen < 100.0 && now < g_ws[slot].due - g_ws_tick &&
        !memcmp(g_ws[slot].dest, dest, 12)) {
        g_ws[slot].seen = now;
        memcpy(hit, g_ws[slot].hit, 12);
        memcpy(steer, g_ws[slot].steer, 12);
        return g_ws[slot].res;
    }
#if GMV_DEV
    uint32_t vt_was = *(const uint32_t *)(uintptr_t)ch;
#endif
    uint32_t res = ((ws_ray_fn)(uintptr_t)WS_RAY_FN)(ch, from, dir, a4, a5, a6, hit, a8, a9, steer);
#if GMV_DEV
    pd_ws_report(ch, vt_was);
#endif
    if (slot == WS_SLOTS) {
        slot = old;
        g_ws[slot].ch = ch;
        g_ws[slot].due = 0;
    }
    /* keep the Mac's average rate across uneven updates; restart after a gap or a new destination */
    g_ws[slot].due = now - g_ws[slot].due < WS_MAC_MS && !memcmp(g_ws[slot].dest, dest, 12)
                     ? g_ws[slot].due + WS_MAC_MS : now + WS_MAC_MS;
    g_ws[slot].seen = now;
    g_ws[slot].res = res;
    memcpy(g_ws[slot].dest, dest, 12);
    memcpy(g_ws[slot].hit, hit, 12);
    memcpy(g_ws[slot].steer, steer, 12);
    return res;
}

/* Walk sync for a tick of ms, as cam_sync_set. */
int ws_installed;

static void walk_sync_in(double ms)
{
    static int failed;
    const char *e = gmv_getenv("GMV_WALK_SYNC");
    int want = !(e && *e == '0') && ms < 16.6;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_ws_qpf = (double)f.QuadPart / 1000.0;
    g_ws_tick = ms;                 /* 0 when uncapped */
    if (want && !ws_installed && !failed) {
        int32_t r = (int32_t)((uint32_t)(uintptr_t)gmv_walk_ray - (WS_RAY_CALL + 5));
        if (!sm_is_call(WS_RAY_CALL, WS_RAY_FN)) {
            gmv_log("[gamepatch] walk command ray call 0x%08x not the expected bytes; walk sync skipped\n",
                    WS_RAY_CALL);
            failed = 1;
        } else if (patch_bytes(WS_RAY_CALL + 1, (const unsigned char *)&r, 4)) {
            ws_installed = 1;
            gmv_log("[gamepatch] walk sync installed: scripted walkers steer as at the Mac's 30 updates/s "
                    "(%.3f ms ticks)\n", ms);
            g_ws_on = 1;
            return;
        } else
            failed = 1;
    }
    if (ws_installed && g_ws_on != want)
        gmv_log("[gamepatch] walk sync %s (%.3f ms ticks)\n", want ? "on" : "off", ms);
    g_ws_on = want && ws_installed;
}

void walk_sync_set(double ms)
{
    patch_undo *keep = patch_undo_now;
    patch_undo_now = &mac_undo;
    walk_sync_in(ms);
    patch_undo_now = keep;
}

void apply_walk_sync(void)
{
    walk_sync_set(gmv_pace_frame_ms());
}
