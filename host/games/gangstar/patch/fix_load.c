/* fix_load.c -- LOAD_SLEEP_SKIP and LOAD_BUDGET: the streaming wait */
#include "compat.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* ---- LOAD_SLEEP_SKIP: no 16 ms sleep per tick while the game waits for streaming ----

   Symptom: for 2-4 s after the main menu appears, a respawn or a zone change, the frame rate drops
   to ~50-60 fps however fast the machine is.

   Mechanism: the game state whose Update is 0xd2164 (vtable slot 2, called from 0x327b4) waits for
   the background loader.  Until 0x1adbc4 reports it idle, each tick raises the per-tick queue budget
   at 0x641114 from 25 to 1500 and then calls the game's ms sleep 0x598100 with 16 (0xd21d8).  At the
   Mac's 60 Hz that sleep hid inside the frame; at a higher frame rate it caps those seconds.  The
   loader runs on its own thread and the idle check takes its mutex each tick, so the main thread
   does not need to yield.  This is the original behaviour, changed here at the user's request.

   Fix: the call at 0xd21d8 becomes a 5-byte NOP; the budget, the 3 s accumulator and the return
   value are untouched.  Default on, and on under ORIGINAL_MAC_EXPERIENCE=1 since 2026-11-05 (the user's
   choice: it changes no picture or rule); GMV_LOAD_SLEEP_SKIP=0 keeps the original. */
#define WAIT_SLEEP_SITE 0x000d21d8u   /* call 0x598100 (sleep ms), arg 16 stored at 0xd21d1 */
#define GAME_SLEEP_MS   0x00598100u

/* 1 = the sleep call is a NOP, 0 = the original call; -1 if the site is neither, -2 if the write failed. */
int load_sleep_skip_set(int on)
{
    static const unsigned char nop5[5] = {0x0f, 0x1f, 0x44, 0x00, 0x00};
    unsigned char call[5] = { 0xe8 };
    int32_t rel = (int32_t)(GAME_SLEEP_MS - (WAIT_SLEEP_SITE + 5));
    memcpy(call + 1, &rel, 4);
    const unsigned char *p = (const unsigned char *)(uintptr_t)WAIT_SLEEP_SITE;
    if ((memcmp(p, call, 5) && memcmp(p, nop5, 5)) || memcmp(p - 7, "\xc7\x04\x24\x10\x00\x00\x00", 7) != 0)
        return -1;
    return patch_bytes(WAIT_SLEEP_SITE, on ? nop5 : call, 5) ? on : -2;
}

void apply_load_sleep_skip(void)
{
    const char *e = gmv_getenv("GMV_LOAD_SLEEP_SKIP");
    if (e && *e == '0')
        return;
    int r = load_sleep_skip_set(1);
    if (r == -1)
        gmv_log("[gamepatch] streaming-wait sleep call 0x%08x not the expected bytes; load sleep skip skipped\n",
                WAIT_SLEEP_SITE);
    else if (r == -2)
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); load sleep skip not applied\n", WAIT_SLEEP_SITE,
                GetLastError());
    else
        gmv_log("[gamepatch] streaming wait no longer sleeps 16 ms per tick (LOAD_SLEEP_SKIP=0 to disable)\n");
}

/* ---- FAST_LOAD: the start-up "Loading" screen takes as many of its steps a frame as fit ----

   Symptom: after the start-up warning, the "Loading" screen stays about 2.2 s, a second of it at 20 % with nothing
   to do (ticks under 3 ms).

   Mechanism: the loading state (vtable 0x65a288, update 0x1b589e) keeps its step at [state+8] and its percent at
   [state+0xc].  Each update (every 33 ms) calls the step function 0x1abc92 once, at 0x1b5935, and stores the step it
   returns; at 16 it goes on to the title.  Step 3 walks a list of about 17 items twice, one item a call, so that step
   alone takes about 34 updates.  Only steps 5 and 8 have much to do (about 440 and 320 ms of reading and uploads).

   Fix: the call at 0x1b5935 goes to fl_step, which calls the step function again with the step it returned until it
   reaches 16 or FL_BUDGET_MS have gone by in this frame.  The update then goes on as before with the last step: the
   game's own code draws the screen and moves on to the title.  The steps and what they load are the game's own, only
   no longer one a frame.  Measured 2026-11-05: 4.0 -> 6.2 s to the title before.  Default on, and on under
   ORIGINAL_MAC_EXPERIENCE=1 since 2026-11-05 (the user's choice); FAST_LOAD=0 keeps the original (read at start, as
   the screen is over by the menu). */
#define FL_CALL_SITE 0x001b5935u   /* call 0x1abc92 in the loading state's update */
#define FL_STEP_FN   0x001abc92u   /* int step(int step, int *percent) */
#define FL_LAST_STEP 15            /* 16 = done */
#define FL_BUDGET_MS 10.0

