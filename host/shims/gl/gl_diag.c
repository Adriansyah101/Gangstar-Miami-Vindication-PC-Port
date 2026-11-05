/* gl_diag.c -- diagnostics: frame dumps and fade captures */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"
#include <xmmintrin.h>
#include <GL/gl.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "glshim.h"

#if GMV_DEV
/* Diagnostic: dump the just-rendered back buffer to a BMP so we can see what's on screen.
   glReadPixels' default 4-byte row alignment + GL_BGR + GL's bottom-up origin match the BMP
   pixel layout exactly, so the pixels go straight into the file. */
void dump_framebuffer(const char *path)
{
    if (!g_window)
        return;
    int w = 0, h = 0;
    gmv_gl_client_size(&w, &h);
    if (w <= 0 || h <= 0)
        return;
    int rowb = (w * 3 + 3) & ~3;
    uint32_t imgsz = (uint32_t)rowb * h;
    unsigned char *px = malloc(imgsz);
    if (!px)
        return;
    /* Diagnose why cleared/drawn pixels don't appear: scissor test, scissor box, viewport and the
       colour write mask all gate what reaches the framebuffer. */
    {
#ifndef GL_SCISSOR_TEST
#define GL_SCISSOR_TEST 0x0C11
#endif
#ifndef GL_SCISSOR_BOX
#define GL_SCISSOR_BOX 0x0C10
#endif
#ifndef GL_COLOR_WRITEMASK
#define GL_COLOR_WRITEMASK 0x0C23
#endif
#ifndef GL_VIEWPORT
#define GL_VIEWPORT 0x0BA2
#endif
        GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
        GLint sb[4] = {0, 0, 0, 0}, vp[4] = {0, 0, 0, 0};
        GLboolean cm[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_SCISSOR_BOX, sb);
        glGetIntegerv(GL_VIEWPORT, vp);
        glGetBooleanv(GL_COLOR_WRITEMASK, cm);
        gmv_log("[gl] state: scissor_test=%d box=(%d,%d,%d,%d) vp=(%d,%d,%d,%d) colormask=%d%d%d%d\n",
                (int)sc, sb[0], sb[1], sb[2], sb[3], vp[0], vp[1], vp[2], vp[3],
                cm[0], cm[1], cm[2], cm[3]);
    }
    diag_read_begin(GL_BACK);
    glReadPixels(0, 0, w, h, GL_BGR_EXT, GL_UNSIGNED_BYTE, px);
    diag_read_end();

    uint8_t fh[14] = {'B', 'M'};
    uint32_t fsz = 14 + 40 + imgsz;
    memcpy(fh + 2, &fsz, 4);
    uint32_t off = 54;
    memcpy(fh + 10, &off, 4);
    uint8_t ih[40] = {0};
    uint32_t v = 40; memcpy(ih + 0, &v, 4);
    int32_t sw = w; memcpy(ih + 4, &sw, 4);
    int32_t sh = h; memcpy(ih + 8, &sh, 4);
    uint16_t planes = 1; memcpy(ih + 12, &planes, 2);
    uint16_t bpp = 24; memcpy(ih + 14, &bpp, 2);
    memcpy(ih + 20, &imgsz, 4);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(fh, 1, 14, f);
        fwrite(ih, 1, 40, f);
        fwrite(px, 1, imgsz, f);
        fclose(f);
        gmv_log("[gl] framebuffer dumped to %s (%dx%d)\n", path, w, h);
    }
    free(px);
}

int g_cap_ticks = -1;                            /* ticks since arming; -1 = off */
unsigned g_cap_session, g_cap_saved, g_cap_presented;
DWORD g_cap_t0, g_cap_last_fade;                 /* GetTickCount at arming / at the last fade call */
unsigned g_cap_clears, g_cap_draws;              /* glClear / glDraw* calls this tick */
char g_cap_seq[200];                             /* this tick's events: C clear, B<fb>, F fade, D<n> draws */
size_t g_cap_seqlen;
unsigned g_cap_run;                              /* draws in the current D run */
uint32_t g_fade_pending;                         /* fade caller whose first draw is still to come */
int g_post_fade;                                 /* the game's main fade was called this tick: probe each call */
uint32_t g_quad_probe;                           /* fade caller whose 6-index quad is still to be dumped */

/* After a generic-fade-state fade call, read back two pixels after every clear and draw up to the
   swap, so the log shows which call darkens the frame and which one brings the picture back. */
