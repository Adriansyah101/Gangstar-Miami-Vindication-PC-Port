/* custom_radio_hooks.c -- CUSTOM_RADIO: the game side of the sixth station */
#include "compat.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* ---- CUSTOM_RADIO: a sixth station playing the player's own music (an enhancement the user asked
   for, off by default and under ORIGINAL_MAC_EXPERIENCE; the music itself is customradio.c) ----

   The music manager cycles its stations 0..[mm+0x270]-1 (next 0x60156, previous 0x60122; both
   then tune 0x6000c, which plays the tuning noise 0x121 and sets [mm+0x21c]).  When a song or the
   noise ends, the update 0x60238 asks the pick 0x5f3e8(mm, station) for the next slot (0x605ba)
   and plays it; the radio start 0x60190 does the same at once (0x601cd).  With the setting on and
   songs in the folder, [mm+0x270] is 6 instead of 5, and both pick calls go to gmv_cr_pick, which
   answers the sixth station with -1: the manager's own "nothing" slot (set at its init, 0x60ae9).
   Playing -1 does nothing (0x5fee8 range-checks it) and the update only asks again for the slot
   [mm+0x218] it is playing, so the game stays silent there while customradio.c plays.  Tuning
   away works as usual (0x6000c skips the stop for an out-of-range slot).  A car remembers its
   station; one left on the sixth after the setting went off gets station 0.  The HUD shows
   PLAY LIST / MY MUSIC for the sixth (gmv_cr_logo, below).  The hook is in from the start
   (pass-through while off; not under ORIGINAL_MAC_EXPERIENCE without the options tab) and the
   setting switches live on the SOUND page (below). */
#define CR_COUNT(mm)     (*(int *)((mm) + 0x270))

static const uint32_t cr_pick_sites[] = { 0x000601cdu, 0x000605bau };
static uint32_t g_cr_pick_next;      /* what they reached before: 0x5f3e8, or the radio trace's hook */
int g_cr_hooked, g_cr_on;
int g_cr_row_want, g_cr_row;   /* the SOUND page's row: asked for, in the menu file */

/* [mm+0x270]: six stations while the setting is on and there are songs, else the game's five. */
static void cr_count_sync(unsigned char *mm)
{
    int want = g_cr_on && gmv_cr_songs() > 0 ? CR_GAME_STATIONS + 1 : CR_GAME_STATIONS;
    if (CR_COUNT(mm) == CR_GAME_STATIONS || CR_COUNT(mm) == CR_GAME_STATIONS + 1)
        CR_COUNT(mm) = want;
}

__attribute__((cdecl, force_align_arg_pointer))
static int gmv_cr_pick(unsigned char *mm, int station)
{
    cr_count_sync(mm);
    if (station >= CR_GAME_STATIONS) {
        if (station == GMV_CR_STATION && CR_COUNT(mm) > GMV_CR_STATION)
            return -1;                  /* nothing from the game: customradio.c plays */
        station = 0;                    /* the setting went off while a car kept the station */
        *(int *)(mm + 0x21c) = 0;
    }
    return ((int (*)(void *, int))(uintptr_t)g_cr_pick_next)(mm, station);
}

/* 1 = both pick calls go to gmv_cr_pick, 0 = not the expected calls, -1 = a write failed. */
static int cr_hook(void)
{
    if (g_cr_hooked)
        return 1;
    const uint32_t mine = (uint32_t)(uintptr_t)gmv_cr_pick, trace = (uint32_t)(uintptr_t)gmv_radio_pick;
    uint32_t next = 0;
    for (size_t i = 0; i < sizeof cr_pick_sites / sizeof *cr_pick_sites; i++) {
        uint32_t s = cr_pick_sites[i];
        uint32_t to = sm_is_call(s, RADIO_PICK_FN) ? RADIO_PICK_FN : sm_is_call(s, trace) ? trace : 0;
        if (!to || (next && to != next))
            return 0;
        next = to;
    }
    g_cr_pick_next = next;
    for (size_t i = 0; i < sizeof cr_pick_sites / sizeof *cr_pick_sites; i++) {
        int32_t rel = (int32_t)(mine - (cr_pick_sites[i] + 5));
        if (!patch_bytes(cr_pick_sites[i] + 1, (const unsigned char *)&rel, 4))
            return -1;
    }
    g_cr_hooked = 1;
    return 1;
}

