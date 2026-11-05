/* tab.c -- GAMEPATCH_TAB: the port's settings as a tab of the options menu */
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

/* ---- GAMEPATCH_TAB: the port's settings as a tab of the game's options menu (an addition, on by default) ----

   The menus are gameswf states of the menu file newmenu.fua.  The options screen (Menus::Options,
   vtable 0x6585e0) keeps its tabs in widgetIGMenu.groupBoutons, seven text buttons btnMenu0-6
   shared with the pause menu: slot i of the tab widget is {string id, enabled} at +0x14 + 8i
   (0x172f0e), and its label loop (0x171a08) sets btnMenu<i>.text from the string id through the
   string table (0x8f426).  Options::show (0x166fe2) labels 0-3 and hides 5 and 6.  A click stores
   the button's number at +0x58 and calls the screen's onEvent (0x167192), whose jump table
   switches to OptionVolumes, OptionKeyBoard, OptionGameSettings or OptionVideo for 0-3 and
   ignores anything above 3.
   A page's show starts with 0x163888, which hides the shown widgets it does not list (widget
   slot 2), keeps the ones it lists (slot 3) and shows the rest of its list (slot 1); the events
   go to the listed widgets' slot 5, and slot 6 sets their texts.  The CONTROLS page
   (OptionKeyBoard) lists the key bindings with widgetActionKeyTitle (index 0x22: ◀ category ▶ over
   two column titles), widgetControlHelps (0x24: six lines wCusLine1-6 of a name and two key
   boxes, and an OK button; 0x17fcca fills them from the key map on each event) and widgetArrows
   (0x12) when there are more than six.
   With GAMEPATCH_TAB=1 our page serves two tabs (om_tabs): VIDEO (slot 3, the game's own label),
   in place of the game's VIDEO page, whose resolution and full screen it holds, with the DISPLAY
   and GRAPHICS categories; and GAMEPATCH (slot 6) with the rest.  Under ORIGINAL_MAC_EXPERIENCE
   (om_mac, which the switch changes in game) only GAMEPATCH, with only that switch, beside the
   game's VIDEO.  Slot 6, not 5: the menu file's
   frame Show4 places btnMenu6 right under btnMenu3 and btnMenu5 a step lower (frame Show5's
   picture button btnMenu4 takes btnMenu6's place).
   - Options::show's hide of btnMenu6 (0x16715e) goes to gmv_om_tab6, which gives slot 6 the
     string id OM_TAB_ID; the label loop's string lookup (0x171b07) answers that id with GAMEPATCH;
   - the read of the clicked number (0x167208) goes to gmv_om_clicked, which for our slots switches
     to the state OptionGamepatch (or, from our other tab, to that tab's categories in place) and
     answers 4, past the jump table;
   - the menu set-up's registration of OptionVideo (0x1609c4) also registers OptionGamepatch: a
     Game page (constructor 0x16c044) renamed, listing the category and list widgets in place of
     its row and language widgets, with our show, hide and onEvent in a copy of its vtable.
   From that registration on, the category and list widgets run our slots from copies of their
   vtables, which do the CONTROLS page's own unless ours is shown (om_active); a CONTROLS page that
   keeps the list from ours gets it shown afresh.  Each line's value is a ◀ value ▶ like the
   category's, added to the menu file as it loads (see om_swf_patch); its arrows step the value.
   The values wait for APPLY (the list's OK) or for leaving the page, which save them to
   GangstarMV.ini and put in force what can be at once (OM_LIVE, each with its setter): the
   anti-aliasing, texture filtering, the fps limiter, vsync, the fog, bloom and ambient occlusion
   (the GL shim takes them after the next present), the sun and moon, SPAWN_FADE, the draw distance,
   the densities, ALLOW_WEAPON_SPRINT, MINIMAP_SHOPS, the fixes and the main menu's two buttons,
   ORIGINAL_MAC_EXPERIENCE (om_set_mac: all of these to the Mac's values or back, and the tabs laid
   out again), and the resolution and full screen, which are the game's own (the VIDEO page's
   0x3850, 0x19b920 and 0x3a24).  A live one its setter refuses gets a * and the list's foot says
   it takes effect at the next start; so do the OM_RESTART ones (NO_INTRO, SHORT_WARNING, FAST_LOAD), which are over by
   the time the menu is up and are only saved.  An add-on's line whose add-on was not there at the start gets
   the * too, and the foot says it needs its add-on, as a new start alone would not help.  If
   the menu file is not the one expected, the lines keep the key boxes: a click on the first steps
   the value up, on the second down, and the second says NEXT START. */
#define OM_LINE_TEXT   0x0015c0f8u   /* (manager, path, text, 1, 0): how the list sets its texts   */
#define OM_EVENT_PATH  0x00180878u   /* RenderFX::Event::GetCharacterPath(event)                   */
#define OM_LABEL_SITE  0x00171b07u   /* the tab label loop's call of it                            */
#define OM_HIDE6_SITE  0x0016715eu   /* Options::show: set-visible(btnMenu6, 0)                    */
#define OM_SLOT        6
#define OM_CLICK_SITE  0x00167208u   /* Options::onEvent: call 0x16eff4, the clicked number        */
#define OM_TABLE_CMP   0x0016720du   /* cmp eax,3; ja: the jump table's bound                      */
#define OM_REG_SITE    0x001609c4u   /* menu set-up: register(menufx, OptionVideo, "dummy")        */
#define OM_MGR_FN      0x0015e4fau   /* the menu manager: widgets at +0x214, count +0x218          */
#define OM_PAGES       0x0016f698u   /* (pager, count, index, force); the index is at +0x18        */
#define OM_OPT_SHOW    0x00166fe2u
#define OM_OPT_EVENT   0x00167192u
#define OM_GS_CTOR     0x0016c044u
#define OM_GS_BYTES    0x90u
#define OM_GS_VT       0x00658ca8u
#define OM_GS_HIDE     0x0016990au   /* lets go of the tabs and the pager                          */
#define OM_SOUND_PP    0x0063e030u   /* -> the sound player's pointer                              */
#define OM_SOUND_FN    0x0005fd5au   /* (player, sound, 0, 0, 0): 0x5a tab, 0x56 step, 0x57 OK      */
#define OM_LOCK        0x0068a544u   /* while set the Game page leaves ◀ ▶ alone                    */
#define OM_LIST_VT     0x00658fa8u   /* widgetControlHelps                                         */
#define OM_LIST_SLOTS  10
#define OM_LIST_SHOW   0x00170c04u   /* also pages the key map and listens to the keyboard         */
#define OM_LIST_HIDE   0x00170d68u
#define OM_LIST_EVENT  0x00170df6u
#define OM_LIST_TEXTS  0x00171430u
#define OM_LIST_RESET  0x0017f4a6u   /* every line back to a name and two boxes, no red edit bar   */
#define OM_APP_FN      0x000031b0u
#define OM_RES_NOW     0x00003850u   /* (app): the resolution in use, an index                      */
#define OM_RES_SET     0x00003a24u   /* (app, {index, full screen}, full screen)                    */
#define OM_RES_COUNT   0x17afc
#define OM_FULL_NOW    0x0019b920u
#define OM_W_LANGUAGE  9
#define OM_W_ROW       0x1d
#define OM_W_LIST      0x24
#define OM_TAB_ID      0x7f00
#define OM_STATE       "OptionGamepatch"

enum { OM_RESTART, OM_LIVE, OM_RESOLUTION, OM_FULL_SCREEN };

#define OM_VALS 12                     /* values of one item at most */

typedef struct {
    const char *key, *title;           /* key: the ini's; none for the game's own settings */
    const char *blank, *unit;          /* what a blank value means; after a value not in the list */
    int live;
    const char *val[OM_VALS], *name[OM_VALS];
    int (*set)(const char *key, double v);   /* OM_LIVE: put v in force; 0 if the running game cannot */
} om_item;

