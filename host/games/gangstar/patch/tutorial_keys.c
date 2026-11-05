/* tutorial_keys.c -- TUTORIAL_KEYS: popups with PC keys */
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

#define CS_VT          0x00658e88u
#define CS_SHOW_SITE   0x000cde01u              /* the dialog line's arrow show */
#define CS_CURSOR_SITE 0x00036a86u              /* the cutscene start's CursorGame show */

/* ---- TUTORIAL_KEYS (menu: POPUP KEYS): Esc, Enter or Space presses the OK of a tutorial popup, and
   Enter/Space = YES, Esc = NO in a question popup (an enhancement the user asked for; off by default
   and under ORIGINAL_MAC_EXPERIENCE) ----
   The popup ("Combat ... OK") is widgetTutoBlock, menu widget 25 (Widgets::BlockingTutorial, vtable
   0x658e28, built at 0x15f725; show 0x16f748 sets [w+0x10], the base hide 0x16e978 clears it).  Its
   OK click (event slot 5, 0x16f842) plays sound 0x57, drops the popup's input listeners and calls
   [w+0xc]->vt[0x3c](l, w, 1).  Its keyboard listener 0x17eaac (called through slot 7, 0x16f9bc) does
   the same for an event of kind 2 and code 0x1b, so a key here calls that with such an event. */
#define TK_WIDGET      25
#define TK_VT          0x00658e28u
#define TK_INPUT       0x0016f9bcu              /* slot 7 */
#define TK_ESC         0x0017eaacu              /* (w, event): kind [ev+8] == 2, code [ev+0xc] == 0x1b */
/* The question popups ("Repair? ... YES / NO") are the dialog widgets: show 0x17d430 sets [w+0x10],
   the click handler 0x17d494 (slot 5) stores the button's answer at [w+0x18] (btnYes 0xb, btnNo 0xd)
   and calls [w+0xc]->vt[0x3c](l, w, 1).  The YES/NO kinds differ only in their texts slot 6:
   0x659a88 (0x17243a) and widgetConfirm 0x659ac8 (0x17da90, the price in blue); 0x659a48
   (0x172324) has one button, btnYes, labelled OK.  With the setting on, Enter or Space answers YES
   and Esc answers NO (the OK one: any of the three), found by a walk of the menu's widgets. */
#define TK_DLG_EVENT   0x0017d494u
#define TK_ANS_YES     0xb
#define TK_ANS_NO      0xd

static const uint32_t tk_dlg_vt[3][2] = {       /* vtable, its texts slot; [0] is the one-button kind */
    { 0x00659a48u, 0x00172324u }, { 0x00659a88u, 0x0017243au }, { 0x00659ac8u, 0x0017da90u },
};

volatile int g_tk_on, g_tk_ok, g_tk_press, g_tk_answer;

/* A question popup up now (or NULL); *one = it has the one button. */
static unsigned char *tk_dialog(int *one)
{
    unsigned char *app = (unsigned char *)(uintptr_t)CS_APP;
    if (!g_tk_ok || !*(const volatile unsigned char *)(uintptr_t)CS_APP_GUARD)
        return NULL;
    int n = *(int *)(app + 0x218);
    unsigned char **list = *(unsigned char ***)(app + 0x214);
    for (int i = 0; list && i < n; i++) {
        unsigned char *w = list[i];
        if (!w || !w[0x10] || !*(void **)(w + 0xc))
            continue;
        for (int k = 0; k < 3; k++)
            if (*(uint32_t *)w == tk_dlg_vt[k][0]) {
                *one = k == 0;
                return w;
            }
    }
    return NULL;
}

static unsigned char *tk_widget(void)
{
    unsigned char *app = (unsigned char *)(uintptr_t)CS_APP, *w;
    if (!g_tk_ok || !*(const volatile unsigned char *)(uintptr_t)CS_APP_GUARD || *(int *)(app + 0x218) <= TK_WIDGET)
        return NULL;
    w = (*(unsigned char ***)(app + 0x214))[TK_WIDGET];
    return w && *(uint32_t *)w == TK_VT ? w : NULL;
}

int tk_set(int on)
{
    if (!g_tk_ok)
        return 0;
    if (on != g_tk_on)
        gmv_log("[gamepatch] TUTORIAL_KEYS %s\n", on ? "on" : "off");
    g_tk_on = on;
    return 1;
}

void apply_tutorial_keys(void)
{
    const char *e = gmv_getenv("GMV_TUTORIAL_KEYS");
    if (gmv_config_mac())
        return;
    int ok = ((const uint32_t *)(uintptr_t)TK_VT)[7] == TK_INPUT;
    for (int k = 0; k < 3; k++)
        ok &= ((const uint32_t *)(uintptr_t)tk_dlg_vt[k][0])[5] == TK_DLG_EVENT &&
              ((const uint32_t *)(uintptr_t)tk_dlg_vt[k][0])[6] == tk_dlg_vt[k][1];
    if (!ok) {
        gmv_log("[gamepatch] the popups' code is not the expected one; no TUTORIAL_KEYS\n");
        return;
    }
    g_tk_ok = 1;
    g_tk_on = e && *e && *e != '0';
    gmv_log("[gamepatch] TUTORIAL_KEYS %s: Esc, Enter or Space presses a tutorial popup's OK; Enter/Space = YES, "
            "Esc = NO in question popups\n", g_tk_on ? "on" : "off");
}

