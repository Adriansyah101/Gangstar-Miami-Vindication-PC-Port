/* pad_hints.c -- PAD_HINTS: tutorial boxes name the pad's buttons */
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

/* ---- PAD_HINTS (menu: PAD BUTTON HINTS): the in-game tutorial boxes name the pad's buttons while the
   pad is the last thing used (an enhancement the user asked for on 2026-10-30; off by default and
   under ORIGINAL_MAC_EXPERIENCE) ----
   A tutorial's text comes from 0x1902a4(tutorials, title id, std::string *out), called once at
   0x16ee2d: the template is the string id title + 1 (GLOBAL_TUTO_CONTENT_*; the files' "\0" "\1" marks,
   held as <font color="#FFFF33"> </font> once loaded, mark the yellow
   parts), filled with the keyboard's key names for the actions it needs (0x182a48 through the
   keyboard schemes [menu mgr+0x1f0..0x1fc]).  With the pad last used (a pad press or a stick push
   after the last key press or mouse click), ours writes the text with the pad's button names instead:
   GTA-LIKE's from pl_map, the Mac layout's from the pad schemes [menu mgr+0x204..0x210] (button ->
   logical id at +0x20, logical id -> action at +0x38; the controls page's rows).  The texts are
   English (several originals talk of the mouse); in another language, or when an action has no
   button, the game's own keyboard text stays. */
#define PH_SITE      0x0016ee2du     /* call 0x1902a4 (tutorials, title, out)                       */
#define PH_FN        0x001902a4u
#define PH_ASSIGN    0x005edde0u     /* std::string::assign(const char *) (the dyld stub)           */
#define PH_MGR       0x00a064a0u     /* the menu manager (0x15e4fa's static) and its guard          */
#define PH_MGR_GUARD 0x00a066d8u
int g_ph_on, g_ph_ok;

int ph_set(int on)
{
    if (on != g_ph_on)
        gmv_log("[gamepatch] PAD_HINTS %s\n", on ? "on" : "off");
    g_ph_on = on;
    return g_ph_ok || !on;
}

/* In-order successor in a libstdc++ red-black tree (node: +4 parent, +8 left, +0xc right). */
static const unsigned char *ph_next(const unsigned char *n)
{
    const unsigned char *p;
    if (*(const unsigned char *const *)(n + 0xc)) {
        n = *(const unsigned char *const *)(n + 0xc);
        while (*(const unsigned char *const *)(n + 8))
            n = *(const unsigned char *const *)(n + 8);
        return n;
    }
    p = *(const unsigned char *const *)(n + 4);
    while (n == *(const unsigned char *const *)(p + 0xc)) {
        n = p;
        p = *(const unsigned char *const *)(p + 4);
    }
    return *(const unsigned char *const *)(n + 0xc) != p ? p : n;
}

/* The Mac layout's pad button (0-11) for an action of a kind, or -1. */
static int ph_mac_button(int kind, int action)
{
    const unsigned char *mgr = (const unsigned char *)(uintptr_t)PH_MGR, *sc = NULL, *n, *end;
    int logical = -1;
    if (!*(const unsigned char *)(uintptr_t)PH_MGR_GUARD)
        return -1;
    for (int i = 0; i < 4; i++) {
        const unsigned char *s = *(const unsigned char *const *)(mgr + 0x204 + 4 * i);
        if (s && *(const uint32_t *)s == pl_scheme_vt[kind])
            sc = s;
    }
    if (!sc)
        return -1;
    for (end = sc + 0x38, n = *(const unsigned char *const *)(sc + 0x40); n != end; n = ph_next(n))
        if (*(const int *)(n + 0x14) == action) {
            logical = *(const int *)(n + 0x10);
            break;
        }
    if (logical < 0)
        return -1;
    for (end = sc + 0x20, n = *(const unsigned char *const *)(sc + 0x28); n != end; n = ph_next(n))
        if (*(const int *)(n + 0x14) == logical)
            return *(const int *)(n + 0x10) < 12 ? *(const int *)(n + 0x10) : -1;
    return -1;
}