/* Put the setting in force; 0 if the radio code is not the expected one. */
static int custom_radio_set(int on)
{
    if (cr_hook() != 1)
        return 0;
    if (on != g_cr_on)
        gmv_log("[radio] CUSTOM_RADIO %s\n", on ? "on" : "off");
    g_cr_on = on;
    gmv_cr_enable(on);
    unsigned char **mmp = *(unsigned char ***)(uintptr_t)RADIO_MM_PP;
    unsigned char *mm = mmp ? *mmp : NULL;
    if (mm) {
        /* off while it plays: over to the first station, with the tuning noise */
        if (!on && (*(uint32_t *)(mm + 0x220) & 1) && *(int *)(mm + 0x21c) >= CR_GAME_STATIONS)
            ((void (*)(void *, int))(uintptr_t)RADIO_TUNE_FN)(mm, 0);
        cr_count_sync(mm);
    }
    return 1;
}

/* ---- its rows on the SOUND page (the menu file gets them, see cr_swf_patch) ----
   The volume widget (vtable 0x6593c8, two instances: the main menu's and the pause menu's options)
   keeps its own three rows and we take ours:
   - its texts (slot 6, 0x178cf4, which the game calls on show and after each event) are followed
     by ours: row 4's slider at CUSTOM_RADIO_VOLUME and its speaker, row 5's ON / OFF;
   - its events (slot 5, 0x17860c) for our parts are ours: dragging or pressing row 4's slider sets
     the volume the way the game reads its sliders (x / width + 0.22 + 0.05, 0..1), its speaker
     mutes (volume 0) and gives the volume back, as the game's own speakers show; row 5's arrows
     switch CUSTOM_RADIO.  A press, a release or a click saves both keys to GangstarMV.ini.
   Hidden under ORIGINAL_MAC_EXPERIENCE. */
#define CR_VOL_VT      0x006593c8u
#define CR_VOL_EVENT   0x0017860cu      /* slot 5 */
#define CR_VOL_TEXTS   0x00178cf4u      /* slot 6 */
#define CR_EV_PATH     0x005edb94u      /* RenderFX::Event::GetCharacterPath(event) */
#define CR_GOTO_FRAME  0x002fd810u      /* (flash, path, frame, 0) */
#define CR_GOTO_LABEL  0x002fd8a0u      /* (flash, path, label, 0) */
#define CR_SET_TEXT    0x002fdea0u      /* (flash, path, text, 1) */
#define CR_FIND        0x002fd160u      /* (flash, path): the character, or NULL */
#define CR_SLIDER_FRAMES 0x005f1518u    /* float 20: a value's slider frame is value * this */
#define CR_SLIDER_ADD1 0x005f6940u      /* float 0.22 */
#define CR_SLIDER_ADD2 0x005f4be8u      /* float 0.05 */

#define CR_MOVE        0x002fdf30u      /* (flash, path, x, y) */

/* The game's three rows: where its menu file has them (the Mac's, untouched), where the five rows have them */
static const struct { const char *name; int32_t was, now; } cr_rows[6] = {
    {"txtSFX", 203, CR_TITLE0}, {"barSFX", 732, CR_BAR0},
    {"txtVFX", 2180, CR_TITLE0 + CR_STEP}, {"barVFX", 2699, CR_BAR0 + CR_STEP},
    {"txtMusic", 4150, CR_TITLE0 + 2 * CR_STEP}, {"barMusic", 4671, CR_BAR0 + 2 * CR_STEP},
};

static float g_cr_vol = 1.0f;           /* CUSTOM_RADIO_VOLUME, 0..1 */
static float g_cr_unmuted = 1.0f;       /* the volume the speaker gives back */

static void cr_row_visible(void *fx, const char *name, int on)
{
    char path[64];
    snprintf(path, sizeof path, "widgetVolumes.%s", name);
    ((void (*)(void *, const char *, int))(uintptr_t)CR_VISIBLE)(fx, path, on);
}

/* The menu file has the game's rows where the five rows want them.  Under ORIGINAL_MAC_EXPERIENCE they go
   back where the Mac has them.  The move gives a clip a matrix of its own ({sx, rx, x, ry, sy, y}, twips)
   but takes whole pixels, so the exact twips go into it. */
static void cr_rows_place(void *fx, int mac)
{
    char path[64];
    for (size_t i = 0; i < sizeof cr_rows / sizeof *cr_rows; i++) {
        snprintf(path, sizeof path, "widgetVolumes.%s", cr_rows[i].name);
        unsigned char *ch = ((unsigned char *(*)(void *, const char *))(uintptr_t)CR_FIND)(fx, path);
        float *m = ch ? *(float **)(ch + 0x5c) : NULL, y = (float)(mac ? cr_rows[i].was : cr_rows[i].now);
        if (!m || m[5] == y)
            continue;
        float x = m[2];
        ((void (*)(void *, const char *, int, int))(uintptr_t)CR_MOVE)(fx, path, (int)(x / 20), (int)(y / 20));
        if ((m = *(float **)(ch + 0x5c)) != NULL)
            m[2] = x, m[5] = y;
    }
}