static int om_set_radio_songs(const char *key, double v);
static int om_set_uc(const char *key, double v);

static int om_set_msaa(const char *key, double v)
{
    (void)key;
    return gmv_set_msaa((int)v);
}

static int om_set_aniso(const char *key, double v)
{
    (void)key;
    return gmv_set_aniso((float)v);
}

/* The pacer, and the syncs that depend on its rate. */
static int om_set_frame_ms(const char *key, double v)
{
    (void)key;
    if (fabs(v - 1000.0 / 60.0) < 0.002)
        v = 1000.0 / 60.0;
    gmv_set_frame_ms(v);
    physics_sync_set(v);
    cam_sync_set(v);
    walk_sync_set(v);
    ai_sync_set(v);
    return 1;
}

static int om_set_vsync(const char *key, double v)
{
    (void)key;
    return gmv_set_swap_interval((int)v);
}

static int om_set_fix(const char *key, double v)
{
    int on = v != 0;
    if (!strcmp(key, "MENUFLASH_FIX")) {
        gmv_set_menuflash_fix(on);
        return 1;
    }
    return !strcmp(key, "LOAD_SLEEP_SKIP") ? load_sleep_skip_set(on) == on
         : !strcmp(key, "MENU_FADE_CLICKS") ? menu_fade_clicks_set(on) == on
         : !strcmp(key, "LOAD_BUDGET") ? load_budget_set((int)v) == (int)v
         : !strcmp(key, "RADIO_LOGO_FIX") ? radio_logo_set(on) == on
         : !strcmp(key, "STOPLOCK_FIX") ? stoplock_set(on) == on
         : !strcmp(key, "FADE_EDGE_FIX") ? fade_edge_set(on) == on
         : !strcmp(key, "HIDE_MORE_GAMES") ? hide_main_menu_set(on, mg_live) == 1
         : !strcmp(key, "HIDE_GAMELOFT_LIVE") ? hide_main_menu_set(mg_more, on) == 1
         : !strcmp(key, "CUTSCENE_KEYS") ? cs_set(on)
         : !strcmp(key, "TUTORIAL_KEYS") ? tk_set(on)
         : !strcmp(key, "PAD_LAYOUT") ? pl_set(on)
         : !strcmp(key, "PAD_MENU") ? pm_set(on)
         : !strcmp(key, "PAD_HINTS") ? ph_set(on)
         : !strcmp(key, "PAD_ICONS") ? gmv_padicons_on()        /* (gmv_padicons_sync, after the next present) */
         : !strcmp(key, "KEY_MOVE") ? km_set(on)
         : !strcmp(key, "SWITCH_SAVE_FIX") ? ssf_set(on) : !strcmp(key, "PLAYERSWITCH_INVENTORY") ? si_set(on)
         : !strcmp(key, "LC_SWITCH") ? lc_set(on) : 0;
}

/* The sun and moon here; the post effects and the fog at the next present (the GL shim). */
static int om_set_graphics(const char *key, double v)
{
    if (!strcmp(key, "SUN_MOON"))
        return sun_moon_set((int)v) == (int)v;
    return gmv_set_post(key, (float)v);
}

static int om_set_dd(const char *key, double v)
{
    (void)key;
    return dd_set(v);
}

static int om_set_mac(const char *key, double v);

#define OM_OFF_ON {"0", "1"}, {"OFF", "ON"}
#define OM_DENSITIES {"0", "0.5", "1", "1.5", "2", "2.5", "3", "3.5", "4"}, \
                     {"NONE", "0.5X", "1X (MAC)", "1.5X", "2X", "2.5X", "3X", "3.5X", "4X"}