/* The button (0-11, 12-15 the D-pad's up right down left) that does an action, or -1. */
static int ph_button(int kind, int action)
{
    if (g_pl_on) {
        for (int b = 0; b < PL_BTNS; b++)
            if (pl_map[kind][b] == action)
                return b;
        return -1;
    }
    return ph_mac_button(kind, action);
}

static const char *ph_name(int b)
{
    static const char *const xbox[16] = { "X", "A", "B", "Y", "LB", "RB", "LT", "RT", "View", "Menu", "LS", "RS",
                                          "D-pad Up", "D-pad Right", "D-pad Down", "D-pad Left" };
    static const char *const ps[16] = { "Square", "Cross", "Circle", "Triangle", "L1", "R1", "L2", "R2", "Share",
                                        "Options", "L3", "R3", "D-pad Up", "D-pad Right", "D-pad Down", "D-pad Left" };
    int st = gmv_joy_style();
    if (b < 0 || b > 15)
        return NULL;
    if (st == 3 && b == 8)
        return "Create";
    return st >= 2 ? ps[b] : xbox[b];
}

/* The name of the first action of `acts` (0-ended) with a button, or NULL. */
static const char *ph_act(int kind, const int *acts)
{
    for (; *acts; acts++) {
        int b = ph_button(kind, *acts);
        if (b >= 0)
            return ph_name(b);
    }
    return NULL;
}

#define PH_A(...) ((const int[]){ __VA_ARGS__, 0 })

/* The highlight marks as the game holds them once loaded (the "\0" "\1" of the text files). */
#define PH_HL  "<font color=\"#FFFF33\">"
#define PH_END "</font>"