static const char *const cr_parts[7] = {"txtRadio", "barRadio", "txtRadioOn", "crValueBar", "btnCrLeft",
                                        "btnCrRight", "crTextValue"};
static void *g_cr_fx[2];                /* the movies the page's widgets were seen in */

/* Our rows on the stage or, as the menu file has them, below it: with no hook on the page they need
   nothing done to them. */
static void cr_extras_place(void *fx, int show)
{
    char path[64];
    for (size_t i = 0; i < sizeof cr_parts / sizeof *cr_parts; i++) {
        snprintf(path, sizeof path, "widgetVolumes.%s", cr_parts[i]);
        unsigned char *ch = ((unsigned char *(*)(void *, const char *))(uintptr_t)CR_FIND)(fx, path);
        float *m = ch ? *(float **)(ch + 0x5c) : NULL;
        if (!m || (m[5] < CR_OFF / 2) == show)
            continue;
        float x = m[2], y = m[5] + (show ? -CR_OFF : CR_OFF);
        ((void (*)(void *, const char *, int, int))(uintptr_t)CR_MOVE)(fx, path, (int)(x / 20), (int)(y / 20));
        if ((m = *(float **)(ch + 0x5c)) != NULL)
            m[2] = x, m[5] = y;
    }
}

/* The page as the menu file has it, for the moment its hook goes out. */
void cr_page_mac(void)
{
    for (int i = 0; i < 2; i++)
        if (g_cr_fx[i]) {
            cr_rows_place(g_cr_fx[i], 1);
            cr_extras_place(g_cr_fx[i], 0);
        }
}