__attribute__((cdecl, force_align_arg_pointer))
static int fl_step(int step, int *percent)
{
    int (*fn)(int, int *) = (int (*)(int, int *))(uintptr_t)FL_STEP_FN;
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    int s = fn(step, percent);
    while (s >= 0 && s <= FL_LAST_STEP) {
        QueryPerformanceCounter(&t);
        if ((double)(t.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart >= FL_BUDGET_MS)
            break;
        s = fn(s, percent);
    }
    return s;
}

void apply_fast_load(void)
{
    const char *e = gmv_getenv("GMV_FAST_LOAD");
    if (e && *e == '0')
        return;
    unsigned char call[5] = { 0xe8 };
    int32_t rel = (int32_t)(FL_STEP_FN - (FL_CALL_SITE + 5));
    memcpy(call + 1, &rel, 4);
    if (memcmp((const void *)(uintptr_t)FL_CALL_SITE, call, 5)) {
        gmv_log("[gamepatch] loading screen's step call 0x%08x not the expected bytes; FAST_LOAD skipped\n", FL_CALL_SITE);
        return;
    }
    rel = (int32_t)((uint32_t)(uintptr_t)fl_step - (FL_CALL_SITE + 5));
    memcpy(call + 1, &rel, 4);
    if (!patch_bytes(FL_CALL_SITE, call, 5)) {
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); FAST_LOAD not applied\n", FL_CALL_SITE, GetLastError());
        return;
    }
    gmv_log("[gamepatch] start-up loading screen takes as many steps a frame as fit in %.0f ms (FAST_LOAD=0 to disable)\n",
            FL_BUDGET_MS);
}

/* ---- LOAD_BUDGET: the streaming wait's items a tick, 200 in place of the game's 1500 ----

   Symptom: the main menu's frame and buttons come in over the black in jerks, frames of up to about 114 ms.

   Mechanism: on entering the menu the city grid adds the buildings around the camera, and each Box2D CreateProxy
   (0x1b9e50) renumbers the bounds above the one it inserts, so a proxy costs more the more there are (about 2.3-3.3
   ns a bound; 15337 records came to 189 million bounds with DRAW_DISTANCE 2).  While the loader is busy the wait state
   0xd2164 raises the items a tick of the queues at 0x689fe0 and 0x689f28 from 25 to 1500 (0xd21a4, the budget at
   0x641114).  The work is the same however it is cut, so a smaller share a tick spreads it over more frames.

   That cannot start the menu's fade early: the wait ends when 0x1adbc4 finds the queue at 0x689fe0 empty (0x134fc0 is
   given that queue with the limit on, from 0x135616), so a small budget only makes a wait longer.  The queue at
   0x689ee0 has no limit (0x1355f8), and is taken to be what still makes the first ticks heavy.

   Runs of 2026-11-03 at DRAW_DISTANCE 2: gaps over 33 ms in the first 1.1 s from 7 and 4 to 1, the longest after
   the first 234 ms from 113.7 and 101.1 ms to 32.4; the proxies end at 1069 ms, not 517-575, and the fade's timer
   still starts about 2.1-2.3 s in, with the same 30580 bounds.  Default 200, also under ORIGINAL_MAC_EXPERIENCE=1 since
   2026-11-05 (the user's choice: under it the main menu's scene changes stalled 116-219 ms, against 31-89 ms with
   200); 0 keeps the game's 1500. */
#define WAIT_BUDGET_SITE 0x000d21a4u   /* movl $1500, (%eax), and then the accumulator's load */
#define WAIT_BUDGET_GAME 1500

/* n items a tick, 0 = the game's: n when done, -1 if the site is not the expected bytes, -2 if the write failed. */
int load_budget_set(int n)
{
    const unsigned char *p = (const unsigned char *)(uintptr_t)WAIT_BUDGET_SITE;
    const int32_t to = n > 0 && n < WAIT_BUDGET_GAME ? n : WAIT_BUDGET_GAME;
    if (p[0] != 0xc7 || p[1] != 0x00 || p[6] != 0x8b || p[7] != 0x86)
        return -1;
    return patch_bytes(WAIT_BUDGET_SITE + 2, (const unsigned char *)&to, 4) ? n : -2;
}

void apply_load_budget(void)
{
    const char *e = gmv_getenv("GMV_LOAD_BUDGET");
    int n = e && *e ? atoi(e) : 200;
    if (n <= 0 || n >= WAIT_BUDGET_GAME)
        return;
    int r = load_budget_set(n);
    if (r == -1)
        gmv_log("[gamepatch] streaming wait's budget at 0x%08x not the expected bytes; LOAD_BUDGET skipped\n", WAIT_BUDGET_SITE);
    else if (r == -2)
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); LOAD_BUDGET not applied\n", WAIT_BUDGET_SITE + 2, GetLastError());
    else
        gmv_log("[gamepatch] streaming wait takes %d items a tick, not %d (LOAD_BUDGET=0 to disable)\n", n, WAIT_BUDGET_GAME);
}