/* Our text for a tutorial, or 0 to keep the game's. */
static int ph_text(int title, char *t, size_t n)
{
    const char *a, *b, *c, *d;
    switch (title) {
    case 0x3079:                                                    /* moving */
        if (!(a = ph_act(PL_FOOT, PH_A(0x12))))
            return 0;
        snprintf(t, n, "Use the " PH_HL "Left Stick" PH_END " to walk and press " PH_HL "%s" PH_END " to run.", a);
        return 1;
    case 0x307b:                                                    /* the mini-map */
        if (!(a = ph_act(PL_FOOT, PH_A(0xb))))
            return 0;
        snprintf(t, n, "If you can't see the spot you should go to, you can always refer to the mini-map. "
                       "You can even press " PH_HL "%s" PH_END " to enlarge it.", a);
        return 1;
    case 0x307d:                                                    /* get in a car */
        if (!(a = ph_act(PL_FOOT, PH_A(3, 5))))
            return 0;
        snprintf(t, n, "Press " PH_HL "%s" PH_END " to enter or exit a vehicle.", a);
        return 1;
    case 0x307f:                                                    /* driving */
        if (!(a = ph_act(PL_CAR, PH_A(0x27, 0x12))) || !(b = ph_act(PL_CAR, PH_A(0x28, 0x11))))
            return 0;
        snprintf(t, n, "When driving, use " PH_HL "%s" PH_END " to accelerate, " PH_HL "%s" PH_END " to brake/reverse, and the "
                       "" PH_HL "Left Stick" PH_END " to turn left/right.", a, b);
        return 1;
    case 0x3081:                                                    /* the helicopter */
        if (!(a = ph_act(PL_FLY, PH_A(0x27))) || !(b = ph_act(PL_FLY, PH_A(0x28))) ||
            !(c = ph_act(PL_FLY, PH_A(0x29))) || !(d = ph_act(PL_FLY, PH_A(0x2a))))
            return 0;
        snprintf(t, n, "Use " PH_HL "%s" PH_END " and " PH_HL "%s" PH_END " to adjust the altitude and " PH_HL "%s" PH_END " and " PH_HL "%s" PH_END " to rotate "
                       "the helicopter. Use the " PH_HL "Left Stick" PH_END " to move the helicopter.", a, b, c, d);
        return 1;
    case 0x3083:                                                    /* drive-by */
        if (!(a = ph_act(PL_CAR, PH_A(1))))
            return 0;
        b = g_pl_on ? ph_act(PL_CAR, PH_A(PL_AIM)) : NULL;
        if (b)
            snprintf(t, n, "You can target and shoot thugs from the car just like you would on foot. Press " PH_HL "%s" PH_END " "
                           "to shoot and hold " PH_HL "%s" PH_END " to lock onto targets.", a, b);
        else
            snprintf(t, n, "You can target and shoot thugs from the car just like you would on foot. Press " PH_HL "%s" PH_END " "
                           "to shoot.", a);
        return 1;
    case 0x3085:                                                    /* guns */
        if (!(a = ph_act(PL_FOOT, PH_A(1))))
            return 0;
        b = g_pl_on ? ph_act(PL_FOOT, PH_A(PL_AIM)) : NULL;
        if (b)
            snprintf(t, n, "Use the " PH_HL "Right Stick" PH_END " to aim and press " PH_HL "%s" PH_END " to shoot.\n"
                           "You can also hold " PH_HL "%s" PH_END " to lock onto targets.", a, b);
        else
            snprintf(t, n, "Use the " PH_HL "Right Stick" PH_END " to aim and press " PH_HL "%s" PH_END " to shoot.", a);
        return 1;
    case 0x3087:                                                    /* punching */
        if (!(a = ph_act(PL_FOOT, PH_A(1))))
            return 0;
        snprintf(t, n, "Press " PH_HL "%s" PH_END " to punch. The closest target will be automatically selected.", a);
        return 1;
    case 0x3089:                                                    /* shops */
        if (!(a = ph_act(PL_FOOT, PH_A(4, 5))))
            return 0;
        snprintf(t, n, "Yellow markers are shops. Press " PH_HL "%s" PH_END " to enter.\n"
                       "Shops are also available in your " PH_HL "Service" PH_END " menu, with some shipping fees.", a);
        return 1;
    case 0x308b:                                                    /* weapons */
        if (!(a = ph_act(PL_FOOT, PH_A(10))) || !(b = ph_act(PL_FOOT, PH_A(9))))
            return 0;
        snprintf(t, n, "Press " PH_HL "%s" PH_END " or " PH_HL "%s" PH_END " to browse through available weapons.", a, b);
        return 1;
    case 0x308d:                                                    /* the radio */
        if (!(a = ph_act(PL_CAR, PH_A(0xd))) || !(b = ph_act(PL_CAR, PH_A(0xe))))
            return 0;
        snprintf(t, n, "You can press " PH_HL "%s" PH_END " and " PH_HL "%s" PH_END " to change radio station.", a, b);
        return 1;
    case 0x309d:                                                    /* the camera / sniper view */
        if (!(a = ph_act(PL_SNIPER, PH_A(10))) || !(b = ph_act(PL_SNIPER, PH_A(9))) ||
            !(c = ph_act(PL_SNIPER, PH_A(1))) || !(d = ph_act(PL_SNIPER, PH_A(0xf))))
            return 0;
        snprintf(t, n, "Use " PH_HL "%s" PH_END " and " PH_HL "%s" PH_END " to zoom in and out. Press " PH_HL "%s" PH_END " to take a picture and "
                       "" PH_HL "%s" PH_END " to return to normal view mode.", a, b, c, d);
        return 1;
    case 0x309f:                                                    /* cover */
        if (!(a = ph_act(PL_FOOT, PH_A(0x11))))
            return 0;
        snprintf(t, n, "You can press " PH_HL "%s" PH_END " to take cover behind certain objects. You can still shoot or "
                       "walk while in cover. Press " PH_HL "%s" PH_END " again to exit.", a, a);
        return 1;
    case 0x30a1:                                                    /* grenades */
        if (!(a = ph_act(PL_FOOT, PH_A(1))))
            return 0;
        snprintf(t, n, "" PH_HL "Hold" PH_END " and " PH_HL "release" PH_END " " PH_HL "%s" PH_END " to throw a grenade. The " PH_HL "longer" PH_END " you hold, the "
                       "" PH_HL "further" PH_END " you'll throw the grenade.", a);
        return 1;
    case 0x30a3:                                                    /* wheelies */
        if (g_pl_on) {
            snprintf(t, n, "Pull the " PH_HL "Left Stick" PH_END " back or push it forward while riding to do tricks like "
                           "wheelies or stoppies.");
            return 1;
        }
        if (!(a = ph_act(PL_CAR, PH_A(0x12))) || !(b = ph_act(PL_CAR, PH_A(0x11))))
            return 0;
        snprintf(t, n, "You can press " PH_HL "%s" PH_END " or " PH_HL "%s" PH_END " to do tricks like wheelies or stoppies.", a, b);
        return 1;
    }
    return 0;
}