static void cr_row_texts(unsigned char *w)
{
    void *fx = *(void **)(w + 8);
    int show = !gmv_config_mac();
    if (!fx || !g_cr_row)
        return;
    if (g_cr_fx[0] != fx && g_cr_fx[1] != fx)
        g_cr_fx[!!g_cr_fx[0]] = fx;
    cr_rows_place(fx, !show);
    cr_extras_place(fx, show);
    for (size_t i = 0; i < sizeof cr_parts / sizeof *cr_parts; i++)
        cr_row_visible(fx, cr_parts[i], show);
    if (!show)
        return;
    ((void (*)(void *, const char *, int, int))(uintptr_t)CR_GOTO_FRAME)(
        fx, "widgetVolumes.barRadio.btnVolume", (int)(g_cr_vol * *(const float *)(uintptr_t)CR_SLIDER_FRAMES), 0);
    ((void (*)(void *, const char *, const char *, int))(uintptr_t)CR_GOTO_LABEL)(
        fx, "widgetVolumes.barRadio.btnMute", g_cr_vol > 0 ? "on" : "off", 0);
    ((void (*)(void *, const char *, const char *, int))(uintptr_t)CR_SET_TEXT)(
        fx, "widgetVolumes.crTextValue", g_cr_on ? "ON" : "OFF", 1);
    /* the game sets its own titles too ("Music" from the string table): a title's text in the
       file is not what shows */
    ((void (*)(void *, const char *, const char *, int))(uintptr_t)CR_SET_TEXT)(
        fx, "widgetVolumes.txtRadio", "Custom Radio Volume", 1);
    ((void (*)(void *, const char *, const char *, int))(uintptr_t)CR_SET_TEXT)(
        fx, "widgetVolumes.txtRadioOn", "Custom Radio", 1);
    static int told;
    if (!told++)
        gmv_trace("[gamepatch] SOUND page: txtRadio %p, txtRadioOn %p, btnCrLeft %p (txtMusic %p)\n",
                ((void *(*)(void *, const char *))(uintptr_t)CR_FIND)(fx, "widgetVolumes.txtRadio"),
                ((void *(*)(void *, const char *))(uintptr_t)CR_FIND)(fx, "widgetVolumes.txtRadioOn"),
                ((void *(*)(void *, const char *))(uintptr_t)CR_FIND)(fx, "widgetVolumes.btnCrLeft"),
                ((void *(*)(void *, const char *))(uintptr_t)CR_FIND)(fx, "widgetVolumes.txtMusic"));
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_cr_vol_texts(unsigned char *w)
{
    ((void (*)(void *))(uintptr_t)CR_VOL_TEXTS)(w);
    cr_row_texts(w);
}

static void cr_save(void)
{
    char b[16];
    snprintf(b, sizeof b, "%.2f", (double)g_cr_vol);
    gmv_config_save("CUSTOM_RADIO", g_cr_on ? "1" : "0");
    gmv_config_save("CUSTOM_RADIO_VOLUME", b);
}

static void cr_set_volume(float v)
{
    g_cr_vol = v < 0 ? 0.0f : v > 1 ? 1.0f : v;
    if (g_cr_vol > 0.001f)
        g_cr_unmuted = g_cr_vol;
    gmv_cr_set_volume(g_cr_vol);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_cr_vol_event(unsigned char *w, uint32_t *ev)
{
    const char *path = ((const char *(*)(void *))(uintptr_t)CR_EV_PATH)(ev);
    const char *name = (const char *)(uintptr_t)ev[1];
    int kind = (int)ev[2];
    /* Row 4's slider and speaker are under barRadio; row 5's arrows are btnCrLeft/btnCrRight (a
       widget only takes clicks from parts named btn..., listed as it shows: 0x16e9e8). */
    if (!g_cr_row || gmv_config_mac() || !path || !name ||
        (!strstr(path, "barRadio") && strcmp(name, "btnCrLeft") && strcmp(name, "btnCrRight"))) {
        ((void (*)(void *, void *))(uintptr_t)CR_VOL_EVENT)(w, ev);
        return;
    }
    if (!strcmp(name, "btnVolume") && kind >= 3 && kind <= 5) {        /* row 4: the slider */
        float r[8] = {0}, x;
        void *ch = (void *)(uintptr_t)ev[0];
        ((void (*)(void *, float *))(uintptr_t)(*(uint32_t **)ch)[0x12c / 4])(ch, r);
        memcpy(&x, &ev[3], 4);
        cr_set_volume(r[1] > r[0] ? x / (r[1] - r[0]) + *(const float *)(uintptr_t)CR_SLIDER_ADD1 +
                                        *(const float *)(uintptr_t)CR_SLIDER_ADD2 : 0.0f);
        if (kind != 5)
            cr_save();
    } else if (!strcmp(name, "btnMute") && kind == 6) {               /* row 4: the speaker */
        cr_set_volume(g_cr_vol > 0.001f ? 0.0f : g_cr_unmuted);
        cr_save();
    } else if ((!strcmp(name, "btnCrLeft") || !strcmp(name, "btnCrRight")) && kind == 6) {   /* row 5 */
        om_sound(0x56);
        custom_radio_set(!g_cr_on);
        cr_save();
    } else if (!(!strcmp(name, "btnVolume") && kind == 1))
        return;
    *((unsigned char *)ev + 0x24) = 1;          /* handled */
    gmv_cr_vol_texts(w);
}

/* ORIGINAL_MAC_EXPERIENCE switched in game: off under it, the file's setting without it. */
static int g_cr_logo_hide;               /* the switch went on with play_list perhaps showing */

void cr_mac_switch(int mac)
{
    g_cr_logo_hide = mac;
    if (!g_cr_hooked)
        return;
    const char *f = gmv_config_file_value("CUSTOM_RADIO");
    custom_radio_set(!mac && f && *f && *f != '0');
    if (mac) {
        /* the pick hook that sends the sixth to the first goes out now: nothing may ask for the sixth */
        unsigned char **mmp = *(unsigned char ***)(uintptr_t)RADIO_MM_PP;
        unsigned char *mm = mmp ? *mmp : NULL;
        if (mm && *(int *)(mm + 0x21c) >= CR_GAME_STATIONS)
            *(int *)(mm + 0x21c) = 0;
        cr_cars_off_sixth();
    }
}

/* ---- its HUD logo: "PLAY LIST" over "MY MUSIC", as the file has it (the user's choice, 2026-11-02) ----
   The HUD radio's logo 0x17357e(widget, station) sets [widget+0x14] and shows, of its five clips
   (their paths are the strings at widget+0x1c), the station's and hides the rest.  Its first bytes
   now jump to gmv_cr_logo, which does the same and also shows play_list for the sixth station: the
   iPhone's iPod logo clip, hidden since the HUD was built (0x17b7c4) and unused on the Mac.  The game's
   0x17357e knows nothing of that clip, so it would stay up over the station's logo once the hook is
   out: as ORIGINAL_MAC_EXPERIENCE goes on, cr_logo_hide_tick hides it, with the HUD there to do it on. */
#define CR_STR_CSTR 0x005edc48u         /* std::string::c_str */
#define CR_PLAY_LIST "IG_HUD_Radio.radioON.play_list"

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_cr_logo(unsigned char *w, int station)
{
    typedef void (*vis_fn)(void *, const char *, int);
    *(int *)(w + 0x14) = station;
    for (int i = 0; i < CR_GAME_STATIONS; i++) {
        const char *path = ((const char *(*)(void *))(uintptr_t)CR_STR_CSTR)(w + 0x1c + 4 * i);
        ((vis_fn)(uintptr_t)CR_VISIBLE)(*(void **)(w + 8), path, i == station);
    }
    ((vis_fn)(uintptr_t)CR_VISIBLE)(*(void **)(w + 8), CR_PLAY_LIST, station == GMV_CR_STATION);
}

void cr_logo_hide_tick(void)
{
    if (!g_cr_logo_hide)
        return;
    unsigned char *app = ((unsigned char *(*)(void))(uintptr_t)RADIO_APP_FN)();
    unsigned char *hud = app && *(int *)(app + 0x218) > 0x34 ? *(unsigned char **)(app + 0x214) : NULL;
    unsigned char *w = hud ? *(unsigned char **)(hud + 0xd0) : NULL;
    if (w) {
        ((void (*)(void *, const char *, int))(uintptr_t)CR_VISIBLE)(*(void **)(w + 8), CR_PLAY_LIST, 0);
        g_cr_logo_hide = 0;
    }
}

static const unsigned char cr_logo_fn_was[5] = {0x55, 0x89, 0xe5, 0x53, 0x57};     /* push ebp; mov ebp, esp; push ebx; push edi */

static int cr_logo_hook(void)
{
    unsigned char jmp[5] = {0xe9};
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)gmv_cr_logo - (RADIO_LOGO_FN + 5));
    if (memcmp((const void *)(uintptr_t)RADIO_LOGO_FN, cr_logo_fn_was, sizeof cr_logo_fn_was))
        return 0;
    memcpy(jmp + 1, &rel, 4);
    return patch_bytes(RADIO_LOGO_FN, jmp, sizeof jmp);
}