/* shimsinput.c, on a key going down: 1 if it is ours (a cutscene or a tutorial popup is up and it is
   Esc, Enter or Space), so the game does not get it. */
int gmv_cutscene_key(unsigned sym)
{
    enum { KEY_ESC = 0x1b, KEY_ENTER = '\r', KEY_SPACE = ' ' };
    unsigned char *w;
    if (sym != KEY_ESC && sym != KEY_ENTER && sym != KEY_SPACE)
        return 0;
    if (g_tk_on && (w = tk_widget()) && w[0x10] && *(void **)(w + 0xc)) {
        g_tk_press = 1;
        return 1;
    }
    int one;
    if (g_tk_on && tk_dialog(&one)) {
        g_tk_answer = one || sym != KEY_ESC ? TK_ANS_YES : TK_ANS_NO;
        return 1;
    }
    if (!g_cs_on || !g_cs_hooked)
        return 0;
    if (!(w = cs_widget()) || !w[0x10])
        return 0;
    if (sym == KEY_SPACE)
        g_cs_next = 1;
    else
        g_cs_skip = 1;
    return 1;
}

/* shimsinput.c, after each input pump: what the keys asked for. */
__attribute__((force_align_arg_pointer))
void gmv_cutscene_pump(void)
{
    unsigned char *w;
    int skip = g_cs_skip, next = g_cs_next;
    if (g_tk_press) {
        g_tk_press = 0;
        if (g_tk_on && (w = tk_widget()) && w[0x10] && *(void **)(w + 0xc)) {
            int ev[4] = { 0, 0, 2, 0x1b };          /* only kind and code are read */
            ((void (*)(void *, void *))(uintptr_t)TK_ESC)(w, ev);
            gmv_trace("[gamepatch] tutorial popup closed by a key\n");
        }
    }
    if (g_tk_answer) {
        int ans = g_tk_answer, one;
        g_tk_answer = 0;
        if (g_tk_on && (w = tk_dialog(&one))) {
            void **l = *(void ***)(w + 0xc);
            if (one)
                ans = TK_ANS_YES;
            *(int *)(w + 0x18) = ans;               /* as the button's click does */
            ((void (*)(void *, void *, int))(*(uintptr_t **)l)[0x3c / 4])(l, w, 1);
            gmv_trace("[gamepatch] question popup answered %s by a key\n", ans == TK_ANS_NO ? "NO" : "YES");
        }
    }
    if (!skip && !next)
        return;
    g_cs_skip = g_cs_next = 0;
    if (!g_cs_on || !(w = cs_widget()) || !w[0x10])
        return;
    if (skip) {
        void **l = *(void ***)(w + 0xc);
        if (l) {
            ((void (*)(void *, void *, int))(*(uintptr_t **)l)[0x3c / 4])(l, w, 1);
            gmv_trace("[gamepatch] cutscene skipped by a key\n");
        }
    } else if (g_cs_line && w[0x18])
        w[0x18] = 0;                            /* as a press on the arrows does */
}

int cs_set(int on)
{
    if (!g_cs_hooked)
        return 0;
    if (on != g_cs_on)
        gmv_log("[gamepatch] CUTSCENE_KEYS %s\n", on ? "on" : "off");
    g_cs_on = on;
    unsigned char *w = cs_widget();
    if (w && *(void **)(w + 8))
        cs_btn_visible(*(void **)(w + 8), !on);
    return 1;
}

void apply_cutscene_keys(void)
{
    const char *e = gmv_getenv("GMV_CUTSCENE_KEYS");
    uint32_t tx = (uint32_t)(uintptr_t)gmv_cs_texts;
    int ok = 1;
    if (gmv_config_mac())
        return;                                 /* the Mac's: the hooks come with the switch */
    ok &= ((const uint32_t *)(uintptr_t)CS_VT)[6] == CS_TEXTS && sm_is_call(CS_SHOW_SITE, CS_ARROW_SHOW) &&
          sm_is_call(CS_CURSOR_SITE, CS_CURSOR_SET);
    for (size_t i = 0; i < sizeof cs_hide_sites / sizeof *cs_hide_sites; i++)
        ok &= sm_is_call(cs_hide_sites[i], CS_ARROW_HIDE);
    if (!ok) {
        gmv_log("[gamepatch] the cutscene widget's code is not the expected one; no CUTSCENE_KEYS\n");
        return;
    }
    ok = patch_bytes(CS_VT + 6 * 4, (const unsigned char *)&tx, 4) && sm_redirect(CS_SHOW_SITE, (void *)gmv_cs_arrow_show) &&
         sm_redirect(CS_CURSOR_SITE, (void *)gmv_cs_cursor);
    for (size_t i = 0; ok && i < sizeof cs_hide_sites / sizeof *cs_hide_sites; i++)
        ok = sm_redirect(cs_hide_sites[i], (void *)gmv_cs_arrow_hide);
    if (!ok) {
        gmv_log("[gamepatch] a write failed (%lu); CUTSCENE_KEYS partly in\n", GetLastError());
        return;
    }
    g_cs_hooked = 1;
    g_cs_on = e && *e && *e != '0';
    gmv_log("[gamepatch] CUTSCENE_KEYS %s: no SKIP button, dialog arrows or pointer in cutscenes; Esc/Enter skip, "
            "Space for the next line\n", g_cs_on ? "on" : "off");
}
