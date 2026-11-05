/* key_move.c -- KEY_MOVE: GTA-like on-foot keys */
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

/* ---- KEY_MOVE (menu: KEYBOARD MOVEMENT, MAC / ENHANCED): GTA-like on-foot keys (an enhancement the
   user asked for on 2026-10-30; off by default and under ORIGINAL_MAC_EXPERIENCE) ----
   The foot dispatcher 0x11edf4 hands the move actions (keys 0x27-0x2a, the Mac D-pad 0x23-0x26) to
   one handler per direction: forward 0x11dcd6, back 0x11db86, left 0x11d9c6, right 0x11d818.  Each
   keeps a held flag in the handler (+0x54 forward, +0x55 back, +0x56 left, +0x57 right) and, on
   every held event, calls the character's move (char = handler+0x34 = player+4, its vtable +0x180 =
   0xf9290 -> 0xf7e86 -> 0x69794) with a fixed angle (0 forward, pi/2 left, pi back, 3pi/2 right,
   from the camera) at speed 1 (0.5 with handler+0x5a).  Only one direction plays at a time, left or
   right with forward held turns the camera instead (10 px a frame), and the character's facing is
   set straight to the angle, so it snaps round.  The stick uses the same call with any angle and
   its push as the speed; 0x69554 picks the walk animation (5) up to 0.5, the run (3) above, and the
   sprint (0x83) above with the sprint flag char+0x666.
   With the setting on, ours replaces the four: it keeps the same held flags, adds the held keys
   into one direction (W+D = forward right), eases the angle sent towards it at KM_TURN rad/s (from
   rest it starts there at once, as the stick does), and sends KM_WALK while Alt is held (not while
   sprinting).  The cover check after the move (0x61686 against handler+0x58) stays as the game has it.
   Only one held key sends the move.  Each call (0x685d2) turns the body by half its error, measured from a facing
   that is renewed only when a frame is drawn, so the calls between two draws add up: a trace run of 2026-11-03
   measured 0.5, 1, 1.5, 2, 3 of the error for 1, 2, 3, 4, 6 calls.  Above 2 the error grows, and with the game's one
   key that is rare (6 calls or more on 2 of 416 draws).  Two keys each sending it reached 3 at every late draw, and
   once the mouse gave it an error to start from, the body swung further and further from side to side. */
static const uint32_t km_site[4] = { 0x0011ef5bu, 0x0011ef6cu, 0x0011ef7du, 0x0011ef8eu };
static const uint32_t km_fn[4]   = { 0x0011dcd6u, 0x0011db86u, 0x0011d9c6u, 0x0011d818u };
#define KM_COVER_FN 0x00061686u      /* (char): the cover state the handler keeps at +0x58          */
#define KM_WALK     0.32f            /* walk animation at its own pace (0x69554: rate 2.2 s + 0.3)  */
#define KM_TURN     7.85f            /* rad/s: 450 degrees a second                                 */
#define KM_PI       3.14159265f
volatile int g_km_on, g_km_ok;

int km_set(int on)
{
    if (on != g_km_on)
        gmv_log("[gamepatch] KEY_MOVE %s\n", on ? "ENHANCED" : "MAC");
    g_km_on = on && g_km_ok;
    return g_km_ok || !on;
}

__attribute__((noinline, force_align_arg_pointer, optimize("no-optimize-sibling-calls")))
static void km_move(unsigned char *ch, float angle, float speed)
{
    ((void (*)(unsigned char *, float, float))(uintptr_t)*(uint32_t *)(uintptr_t)(*(uint32_t *)ch + 0x180))
        (ch, angle, speed);
}

/* Alt held, with the game's window in front. */
static int km_walk_key(void)
{
    return gmv_input_scancode_held(226) || gmv_input_scancode_held(230);    /* SDL's left and right Alt */
}