/* The menu file is read once, at the start, so it gets the rows then, whatever the switch: they wait below
   the stage (cr_swf_patch). */
void cr_file_wants(void)
{
    const uint32_t *vt = (const uint32_t *)(uintptr_t)CR_VOL_VT;
    if (gmv_config_mac() && !tab_live())
        return;                                 /* the Mac's radio, and nothing to switch it in game */
    if (vt[5] == CR_VOL_EVENT && vt[6] == CR_VOL_TEXTS && om_opener_hook())
        g_cr_row_want = 1;
    else
        gmv_log("[gamepatch] the SOUND page's widget is not the expected one; no Custom Radio row "
                "(CUSTOM_RADIO in GangstarMV.ini still works)\n");
}

void apply_custom_radio(void)
{
    const char *e = gmv_getenv("GMV_CUSTOM_RADIO"), *v = gmv_getenv("GMV_CUSTOM_RADIO_VOLUME");
    int want = e && *e && *e != '0';
    if (gmv_config_mac())
        return;                                 /* the Mac's radio: the hooks come with the switch */
    cr_set_volume(v && *v ? (float)atof(v) : 1.0f);
    int r = cr_hook();
    if (r != 1) {
        gmv_log(r ? "[gamepatch] a write failed (%lu); no custom radio station\n"
                  : "[gamepatch] the radio's song-pick calls are not the expected ones (%lu); no custom radio station\n",
                GetLastError());
        return;
    }
    if (g_cr_row_want) {
        uint32_t ev = (uint32_t)(uintptr_t)gmv_cr_vol_event, tx = (uint32_t)(uintptr_t)gmv_cr_vol_texts;
        patch_bytes(CR_VOL_VT + 5 * 4, (const unsigned char *)&ev, 4);
        patch_bytes(CR_VOL_VT + 6 * 4, (const unsigned char *)&tx, 4);
    }
    if (!cr_logo_hook())
        gmv_log("[gamepatch] the HUD radio's logo code is not the expected one; no logo for the sixth station\n");
    if (want)
        custom_radio_set(1);
    gmv_log("[gamepatch] CUSTOM_RADIO %s: a sixth radio station for the songs in the \"custom radio\" folder, "
            "switched on the SOUND page (Custom Radio)\n", want ? "on" : "off");
}
