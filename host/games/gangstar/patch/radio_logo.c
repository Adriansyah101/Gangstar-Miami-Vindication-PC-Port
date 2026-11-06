/* radio_logo.c -- RADIO_LOGO_FIX: the HUD radio logo after getting into a car, and its trace */
#include "compat.h"
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* ---- diagnostic: radio station vs the HUD logo (GMV_RADIO_TRACE=1) ----

   The user reports entering a car with the HUD showing one station while another station's song
   plays.  The music manager (mm, built by 0x60e2c) keeps the current station in [mm+0x21c]:
   0x60190 turns the radio on (vehicle entry 0x10a57c passes the car's station, [car+0x120] the
   first time, [car+0xc8] after that; it returns at once if the radio is already on), 0x6000c
   switches station (stop, tune sfx 0x121, then [mm+0x21c] = station), and 0x5f3e8 picks the next
   item of a station from the RadioStationController at mm+0x254 (shuffle bags, the game's own RNG
   0x45ce10).  The HUD logo is set only by 0x17357e (widget [+0x14] = station, show that logo),
   called from Radio::show 0x930bc and the refresh 0x9323a, both with [mm+0x21c].  This logs the
   station table once with sound names, each start, switch and pick, and each logo change, so one
   occurrence shows which side disagrees.  Pure diagnostic; installed only when GMV_RADIO_TRACE is set. */
#define RADIO_START_FN  0x00060190u   /* (mm, station or -1): radio on                       */
#define RADIO_SHOW_RET  0x000931beu   /* return address of the logo call in Radio::show      */
#define RADIO_BANK_PP   0x0063e56cu   /* -> global -> sound bank; name(id) = [[bank]+4][id*8] */

static const uint32_t radio_pick_sites[]  = { 0x000601cdu, 0x000605bau };
static const uint32_t radio_tune_sites[]  = { 0x0006014bu, 0x00060186u };
static const uint32_t radio_start_sites[] = { 0x00036d1fu, 0x0010a5f5u, 0x0010a65au,
                                              0x0012cecfu, 0x0012f37fu, 0x001b493fu };
static const uint32_t radio_logo_sites[]  = { 0x000931b9u, 0x000932feu };
static const char *const radio_logo_names[5] = { "104FM", "303FM", "42FM", "101.1FM", "99FM" };

static int g_radio_logo = -1;   /* station the HUD logo last showed */

static uint32_t radio_rd(uint32_t a)
{
    return (a && !IsBadReadPtr((void *)(uintptr_t)a, 4)) ? *(const uint32_t *)(uintptr_t)a : 0;
}

static const char *radio_name(int id)
{
    uint32_t tbl = radio_rd(radio_rd(radio_rd(radio_rd(RADIO_BANK_PP))) + 4);
    if (!tbl || id < 0)
        return "?";
    const char *s = (const char *)(uintptr_t)radio_rd(tbl + (uint32_t)id * 8);
    return (s && !IsBadReadPtr(s, 1)) ? s : "?";
}

static const char *radio_logo_name(int station)
{
    return (station >= 0 && station < 5) ? radio_logo_names[station] : station == GMV_CR_STATION ? "CUSTOM" : "-";
}

/* RadioStationController (mm+0x254): +0x1c count, +0x18 RadioStation*[]; each station is
   {nA, A[], nB, B[], x} as RadioStation::Read 0x95610 reads it. */
static void radio_dump_stations(const unsigned char *mm)
{
    uint32_t ctl = (uint32_t)(uintptr_t)(mm + 0x254);
    int n = (int)radio_rd(ctl + 0x1c), base = (int)radio_rd((uint32_t)(uintptr_t)(mm + 0x250));
    uint32_t arr = radio_rd(ctl + 0x18);
    gmv_trace("[radio] %d stations, slot base %d, %d slots, mm station count %d\n", n, base,
            (int)radio_rd((uint32_t)(uintptr_t)(mm + 0x24c)), (int)radio_rd((uint32_t)(uintptr_t)(mm + 0x270)));
    for (int i = 0; i < n && i < 16; i++) {
        uint32_t st = radio_rd(arr + 4 * (uint32_t)i);
        gmv_trace("[radio] station %d (HUD logo %s), x=%d\n", i, radio_logo_name(i), (int)radio_rd(st + 0x10));
        for (int l = 0; l < 2; l++) {
            int cnt = (int)radio_rd(st + 8 * (uint32_t)l);
            uint32_t list = radio_rd(st + 8 * (uint32_t)l + 4);
            for (int k = 0; k < cnt && k < 32; k++) {
                int v = (int)radio_rd(list + 4 * (uint32_t)k);
                gmv_trace("[radio]   %c[%d] = %d \"%s\" (base+%d: \"%s\")\n", l ? 'B' : 'A', k, v,
                        radio_name(v), v, radio_name(base + v));
            }
        }
    }
}

