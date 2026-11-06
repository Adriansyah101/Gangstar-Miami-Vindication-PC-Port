/* present_hold.c -- MENUFLASH_FIX, and the start-up flicker diagnostic */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"
#include <GL/gl.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "render.h"

/* Diagnostic for the start-up flicker (GMV_FLICKER_CAP=1).  Armed when the rating splash (top
   state vtable 0x65a388) or the title/menu state (0x659f28, "press any key" and the main menu)
   comes to the top of the game's state stack, it reads back every presented frame for 60 s after
   the last arming, subsampled to at most 480 px wide, and keeps the last three.  A frame unlike
   both neighbours while they are alike (A B A), much darker or brighter than both (a flash), or
   sharply different from both (A B C, a one-frame shot between two cuts) is saved with its neighbours as gmv_flick_e<n>_{1,2,3}_f<frame>.bmp; every 30th presented frame is
   saved as gmv_flick_ctx_f<frame>.bmp for context.  Each presented frame logs a [flickcap] line:
   ticks held back since the last present, clears and draws, mean brightness, and the largest
   change of a 16x9-cell mean from the previous frame.  Images go to the game's cwd. */
#define CINE_FADE_TIMER 0x006424b0u   /* cutscene fader: ms left, -999 idle (CineFadeIn/Out 0xcd8be/0xcda6c) */
#define CINE_FADE_ALPHA 0x006424b4u   /* black overlay alpha 0..255, -999 = none drawn (0x34918)            */
#if GMV_DEV
#define FLK_MS 60000u
#define FLK_W 480
#define FLK_GX 16
#define FLK_GY 9
typedef struct { unsigned char *px; unsigned frame; int mean, cell[FLK_GX * FLK_GY]; } flk_frame;
static flk_frame g_flk[3];
static int g_flk_ow, g_flk_oh, g_flk_row, g_flk_n, g_flk_i;   /* g_flk_i = slot of the newest */
static DWORD g_flk_until, g_flk_t0;
static uint32_t g_flk_vt;
static unsigned g_flk_presented, g_flk_held, g_flk_events, g_flk_saved;
#endif

/* vtable of the state on top of the game's state stack (App [0x6853a0] -> manager [+0x17ac0] ->
   stack base [+0xc], top [+0x10]; see state_trace in shimsinput.c), 0 when there is none. */
uint32_t gmv_top_state_vtable(void)
{
    const unsigned char *app = *(const unsigned char *const *)(uintptr_t)0x6853a0u;
    const unsigned char *mgr = app ? *(const unsigned char *const *)(app + 0x17ac0) : NULL;
    if (!mgr)
        return 0;
    uint32_t base = *(const uint32_t *)(mgr + 0xc), top = *(const uint32_t *)(mgr + 0x10);
    if (!base || top - base < 4)
        return 0;
    const uint32_t *st = *(const uint32_t *const *)(uintptr_t)base;
    return st ? *st : 0;
}

#if GMV_DEV
static int flk_diff(const flk_frame *a, const flk_frame *b)
{
    int m = 0;
    for (int i = 0; i < FLK_GX * FLK_GY; i++) {
        int d = abs(a->cell[i] - b->cell[i]);
        if (d > m) m = d;
    }
    return m;
}

static void flk_save(const flk_frame *fr, const char *tag)
{
    char path[96];
    snprintf(path, sizeof path, "gmv_flick_%s_f%u.bmp", tag, fr->frame);
    write_bmp24(path, g_flk_ow, g_flk_oh, g_flk_row, fr->px);
    g_flk_saved++;
}
#endif

/* Main-menu entry flash (original game; GMV_MENUFLASH_FIX, default on).  Pressing a key on the
   title (state vtable 0x65a388) switches to the main-menu state (0x659f28), whose 3D background is a
   scripted camera flight that opens with CineFadeIn.  The game draws two frames before the script
   runs its fades: the camera still at its default spot (facing a building) and no black overlay.
   The script opens with a 1 ms CineFadeOut, so the first frame with the fader running has alpha 0
   (transparent) and the next one 255; the raw scene therefore flashes between the title and the
   fade from black -- ~66 ms at 60 ticks/s, the same frames on the Mac.  From that switch, frames
   are drawn as normal but not presented until the overlay is fully black (alpha 255), so the title
   stays up until the first black frame.  Capped at 300 ms in case a fade never comes.  Returns 1
   to skip this present. */