/* The game's English moving text reads like this (marks already turned into font tags); another
   language keeps the game's texts. */
static int ph_english(void)
{
    static const char want[] = "Use " PH_HL "%s" PH_END " to walk";
    const char *s = (const char *)(uintptr_t)pl_call2(PL_LABEL_FN,
                        *(const uint32_t *)(uintptr_t)(*(const uint32_t *)(uintptr_t)0x006853a0u + 0x17ac4), 0x307a);
    static int told;
    int ok = s && !strncmp(s, want, sizeof want - 1);
    if (!ok && !told) {
        told = 1;
        gmv_trace("[padhints] the moving text reads \"%.40s\"\n", s ? s : "(none)");
    }
    return ok;
}

__attribute__((cdecl, force_align_arg_pointer))
static int gmv_ph_text(void *tuto, int title, void *out)
{
    char t[640];
    unsigned pad = gmv_joy_last_use(), kbm = gmv_input_last_key(), now = gmv_input_tick();
    const char *why = !g_ph_on ? "PAD BUTTON HINTS is off"
                    : !pad ? "no pad use seen yet"
                    : kbm && (int)(pad - kbm) <= 0 ? "a key or mouse click came after the pad"
                    : !ph_english() ? "the game's texts are not English"
                    : !ph_text(title, t, sizeof t) ? "no pad text for it (or an action without a button)" : NULL;
    if (!why) {
        pl_call2(PH_ASSIGN, (uint32_t)(uintptr_t)out, (uint32_t)(uintptr_t)t);
        gmv_trace("[padhints] tutorial 0x%x with the pad's buttons\n", title);
        return 1;
    }
    /* each tutorial shows once, so a line per box is cheap; it says why the keyboard text stayed */
    gmv_trace("[padhints] tutorial 0x%x keeps the keyboard text: %s (pad used %d ms ago, key/click %d ms ago)\n",
            title, why, pad ? (int)(now - pad) : -1, kbm ? (int)(now - kbm) : -1);
    return (unsigned char)pl_call3(PH_FN, (uint32_t)(uintptr_t)tuto, (uint32_t)title, (uint32_t)(uintptr_t)out);
}

void apply_pad_hints(void)
{
    const char *e = gmv_getenv("GMV_PAD_HINTS");
    g_ph_on = e && *e && *e != '0';
    if (gmv_config_mac())
        return;
    if (!sm_is_call(PH_SITE, PH_FN) || !sm_redirect(PH_SITE, (void *)gmv_ph_text)) {
        gmv_log("[gamepatch] the tutorial text call is not the expected one; no PAD_HINTS\n");
        return;
    }
    g_ph_ok = 1;
    gmv_log("[gamepatch] PAD_HINTS %s (tutorials name the pad's buttons while the pad is in use)\n",
            g_ph_on ? "on" : "off");
}
