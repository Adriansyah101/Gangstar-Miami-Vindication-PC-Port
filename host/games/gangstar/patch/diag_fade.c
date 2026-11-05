/* diag_fade.c -- diagnostic: full-screen black fades, and the player's death */
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

/* ---- diagnostic: full-screen black fades (GMV_FADE_TRACE=1) -------------------------------------

   Every screen fade in the game is a black draw2DRectangle(driver, SColor, rect*, clip) at 0x4c5ce0
   with alpha in the colour's top byte: the cutscene fade (0x34918, alpha global 0x6424b4), the menu
   transitions (0x8d050) and the state-machine fades (0x1a5b32, 0x1b279e, 0x1b4f00 -- the last is the
   generic fade state pushed by gameplay/mission code).  To tell whether the death -> hospital respawn
   asks for a fade at all, log each origin-anchored rectangle whenever its caller or alpha changes,
   with a millisecond stamp.  If alpha ramps here but the screen never darkens, the host loses it; if
   nothing is logged at the death, the game never draws one.  Pure diagnostic. */
#define FADE_FUNC 0x004c5ce0u      /* draw2DRectangle; prologue 55 89 e5 57 56 53 (6 bytes displaced)  */

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_fade_log(uint32_t caller, uint32_t color, const int32_t *rect)
{
    if (!rect || IsBadReadPtr(rect, 16) || rect[0] != 0 || rect[1] != 0)
        return;
    gmv_gl_fade_mark(caller, caller == 0x1b5094);                   /* the generic fade state (death, menus): film it */
    static uint32_t lcaller, lcolor;
    static unsigned lines;
    if (caller == lcaller && color == lcolor)
        return;
    lcaller = caller; lcolor = color;
    if (lines++ < 4000)
        gmv_log("[fade] t=%lu caller=0x%06x alpha=%u rgb=%06x rect=%dx%d\n", (unsigned long)GetTickCount(),
                caller, color >> 24, color & 0xffffff, (int)rect[2], (int)rect[3]);
}

void apply_fade_trace(void)
{
    const char *e = gmv_dbgenv("GMV_FADE_TRACE");
    if (!(e && *e && *e != '0'))
        return;
    static const unsigned char expect[6] = { 0x55, 0x89, 0xe5, 0x57, 0x56, 0x53 };
    if (memcmp((const void *)(uintptr_t)FADE_FUNC, expect, sizeof expect) != 0) {
        gmv_log("[gamepatch] fn 0x%08x not the expected bytes; fade trace skipped\n", FADE_FUNC);
        return;
    }
    unsigned char *tr = VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return;
    /* Entered via jmp at entry: [esp]=ret [esp+4]=driver [esp+8]=color [esp+0xc]=rect* (cdecl). */
    size_t o = 0;
    tr[o++] = 0xff; tr[o++] = 0x74; tr[o++] = 0x24; tr[o++] = 0x0c;  /* push dword [esp+0xc] ; rect    */
    tr[o++] = 0xff; tr[o++] = 0x74; tr[o++] = 0x24; tr[o++] = 0x0c;  /* push dword [esp+0xc] ; color   */
    tr[o++] = 0xff; tr[o++] = 0x74; tr[o++] = 0x24; tr[o++] = 0x08;  /* push dword [esp+8]   ; caller  */
    tr[o++] = 0xb8; { uint32_t fnp = (uint32_t)(uintptr_t)&gmv_fade_log; memcpy(tr + o, &fnp, 4); o += 4; }
    tr[o++] = 0xff; tr[o++] = 0xd0;                                 /* call eax                        */
    tr[o++] = 0x83; tr[o++] = 0xc4; tr[o++] = 0x0c;                 /* add esp,0xc                     */
    tr[o++] = 0x55; tr[o++] = 0x89; tr[o++] = 0xe5;                 /* displaced 6: push ebp;mov ebp,esp */
    tr[o++] = 0x57; tr[o++] = 0x56; tr[o++] = 0x53;                 /* push edi;push esi;push ebx       */
    tr[o++] = 0xff; tr[o++] = 0x25; size_t contref = o; o += 4;
    size_t contslot = o;
    { uint32_t c = FADE_FUNC + 6; memcpy(tr + o, &c, 4); o += 4; }
    { uint32_t a = (uint32_t)(uintptr_t)(tr + contslot); memcpy(tr + contref, &a, 4); }
    FlushInstructionCache(GetCurrentProcess(), tr, o);
    unsigned char site[6];
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)tr - (FADE_FUNC + 5));
    site[0] = 0xe9; memcpy(site + 1, &rel, 4); site[5] = 0x90;
    if (!patch_bytes(FADE_FUNC, site, sizeof site)) {
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); fade trace skipped\n", FADE_FUNC, GetLastError());
        return;
    }
    gmv_log("[gamepatch] fade trace installed at 0x%08x (trampoline @ %p); [fade] logs black-rect alpha changes\n",
            FADE_FUNC, (void *)tr);
}