void post_fade_probe(const char *what, unsigned a, unsigned b)
{
    static unsigned lines;
    if (!g_post_fade || lines >= 600)
        return;
    lines++;
    GLint vp[4] = {0}, tex = 0, env = 0, abuf = 0, bs = 0, bd = 0;
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &env);
    glGetIntegerv(0x8894, &abuf);                       /* GL_ARRAY_BUFFER_BINDING */
    glGetIntegerv(GL_BLEND_SRC, &bs);
    glGetIntegerv(GL_BLEND_DST, &bd);
    unsigned char c[4] = {0}, q[4] = {0};
    diag_read_begin(GL_BACK);
    glReadPixels(vp[0] + vp[2] / 2, vp[1] + vp[3] / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
    glReadPixels(vp[0] + vp[2] / 4, vp[1] + vp[3] / 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, q);
    diag_read_end();
    gmv_log("[postfade] %s %u %u tex=%d env=0x%x vbo=%d blend=%d 0x%x/0x%x colarr=%d -> centre=%d,%d,%d quarter=%d,%d,%d\n",
            what, a, b, tex, env, abuf, glIsEnabled(GL_BLEND), bs, bd, glIsEnabled(GL_COLOR_ARRAY),
            c[0], c[1], c[2], q[0], q[1], q[2]);
}

void cap_seq(const char *s)
{
    if (g_cap_ticks < 0)
        return;
    char buf[24];
    if (g_cap_run) {
        snprintf(buf, sizeof buf, "D%u ", g_cap_run);
        g_cap_run = 0;
        cap_seq(buf);
    }
    size_t n = strlen(s);
    if (g_cap_seqlen + n < sizeof g_cap_seq - 1) {
        memcpy(g_cap_seq + g_cap_seqlen, s, n);
        g_cap_seqlen += n;
        g_cap_seq[g_cap_seqlen] = 0;
    }
}

void gmv_gl_death_capture_arm(int health, int max_health)
{
    if (g_cap_ticks >= 0 || g_cap_session >= 3)
        return;
    g_cap_ticks = 0;
    g_cap_saved = g_cap_presented = 0;
    g_cap_seqlen = 0; g_cap_seq[0] = 0; g_cap_run = 0;
    g_cap_t0 = GetTickCount();
    gmv_log("[fadecap] armed session %u: player health %d/%d at t=%lu\n", g_cap_session, health,
            max_health, (unsigned long)g_cap_t0);
    g_cap_session++;
}

static int g_fade_main;                                 /* the fade being drawn is the game's main one */

void gmv_gl_fade_mark(uint32_t caller, int main_fade)
{
    g_fade_main = main_fade;
    g_cap_last_fade = GetTickCount();
    cap_seq(main_fade ? "F " : "f ");
    g_fade_pending = caller;
    g_quad_probe = caller;
    if (main_fade) {
        g_post_fade = 1;
        post_fade_probe("FADE-CALL", 0, 0);
    }
}

/* The frame's corner pixels after a dumped fade quad is drawn, and again before that frame is
   presented: whether the black reaches the frame's top row and left column, and whether a later draw
   paints over them. */
int g_corner_watch;                                     /* 1 = after the next draw, 2 = at the present */
static int g_corner_w, g_corner_h;

void fade_corners(const char *when)
{
    static unsigned lines;
    if (lines >= 48)
        return;
    lines++;
    if (g_corner_watch == 1) {
        GLint vp[4] = {0};
        glGetIntegerv(GL_VIEWPORT, vp);
        g_corner_w = vp[2]; g_corner_h = vp[3];
    }
    int w = g_corner_w, h = g_corner_h;
    const int at[6][2] = { {0, h - 1}, {1, h - 2}, {w / 2, h - 1}, {0, h / 2}, {w - 1, 0}, {w / 2, h / 2} };
    unsigned char c[6][4] = {{0}};
    diag_read_begin(GL_BACK);
    for (int i = 0; i < 6; i++)
        glReadPixels(at[i][0], at[i][1], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c[i]);
    diag_read_end();
    gmv_log("[fadecorner] %s %dx%d: top-left %d,%d,%d  next-in %d,%d,%d  top-mid %d,%d,%d  left-mid %d,%d,%d  "
            "bottom-right %d,%d,%d  centre %d,%d,%d\n", when, w, h,
            c[0][0], c[0][1], c[0][2], c[1][0], c[1][1], c[1][2], c[2][0], c[2][1], c[2][2],
            c[3][0], c[3][1], c[3][2], c[4][0], c[4][1], c[4][2], c[5][0], c[5][1], c[5][2]);
}

