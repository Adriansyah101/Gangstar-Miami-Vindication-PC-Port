/* cutscene_keys.c -- CUTSCENE_KEYS: cutscenes with PC keys */
#include "compat.h"
#include <stdint.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* ---- CUTSCENE_KEYS: cutscenes with PC keys and nothing touch-only on screen (an enhancement the user
   asked for; off by default and under ORIGINAL_MAC_EXPERIENCE) ----
   During a cutscene the HUD shows widgetSkipScene (menu widget 43, vtable 0x658e88: show 0x16fa34
   sets [w+0x10], its texts slot 6 0x16fbee): the letterbox bars, the SKIP button btnSkip and, while
   a dialog line waits, the arrows (the app's 0x15ebfa shows Texts.dialogArrow and whiteArrows,
   0x15eb48 hides them).  A click on SKIP calls the cutscene's listener [w+0xc]->vt[0x3c](l, w, 1)
   (0x16fb70); a dialog line (update 0xce02c) waits while [w+0x18] is set, which a press on the
   arrows clears.  The cutscene start also shows the game's own pointer (CursorGame +4, 0x181d14
   at 0x36a86).  With the setting on:
   - the texts slot hides btnSkip after the game's texts, and the dialog's arrow show (0xcde01)
     hides the arrows instead (the two dialog hides, 0xce020 / 0xce34c, end the line);
   - the cutscene start leaves the pointer off;
   - Esc or Enter does what the SKIP click does, Space clears [w+0x18] while a line waits.  The keys
     come from shimsinput.c's key events (gmv_cutscene_key, which keeps them from the game while a
     cutscene is up) and act in the next input pump (gmv_cutscene_pump), on the game's thread.
   The hooks are in from the start; the setting switches live. */
#define CS_WIDGET      43                       /* widgetSkipScene in the menu's widget list */

void cs_btn_visible(void *fx, int on)
{
    ((void (*)(void *, const char *, int))(uintptr_t)CR_VISIBLE)(fx, "widgetSkipScene.btnSkip", on);
}

const uint32_t cs_hide_sites[] = { 0x000ce020u, 0x000ce34cu };
volatile int g_cs_on, g_cs_hooked;
volatile int g_cs_line;                  /* a dialog line waits for its tap */
volatile int g_cs_skip, g_cs_next;       /* keys waiting for the next pump */

unsigned char *cs_widget(void)
{
    unsigned char *app = (unsigned char *)(uintptr_t)CS_APP;     /* read, not built: the keys can come early */
    if (!*(const volatile unsigned char *)(uintptr_t)CS_APP_GUARD || *(int *)(app + 0x218) <= CS_WIDGET)
        return NULL;
    return (*(unsigned char ***)(app + 0x214))[CS_WIDGET];
}

__attribute__((cdecl, force_align_arg_pointer))
void gmv_cs_texts(unsigned char *w)
{
    ((void (*)(void *))(uintptr_t)CS_TEXTS)(w);
    void *fx = *(void **)(w + 8);
    if (fx)
        cs_btn_visible(fx, !g_cs_on);
}

__attribute__((cdecl, force_align_arg_pointer))
void gmv_cs_arrow_show(void *app)
{
    g_cs_line = 1;
    ((void (*)(void *))(uintptr_t)(g_cs_on ? CS_ARROW_HIDE : CS_ARROW_SHOW))(app);
}

__attribute__((cdecl, force_align_arg_pointer))
void gmv_cs_arrow_hide(void *app)
{
    g_cs_line = 0;
    ((void (*)(void *))(uintptr_t)CS_ARROW_HIDE)(app);
}

__attribute__((cdecl, force_align_arg_pointer))
void gmv_cs_cursor(void *cursor, int on)
{
    ((void (*)(void *, int))(uintptr_t)CS_CURSOR_SET)(cursor, on && !g_cs_on);
}