__attribute__((cdecl, force_align_arg_pointer))
int gmv_radio_pick(unsigned char *mm, int station)
{
    static int dumped;
    if (!dumped) {
        dumped = 1;
        radio_dump_stations(mm);
    }
    int slot = ((int (*)(void *, int))(uintptr_t)RADIO_PICK_FN)(mm, station);
    int base = *(int *)(mm + 0x250);
    gmv_trace("[radio] pick: station %d -> slot %d \"%s\" (base+slot \"%s\"); HUD logo %d %s; caller=%p\n",
            station, slot, radio_name(slot), radio_name(base + slot), g_radio_logo,
            radio_logo_name(g_radio_logo), __builtin_return_address(0));
    return slot;
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_radio_tune(unsigned char *mm, int station)
{
    int old = *(int *)(mm + 0x21c);
    uint32_t flags = *(uint32_t *)(mm + 0x220);
    ((void (*)(void *, int))(uintptr_t)RADIO_TUNE_FN)(mm, station);
    gmv_log("[radio] tune: station %d -> %d (flags 0x%x, now %d) caller=%p\n", old, station,
            flags, *(int *)(mm + 0x21c), __builtin_return_address(0));
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_radio_start(unsigned char *mm, int station)
{
    int old = *(int *)(mm + 0x21c);
    uint32_t flags = *(uint32_t *)(mm + 0x220);
    ((void (*)(void *, int))(uintptr_t)RADIO_START_FN)(mm, station);
    if (GMV_DEV)                    /* the trace is never installed in a release; radio_logo_set knows the address */
        gmv_log("[radio] start(%d): flags 0x%x -> 0x%x, station %d -> %d, slot %d%s caller=%p\n", station,
            flags, *(uint32_t *)(mm + 0x220), old, *(int *)(mm + 0x21c), *(int *)(mm + 0x218),
            (flags & 0x11) ? " (radio already on: ignored)" : "", __builtin_return_address(0));
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_radio_logo(void *widget, int station)
{
    ((void (*)(void *, int))(uintptr_t)RADIO_LOGO_FN)(widget, station);
    void *ret = __builtin_return_address(0);
    int show = (uint32_t)(uintptr_t)ret == RADIO_SHOW_RET;
    if (show || station != g_radio_logo)
        gmv_log("[radio] HUD logo: station %d %s (%s)\n", station, radio_logo_name(station),
                show ? "Radio::show" : "refresh");
    g_radio_logo = station;
}

/* Point every `call fn` in sites[] at hook; checks them all first, so it patches all or none. */
static int radio_redirect(const uint32_t *sites, size_t n, uint32_t fn, void *hook)
{
    for (size_t i = 0; i < n; i++) {
        const unsigned char *p = (const unsigned char *)(uintptr_t)sites[i];
        int32_t rel;
        memcpy(&rel, p + 1, 4);
        if (p[0] != 0xe8 || sites[i] + 5 + (uint32_t)rel != fn) {
            gmv_log("[gamepatch] 0x%08x is not the expected call to 0x%08x; radio trace skipped\n", sites[i], fn);
            return 0;
        }
    }
    for (size_t i = 0; i < n; i++) {
        int32_t rel = (int32_t)((uint32_t)(uintptr_t)hook - (sites[i] + 5));
        if (!patch_bytes(sites[i] + 1, (const unsigned char *)&rel, 4)) {
            gmv_log("[gamepatch] write at 0x%08x failed (%lu); radio trace partly installed\n",
                    sites[i] + 1, GetLastError());
            return 0;
        }
    }
    return 1;
}

void apply_radio_trace(void)
{
    const char *e = gmv_dbgenv("GMV_RADIO_TRACE");
    if (!(e && *e && *e != '0'))
        return;
#define RADIO_N(a) (sizeof (a) / sizeof *(a))
    if (radio_redirect(radio_pick_sites, RADIO_N(radio_pick_sites), RADIO_PICK_FN, (void *)gmv_radio_pick) &&
        radio_redirect(radio_tune_sites, RADIO_N(radio_tune_sites), RADIO_TUNE_FN, (void *)gmv_radio_tune) &&
        radio_redirect(radio_start_sites, RADIO_N(radio_start_sites), RADIO_START_FN, (void *)gmv_radio_start) &&
        radio_redirect(radio_logo_sites, RADIO_N(radio_logo_sites), RADIO_LOGO_FN, (void *)gmv_radio_logo))
        gmv_log("[gamepatch] radio trace installed: [radio] logs the station table, radio start, station "
                "switches, each pick and the HUD logo\n");
#undef RADIO_N
}

/* ---- the HUD radio logo after getting into a car (original game, changed at the user's request) ----

   Getting into a car switches the HUD first, and the radio refresh 0x9323a sets the logo from
   [mm+0x21c] then.  Only afterwards does the car code 0x10a57c start the radio with the car's own
   station (0x60190; [car+0x120] the first time, [car+0xc8] after that), and nothing refreshes the
   logo again.  So the HUD shows the station that played last (104FM before the radio has ever been
   on) while the car's station plays.  GMV_RADIO_TRACE caught it: logo 0, then start(4) playing
   m_radio_rock_08.  The Mac runs the same code.

   The two moments are apart by the whole get-in animation: the player's enter-vehicle step
   0xf89fe refreshes the logo (through 0x28564 and again inline at 0xf8ab7) as the player starts
   getting in, and the car's radio starts once they are seated, so the wrong logo was on screen
   for about a second even with the first version of this fix.

   Fix, two parts:
   1. The last call of 0xf89fe, 0xf6032(character, 1, 0) at 0xf8aef, goes to gmv_radio_enter.
      Before making it, for the player only (0x617b2(character+4)), it works out the station the
      car is about to start, the way 0x10a57c and 0x60190 will: nothing if the byte at 0x647ed0 is
      clear (0x10a57c then starts no radio) or [mm+0x220] has 0x11 (0x60190 then returns at once);
      otherwise [car+0x183] bit 0 ? [car+0xc8] : [car+0x120], with -1 meaning [mm+0x21c].  The car
      is [character+0x55c], the vehicle 0xf89fe hands the HUD.  It shows that logo the way 0x9323a
      does: unless [mm+0x220] has 0x20, and only once the HUD has its radio widget
      ([app+0x218] > 0x34, widget [[app+0x214]+0xd0]), 0x17357e(widget, station).  This is the
      same game tick as the game's refresh, so the old logo is never drawn.
   2. The two car-entry calls go to gmv_radio_entry, which makes the call and then shows the logo
      of the station that actually started, in case part 1 guessed wrong or did not run.
   Default on; GMV_RADIO_LOGO_FIX=0 or ORIGINAL_MAC_EXPERIENCE=1 keeps the original. */
#define RADIO_ON_FLAG    0x00647ed0u   /* byte: 0x10a57c starts the car radio only when set       */
#define RADIO_IS_PLAYER  0x000617b2u   /* (character+4) -> 1 if that character is the player      */
#define RADIO_ENTER_SITE 0x000f8aefu   /* last call of the enter-vehicle step 0xf89fe              */
#define RADIO_ENTER_FN   0x000f6032u   /* what it calls: (character, 1, 0)                        */

static const uint32_t radio_entry_sites[] = { 0x0010a5f5u, 0x0010a65au };
static uint32_t g_radio_entry_next;  /* what those calls reached before: 0x60190, or the trace hook */
static int g_radio_predicted = -1;   /* station part 1 showed for the car being entered */

/* Show station's logo the way the refresh 0x9323a does.  Returns the logo shown before, or -2 if
   the HUD has no radio logo to set yet. */
static int radio_logo_show(const unsigned char *mm, int station)
{
    unsigned char *app = ((unsigned char *(*)(void))(uintptr_t)RADIO_APP_FN)();
    if (!app || *(int *)(app + 0x218) <= 0x34 || (*(const uint32_t *)(mm + 0x220) & 0x20))
        return -2;
    unsigned char *hud = *(unsigned char **)(app + 0x214);
    unsigned char *w = hud ? *(unsigned char **)(hud + 0xd0) : NULL;
    if (!w)
        return -2;
    int old = *(int *)(w + 0x14);
    ((void (*)(void *, int))(uintptr_t)RADIO_LOGO_FN)(w, station);
    g_radio_logo = station;
    return old;
}

__attribute__((cdecl, force_align_arg_pointer))
static int gmv_radio_enter(unsigned char *ch, int a, int b)
{
    unsigned char *car = *(unsigned char **)(ch + 0x55c);
    unsigned char **mmp = *(unsigned char ***)(uintptr_t)RADIO_MM_PP;
    unsigned char *mm = mmp ? *mmp : NULL;
    if (car && mm && *(const unsigned char *)(uintptr_t)RADIO_ON_FLAG &&
        !(*(uint32_t *)(mm + 0x220) & 0x11) && ((int (*)(void *))(uintptr_t)RADIO_IS_PLAYER)(ch + 4)) {
        int station = (car[0x183] & 1) ? *(int *)(car + 0xc8) : *(int *)(car + 0x120);
        if (station == -1)
            station = *(int *)(mm + 0x21c);
        int old = radio_logo_show(mm, station);
        if (old != -2) {
            g_radio_predicted = station;
            if (old != station)
                gmv_trace("[radiologo] getting into a car with station %d: HUD logo %s -> %s\n", station,
                        radio_logo_name(old), radio_logo_name(station));
        }
    }
    return ((int (*)(void *, int, int))(uintptr_t)RADIO_ENTER_FN)(ch, a, b);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_radio_entry(unsigned char *mm, int station)
{
    ((void (*)(void *, int))(uintptr_t)g_radio_entry_next)(mm, station);
    int now = *(int *)(mm + 0x21c), old = radio_logo_show(mm, now);
    if (g_radio_predicted >= 0 && g_radio_predicted != now)
        gmv_trace("[radiologo] the car radio started station %d, not the %d shown on getting in\n", now,
                g_radio_predicted);
    else if (old != -2 && old != now)
        gmv_trace("[radiologo] car radio on station %d: HUD logo %s -> %s\n", now, radio_logo_name(old),
                radio_logo_name(now));
    g_radio_predicted = -1;
}

/* 1 = the car-entry calls go to gmv_radio_entry and the enter step's to gmv_radio_enter, 0 = where
   they went before; -1 if a site is none of those, -2 if a write failed. */
int radio_logo_set(int on)
{
    const uint32_t entry = (uint32_t)(uintptr_t)gmv_radio_entry, enter = (uint32_t)(uintptr_t)gmv_radio_enter;
    const uint32_t trace = (uint32_t)(uintptr_t)gmv_radio_start;
    uint32_t next = g_radio_entry_next;
    if (!sm_is_call(RADIO_ENTER_SITE, RADIO_ENTER_FN) && !sm_is_call(RADIO_ENTER_SITE, enter))
        return -1;
    for (size_t i = 0; i < sizeof radio_entry_sites / sizeof *radio_entry_sites; i++) {
        uint32_t s = radio_entry_sites[i];
        if (next && sm_is_call(s, entry))
            continue;
        uint32_t to = sm_is_call(s, RADIO_START_FN) ? RADIO_START_FN : sm_is_call(s, trace) ? trace : 0;
        if (!to || (next && to != next))
            return -1;
        next = to;
    }
    g_radio_entry_next = next;
    for (size_t i = 0; i < sizeof radio_entry_sites / sizeof *radio_entry_sites; i++) {
        int32_t rel = (int32_t)((on ? entry : next) - (radio_entry_sites[i] + 5));
        if (!patch_bytes(radio_entry_sites[i] + 1, (const unsigned char *)&rel, 4))
            return -2;
    }
    int32_t qrel = (int32_t)((on ? enter : RADIO_ENTER_FN) - (RADIO_ENTER_SITE + 5));
    if (!patch_bytes(RADIO_ENTER_SITE + 1, (const unsigned char *)&qrel, 4))
        return -2;
    g_radio_predicted = -1;
    return on;
}

void apply_radio_logo_fix(void)
{
    const char *e = gmv_getenv("GMV_RADIO_LOGO_FIX");
    if (e && *e == '0')
        return;
    int r = radio_logo_set(1);
    if (r == -1)
        gmv_log("[gamepatch] the car-entry calls (0x%08x, 0x%08x, 0x%08x) are not the expected ones; radio logo fix "
                "skipped\n", RADIO_ENTER_SITE, radio_entry_sites[0], radio_entry_sites[1]);
    else if (r == -2)
        gmv_log("[gamepatch] a write failed (%lu); radio logo fix partly applied\n", GetLastError());
    else
        gmv_log("[gamepatch] HUD radio logo shows the car's station as you get in (RADIO_LOGO_FIX=0 to disable)\n");
}