static const om_item om_items[] = {
    {NULL, "RESOLUTION", "", "", OM_RESOLUTION, {NULL}, {NULL}, NULL},
    {NULL, "FULL SCREEN", "", "", OM_FULL_SCREEN, OM_OFF_ON, NULL},
    /* Blank: off while the pacer runs (pace_frame_ms). On waits for the screen's refresh at each
       drawn frame, so it caps the FPS LIMITER at the refresh rate. */
    {"SWAP_INTERVAL", "VSYNC", "0", "", OM_LIVE, OM_OFF_ON, om_set_vsync},
    {"MSAA", "ANTI-ALIASING", "4", "X", OM_LIVE, {"0", "2", "4", "8"}, {"OFF", "2X", "4X (MAC)", "8X"}, om_set_msaa},
    {"ANISOTROPIC_FILTERING", "TEXTURE FILTERING", "0", "X", OM_LIVE,
     {"0", "2", "4", "8", "16"}, {"MAC", "2X", "4X", "8X", "16X"}, om_set_aniso},
    /* The game draws every other tick, so a drawn rate of F is a tick of 500/F ms; all within
       PHYSICS_SYNC's 1.2-40 ms. */
    {"FRAME_MS", "FPS LIMITER", "16.667", " MS", OM_LIVE,
     {"20", "", "8.3333", "6.6667", "5.5556", "4.1667", "3.4722", "3.0303", "2.0833", "1.3889", "1.25"},
     {"25", "30 (MAC)", "60", "75", "90", "120", "144", "165", "240", "360", "400"}, om_set_frame_ms},
    {"FOG_MODE", "FOG", "0", "", OM_LIVE, {"0", "1", "2"}, {"MAC", "LINEAR", "HYBRID"}, om_set_graphics},
    {"DRAW_DISTANCE", "DRAW DISTANCE", "1", "X", OM_LIVE,
     {"1", "1.5", "2", "2.5", "3", "4"}, {"1X (MAC)", "1.5X", "2X", "2.5X", "3X", "4X"}, om_set_dd},
    {"TRAFFIC_DENSITY", "TRAFFIC", "1", "X", OM_LIVE, OM_DENSITIES, om_density},
    {"PEDESTRIAN_DENSITY", "PEDESTRIANS", "1", "X", OM_LIVE, OM_DENSITIES, om_density},
    {"PARKED_CAR_DENSITY", "PARKED CARS", "1", "X", OM_LIVE, OM_DENSITIES, om_density},
    {"AMBIENT_OCCLUSION", "AMBIENT OCCLUSION", "0", "", OM_LIVE, OM_OFF_ON, om_set_graphics},
    /* HIGH adds the lights and the lit windows, at BLOOM_LIGHTS / BLOOM_WINDOWS (blank = 1). */
    {"BLOOM", "BLOOM", "0", "", OM_LIVE, {"0", "1", "2"}, {"OFF", "LOW", "HIGH"}, om_set_graphics},
    /* The brightness a pixel must reach to glow: lower makes more of the picture glow (user's values). */
    {"BLOOM_THRESHOLD", "BLOOM INTENSITY", "0.75", "", OM_LIVE, {"0.75", "0.5"}, {"LOW", "HIGH"}, om_set_graphics},
    /* MAC / ENHANCED (the user's names): the Mac draws the sea and the cars with no reflections. */
    {"WATER_REFLECTIONS", "WATER REFLECTIONS", "0", "", OM_LIVE, {"0", "1"}, {"MAC", "ENHANCED"}, om_set_graphics},
    {"VEHICLE_REFLECTIONS", "CAR REFLECTIONS", "0", "", OM_LIVE, {"0", "1"}, {"MAC", "ENHANCED"}, om_set_graphics},
    {"SUN_MOON", "SUN AND MOON", "0", "", OM_LIVE, {"0", "2", "1"}, {"MAC", "IPHONE", "ENHANCED"}, om_set_graphics},
    {"DYNAMIC_SHADOWS", "DYNAMIC SHADOWS", "0", "", OM_LIVE, {"0", "1"}, {"MAC", "ENHANCED"}, om_set_graphics},
    {"SPAWN_FADE", "FADE IN TRAFFIC", "0", "", OM_LIVE, OM_OFF_ON, om_set_spawn_fade},
    {"ORIGINAL_MAC_EXPERIENCE", "ORIGINAL MAC EXPERIENCE", "0", "", OM_LIVE, OM_OFF_ON, om_set_mac},
    {"ALLOW_WEAPON_SPRINT", "WEAPON WHILE SPRINTING", "0", "", OM_LIVE, OM_OFF_ON, om_set_sprint},
    {"MINIMAP_SHOPS", "SHOPS ON MINIMAP", "0", "", OM_LIVE, OM_OFF_ON, om_set_minimap},
    {"MISSION_REPLAY", "MISSION REPLAY", "0", "", OM_LIVE, OM_OFF_ON, om_set_replay},
    {"PAD_LAYOUT", "PAD LAYOUT", "0", "", OM_LIVE, {"0", "1"}, {"MAC", "GTA-LIKE"}, om_set_fix},
    {"PAD_MENU", "PAD MENU POINTER", "0", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"PAD_HINTS", "PAD BUTTON HINTS", "0", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"PAD_ICONS", "PAD BUTTON ICONS", "0", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"KEY_MOVE", "KEYBOARD MOVEMENT", "0", "", OM_LIVE, {"0", "1"}, {"MAC", "ENHANCED"}, om_set_fix},
    {"PLAYERSWITCH_INVENTORY", "PLAYERSWITCH INVENTORY", "0", "", OM_LIVE, {"0", "1"}, {"MAC", "SHARED"}, om_set_fix},
    {"LC_SWITCH", "PLAY AS L.C.", "0", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    /* These two are read in with the game's lists at every start when the add-on is there, whatever the key says. */
    {"UC_CRUSHER", "CRUSHER MONSTER TRUCK", "0", "", OM_LIVE, OM_OFF_ON, om_set_uc},
    {"RADIO_SONGS", "RESTORED RADIO SONGS", "0", "", OM_LIVE, OM_OFF_ON, om_set_radio_songs},
    {"LOAD_SLEEP_SKIP", "NO SLEEP WHILE STREAMING", "1", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"MENU_FADE_CLICKS", "MENU CLICKS DURING FADES", "1", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"LOAD_BUDGET", "SMOOTH CITY LOADING", "200", "", OM_LIVE, {"0", "200"}, {"OFF", "ON"}, om_set_fix},
    {"RADIO_LOGO_FIX", "RADIO LOGO FIX", "1", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"STOPLOCK_FIX", "GAS AND BRAKE FIX", "1", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"MENUFLASH_FIX", "MENU FLASH FIX", "1", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"FADE_EDGE_FIX", "FADE EDGE FIX", "1", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"SWITCH_SAVE_FIX", "PLAYERSWITCH SAVE FIX", "1", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"HIDE_MORE_GAMES", "HIDE MORE GAMES", "0", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"HIDE_GAMELOFT_LIVE", "HIDE GAMELOFT LIVE", "0", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"NO_INTRO", "SKIP INTRO MOVIE", "0", "", OM_RESTART, OM_OFF_ON, NULL},
    /* The warning is over by the time the menu is up: saved now, in force from the next start. */
    {"SHORT_WARNING", "SHORT START-UP WARNING", "0", "", OM_RESTART, OM_OFF_ON, NULL},
    {"FAST_LOAD", "FAST START-UP LOADING", "1", "", OM_RESTART, OM_OFF_ON, NULL},
    {"CUTSCENE_KEYS", "HIDE CUTSCENE KEYS", "0", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"TUTORIAL_KEYS", "POPUP KEYS", "0", "", OM_LIVE, OM_OFF_ON, om_set_fix},
};
#define OM_N (sizeof om_items / sizeof *om_items)
#define OM_K_RES  0
#define OM_K_FULL 1
#define OM_K_MAC  19

/* A run of om_items. page1: the lines on its first page when fewer than OM_LINES (0 = OM_LINES). */
typedef struct { const char *name; int first, n, page1; } om_category;

/* More than OM_LINES in a category: the pager shows them a page at a time. GRAPHICS: fog, the
   world's settings and AO on page 1, the other effects on page 2 (the user's layout). */
static const om_category om_cats[] = {
    {"DISPLAY", 0, 6, 0}, {"GRAPHICS", 6, 13, 0}, {"FIXES", 19, 21, 0}, {"MENU", 40, 7, 0},
};
#define OM_CATS (int)(sizeof om_cats / sizeof *om_cats)
static const om_category om_mac_cat = {"PRESET", OM_K_MAC, 1, 0};

typedef struct { int slot, first, n; } om_tab;   /* a tab of the options screen: a run of om_cats */

static const om_tab om_tabs[] = { {3, 0, 2}, {OM_SLOT, 2, 2} };   /* VIDEO, GAMEPATCH */
#define OM_TABS (int)(sizeof om_tabs / sizeof *om_tabs)

static uint32_t om_vt[OM_VT_N];
static uint32_t om_cat_vt[2 + OM_CAT_SLOTS], om_list_vt[2 + OM_LIST_SLOTS];   /* with their RTTI words */
static double om_now[OM_N];        /* in force in this run */
static int om_sel[OM_N];           /* the value shown: an index into val, -1 for the file's own */
static int om_later[OM_N];         /* saved, but the running game could not take it */
static int om_res_now, om_full_now;
static int om_mac;                 /* ORIGINAL_MAC_EXPERIENCE: only its switch, in OM_SLOT */
static int om_tab_now;             /* the tab shown */
static int om_tab_cat[OM_TABS];    /* each tab's category shown */
static int om_active;              /* our page is shown */
static int om_list_keys;           /* the list's own show ran: it listens to the keyboard */
static int om_list_ours;           /* the list shows our lines */
int om_tab_files;           /* the options tab patches the menu file too */
int om_credit;              /* the header has the credit (shown on our page only) */
int om_selectors;           /* the menu file's lines have our ◀ value ▶ */
static int om_foot;                /* the list's foot says what takes effect at the next start */
void *(*om_opener)(const char *);   /* the game's gameswf file opener */
const char *const om_sel_names[4] = {"wValueBar", "btnValueLeft", "btnValueRight", "textValue"};

static double om_num(const om_item *it, const char *s)
{
    if (s && *s)
        return atof(s);
    return atof(it->blank);
}

static int om_count(const om_item *it)
{
    int n = 0;
    while (n < OM_VALS && it->val[n])
        n++;
    return n;
}

static int om_index(const om_item *it, double v)
{
    for (int i = 0; i < om_count(it); i++)
        if (fabs(om_num(it, it->val[i]) - v) < 0.002)
            return i;
    return -1;
}

static double om_saved(const om_item *it)
{
    return om_num(it, gmv_config_file_value(it->key));
}

static double om_shown(int k)
{
    const om_item *it = &om_items[k];
    return om_sel[k] >= 0 ? om_num(it, it->val[om_sel[k]]) : om_saved(it);
}

void *om_mgr(void)
{
    return ((void *(*)(void))(uintptr_t)OM_MGR_FN)();
}

unsigned char *om_widget(int i)
{
    unsigned char *m = om_mgr();
    return m && *(int *)(m + 0x218) > i ? (*(unsigned char ***)(m + 0x214))[i] : NULL;
}

void *om_app(void)
{
    return ((void *(*)(void))(uintptr_t)OM_APP_FN)();
}

void om_sound(int id)
{
    void **player = *(void ***)(uintptr_t)OM_SOUND_PP;
    if (player && *player)
        ((void (*)(void *, int, int, int, int))(uintptr_t)OM_SOUND_FN)(*player, id, 0, 0, 0);
}

void om_visible(void *fx, const char *path, int on)
{
    ((void (*)(void *, const char *, int))(uintptr_t)OM_SET_VISIBLE)(fx, path, on);
}

void om_text(void *fx, const char *path, const char *text)
{
    ((void (*)(void *, const char *, const char *, int))(uintptr_t)OM_SET_TEXT)(fx, path, text, 1);
}

static void om_line_text(int line, const char *field, const char *text)
{
    char path[96];
    snprintf(path, sizeof path, "widgetControlHelps.wCusLine%d.%s", line, field);
    ((void (*)(void *, const char *, const char *, int, int))(uintptr_t)OM_LINE_TEXT)(om_mgr(), path, text, 1, 0);
}

void om_line_visible(void *fx, int line, const char *field, int on)
{
    char path[96];
    snprintf(path, sizeof path, "widgetControlHelps.wCusLine%d.%s", line, field);
    om_visible(fx, path, on);
}

/* UC_CRUSHER: its files were read at the start if the add-on is there.  On, its hooks; gmv_uc_tick parks it, and
   takes it out of the city and then its hooks when the key goes off.  0 without the add-on. */
static int om_set_uc(const char *key, double v)
{
    (void)key;
    if (!uc_loaded)
        return 0;
    if (v != 0)
        uc_hooks_in();
    return 1;
}

/* RADIO_SONGS (radiosongs.c): the new songs come after each station's own, so its count alone leaves them out.
   A station's bag is emptied so that it fills again from the list at its next pick (0x953de).  One of them
   playing when they go: on to the next, with the tuning noise.  0 without the add-on; before the game has
   read its lists, rs_tick sees to it. */
static int om_set_radio_songs(const char *key, double v)
{
    (void)key;
    unsigned char **mmp = *(unsigned char ***)(uintptr_t)RADIO_MM_PP;
    unsigned char *mm = mmp ? *mmp : NULL;
    int mac, all;
    if (!gmv_rs_have())
        return 0;
    if (!mm || !gmv_rs_counts(0, &mac, &all))
        return 1;
    int on = v != 0, n = *(int *)(mm + 0x254 + 0x1c);
    unsigned char **st = *(unsigned char ***)(mm + 0x254 + 0x18), **bag = *(unsigned char ***)(mm + 0x254 + 0x14);
    while (mm[0x275])                           /* as 0x5fee8: not while the update is at it */
        Sleep(0);
    for (int i = 0; i < n; i++)
        if (gmv_rs_counts(i, &mac, &all)) {
            *(int *)(st[i] + 8) = on ? all : mac;
            *(int *)(bag[i] + 4) = 0;
        }
    if (!on && (*(uint32_t *)(mm + 0x220) & 1) && gmv_rs_is_song(*(int *)(mm + 0x218)))
        ((void (*)(void *, int))(uintptr_t)RADIO_TUNE_FN)(mm, *(int *)(mm + 0x21c));
    gmv_log("[radio songs] %s\n", on ? "on" : "off: the Mac's songs");
    return 1;
}

/* Each present: the game reads the stations with every song in, so a key that is off (or a preset) is put in
   force as soon as they are all there, and again should they ever be read anew. */
void rs_tick(void)
{
    int mac, all;
    if (!gmv_rs_counts(0, &mac, &all))
        return;
    unsigned char **mmp = *(unsigned char ***)(uintptr_t)RADIO_MM_PP;
    unsigned char *mm = mmp ? *mmp : NULL;
    if (!mm)
        return;
    int n = *(int *)(mm + 0x254 + 0x1c), on = gmv_rs_on(), differs = 0;
    unsigned char **st = *(unsigned char ***)(mm + 0x254 + 0x18), **bag = *(unsigned char ***)(mm + 0x254 + 0x14);
    if (n <= 0 || !st || !bag)
        return;
    for (int i = 0; i < n; i++) {
        if (!st[i] || !bag[i])
            return;
        if (gmv_rs_counts(i, &mac, &all))
            differs |= *(int *)(st[i] + 8) != (on ? all : mac);
    }
    if (differs)
        om_set_radio_songs(NULL, on);
}

static const om_category *om_category_now(void)
{
    return om_mac ? &om_mac_cat : &om_cats[om_tab_cat[om_tab_now]];
}

static int om_slot(void)
{
    return om_mac ? OM_SLOT : om_tabs[om_tab_now].slot;
}

/* The values as the ini and the game have them now. */
static void om_load(void)
{
    om_res_now = ((int (*)(void *))(uintptr_t)OM_RES_NOW)(om_app());
    om_full_now = (((unsigned (*)(void))(uintptr_t)OM_FULL_NOW)() & 0xff) != 0;
    for (size_t k = 0; k < OM_N; k++)
        om_sel[k] = k == OM_K_RES ? om_res_now : k == OM_K_FULL ? om_full_now
                                              : om_index(&om_items[k], om_saved(&om_items[k]));
}

static void om_step(int k, int dir)
{
    const om_item *it = &om_items[k];
    int n, i = om_sel[k];
    if (it->live == OM_RESOLUTION) {
        unsigned h;
        om_resolution(0, &h);
        n = *(int *)((unsigned char *)om_app() + OM_RES_COUNT);
        if (n > 0)
            om_sel[k] = ((i + dir) % n + n) % n;
        return;
    }
    n = om_count(it);
    if (i >= 0)
        i = (i + dir + n) % n;
    else {                                      /* a value of the file's own: the next one in the list's order */
        double v = om_saved(it), up = om_num(it, it->val[n - 1]) > om_num(it, it->val[0]) ? 1.0 : -1.0;
        if (dir > 0) {
            for (i = 0; i < n && (om_num(it, it->val[i]) - v) * up <= 0.0; i++)
                ;
            if (i == n)
                i = 0;
        } else {
            for (i = n - 1; i >= 0 && (om_num(it, it->val[i]) - v) * up >= 0.0; i--)
                ;
            if (i < 0)
                i = n - 1;
        }
    }
    om_sel[k] = i;
}

/* ORIGINAL_MAC_EXPERIENCE in game: gmv_getenv answers the Mac's values (or the file's) from now on,
   and every setting it covers is put to them, through the menu's setters and, for the ones not in
   the menu, the syncs (which the fps limiter's setter reads again), the fog's distances and
   height.  The fixes it keeps on and the test aids it turns off are only compared: a difference
   takes effect at the next start. */
static int om_no_addon(const om_item *it)
{
    return (it->set == om_set_uc && !uc_loaded) || (it->set == om_set_radio_songs && !gmv_rs_have());
}

static int om_set_mac(const char *key, double v)
{
    static const char *const startup_keys[] = {
        "TEXFIX", "SWAP_COPY", "NO_FLICKER", "VTXFIX", "CRASH58_GUARD", "FAST_FTELL", "FAST_FSEEK",
        "FAST_MEMMOVE",
#if GMV_DEV
        "SOLO_CAR",
#endif
        "NO_INTRO", "SHOW_CURSOR",
    };
    enum { NSTART = sizeof startup_keys / sizeof *startup_keys };
    const char *was[NSTART];
    char env[64];
    int on = v != 0, later = 0;
    (void)key;
    if (!tab_live())
        return 0;
    for (int i = 0; i < NSTART; i++) {
        snprintf(env, sizeof env, "GMV_%s", startup_keys[i]);
        was[i] = gmv_getenv(env);
    }
    gmv_config_set_mac(on);
    om_mac = on;
    gmv_log("[gamepatch] ORIGINAL_MAC_EXPERIENCE %s: the settings to %s\n", on ? "on" : "off",
            on ? "the Mac's" : "GangstarMV.ini's");
    if (!on)
        mac_hooks_in();                         /* before the setters below, which need them */
    for (size_t k = 0; k < OM_N; k++) {
        const om_item *it = &om_items[k];
        if (!it->key || k == OM_K_MAC || it->live != OM_LIVE)
            continue;
        snprintf(env, sizeof env, "GMV_%s", it->key);
        double t = on ? om_num(it, gmv_getenv(env)) : om_saved(it);
        int ok = it->set(it->key, t);
        if (ok)
            om_now[k] = t;
        om_later[k] = !ok && fabs(t - om_now[k]) >= 0.002;
        later |= om_later[k] && !om_no_addon(it);
    }
    const char *fh = gmv_getenv("GMV_FOG_HEIGHT");
    gmv_set_post("FOG_HEIGHT", fh && *fh && atof(fh) >= 0 ? (float)atof(fh) : 60.0f);
    cr_mac_switch(on);                          /* (on the SOUND page, not in om_items) */
    if (on)
        mac_hooks_out();                        /* after the setters: the radius and the densities are back */
    for (int i = 0; i < NSTART; i++) {
        snprintf(env, sizeof env, "GMV_%s", startup_keys[i]);
        const char *now = gmv_getenv(env);
        if (strcmp(was[i] ? was[i] : "", now ? now : "")) {
            gmv_log("[gamepatch] options tab: %s=%s from the next start\n", startup_keys[i], now ? now : "");
            later = 1;
        }
    }
    if (later)
        gmv_log("[gamepatch] ORIGINAL_MAC_EXPERIENCE: some of it takes effect at the next start\n");
    return 1;
}

static int om_relayout;            /* APPLY switched ORIGINAL_MAC_EXPERIENCE: the tabs change */

static void om_apply_one(size_t k)
{
    const om_item *it = &om_items[k];
    if (!it->key || om_sel[k] < 0)
        return;
    double v = om_num(it, it->val[om_sel[k]]);
    if (fabs(v - om_saved(it)) < 0.002 || !gmv_config_save(it->key, it->val[om_sel[k]]))
        return;
    if (om_mac && k != OM_K_MAC) {              /* kept for when ORIGINAL_MAC_EXPERIENCE goes off */
        gmv_log("[gamepatch] options tab: %s=%s saved, in force without ORIGINAL_MAC_EXPERIENCE\n",
                it->key, it->val[om_sel[k]]);
        return;
    }
    int mac = om_mac;
    int now = it->live == OM_LIVE && it->set(it->key, v);
    if (now)
        om_now[k] = v;
    om_later[k] = !now && it->live != OM_RESTART && fabs(v - om_now[k]) >= 0.002;
    om_relayout |= om_mac != mac;
    gmv_log("[gamepatch] options tab: %s=%s %s\n", it->key, it->val[om_sel[k]],
            now ? "in force now" : fabs(v - om_now[k]) < 0.002 ? "(as running)"
            : om_no_addon(it) ? "saved; it needs its add-on, which was not there at the start" : "from the next start");
}

/* APPLY: save what changed and put in force what the running game can take; ORIGINAL_MAC_EXPERIENCE
   first, as it decides what the others do. */
static void om_apply(void)
{
    om_apply_one(OM_K_MAC);
    for (size_t k = 0; k < OM_N; k++)
        if (k != OM_K_MAC)
            om_apply_one(k);
    if (om_sel[OM_K_RES] != om_res_now || om_sel[OM_K_FULL] != om_full_now) {
        int mode[2] = { om_sel[OM_K_RES], om_sel[OM_K_FULL] };
        unsigned h, w = om_resolution(mode[0], &h);
        ((void (*)(void *, int *, int))(uintptr_t)OM_RES_SET)(om_app(), mode, mode[1]);
        om_res_now = mode[0];
        om_full_now = mode[1];
        gmv_log("[gamepatch] options tab: resolution %ux%u%s\n", w, h, mode[1] ? ", full screen" : "");
    }
}

static const char *om_note(int k)
{
    const om_item *it = &om_items[k];
    if (!it->key || fabs(om_shown(k) - om_now[k]) < 0.002)
        return NULL;
    if (it->live != OM_RESTART && !om_later[k])
        return NULL;
    return om_no_addon(it) ? "NO ADD ON" : "NEXT START";
}

/* The category's item on line l (1-based) of a page, or -1 for an empty line. */
static int om_line_item(const om_category *c, int page, int l)
{
    int p1 = c->page1 ? c->page1 : OM_LINES;
    int i = page ? p1 + (page - 1) * OM_LINES + l - 1 : l - 1;
    return l <= (page ? OM_LINES : p1) && i < c->n ? i : -1;
}

static int om_page_count(const om_category *c)
{
    int p1 = c->page1 ? c->page1 : OM_LINES;
    return c->n <= p1 ? 1 : 1 + (c->n - p1 + OM_LINES - 1) / OM_LINES;
}

/* The lines of the category's page shown. */
static void om_fill(void)
{
    unsigned char *list = om_widget(OM_W_LIST), *pager = om_widget(OM_W_PAGER);
    if (!list)
        return;
    void *fx = *(void **)(list + 8);
    const om_category *c = om_category_now();
    int page = pager ? *(int *)(pager + 0x18) : 0, later = 0, later_start = 0;
    ((void (*)(void *))(uintptr_t)OM_LIST_RESET)(list);
    for (int l = 1; l <= OM_LINES; l++) {
        int i = om_line_item(c, page, l);
        char path[80], value[48];
        snprintf(path, sizeof path, "widgetControlHelps.wCusLine%d", l);
        om_visible(fx, path, i >= 0);
        if (i < 0)
            continue;
        int k = c->first + i;
        const char *note = om_note(k);
        if (om_items[k].live == OM_RESOLUTION) {
            unsigned h, w = om_resolution(om_sel[k], &h);
            snprintf(value, sizeof value, "%uX%u", w, h);
        } else if (om_sel[k] >= 0)
            snprintf(value, sizeof value, "%s", om_items[k].name[om_sel[k]]);
        else
            snprintf(value, sizeof value, "%g%s", om_saved(&om_items[k]), om_items[k].unit);
        om_line_text(l, "wCostAction.textAction", om_items[k].title);
        if (om_selectors) {                     /* ◀ value ▶ over the key boxes, which go */
            om_line_visible(fx, l, "wCostKey", 0);
            om_line_visible(fx, l, "wCostKey1", 0);
            om_line_visible(fx, l, "wCostKey2", 0);
            om_line_text(l, "textKey1", "");
            om_line_text(l, "textKey2", "");
            om_selectors_visible(fx, l, 1);
            if (note)
                strcat(value, " *");
            om_line_text(l, "textValue", value);
            later |= note != NULL;
            later_start |= note != NULL && !om_no_addon(&om_items[k]);
            continue;
        }
        om_line_text(l, "textKey1", value);
        om_line_text(l, "textKey2", note ? note : "");
        snprintf(path, sizeof path, "widgetControlHelps.wCusLine%d.wCostKey2", l);
        om_visible(fx, path, note != NULL);
    }
    if (om_selectors) {                         /* the foot the CONTROLS page asks for a key in */
        if (later)
            ((void (*)(void *, const char *, const char *, int, int))(uintptr_t)OM_LINE_TEXT)(
                om_mgr(), "widgetControlHelps.cust.wTextPressKey",
                later_start ? "* TAKES EFFECT AT THE NEXT START" : "* NEEDS ITS ADD ON FILE", 1, 0);
        om_foot = later;
        om_visible(fx, "widgetControlHelps.cust", later);
    }
    om_list_ours = 1;
}

static void om_cat_texts(unsigned char *w)
{
    void *fx = *(void **)(w + 8);
    om_text(fx, "widgetActionKeyTitle.titleAction", "SETTING");
    om_text(fx, "widgetActionKeyTitle.titleKey", "VALUE");
    om_text(fx, "widgetActionKeyTitle.txtValue", om_category_now()->name);
}

/* The pager to n pages at page i, and its "i/n" (which only its texts write: the count alone
   leaves the old one). */
void om_pages(unsigned char *pager, int n, int i)
{
    *(int *)(pager + 0x18) = i;
    ((void (*)(void *, int, int, int))(uintptr_t)OM_PAGES)(pager, n, i, 1);
    ((void (*)(void *))(uintptr_t)(*(uint32_t **)pager)[OM_W_TEXTS])(pager);
}

static void om_credit_show(unsigned char *w);

static void om_show_category(void)
{
    unsigned char *pager = om_widget(OM_W_PAGER), *cat = om_widget(OM_W_CAT);
    if (pager)
        om_pages(pager, om_page_count(om_category_now()), 0);
    if (cat) {
        om_cat_texts(cat);
        om_credit_show(cat);                    /* VIDEO <-> GAMEPATCH without leaving */
    }
    om_fill();
}

/* Alt+Enter (shimsinput.c, after the message queue is drained): full screen <-> window with the game's
   own switch, as the options' FULL SCREEN line applies it (OM_RES_SET, the resolution kept).  A shown
   options page follows, unless its FULL SCREEN line holds a change not applied yet.
   The game's own toggle (0x19bcd0) does nothing within 500 ms of its last switch (its clock 0x45d000,
   the time kept at 0x68a6c0), so a quick second press was dropped: until the screen is as asked, further
   presses wait, and a request the game dropped is made again as soon as those 500 ms are over. */
#define FS_GAME_MS   0x0045d000u    /* the game's clock, ms (gettimeofday)                            */
#define FS_LAST_MS   0x0068a6c0u    /* when the toggle last switched                                 */
#define FS_HOLD_MS   0x1f5u         /* the toggle's wait, 501 ms                                      */
static int fs_want = -1, fs_tries, fs_waits;
static DWORD fs_at;

static int fs_now(void)
{
    return (((unsigned (*)(void))(uintptr_t)OM_FULL_NOW)() & 0xff) != 0;
}

static void fs_request(void *app, int full)
{
    int mode[2] = { ((int (*)(void *))(uintptr_t)OM_RES_NOW)(app), full };
    ((void (*)(void *, int *, int))(uintptr_t)OM_RES_SET)(app, mode, mode[1]);
    om_res_now = mode[0];
    fs_at = GetTickCount();
}

__attribute__((force_align_arg_pointer, noinline))
void gmv_toggle_full_screen(void)
{
    void *app = om_app();
    if (!app)
        return;
    if (fs_want >= 0) {
        if (!fs_waits++)
            gmv_log("[gamepatch] Alt+Enter: still switching to %s, press ignored\n", fs_want ? "full screen" : "window");
        return;
    }
    int full = !fs_now();
    fs_request(app, full);
    fs_want = full;
    fs_tries = 1;
    fs_waits = 0;
    if (om_sel[OM_K_FULL] == om_full_now)
        om_sel[OM_K_FULL] = full;
    om_full_now = full;
    if (om_active && om_list_ours)
        om_fill();
    gmv_log("[gamepatch] Alt+Enter: %s\n", full ? "full screen" : "window");
}

/* Each pump, while an Alt+Enter switch is pending. */
__attribute__((force_align_arg_pointer, noinline))
void gmv_full_screen_tick(void)
{
    if (fs_want < 0)
        return;
    void *app = om_app();
    if (!app || fs_now() == fs_want) {
        fs_want = -1;
        return;
    }
    uint32_t since = ((uint32_t (*)(void))(uintptr_t)FS_GAME_MS)() - ms_rd(FS_LAST_MS);
    if (since < FS_HOLD_MS + 10 && GetTickCount() - fs_at < 2000)
        return;
    if (fs_tries >= 3) {
        gmv_log("[gamepatch] Alt+Enter: the switch to %s did not happen\n", fs_want ? "full screen" : "window");
        fs_want = -1;
        return;
    }
    if (GetTickCount() - fs_at < 50)        /* give the game's own request a moment first */
        return;
    fs_tries++;
    gmv_log("[gamepatch] Alt+Enter: the game's 500 ms wait dropped the switch to %s; asked again\n",
            fs_want ? "full screen" : "window");
    fs_request(app, fs_want);
}

/* After ORIGINAL_MAC_EXPERIENCE changed: its own line, now alone (on) or among its category (off). */
static void om_show_mac_switch(void)
{
    om_relayout = 0;
    if (!om_mac)
        for (int t = 0; t < OM_TABS; t++)
            for (int c = om_tabs[t].first; c < om_tabs[t].first + om_tabs[t].n; c++)
                if (OM_K_MAC >= om_cats[c].first && OM_K_MAC < om_cats[c].first + om_cats[c].n)
                    om_tab_now = t, om_tab_cat[t] = c;
    unsigned char *tabs = om_widget(OM_W_TABS);
    if (tabs)
        ((void (*)(void *, int, int))(uintptr_t)OM_TAB_SELECT)(tabs, om_slot(), 1);
    om_show_category();
}

const char *om_event_path(unsigned char *ev)
{
    return ((const char *(*)(void *))(uintptr_t)OM_EVENT_PATH)(ev);
}

/* The category widget and the list: ours while our page is shown, the CONTROLS page's otherwise.
   An event is {+4 name, +8 kind (6 a click, 0 a press on a key box), +0x24 handled}. */
__attribute__((cdecl, force_align_arg_pointer))
static void gmv_om_cat_event(unsigned char *w, unsigned char *ev)
{
    if (!om_active) {
        ((void (*)(void *, void *))(uintptr_t)OM_CAT_EVENT)(w, ev);
        return;
    }
    const char *name = *(const char **)(ev + 4), *path;
    if (*(int *)(ev + 8) != 6 || ev[0x24] || !name)
        return;
    int d = !strcmp(name, "btnRight") ? 1 : !strcmp(name, "btnLeft") ? -1 : 0;
    if (!d || !(path = om_event_path(ev)) || strncmp(path, "widgetActionKeyTitle.", 21))
        return;
    ev[0x24] = 1;
    if (om_mac)
        return;
    const om_tab *t = &om_tabs[om_tab_now];
    int *c = &om_tab_cat[om_tab_now];
    om_sound(0x56);
    *c = t->first + (*c - t->first + d + t->n) % t->n;
    om_show_category();
}

/* OM_CREDIT in the header: on the GAMEPATCH page only (not on our VIDEO page, and the CONTROLS page
   and the mission replay use the header too). */
static void om_credit_show(unsigned char *w)
{
    void *fx = *(void **)(w + 8);
    int on = om_active && om_slot() == OM_SLOT;
    if (!om_credit || !fx)
        return;
    om_visible(fx, "widgetActionKeyTitle.txtPatchBy", on);
    if (on) {
        static int told;
        om_text(fx, "widgetActionKeyTitle.txtPatchBy", OM_CREDIT);
        if (!told++)
            gmv_trace("[gamepatch] options tab: txtPatchBy %p\n",
                    ((void *(*)(void *, const char *))(uintptr_t)0x002fd160u)(fx, "widgetActionKeyTitle.txtPatchBy"));
    }
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_om_cat_texts(unsigned char *w)
{
    if (om_active)
        om_cat_texts(w);
    else
        ((void (*)(void *))(uintptr_t)OM_CAT_TEXTS)(w);
    om_credit_show(w);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_om_cat_keep(unsigned char *w)
{
    if (!om_active)
        ((void (*)(void *))(uintptr_t)OM_CAT_TEXTS)(w);
    ((void (*)(void *))(uintptr_t)OM_KEEP)(w);
    om_credit_show(w);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_om_cat_show(unsigned char *w)
{
    ((void (*)(void *))(uintptr_t)OM_PLAIN_SHOW)(w);
    om_credit_show(w);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_om_list_show(unsigned char *w)
{
    if (om_active) {
        ((void (*)(void *))(uintptr_t)OM_PLAIN_SHOW)(w);
        return;
    }
    for (int l = 1; om_selectors && l <= OM_LINES; l++)
        om_selectors_visible(*(void **)(w + 8), l, 0);
    ((void (*)(void *))(uintptr_t)OM_LIST_SHOW)(w);
    om_list_keys = 1;
    om_list_ours = 0;
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_om_list_hide(unsigned char *w, int fade)
{
    if (om_list_keys)
        ((void (*)(void *, int))(uintptr_t)OM_LIST_HIDE)(w, fade);
    else
        ((void (*)(void *, int))(uintptr_t)OM_PLAIN_HIDE)(w, fade);
    om_list_keys = 0;
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_om_list_keep(unsigned char *w)
{
    if (!om_active && om_list_ours) {           /* the CONTROLS page after ours: its own lines afresh */
        gmv_om_list_hide(w, 0);
        gmv_om_list_show(w);
    }
    ((void (*)(void *))(uintptr_t)OM_KEEP)(w);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_om_list_texts(unsigned char *w)
{
    if (!om_active) {
        ((void (*)(void *))(uintptr_t)OM_LIST_TEXTS)(w);
        return;
    }
    void *fx = *(void **)(w + 8);
    om_visible(fx, "widgetControlHelps.cust", om_selectors && om_foot);
    om_text(fx, "widgetControlHelps.btnOk.text", "APPLY");
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_om_list_event(unsigned char *w, unsigned char *ev)
{
    if (!om_active) {
        ((void (*)(void *, void *))(uintptr_t)OM_LIST_EVENT)(w, ev);
        return;
    }
    const char *name = *(const char **)(ev + 4), *path;
    int kind = *(int *)(ev + 8);
    if (ev[0x24] || !name)
        return;
    if (kind == 6 && !strcmp(name, "btnOk")) {
        ev[0x24] = 1;
        om_sound(0x57);
        om_apply();
        if (om_relayout)
            om_show_mac_switch();
        else
            om_fill();
        return;
    }
    int dir = 0;
    if (kind == 6 && om_selectors)              /* an arrow of a line's ◀ value ▶ */
        dir = !strcmp(name, "btnValueRight") ? 1 : !strcmp(name, "btnValueLeft") ? -1 : 0;
    else if (kind == 0 && !om_selectors)        /* a key box */
        dir = !strcmp(name, "wCostKey1") ? 1 : !strcmp(name, "wCostKey2") ? -1 : 0;
    if (!dir || !(path = om_event_path(ev)))
        return;
    unsigned char *pager = om_widget(OM_W_PAGER);
    const om_category *c = om_category_now();
    for (int l = 1; l <= OM_LINES; l++) {
        char want[80];
        snprintf(want, sizeof want, "widgetControlHelps.wCusLine%d.%s", l, name);
        if (strcmp(path, want))
            continue;
        int i = om_line_item(c, pager ? *(int *)(pager + 0x18) : 0, l);
        ev[0x24] = 1;
        if (i < 0 || *(const unsigned char *)(uintptr_t)OM_LOCK)
            return;
        om_sound(0x56);
        om_step(c->first + i, dir);
        om_fill();
        return;
    }
}

__attribute__((cdecl, force_align_arg_pointer))
static const char *gmv_tab_label(void *table, int id)
{
    if (id == OM_TAB_ID)
        return "GAMEPATCH";
    if (id == MR_TAB_ID)
        return "MISSION REPLAY";
    return ((const char *(*)(void *, int))(uintptr_t)OM_STRING_FN)(table, id);
}

/* The tab label loop's lookup, for GAMEPATCH_TAB and MISSION_REPLAY alike: put in once. */
int tab_label_hook(void)
{
    static int done;
    if (!done && sm_is_call(OM_LABEL_SITE, OM_STRING_FN) && sm_redirect(OM_LABEL_SITE, (void *)gmv_tab_label))
        done = 1;
    return done;
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_om_tab6(void *fx, const char *path, int visible)
{
    unsigned char *tabs = om_widget(OM_W_TABS);
    if (tabs) {
        ((void (*)(void *, int, int, int))(uintptr_t)OM_SET_ENTRY)(tabs, OM_SLOT, OM_TAB_ID, 1);
        visible = 1;
    }
    om_visible(fx, path, visible);
}

__attribute__((cdecl, force_align_arg_pointer))
static int gmv_om_clicked(unsigned char *tabs)
{
    int n = *(int *)(tabs + 0x58), t = 0;
    while (!om_mac && t < OM_TABS && om_tabs[t].slot != n)
        t++;
    if (om_mac ? n != OM_SLOT : t == OM_TABS)
        return n;
    om_sound(0x5a);
    if (!om_active) {
        om_tab_now = om_mac ? 0 : t;
        ((void (*)(void *, const char *, int))(uintptr_t)OM_SWITCH_FN)(om_mgr(), OM_STATE, 0);
    } else if (!om_mac && t != om_tab_now) {    /* our other tab: as leaving, then its categories */
        om_apply();
        if (om_relayout) {                      /* ORIGINAL_MAC_EXPERIENCE came on: its line only */
            om_show_mac_switch();
            return 4;
        }
        om_tab_now = t;
        ((void (*)(void *, int, int))(uintptr_t)OM_TAB_SELECT)(tabs, n, 1);
        om_show_category();
    }
    return 4;
}

/* The category and list widgets to the copies of their vtables (from the menu set-up on). */
static void om_take_widgets(void)
{
    unsigned char *cat = om_widget(OM_W_CAT), *list = om_widget(OM_W_LIST);
    if (cat && *(uint32_t *)cat == OM_CAT_VT)
        *(uint32_t **)cat = om_cat_vt + 2;
    if (list && *(uint32_t *)list == OM_LIST_VT) {
        om_list_keys = list[0x10];              /* shown by the CONTROLS page */
        *(uint32_t **)list = om_list_vt + 2;
    }
}

/* Our page's show: the Options show (which shows the listed widgets), the Game page's set-up, the
   pager where the CONTROLS page has it, and the category's lines. */
__attribute__((cdecl, force_align_arg_pointer))
static void gmv_om_show(unsigned char *self)
{
    unsigned char *tabs = om_widget(OM_W_TABS), *pager = om_widget(OM_W_PAGER);
    unsigned char *cat = om_widget(OM_W_CAT), *list = om_widget(OM_W_LIST);
    void *m = om_mgr();
    om_take_widgets();
    om_active =cat && list && pager && *(uint32_t **)cat == om_cat_vt + 2 && *(uint32_t **)list == om_list_vt + 2;
    om_load();
    if (pager)
        ((void (*)(void *, void *))(uintptr_t)OM_LISTEN)(pager, self);
    ((void (*)(void *))(uintptr_t)OM_OPT_SHOW)(self);
    ((void (*)(void *, int))(uintptr_t)0x0015b4b8u)(m, 1);
    if (tabs)
        ((void (*)(void *, int, int))(uintptr_t)OM_TAB_SELECT)(tabs, om_slot(), 1);
    if (!om_active) {
        gmv_log("[gamepatch] options tab: the CONTROLS page's widgets are not the expected ones\n");
        return;
    }
    if (((unsigned char (*)(void))(uintptr_t)0x001b5520u)())
        ((void (*)(void *))(uintptr_t)0x0015b48eu)(m);
    ((void (*)(void *, const char *, int, int))(uintptr_t)OM_MOVE)(*(void **)(self + 4), "widgetArrows", 0x276, 0x183);
    om_show_category();
}

/* Leaving applies what waits, puts the pager back where the other pages have it and lets go. */
__attribute__((cdecl, force_align_arg_pointer))
static void gmv_om_hide(unsigned char *self)
{
    unsigned char *pager = om_widget(OM_W_PAGER);
    if (om_active) {
        om_apply();
        om_relayout = 0;                        /* the next show lays the tabs out as om_mac says */
        if (pager && *(unsigned char **)(pager + 0xc) == self)
            ((void (*)(void *, const char *, int, int))(uintptr_t)OM_MOVE)(*(void **)(self + 4), "widgetArrows", 0x1c0, 0x1ad);
    }
    ((void (*)(void *))(uintptr_t)OM_GS_HIDE)(self);
    om_active = 0;
}

/* The Options onEvent for the tabs, then the pager's page change. */
__attribute__((cdecl, force_align_arg_pointer))
static int gmv_om_event(unsigned char *self, unsigned char *w, int type)
{
    if (((unsigned char (*)(void *, void *, int))(uintptr_t)OM_OPT_EVENT)(self, w, type))
        return 1;
    if (!om_active || !w || w != om_widget(OM_W_PAGER) || (type != 1 && type != 2))
        return 0;
    om_fill();
    return 1;
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_om_register(void *menufx, void *state, const char *clip)
{
    typedef void (*reg_fn)(void *, void *, const char *);
    ((reg_fn)(uintptr_t)OM_REG_FN)(menufx, state, clip);
    unsigned char *s = ((unsigned char *(*)(uint32_t, const char *, int, int))(uintptr_t)OM_ALLOC)(
        OM_GS_BYTES, "the patches", __LINE__, 1);
    if (!s) {
        gmv_log("[gamepatch] no memory for the GAMEPATCH options page\n");
        return;
    }
    ((void (*)(void *))(uintptr_t)OM_GS_CTOR)(s);
    uint32_t *ids = *(uint32_t **)(s + 0x5c);
    int nids = *(int *)(s + 0x60), row = -1, lang = -1;
    for (int i = 0; i < nids; i++) {
        if (ids[i] == OM_W_ROW)
            row = i;
        else if (ids[i] == OM_W_LANGUAGE)
            lang = i;
    }
    if (row < 0 || lang < 0) {
        gmv_log("[gamepatch] the Game page's widgets are not the expected ones; no GAMEPATCH tab\n");
        return;
    }
    ids[row] = OM_W_CAT;
    ids[lang] = OM_W_LIST;
    strcpy((char *)s + 8, OM_STATE);
    memcpy(om_vt, (const void *)(uintptr_t)OM_GS_VT, sizeof om_vt);
    om_vt[OM_VT_SHOW] = (uint32_t)(uintptr_t)gmv_om_show;
    om_vt[OM_VT_HIDE] = (uint32_t)(uintptr_t)gmv_om_hide;
    om_vt[OM_VT_EVENT] = (uint32_t)(uintptr_t)gmv_om_event;
    *(uint32_t **)s = om_vt;
    ((reg_fn)(uintptr_t)OM_REG_FN)(menufx, s, clip);
    om_take_widgets();                          /* so that the CONTROLS page hides our ◀ value ▶ */
    if (om_mac)
        gmv_log("[gamepatch] options menu: GAMEPATCH tab registered in slot %d (1 setting)\n", OM_SLOT);
    else
        gmv_log("[gamepatch] options menu: VIDEO (slot 3, %d categories) and GAMEPATCH (slot %d, %d categories) "
                "registered (%d settings)\n", om_tabs[0].n, OM_SLOT, om_tabs[1].n, (int)OM_N);
}

void apply_options_tab(void)
{
    if (!tab_live())
        return;
    static const unsigned char table_cmp[4] = { 0x83, 0xf8, 0x03, 0x77 };
    const uint32_t *vt = (const uint32_t *)(uintptr_t)OM_GS_VT;
    const uint32_t *cv = (const uint32_t *)(uintptr_t)OM_CAT_VT, *lv = (const uint32_t *)(uintptr_t)OM_LIST_VT;
    int last = 0;
    for (int c = 0; c < OM_CATS; c++)
        last = om_cats[c].first == last ? last + om_cats[c].n : -1;
    int covered = 0;                            /* and the tabs the categories */
    for (int t = 0; t < OM_TABS; t++) {
        covered = om_tabs[t].first == covered && om_tabs[t].n > 0 ? covered + om_tabs[t].n : -1;
        om_tab_cat[t] = om_tabs[t].first;
    }
    if (!sm_is_call(OM_HIDE6_SITE, OM_SET_VISIBLE) || !sm_is_call(OM_CLICK_SITE, OM_CLICK_FN) ||
        !sm_is_call(OM_REG_SITE, OM_REG_FN) || !sm_is_call(OM_LABEL_SITE, OM_STRING_FN) ||
        memcmp((const void *)(uintptr_t)OM_TABLE_CMP, table_cmp, 4) || vt[OM_VT_SHOW] != 0x00169a6au ||
        vt[OM_VT_HIDE] != OM_GS_HIDE || vt[OM_VT_EVENT] != 0x00169b62u ||
        cv[OM_W_SHOW] != OM_PLAIN_SHOW || cv[OM_W_KEEP] != OM_KEEP || cv[OM_W_EVENT] != OM_CAT_EVENT ||
        cv[OM_W_TEXTS] != OM_CAT_TEXTS || lv[OM_W_SHOW] != OM_LIST_SHOW || lv[OM_W_HIDE] != OM_LIST_HIDE ||
        lv[OM_W_KEEP] != OM_KEEP || lv[OM_W_EVENT] != OM_LIST_EVENT || lv[OM_W_TEXTS] != OM_LIST_TEXTS) {
        gmv_log("[gamepatch] options menu not the expected bytes; no GAMEPATCH tab\n");
        return;
    }
    if (last != (int)OM_N || covered != OM_CATS || strcmp(om_items[OM_K_MAC].key, "ORIGINAL_MAC_EXPERIENCE") ||
        om_items[OM_K_RES].live != OM_RESOLUTION || om_items[OM_K_FULL].live != OM_FULL_SCREEN) {
        gmv_log("[gamepatch] options tab: the categories do not cover the settings; no GAMEPATCH tab\n");
        return;
    }
    const char *m = gmv_getenv("GMV_ORIGINAL_MAC_EXPERIENCE");
    om_mac = m && *m && *m != '0';
    for (size_t k = 0; k < OM_N; k++) {
        char env[64];
        if (!om_items[k].key)
            continue;
        snprintf(env, sizeof env, "GMV_%s", om_items[k].key);
        om_now[k] = om_num(&om_items[k], gmv_getenv(env));
    }
    memcpy(om_cat_vt, cv - 2, sizeof om_cat_vt);
    om_cat_vt[2 + OM_W_SHOW] = (uint32_t)(uintptr_t)gmv_om_cat_show;
    om_cat_vt[2 + OM_W_KEEP] = (uint32_t)(uintptr_t)gmv_om_cat_keep;
    om_cat_vt[2 + OM_W_EVENT] = (uint32_t)(uintptr_t)gmv_om_cat_event;
    om_cat_vt[2 + OM_W_TEXTS] = (uint32_t)(uintptr_t)gmv_om_cat_texts;
    memcpy(om_list_vt, lv - 2, sizeof om_list_vt);
    om_list_vt[2 + OM_W_SHOW] = (uint32_t)(uintptr_t)gmv_om_list_show;
    om_list_vt[2 + OM_W_HIDE] = (uint32_t)(uintptr_t)gmv_om_list_hide;
    om_list_vt[2 + OM_W_KEEP] = (uint32_t)(uintptr_t)gmv_om_list_keep;
    om_list_vt[2 + OM_W_EVENT] = (uint32_t)(uintptr_t)gmv_om_list_event;
    om_list_vt[2 + OM_W_TEXTS] = (uint32_t)(uintptr_t)gmv_om_list_texts;
    if (sm_redirect(OM_REG_SITE, (void *)gmv_om_register) && tab_label_hook() &&
        sm_redirect(OM_HIDE6_SITE, (void *)gmv_om_tab6) && sm_redirect(OM_CLICK_SITE, (void *)gmv_om_clicked))
        gmv_log("[gamepatch] a GAMEPATCH tab in the options menu%s (GAMEPATCH_TAB=0 to remove it)\n",
                om_mac ? ", with only the ORIGINAL_MAC_EXPERIENCE switch" : ", and VIDEO with DISPLAY and GRAPHICS");
    else
        return;
    om_tab_files = 1;
    if (!om_opener_hook())
        gmv_log("[gamepatch] options tab: the file opener is not the expected one; the list keeps its key boxes\n");
}