/* The cutscene fader is driven by two script commands, ScriptCommand<Data::CineFadeIn/Out>::update
   (vtbl+8, update(this, int dt)) at 0xcd8be / 0xcda6c.  Each ramps the shared timer 0x6424b0 by dt
   over the length [this+0x10] read from the script.  After a death the same fades get shorter; the
   [cine] lines tell whether the length or the dt changed, and who called update. */
#define CINE_FADEIN_UPD  0x000cd8beu   /* prologue 55 89 e5 53 57 56 (6 bytes displaced) */
#define CINE_FADEOUT_UPD 0x000cda6cu
#define CINE_TIMER       0x006424b0u
#define CINE_ALPHA       0x006424b4u

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_cine_log(uint32_t which, uint32_t ret, const int32_t *cmd, int32_t dt)
{
    static unsigned lines;
    if (lines >= 6000 || !cmd || IsBadReadPtr(cmd, 0x14))
        return;
    lines++;
    gmv_log("[cine] t=%lu %s ret=0x%06x cmd=%p len=%d c=%d dt=%d timer=%d alpha=%d\n",
            (unsigned long)GetTickCount(), which ? "out" : "in ", ret, (const void *)cmd, (int)cmd[4],
            (int)cmd[3], (int)dt, *(const int32_t *)(uintptr_t)CINE_TIMER,
            *(const int32_t *)(uintptr_t)CINE_ALPHA);
}

/* Entry hook for a function with the 6-byte prologue 55 89 e5 53 57 56: calls
   cb(which, ret, arg1, arg2) (cdecl), then runs the function unchanged. */
static void hook_cine_update(uint32_t fn, uint32_t which, void *cb)
{
    static const unsigned char expect[6] = { 0x55, 0x89, 0xe5, 0x53, 0x57, 0x56 };
    if (memcmp((const void *)(uintptr_t)fn, expect, sizeof expect) != 0) {
        gmv_log("[gamepatch] fn 0x%08x not the expected bytes; trace hook skipped\n", fn);
        return;
    }
    unsigned char *tr = VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return;
    /* Entered via jmp at entry: [esp]=ret [esp+4]=this [esp+8]=dt. */
    size_t o = 0;
    tr[o++] = 0xff; tr[o++] = 0x74; tr[o++] = 0x24; tr[o++] = 0x08;  /* push dword [esp+8]   ; dt      */
    tr[o++] = 0xff; tr[o++] = 0x74; tr[o++] = 0x24; tr[o++] = 0x08;  /* push dword [esp+8]   ; this    */
    tr[o++] = 0xff; tr[o++] = 0x74; tr[o++] = 0x24; tr[o++] = 0x08;  /* push dword [esp+8]   ; ret     */
    tr[o++] = 0x68; memcpy(tr + o, &which, 4); o += 4;              /* push which                      */
    tr[o++] = 0xb8; { uint32_t fnp = (uint32_t)(uintptr_t)cb; memcpy(tr + o, &fnp, 4); o += 4; }
    tr[o++] = 0xff; tr[o++] = 0xd0;                                 /* call eax                        */
    tr[o++] = 0x83; tr[o++] = 0xc4; tr[o++] = 0x10;                 /* add esp,0x10                    */
    memcpy(tr + o, expect, sizeof expect); o += sizeof expect;      /* displaced prologue              */
    tr[o++] = 0xff; tr[o++] = 0x25; size_t contref = o; o += 4;
    size_t contslot = o;
    { uint32_t c = fn + 6; memcpy(tr + o, &c, 4); o += 4; }
    { uint32_t a = (uint32_t)(uintptr_t)(tr + contslot); memcpy(tr + contref, &a, 4); }
    FlushInstructionCache(GetCurrentProcess(), tr, o);
    unsigned char site[6];
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)tr - (fn + 5));
    site[0] = 0xe9; memcpy(site + 1, &rel, 4); site[5] = 0x90;
    if (!patch_bytes(fn, site, sizeof site)) {
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); trace hook skipped\n", fn, GetLastError());
        return;
    }
    gmv_log("[gamepatch] trace hook installed at 0x%08x (trampoline @ %p)\n", fn, (void *)tr);
}