static void km_dir(int d, unsigned char *h, int *ev)
{
    static LARGE_INTEGER last, freq, seen[4];
    static float cur;
    static int moving;
    static unsigned char *last_h;
    unsigned char *ch = *(unsigned char **)(h + 0x34);
    LARGE_INTEGER now;
    float target, dt, speed, a;
    int fx, sx, lead = d;
    if (!g_km_on || !ch) {
        pl_call2(km_fn[d], (uint32_t)(uintptr_t)h, (uint32_t)(uintptr_t)ev);
        return;
    }
    if (ev[2] <= 1)
        h[0x54 + d] = 1;
    else {
        h[0x54 + d] = 0;
        if (d >= 2)
            ch[0x677] = 0;                      /* the game's left/right release clears it too */
    }
    fx = h[0x54] - h[0x55];
    sx = h[0x56] - h[0x57];
    if (!fx && !sx) {                           /* nothing held, or only opposite keys */
        km_move(ch, 0.0f, 0.0f);
        moving = 0;
        return;
    }
    target = atan2f((float)sx, (float)fx);      /* 0 forward, pi/2 left, -pi/2 right, pi back */
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    seen[d] = now;
    for (int i = 3; i >= 0; i--)                /* the first held key whose events still come: a flag can outlive its key */
        if (h[0x54 + i] && now.QuadPart - seen[i].QuadPart < freq.QuadPart / 10)
            lead = i;
    if (d != lead)
        return;                                 /* see above */
    dt = (float)((double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart);
    last = now;
    if (!moving || last_h != h || dt > 0.25f)
        cur = target;                           /* from rest (or after a pause): straight there */
    else {
        float diff = target - cur, step = KM_TURN * dt;
        while (diff > KM_PI)
            diff -= 2.0f * KM_PI;
        while (diff < -KM_PI)
            diff += 2.0f * KM_PI;
        if (fabsf(diff) <= step)
            cur = target;
        else
            cur += diff > 0.0f ? step : -step;
        if (cur > KM_PI)
            cur -= 2.0f * KM_PI;
        else if (cur < -KM_PI)
            cur += 2.0f * KM_PI;
    }
    moving = 1;
    last_h = h;
    speed = !ch[0x666] && km_walk_key() ? KM_WALK : 1.0f;
    if (h[0x5a] && speed > 0.5f)
        speed = 0.5f;
    a = fabsf(cur) < 0.0175f ? (cur < 0.0f ? -0.0175f : 0.0175f) : cur;   /* the game's forward is 1 degree */
    km_move(ch, a, speed);
    if ((unsigned char)pl_call1(KM_COVER_FN, (uint32_t)(uintptr_t)ch) != h[0x58]) {
        km_move(ch, 0.0f, 0.0f);
        *(uint32_t *)(h + 0x54) = 0;
        h[0x58] = (unsigned char)pl_call1(KM_COVER_FN, (uint32_t)(uintptr_t)ch);
        moving = 0;
    }
}

__attribute__((cdecl, force_align_arg_pointer)) static void gmv_km_fwd(unsigned char *h, int *ev)   { km_dir(0, h, ev); }
__attribute__((cdecl, force_align_arg_pointer)) static void gmv_km_back(unsigned char *h, int *ev)  { km_dir(1, h, ev); }
__attribute__((cdecl, force_align_arg_pointer)) static void gmv_km_left(unsigned char *h, int *ev)  { km_dir(2, h, ev); }
__attribute__((cdecl, force_align_arg_pointer)) static void gmv_km_right(unsigned char *h, int *ev) { km_dir(3, h, ev); }

void apply_key_move(void)
{
    void *const hooks[4] = { (void *)gmv_km_fwd, (void *)gmv_km_back, (void *)gmv_km_left, (void *)gmv_km_right };
    const char *e = gmv_getenv("GMV_KEY_MOVE");
    g_km_on = e && *e && *e != '0';
    if (gmv_config_mac())
        return;
    for (int i = 0; i < 4; i++)
        if (!sm_is_call(km_site[i], km_fn[i])) {
            gmv_log("[gamepatch] the on-foot move calls are not the expected ones; no KEY_MOVE\n");
            return;
        }
    for (int i = 0; i < 4; i++)
        if (!sm_redirect(km_site[i], hooks[i])) {
            g_km_on = 0;                        /* the sites already redirected run the game's own */
            gmv_log("[gamepatch] a write failed (%lu); no KEY_MOVE\n", GetLastError());
            return;
        }
    g_km_ok = 1;
    gmv_log("[gamepatch] KEY_MOVE %s (ENHANCED: W/A/S/D walk diagonally, smooth turns, Alt walks)\n",
            g_km_on ? "ENHANCED" : "MAC");
}
