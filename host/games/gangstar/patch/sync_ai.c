/* sync_ai.c -- AI_SYNC: AI characters' stuck check at high tick rates */
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

/* ---- AI characters' stuck check at high tick rates (GMV_AI_SYNC, on by default) ---------------

   The characters' AI is a tree of behaviour states (0xf267c, one state vtable at 0x6525c8: enter
   0xc653a, exit 0xc645e) whose transitions test bytecode expressions (0xf3ee0) over the
   character's properties (vtable +0x34, 0x782e8 for NPCs).  Property 0x54 is a stuck check that
   counts its calls instead of time: every 5th call (0x6425ac) it compares how far the character
   moved since the last check with 20 units (0x6425b0 = 400, squared) and answers 0 when he moved
   less.  A transition's expression is tested every game update, so on the Mac the check covers
   5 updates at 30/s, 167 ms; at 240 ticks/s it covers 42 ms, and a character running at 3 m/s
   moves 12 units in that time and is taken as stuck.  The main menu's shotgun walker, running back
   after his shots, left his run state on that (its exit script stops him, ActorStop, anim 212)
   and came straight back (its enter script runs him again, ActorRun, anim 213) every 42 ms for up
   to 4 s, so his run cycle restarted over and over (a temporary probe on 0x11b03c: 84-143
   restarts per run back at 240 ticks/s, none at 60).  The fix lets the check count at most one call per Mac
   update per character, as WALK_SYNC paces the walk ray; the calls in between answer 1, as the
   original's uncounted calls do.  Installed only when the tick pacer runs faster than 60
   ticks/s, and in force only while it does (ai_sync_set). */
#define AS_PROP_FN    0x000782e8u   /* Character's property getter (this, id) */
#define AS_STUCK      0x54          /* the stuck check */
#define AS_SLOTS      64

typedef uint32_t (*as_prop_fn)(uint32_t, uint32_t);
static struct { uint32_t ch; double due, seen; } g_as[AS_SLOTS];

__attribute__((cdecl, force_align_arg_pointer))
static uint32_t gmv_ai_prop(uint32_t ch, uint32_t id)
{
    if (id == AS_STUCK && g_as_on) {
        double now = ws_now_ms();
        unsigned i, slot = AS_SLOTS, old = 0;
        for (i = 0; i < AS_SLOTS; i++) {
            if (g_as[i].ch == ch) { slot = i; break; }
            if (g_as[i].seen < g_as[old].seen) old = i;
        }
        if (slot < AS_SLOTS && now - g_as[slot].seen < 100.0 && now < g_as[slot].due - g_ws_tick) {
            g_as[slot].seen = now;
            return 1;
        }
        if (slot == AS_SLOTS) {
            slot = old;
            g_as[slot].ch = ch;
            g_as[slot].due = 0;
        }
        /* keep the Mac's average rate across uneven updates; restart after a gap */
        g_as[slot].due = now - g_as[slot].due < WS_MAC_MS ? g_as[slot].due + WS_MAC_MS : now + WS_MAC_MS;
        g_as[slot].seen = now;
    }
    return ((as_prop_fn)(uintptr_t)AS_PROP_FN)(ch, id);
}

/* AI sync for a tick of ms, as cam_sync_set. */
int as_installed;

static void ai_sync_in(double ms)
{
    /* the NPC vtables' getter slots, and the Player's two forwarders (0xf6660 and its thunk) */
    static const uint32_t slots[] = { 0x0064efbcu, 0x0064f7fcu }, calls[] = { 0x000f6673u, 0x000f6694u };
    static int failed;
    const char *e = gmv_getenv("GMV_AI_SYNC");
    int want = !(e && *e == '0') && ms < 16.6;
    unsigned i;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_ws_qpf = (double)f.QuadPart / 1000.0;
    g_ws_tick = ms;
    if (want && !as_installed && !failed) {
        for (i = 0; i < 2; i++)
            if (*(const uint32_t *)(uintptr_t)slots[i] != AS_PROP_FN || !sm_is_call(calls[i], AS_PROP_FN)) {
                gmv_log("[gamepatch] character property getter not at the expected places; AI sync skipped\n");
                failed = 1;
                return;
            }
        uint32_t fn = (uint32_t)(uintptr_t)gmv_ai_prop;
        for (i = 0; i < 2; i++) {
            int32_t r = (int32_t)(fn - (calls[i] + 5));
            patch_bytes(slots[i], (const unsigned char *)&fn, 4);
            patch_bytes(calls[i] + 1, (const unsigned char *)&r, 4);
        }
        as_installed = 1;
        g_as_on = 1;
        gmv_log("[gamepatch] AI sync installed: characters' stuck check counts as at the Mac's 30 updates/s "
                "(%.3f ms ticks)\n", ms);
        return;
    }
    if (as_installed && g_as_on != want)
        gmv_log("[gamepatch] AI sync %s (%.3f ms ticks)\n", want ? "on" : "off", ms);
    g_as_on = want && as_installed;
}

void ai_sync_set(double ms)
{
    patch_undo *keep = patch_undo_now;
    patch_undo_now = &mac_undo;
    ai_sync_in(ms);
    patch_undo_now = keep;
}

void apply_ai_sync(void)
{
    ai_sync_set(gmv_pace_frame_ms());
}