/* GL state the fade rectangle is drawn with (first glDraw* after a fade call). */
void fade_state_log(const char *what, GLenum mode, GLsizei count)
{
    static unsigned n_g, n_o;
    uint32_t caller = g_fade_pending;
    g_fade_pending = 0;
    if (g_fade_main ? n_g++ >= 16 : n_o++ >= 8)
        return;
    GLint fb = -1, vp[4] = {0}, sc[4] = {0}, df = 0, bs = 0, bd = 0;
    GLboolean dm = 0, cm[4] = {0};
    glGetIntegerv(0x8CA6, &fb);                         /* GL_FRAMEBUFFER_BINDING */
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_SCISSOR_BOX, sc);
    glGetIntegerv(GL_DEPTH_FUNC, &df);
    glGetIntegerv(GL_BLEND_SRC, &bs);
    glGetIntegerv(GL_BLEND_DST, &bd);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &dm);
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    gmv_log("[fadestate] caller=0x%06x %s mode=%u count=%d fb=%d vp=%d,%d,%d,%d depth=%d func=0x%x mask=%d "
            "blend=%d 0x%x/0x%x scissor=%d %d,%d,%d,%d cull=%d alphatest=%d stencil=%d tex2d=%d cmask=%d%d%d%d\n",
            caller, what, mode, (int)count, fb, vp[0], vp[1], vp[2], vp[3], glIsEnabled(GL_DEPTH_TEST), df, dm,
            glIsEnabled(GL_BLEND), bs, bd, glIsEnabled(GL_SCISSOR_TEST), sc[0], sc[1], sc[2], sc[3],
            glIsEnabled(GL_CULL_FACE), glIsEnabled(GL_ALPHA_TEST), glIsEnabled(GL_STENCIL_TEST),
            glIsEnabled(GL_TEXTURE_2D), cm[0], cm[1], cm[2], cm[3]);
}

/* 24-bit bottom-up BMP from rows of `row` bytes (BGR, the layout glReadPixels gives with GL_BGR). */
void write_bmp24(const char *path, int w, int h, int row, const unsigned char *px)
{
    uint32_t imgsz = (uint32_t)row * h, fsz = 54 + imgsz, off = 54, v = 40;
    uint8_t fh[14] = {'B', 'M'}, ih[40] = {0};
    memcpy(fh + 2, &fsz, 4); memcpy(fh + 10, &off, 4);
    int32_t sw = w, sh = h; uint16_t planes = 1, bpp = 24;
    memcpy(ih, &v, 4); memcpy(ih + 4, &sw, 4); memcpy(ih + 8, &sh, 4);
    memcpy(ih + 12, &planes, 2); memcpy(ih + 14, &bpp, 2); memcpy(ih + 20, &imgsz, 4);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(fh, 1, 14, f); fwrite(ih, 1, 40, f); fwrite(px, 1, imgsz, f);
        fclose(f);
    }
}

void fade_capture_dump(unsigned frame, DWORD t)
{
    int w = 0, h = 0;
    gmv_gl_client_size(&w, &h);
    if (w <= 0 || h <= 0)
        return;
    int step = (w + 639) / 640;
    int ow = w / step, oh = h / step;
    int inrow = (w * 3 + 3) & ~3, outrow = (ow * 3 + 3) & ~3;
    unsigned char *px = malloc((size_t)inrow * h), *out = calloc((size_t)outrow, oh);
    if (!px || !out) { free(px); free(out); return; }
    diag_read_begin(GL_BACK);
    glReadPixels(0, 0, w, h, GL_BGR_EXT, GL_UNSIGNED_BYTE, px);
    diag_read_end();
    long sum = 0;
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            const unsigned char *s = px + (size_t)y * step * inrow + (size_t)x * step * 3;
            unsigned char *d = out + (size_t)y * outrow + (size_t)x * 3;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
            sum += s[0] + s[1] + s[2];
        }
    char path[96];
    snprintf(path, sizeof path, "gmv_fadecap_s%u_%03u_f%u_t%lu_+%05lums.bmp", g_cap_session - 1, g_cap_saved++,
             frame, (unsigned long)t, (unsigned long)(t - g_cap_t0));
    write_bmp24(path, ow, oh, outrow, out);
    gmv_log("[fadecap] f%u saved %s (back buffer mean brightness %ld/255)\n", frame, path,
            sum / ((long)ow * oh * 3));
    free(px); free(out);
}
#else                                                   /* release: no frame dump, no fade capture */
unsigned g_cap_clears, g_cap_draws, g_cap_run;
void gmv_gl_death_capture_arm(int health, int max_health) { (void)health; (void)max_health; }
void gmv_gl_fade_mark(uint32_t caller, int main_fade) { (void)caller; (void)main_fade; }
#endif