#define MF_CAP_MS 300u
static int g_menuflash = -1;                            /* MENUFLASH_FIX; the options menu sets it */

void gmv_set_menuflash_fix(int on)
{
    g_menuflash = on;
}

int menu_flash_hold(unsigned frame, uint32_t vt, int drew)
{
    static int holding;
    static uint32_t prev_vt;
    static DWORD t0;
    static unsigned held;
    if (g_menuflash < 0) { const char *e = gmv_getenv("GMV_MENUFLASH_FIX"); g_menuflash = !(e && *e == '0'); }
    if (vt != prev_vt) {
        if (g_menuflash && prev_vt == 0x65a388u && vt == 0x659f28u) {
            holding = 1;
            t0 = GetTickCount();
            held = 0;
            gmv_trace("[menuflash] f%u title -> main menu: holding the title until the menu fade starts\n", frame);
        }
        prev_vt = vt;
    }
    if (!holding)
        return 0;
    int alpha = *(const int *)(uintptr_t)CINE_FADE_ALPHA;
    DWORD el = GetTickCount() - t0;
    if (alpha >= 255 || el > MF_CAP_MS) {
        holding = 0;
        if (alpha >= 255)
            gmv_trace("[menuflash] f%u released after %u held frames, %lu ms: screen black (alpha %d)\n", frame, held,
                    (unsigned long)el, alpha);
        else
            gmv_trace("[menuflash] f%u released after %u held frames: no black within %u ms (alpha %d)\n", frame, held,
                    MF_CAP_MS, alpha);
        return 0;
    }
    held += drew ? 1u : 0u;
    return 1;
}

