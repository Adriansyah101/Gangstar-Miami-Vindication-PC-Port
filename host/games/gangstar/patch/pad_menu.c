/* pad_menu.c -- PAD_MENU: the pad works the menus */
#include "compat.h"
#include <stdint.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* ---- PAD_MENU (menu: PAD MENU POINTER): the pad works the menus (an enhancement the user asked for on
   2026-10-30; off by default and under ORIGINAL_MAC_EXPERIENCE) ----
   The Mac's menus are mouse only: the menu states listen to the pad's events but act only on Esc
   (e.g. 0x1b1d22), and the gameswf pages take hover and click events from the pointer.  The yellow
   pointer is the HUD's CursorGame ([[0x63e038]+0xd8]), drawn at the mouse (0x181d70) while its +4
   (set by 0x181d14: title, language page, pause 0x1a16ac, cutscenes) or +5 is set.  While it shows,
   shimsinput.c moves it with the pad's left stick / D-pad and clicks with A, as a mouse would. */
static int g_pm_on;

int pm_set(int on)
{
    if (on != g_pm_on)
        gmv_log("[gamepatch] PAD_MENU %s\n", on ? "on" : "off");
    g_pm_on = on;
    return 1;
}

/* The game shows its pointer: a menu is up, or a cutscene. */
int pointer_shown(void)
{
    const unsigned char *g1, *cur;
    g1 = *(const unsigned char *const *)(uintptr_t)*(const uint32_t *)(uintptr_t)0x63e038u;
    cur = g1 ? *(const unsigned char *const *)(g1 + 0xd8) : NULL;
    return cur && (cur[4] || cur[5]);
}

/* For the host's mouse: the pointer shows, or the game has not got that far.  (A menu's or cutscene's camera
   does not follow the mouse, so nothing is lost by the mouse being a normal one then.) */
int gmv_pointer_up(void)
{
    const uint32_t *cell = (const uint32_t *)(uintptr_t)*(const uint32_t *)(uintptr_t)0x63e038u;
    const unsigned char *g1 = cell ? (const unsigned char *)(uintptr_t)*cell : NULL;
    const unsigned char *cur = g1 ? *(const unsigned char *const *)(g1 + 0xd8) : NULL;
    return !cur || cur[4] || cur[5];
}

int gmv_pad_menu_active(void)
{
    return g_pm_on && pointer_shown();
}

void apply_pad_menu(void)
{
    const char *e = gmv_getenv("GMV_PAD_MENU");
    g_pm_on = e && *e && *e != '0';
    if (g_pm_on)
        gmv_log("[gamepatch] PAD_MENU on: the pad moves the menu pointer (stick / D-pad), A clicks, B = Esc\n");
}
