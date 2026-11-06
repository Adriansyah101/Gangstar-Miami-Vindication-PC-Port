/* switch_inventory.c -- PLAYERSWITCH_INVENTORY: Betty shares Johnny's cash and weapons */
#include "compat.h"
#include <stdint.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* ---- PLAYERSWITCH_INVENTORY (menu: PLAYERSWITCH INVENTORY): in free roam, the player the story
   switched you to (Betty) shares Johnny's cash and weapons (an enhancement the user asked for on
   2026-10-30; off by default and under ORIGINAL_MAC_EXPERIENCE) ----
   Each Player is built (0xf9e94) with 10000 cash and a fresh WeaponInfos (0x8ae3c, 0x74 bytes at
   +0x7f4): +0 the 16 weapon slots' objects (slot i is always the same kind, 0x8a998(i)), +0x40 the 11
   ammo counts, +0x6c the slot in hand (0x8a4ea), +0x70 "armed" (on foot the weapon keys switch only
   while it is set: 0xf4b44 → 0x8a44a), +0x71 "never runs dry" (set on the player not in control by
   0x8a456).  A weapon object's +0xa1 is 1 while it is locked (0x8616a locks, 0x8612e gives;
   both also flip one bit of a mask shared by everyone) and +0x3c/+0x44 are what the save keeps of it
   (WEPN, read back by 0x85e00/0x85e0e).
   Once a present (the main thread, between updates): while the player in control is not the main one
   and no mission is on (the manager idle: record -1, state 0), the first look copies Johnny's cash,
   ammo and weapons onto them, and each later one copies theirs back onto Johnny, so what they spend
   or pick up is his too (the user chose one wallet).  Missions keep the game's own loadout: sharing
   stops while one is on (the user's choice), and money the same player made during it is added to
   Johnny's when sharing resumes (a reward for a mission passed as Betty).  The slot in hand is never
   locked under its holder.  The destructors (0xf6f1c, 0xf92dc) clear *0x689ea8 and +0x7f4. */
#define SI_CASH     0x754u
#define SI_WEAPONS  0x7f4u
#define SI_SLOTS    16
#define SI_AMMO     0x40u
#define SI_AMMO_N   11
#define SI_IN_HAND  0x6cu
#define SI_ARMED    0x70u
#define SI_W_LOCKED 0xa1u
#define SI_W_KEEP1  0x3cu
#define SI_W_KEEP2  0x44u
static volatile int g_si_on;
static uint32_t si_who;              /* the player sharing Johnny's things now, or 0               */
static uint32_t si_paused;           /* the one sharing until a mission began, and its cash then    */
static int32_t si_paused_cash;

int si_set(int on)
{
    if (on != g_si_on)
        gmv_log("[gamepatch] PLAYERSWITCH_INVENTORY %s\n", on ? "on" : "off");
    g_si_on = on;
    if (!on)
        si_who = si_paused = 0;
    return 1;
}

/* n bytes from a are committed, writable memory (the copy writes; ms_rd's 0 can't tell a bad read). */
static int si_mem(uint32_t a, uint32_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    return a >= 0x10000 && VirtualQuery((const void *)(uintptr_t)a, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
           !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
           (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) &&
           (uint32_t)((const char *)mbi.BaseAddress + mbi.RegionSize - (const char *)(uintptr_t)a) >= n;
}

static uint32_t si_infos(uint32_t p)
{
    uint32_t w = si_mem(p, SI_WEAPONS + 4) ? ms_rd(p + SI_WEAPONS) : 0;
    return w && si_mem(w, 0x74) && ms_rd(w + SI_IN_HAND) < SI_SLOTS ? w : 0;
}

/* may_lock 0 (the copy back onto Johnny): weapons are only ever added to him, never taken away. */
static void si_copy(uint32_t from, uint32_t to, int may_lock)
{
    uint32_t fi = si_infos(from), ti = si_infos(to), hand = ms_rd(ti + SI_IN_HAND);
    *(int32_t *)(uintptr_t)(to + SI_CASH) = (int32_t)ms_rd(from + SI_CASH);
    for (int i = 0; i < SI_AMMO_N; i++)
        *(uint32_t *)(uintptr_t)(ti + SI_AMMO + 4 * i) = ms_rd(fi + SI_AMMO + 4 * i);
    /* The "armed" byte: on foot, 0xf4b44 (asked by the weapon keys 0x11e5ba / 0x11d222) lets the
       weapons be switched only while it is set; 0x8b00c sets it when a weapon is given. */
    unsigned char armed = *(unsigned char *)(uintptr_t)(fi + SI_ARMED);
    if (armed || may_lock)
        *(unsigned char *)(uintptr_t)(ti + SI_ARMED) = armed;
    for (int i = 0; i < SI_SLOTS; i++) {
        uint32_t fw = ms_rd(fi + 4 * i), tw = ms_rd(ti + 4 * i);
        if (!fw || !tw || fw == tw || !si_mem(fw, SI_W_LOCKED + 1) || !si_mem(tw, SI_W_LOCKED + 1))
            continue;
        unsigned char lock = *(unsigned char *)(uintptr_t)(fw + SI_W_LOCKED);
        if (lock && !may_lock)
            continue;
        *(unsigned char *)(uintptr_t)(tw + SI_W_LOCKED) = (uint32_t)i == hand ? 0 : lock;
        *(uint32_t *)(uintptr_t)(tw + SI_W_KEEP1) = *(uint32_t *)(uintptr_t)(fw + SI_W_KEEP1);
        *(uint32_t *)(uintptr_t)(tw + SI_W_KEEP2) = *(uint32_t *)(uintptr_t)(fw + SI_W_KEEP2);
    }
}

static int si_weapons_owned(uint32_t p)
{
    uint32_t wi = si_infos(p);
    int n = 0;
    for (int i = 0; wi && i < SI_SLOTS; i++) {
        uint32_t w = ms_rd(wi + 4 * i);
        n += w && si_mem(w, SI_W_LOCKED + 1) && !*(unsigned char *)(uintptr_t)(w + SI_W_LOCKED);
    }
    return n;
}

__attribute__((force_align_arg_pointer))
void gmv_playerswitch_inventory_tick(void)
{
    lc_tick();                                            /* LC_SWITCH */
    if (!g_si_on)
        return;
    uint32_t mainp = ms_rd(SSF_MAIN_PP), ctrl = ms_rd(SSF_CTRL_PP), mgr = mr_mgr();
    int players = mainp && ctrl && mainp != ctrl && ms_rd(mainp) == SSF_PLAYER_VT && ms_rd(ctrl) == SSF_PLAYER_VT &&
                  si_infos(mainp) && si_infos(ctrl);
    int mission = !mgr || (int)ms_rd(mgr + 4) != -1 || ms_rd(mgr + 0x1c);
    if (si_who && (!players || ctrl != si_who || mission)) {
        if (players && ctrl == si_who) {                  /* a mission began: hold on to who it was */
            si_paused = ctrl;
            si_paused_cash = (int32_t)ms_rd(ctrl + SI_CASH);
        }
        gmv_trace("[switchinv] sharing stopped (%s); Johnny's cash %d\n",
                !players ? "back to Johnny" : ctrl != si_who ? "another player" : "a mission began",
                mainp ? (int)ms_rd(mainp + SI_CASH) : 0);
        si_who = 0;
    }
    if (!players || mission) {
        if (!players)
            si_paused = 0;
        return;
    }
    if (!si_who) {
        int32_t made = 0;
        if (si_paused == ctrl && (made = (int32_t)ms_rd(ctrl + SI_CASH) - si_paused_cash) > 0)
            *(int32_t *)(uintptr_t)(mainp + SI_CASH) += made;
        else
            made = 0;
        si_paused = 0;
        si_copy(mainp, ctrl, 1);
        si_who = ctrl;
        gmv_trace("[switchinv] the player in control shares Johnny's things: cash %d (%d made in the mission), "
                "%d weapons\n", (int)ms_rd(ctrl + SI_CASH), (int)made, si_weapons_owned(ctrl));
        return;
    }
    si_copy(ctrl, mainp, 0);
}

void apply_playerswitch_inventory(void)
{
    const char *e = gmv_getenv("GMV_PLAYERSWITCH_INVENTORY");
    g_si_on = e && *e == '1';
    gmv_log("[gamepatch] PLAYERSWITCH_INVENTORY %s (in free roam the switched-to player shares Johnny's cash and "
            "weapons)\n", g_si_on ? "on" : "off");
}