#if GMV_DEV
void flicker_cap(unsigned frame, int presented, uint32_t vt)
{
    static int on = -1;
    static unsigned char *full;
    static size_t full_sz;
    if (on < 0) { const char *e = gmv_dbgenv("GMV_FLICKER_CAP"); on = e && *e && *e != '0'; }
    if (!on)
        return;
    DWORD t = GetTickCount();
    if (vt != g_flk_vt) {
        gmv_log("[flickcap] f%u top state vtable 0x%06x -> 0x%06x\n", frame, g_flk_vt, vt);
        g_flk_vt = vt;
        if (vt == 0x65a388u || vt == 0x659f28u) {
            if (!g_flk_until)
                g_flk_t0 = t;
            g_flk_until = t + FLK_MS;
            gmv_log("[flickcap] armed until t=%lu (%s)\n", (unsigned long)g_flk_until,
                    vt == 0x65a388u ? "rating splash" : "title / main menu");
        }
    }
    if (!g_flk_until)
        return;
    if ((int)(t - g_flk_until) > 0) {
        gmv_log("[flickcap] window over: %u frames presented, %u suspect events, %u images saved\n",
                g_flk_presented, g_flk_events, g_flk_saved);
        g_flk_until = 0;
        return;
    }
    if (!presented) {
        g_flk_held++;
        if (vt == 0x659f28u && g_flk_presented < 400)
            gmv_log("[flickcap] f%u held back: clears=%u draws=%u fade alpha=%d timer=%d\n", frame, g_cap_clears,
                    g_cap_draws, *(const int *)(uintptr_t)CINE_FADE_ALPHA, *(const int *)(uintptr_t)CINE_FADE_TIMER);
        return;
    }
    int w = 0, h = 0;
    gmv_gl_client_size(&w, &h);
    if (w <= 0 || h <= 0)
        return;
    int step = (w + FLK_W - 1) / FLK_W, ow = w / step, oh = h / step;
    int inrow = (w * 3 + 3) & ~3, outrow = (ow * 3 + 3) & ~3;
    if (ow != g_flk_ow || oh != g_flk_oh) {             /* first use or a resolution change */
        for (int k = 0; k < 3; k++) {
            free(g_flk[k].px);
            g_flk[k].px = malloc((size_t)outrow * oh);
            if (!g_flk[k].px) { on = 0; return; }
        }
        g_flk_ow = ow; g_flk_oh = oh; g_flk_row = outrow; g_flk_n = 0;
    }
    if (full_sz < (size_t)inrow * h) {
        free(full);
        full_sz = (size_t)inrow * h;
        full = malloc(full_sz);
        if (!full) { on = 0; return; }
    }
    diag_read_begin(GL_BACK);
    glReadPixels(0, 0, w, h, GL_BGR_EXT, GL_UNSIGNED_BYTE, full);
    diag_read_end();

    g_flk_i = (g_flk_i + 1) % 3;
    flk_frame *cur = &g_flk[g_flk_i];
    long sum = 0, csum[FLK_GX * FLK_GY] = {0}, ccnt[FLK_GX * FLK_GY] = {0};
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            const unsigned char *s = full + (size_t)y * step * inrow + (size_t)x * step * 3;
            unsigned char *d = cur->px + (size_t)y * outrow + (size_t)x * 3;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
            int lum = s[0] + s[1] + s[2], c = (y * FLK_GY / oh) * FLK_GX + x * FLK_GX / ow;
            sum += lum; csum[c] += lum; ccnt[c]++;
        }
    cur->frame = frame;
    cur->mean = (int)(sum / ((long)ow * oh * 3));
    for (int c = 0; c < FLK_GX * FLK_GY; c++)
        cur->cell[c] = ccnt[c] ? (int)(csum[c] / (ccnt[c] * 3)) : 0;
    if (g_flk_n < 3)
        g_flk_n++;
    const flk_frame *p1 = &g_flk[(g_flk_i + 2) % 3], *p2 = &g_flk[(g_flk_i + 1) % 3];
    gmv_log("[flickcap] f%u +%lums held=%u clears=%u draws=%u mean=%d change=%d fade alpha=%d timer=%d\n", frame,
            (unsigned long)(t - g_flk_t0), g_flk_held, g_cap_clears, g_cap_draws, cur->mean,
            g_flk_n >= 2 ? flk_diff(cur, p1) : -1, *(const int *)(uintptr_t)CINE_FADE_ALPHA,
            *(const int *)(uintptr_t)CINE_FADE_TIMER);
    g_flk_held = 0;

    if (g_flk_n == 3 && g_flk_events < 40) {
        int d12 = flk_diff(p1, p2), d1c = flk_diff(p1, cur), d2c = flk_diff(p2, cur);
        int lo = p2->mean < cur->mean ? p2->mean : cur->mean, hi = p2->mean > cur->mean ? p2->mean : cur->mean;
        int aba = d12 >= 24 && d1c >= 24 && 2 * d2c < (d12 < d1c ? d12 : d1c);
        int flash = p1->mean + 24 < lo || p1->mean > hi + 24;
        int odd = d12 >= 96 && d1c >= 96;       /* A B C: one frame unlike both sides, e.g. between two cuts */
        if (aba || flash || odd) {
            char tag[16];
            gmv_log("[flickcap] EVENT %u (%s%s%s) at f%u: means %d / %d / %d, cell changes %d / %d, ends %d apart\n",
                    g_flk_events, aba ? "A-B-A " : "", flash ? "flash " : "", odd ? "odd-one-out" : "", p1->frame,
                    p2->mean, p1->mean, cur->mean, d12, d1c, d2c);
            snprintf(tag, sizeof tag, "e%02u_1", g_flk_events); flk_save(p2, tag);
            snprintf(tag, sizeof tag, "e%02u_2", g_flk_events); flk_save(p1, tag);
            snprintf(tag, sizeof tag, "e%02u_3", g_flk_events); flk_save(cur, tag);
            g_flk_events++;
        }
    }
    if (g_flk_presented++ % 30 == 0)
        flk_save(cur, "ctx");
}
#endif

int gmv_game_present_hold(unsigned frame, int drew)
{
    return menu_flash_hold(frame, gmv_top_state_vtable(), drew);
}

void gmv_game_present_decided(unsigned frame, int presented)
{
#if GMV_DEV
    flicker_cap(frame, presented, gmv_top_state_vtable());
#else
    (void)frame; (void)presented;
#endif
}