/* Death trigger for the fade capture.  Player::applyDamage(this, damage, type, attacker) is 0xf8282
   (reached from the Player vtable through the this-adjusting thunk 0xf9050); its Character part
   starts at this+4, and Character::getHealth / getMaxHealth read +0x578 / +0x574 of that part
   (0x2f644 / 0x7e278).  The hook only learns the player pointer; gmv_death_poll, called once per
   present, watches the health and arms the capture on the tick it goes from above 0 to 0 or below,
   whatever lowered it. */
#define PLAYER_APPLY_DAMAGE 0x000f8282u
#define PLAYER_HEALTH_OFF   (4 + 0x578)
#define PLAYER_MAXHP_OFF    (4 + 0x574)
static volatile uint32_t g_player;
static volatile int g_player_hp0;                   /* health before the hit that revealed g_player */

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_player_damage(uint32_t which, uint32_t ret, uint32_t player, int32_t damage)
{
    (void)which;
    if (player != g_player && !IsBadReadPtr((const void *)(uintptr_t)player, PLAYER_HEALTH_OFF + 4)) {
        g_player_hp0 = *(const int32_t *)(uintptr_t)(player + PLAYER_HEALTH_OFF);
        gmv_trace("[death] player object %08x health %d (applyDamage from 0x%06x, damage %d)\n", player,
                g_player_hp0, ret, (int)damage);
        g_player = player;
    }
}

void gmv_death_poll(void)
{
    static uint32_t lplayer;
    static int lhp;
    uint32_t p = g_player;
    if (!p || IsBadReadPtr((const void *)(uintptr_t)p, PLAYER_HEALTH_OFF + 4))
        return;
    int hp = *(const int32_t *)(uintptr_t)(p + PLAYER_HEALTH_OFF);
    int maxhp = *(const int32_t *)(uintptr_t)(p + PLAYER_MAXHP_OFF);
    if (p != lplayer)
        lhp = g_player_hp0;                         /* so a first, lethal hit still counts */
    if (lhp > 0 && hp <= 0) {
        gmv_trace("[death] t=%lu player health %d -> %d (max %d)\n", (unsigned long)GetTickCount(), lhp, hp, maxhp);
        gmv_gl_death_capture_arm(hp, maxhp);
    }
    lplayer = p;
    lhp = hp;
}

void apply_cine_trace(void)
{
    const char *e = gmv_dbgenv("GMV_FADE_TRACE");
    if (!(e && *e && *e != '0'))
        return;
    hook_cine_update(CINE_FADEIN_UPD, 0, (void *)&gmv_cine_log);
    hook_cine_update(CINE_FADEOUT_UPD, 1, (void *)&gmv_cine_log);
    hook_cine_update(PLAYER_APPLY_DAMAGE, 2, (void *)&gmv_player_damage);
}
