/* padicons.c -- PAD_ICONS: the options menu's gamepad pictures drawn for the pad in use.
   An enhancement, off by default and under ORIGINAL_MAC_EXPERIENCE.

   Every gamepad picture of newmenu.fua (the calibration page's pad and its yellow highlights,
   widgetCalibration.wGP*, and the button of each line of the button list, wCusLine%d.wGP.wgp_%d) is
   cut from one 4096x2048 atlas, "./splash_gamepad.bmp", which the game loads as the DXT5 texture
   _packMac/file002441.dat (13 mip levels, one glCompressedTexImage2D each).  The Mac's art is
   neutral: the face buttons are a diamond of four dots with the one meant lit in yellow, the
   shoulders and Back/Start a picture of that part of the pad.

   With PAD_ICONS=1 the atlas is recognised at its upload (size, format and a sample of its bytes)
   and its blocks under the pictures are kept; after a present, when the kind of pad last used
   (gmv_joy_style) differs from what the texture shows, those blocks are replaced in place with
   glCompressedTexSubImage2D:
     - Xbox (XInput): A B X Y in their colours, LB RB LT RT, the View and Menu signs;
     - DualShock 4 / DualSense: cross, circle, square and triangle in their colours, L1 R1 L2 R2,
       L3 R3, SHARE (CREATE on a DualSense) and OPTIONS;
     - any other pad, or none: the Mac's own blocks.
   The new art is drawn here (shapes from distance functions, letters with GDI's Arial; FreeType's on Linux), over the
   decoded original, and its mip levels 1-6 are box-filtered from it; blocks no picture touches stay
   the original bytes.  Levels 7-12 (32x16 texels and less) stay the Mac's.  Nothing of the menu
   file changes, so the pictures keep their places, sizes and the game's own show/hide.

   The button list shows the game's logical buttons (calibration order X A B Y LB RB LT RT Back
   Start LS RS, wgp_0..11); shimsjoy.c gives a DualShock's buttons in that order as Square Cross Circle
   Triangle L1 R1 L2 R2 Share Options L3 R3, so square = X, cross = A, circle = B, triangle = Y. */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

#ifndef PADICONS_TEST
#include <GL/gl.h>
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#endif

#define AT_W      4096
#define AT_H      2048
#define AT_DXT5   0x83F3u                       /* GL_COMPRESSED_RGBA_S3TC_DXT5_EXT */
#define AT_HASH   0xafb33352u                   /* FNV-1a of every 37th byte of level 0 */
#define AT_LEVELS 7                             /* levels 0..6 are redrawn */
#define NSTYLE    4                             /* 0 the Mac's, 1 Xbox, 2 DualShock 4, 3 DualSense */

/* The parts of the atlas that hold pictures, on 256-texel bounds so that each is whole blocks down
   to level 6 and no two share a block there: the calibration page's highlight dots, the pad's face
   buttons, and the rows of button pictures. */
static const struct { int x, y, w, h; } g_clus[] = {
    {256, 0, 512, 768}, {1536, 256, 512, 512}, {0, 1024, 2304, 768},
};
#define NCLUS (int)(sizeof g_clus / sizeof g_clus[0])

enum { B_X, B_A, B_B, B_Y, B_LB, B_RB, B_LT, B_RT, B_BACK, B_START, B_LS, B_RS };
enum {
    K_FACE,     /* a button list's face button: redrawn as a round button */
    K_DOT,      /* a calibration highlight dot (yellow): the sign drawn on it */
    K_PAD,      /* a face button of the calibration page's pad: the sign drawn on it, in colour */
    K_SHOULDER, /* a shoulder picture: its name written on it */
    K_MENU,     /* Back / Start: redrawn as a round button with the sign or name */
    K_STICK,    /* a stick's yellow L / R: L3 / R3 on a PlayStation pad */
};

/* Each picture: kind, button, and its rectangle in level-0 texels (from the menu file's shapes). */
static const struct { unsigned char kind, button; short x0, y0, x1, y1; } g_pics[] = {
    {K_FACE, B_X, 86, 1101, 318, 1328},          /* wgp_0 .. wgp_3 */
    {K_FACE, B_A, 860, 1101, 1092, 1330},
    {K_FACE, B_B, 600, 1101, 837, 1332},
    {K_FACE, B_Y, 343, 1101, 574, 1325},
    {K_SHOULDER, B_LB, 1638, 1142, 1870, 1338},  /* wgp_4 .. wgp_7 */
    {K_SHOULDER, B_RB, 595, 1380, 824, 1572},
    {K_SHOULDER, B_LT, 1914, 1142, 2145, 1335},
    {K_SHOULDER, B_RT, 863, 1380, 1091, 1572},
    {K_MENU, B_BACK, 1150, 1356, 1378, 1595},    /* wgp_8, wgp_9 */
    {K_MENU, B_START, 1421, 1356, 1654, 1584},
    {K_STICK, B_LS, 1118, 1101, 1354, 1333},     /* wgp_10, wgp_11 */
    {K_STICK, B_RS, 1381, 1101, 1615, 1330},
    {K_STICK, B_LS, 106, 1378, 298, 1570},       /* wGPbtnLeft, wGPbtnRight */
    {K_STICK, B_RS, 334, 1383, 536, 1572},
    {K_DOT, B_Y, 411, 159, 528, 268},            /* wGPbtnY B X A */
    {K_DOT, B_B, 415, 319, 527, 422},
    {K_DOT, B_X, 422, 455, 528, 557},
    {K_DOT, B_A, 425, 603, 528, 706},
    {K_PAD, B_Y, 1824, 311, 1914, 401},          /* the pad (shape 642) under those dots */
    {K_PAD, B_B, 1938, 414, 2028, 504},
    {K_PAD, B_X, 1698, 411, 1788, 501},
    {K_PAD, B_A, 1823, 527, 1913, 617},
};
#define NPICS (int)(sizeof g_pics / sizeof g_pics[0])

static const char *const g_style_name[NSTYLE] = {"the Mac's", "Xbox", "DualShock 4", "DualSense"};

/* ---- DXT5 blocks ---- */

static void rgb565(unsigned c, int *r, int *g, int *b)
{
    *r = (int)((c >> 11) & 31) * 255 / 31;
    *g = (int)((c >> 5) & 63) * 255 / 63;
    *b = (int)(c & 31) * 255 / 31;
}

static void dxt5_decode(const uint8_t *b, uint8_t px[16][4])
{
    int a[8], a0 = b[0], a1 = b[1];
    a[0] = a0, a[1] = a1;
    if (a0 > a1)
        for (int i = 2; i < 8; i++)
            a[i] = ((8 - i) * a0 + (i - 1) * a1) / 7;
    else {
        for (int i = 2; i < 6; i++)
            a[i] = ((6 - i) * a0 + (i - 1) * a1) / 5;
        a[6] = 0, a[7] = 255;
    }
    uint64_t ai = 0;
    for (int i = 0; i < 6; i++)
        ai |= (uint64_t)b[2 + i] << (8 * i);
    int c[4][3];
    rgb565((unsigned)(b[8] | b[9] << 8), &c[0][0], &c[0][1], &c[0][2]);
    rgb565((unsigned)(b[10] | b[11] << 8), &c[1][0], &c[1][1], &c[1][2]);
    for (int k = 0; k < 3; k++) {
        c[2][k] = (2 * c[0][k] + c[1][k]) / 3;
        c[3][k] = (c[0][k] + 2 * c[1][k]) / 3;
    }
    uint32_t ci = (uint32_t)(b[12] | b[13] << 8 | b[14] << 16 | (uint32_t)b[15] << 24);
    for (int i = 0; i < 16; i++) {
        const int *cc = c[(ci >> (2 * i)) & 3];
        px[i][0] = (uint8_t)cc[0], px[i][1] = (uint8_t)cc[1], px[i][2] = (uint8_t)cc[2];
        px[i][3] = (uint8_t)a[(ai >> (3 * i)) & 7];
    }
}

static unsigned to565(int r, int g, int b) { return (unsigned)((r * 31 + 127) / 255 << 11 | (g * 63 + 127) / 255 << 5 | (b * 31 + 127) / 255); }

static void dxt5_encode(const uint8_t px[16][4], uint8_t *b)
{
    /* alpha: the eight-value ramp between the block's extremes */
    int amin = 255, amax = 0;
    for (int i = 0; i < 16; i++) {
        if (px[i][3] < amin) amin = px[i][3];
        if (px[i][3] > amax) amax = px[i][3];
    }
    b[0] = (uint8_t)amax, b[1] = (uint8_t)amin;
    uint64_t ai = 0;
    if (amax > amin)
        for (int i = 0; i < 16; i++) {
            int k = ((px[i][3] - amin) * 7 + (amax - amin) / 2) / (amax - amin);     /* 0 = amin .. 7 = amax */
            int idx = k == 7 ? 0 : k == 0 ? 1 : 8 - k;
            ai |= (uint64_t)idx << (3 * i);
        }
    for (int i = 0; i < 6; i++)
        b[2 + i] = (uint8_t)(ai >> (8 * i));

    /* colour: the box of the visible texels, inset a little, as the two ends */
    int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0}, any = 0;
    for (int i = 0; i < 16; i++) {
        if (!px[i][3])
            continue;
        any = 1;
        for (int k = 0; k < 3; k++) {
            if (px[i][k] < lo[k]) lo[k] = px[i][k];
            if (px[i][k] > hi[k]) hi[k] = px[i][k];
        }
    }
    if (!any)
        lo[0] = lo[1] = lo[2] = hi[0] = hi[1] = hi[2] = 0;
    for (int k = 0; k < 3; k++) {
        int in = (hi[k] - lo[k]) / 16;
        lo[k] += in, hi[k] -= in;
    }
    unsigned c0 = to565(hi[0], hi[1], hi[2]), c1 = to565(lo[0], lo[1], lo[2]);
    if (c0 < c1) {
        unsigned t = c0;
        c0 = c1, c1 = t;
    }
    int pal[4][3];
    rgb565(c0, &pal[0][0], &pal[0][1], &pal[0][2]);
    rgb565(c1, &pal[1][0], &pal[1][1], &pal[1][2]);
    for (int k = 0; k < 3; k++) {
        pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
        pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
    }
    uint32_t ci = 0;
    for (int i = 0; i < 16 && c0 != c1; i++) {
        int best = 0, bd = 1 << 30;
        for (int j = 0; j < 4; j++) {
            int dr = px[i][0] - pal[j][0], dg = px[i][1] - pal[j][1], db = px[i][2] - pal[j][2];
            int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
            if (d < bd)
                bd = d, best = j;
        }
        ci |= (uint32_t)best << (2 * i);
    }
    b[8] = (uint8_t)c0, b[9] = (uint8_t)(c0 >> 8), b[10] = (uint8_t)c1, b[11] = (uint8_t)(c1 >> 8);
    b[12] = (uint8_t)ci, b[13] = (uint8_t)(ci >> 8), b[14] = (uint8_t)(ci >> 16), b[15] = (uint8_t)(ci >> 24);
}

/* ---- the kept blocks: [style][cluster][level], whole blocks of the cluster's rectangle ---- */

static uint8_t *g_blocks[NSTYLE][NCLUS][AT_LEVELS];
static unsigned g_orig_levels;                  /* bit L: level L of the original is kept */

static int clus_bw(int c, int lv) { return (g_clus[c].w >> lv) / 4; }
static int clus_bh(int c, int lv) { return (g_clus[c].h >> lv) / 4; }
static size_t clus_bytes(int c, int lv) { return (size_t)clus_bw(c, lv) * clus_bh(c, lv) * 16; }

/* Keep level lv of the original atlas (the whole level's DXT5 data, as uploaded). */
static void keep_level(int lv, const uint8_t *data)
{
    int row = (AT_W >> lv) / 4 * 16;            /* bytes per row of blocks */
    for (int c = 0; c < NCLUS; c++) {
        uint8_t *dst = malloc(clus_bytes(c, lv));
        if (!dst)
            return;
        int bx = (g_clus[c].x >> lv) / 4, by = (g_clus[c].y >> lv) / 4, bw = clus_bw(c, lv);
        for (int y = 0; y < clus_bh(c, lv); y++)
            memcpy(dst + (size_t)y * bw * 16, data + (size_t)(by + y) * row + (size_t)bx * 16, (size_t)bw * 16);
        free(g_blocks[0][c][lv]);
        g_blocks[0][c][lv] = dst;
    }
    g_orig_levels |= 1u << lv;
}

/* ---- drawing: a cluster's level-0 texels (straight RGBA) and which of them were drawn on ---- */

typedef struct {
    int x, y, w, h;                             /* the cluster, in atlas texels */
    uint8_t *px;                                /* w*h*4 */
    uint8_t *mark;                              /* w*h: 1 = drawn on */
} canvas;

static uint8_t *cv_at(canvas *cv, int x, int y) { return cv->px + ((size_t)(y - cv->y) * cv->w + (x - cv->x)) * 4; }

/* Lay colour (r,g,b) with coverage a (0..1) over texel (x,y) in atlas coordinates. */
static void cv_put(canvas *cv, int x, int y, float r, float g, float b, float a)
{
    if (a <= 0 || x < cv->x || y < cv->y || x >= cv->x + cv->w || y >= cv->y + cv->h)
        return;
    if (a > 1)
        a = 1;
    uint8_t *p = cv_at(cv, x, y);
    float da = p[3] / 255.0f, oa = a + da * (1 - a);
    for (int k = 0; k < 3; k++) {
        float s = k == 0 ? r : k == 1 ? g : b;
        p[k] = (uint8_t)lrintf(oa > 0 ? (s * a + p[k] * da * (1 - a)) / oa : s);
    }
    p[3] = (uint8_t)lrintf(oa * 255);
    cv->mark[(size_t)(y - cv->y) * cv->w + (x - cv->x)] = 1;
}

static void cv_clear(canvas *cv, int x0, int y0, int x1, int y1)
{
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (x >= cv->x && y >= cv->y && x < cv->x + cv->w && y < cv->y + cv->h) {
                memset(cv_at(cv, x, y), 0, 4);
                cv->mark[(size_t)(y - cv->y) * cv->w + (x - cv->x)] = 1;
            }
}

typedef struct { float r, g, b; } rgb;

/* Signed distances (texels, negative inside) at a point. */
static float sd_circle(float px, float py, float cx, float cy, float r) { return hypotf(px - cx, py - cy) - r; }

static float sd_seg(float px, float py, float ax, float ay, float bx, float by)
{
    float dx = bx - ax, dy = by - ay, t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return hypotf(px - ax - t * dx, py - ay - t * dy);
}

static float sd_box(float px, float py, float cx, float cy, float hx, float hy, float round)
{
    float qx = fabsf(px - cx) - hx + round, qy = fabsf(py - cy) - hy + round;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    return hypotf(ox, oy) + fminf(fmaxf(qx, qy), 0) - round;
}

/* A shape for fill_shape: its distance at a point. */
typedef struct {
    int type;                                   /* 0 disc, 1 ring, 2 segment, 3 box outline, 4 box, 5 triangle outline */
    float cx, cy, a, b, c, d, w;
} shape;

static float shape_d(const shape *s, float x, float y)
{
    switch (s->type) {
    case 0: return sd_circle(x, y, s->cx, s->cy, s->a);
    case 1: return fabsf(sd_circle(x, y, s->cx, s->cy, s->a)) - s->w / 2;
    case 2: return sd_seg(x, y, s->cx, s->cy, s->a, s->b) - s->w / 2;
    case 3: return fabsf(sd_box(x, y, s->cx, s->cy, s->a, s->b, s->c)) - s->w / 2;
    case 4: return sd_box(x, y, s->cx, s->cy, s->a, s->b, s->c);
    default: {                                  /* equilateral, centre (cx,cy), circumradius a, point up */
        float vx[3], vy[3], d = 1e9f;
        for (int i = 0; i < 3; i++) {
            float ang = -1.5707963f + i * 2.0943951f;
            vx[i] = s->cx + s->a * cosf(ang), vy[i] = s->cy + s->a * sinf(ang);
        }
        for (int i = 0; i < 3; i++)
            d = fminf(d, sd_seg(x, y, vx[i], vy[i], vx[(i + 1) % 3], vy[(i + 1) % 3]));
        return d - s->w / 2;
    }
    }
}

/* Fill a shape, with its colour shaded from top (col) to bottom (col2) across [y0, y1]. */
static void fill_shape2(canvas *cv, const shape *s, rgb col, rgb col2, float y0, float y1)
{
    float reach = s->a + s->b + s->w + 4;
    if (s->type == 2)
        reach = hypotf(s->a - s->cx, s->b - s->cy) + s->w + 4;
    for (int y = (int)(s->cy - reach); y <= (int)(s->cy + reach) + 1; y++)
        for (int x = (int)(s->cx - reach); x <= (int)(s->cx + reach) + 1; x++) {
            float d = shape_d(s, x + 0.5f, y + 0.5f);
            if (d >= 0.5f)
                continue;
            float t = y1 > y0 ? (y + 0.5f - y0) / (y1 - y0) : 0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            cv_put(cv, x, y, col.r + (col2.r - col.r) * t, col.g + (col2.g - col.g) * t, col.b + (col2.b - col.b) * t,
                   0.5f - d);
        }
}

static void fill_shape(canvas *cv, const shape *s, rgb col) { fill_shape2(cv, s, col, col, 0, 0); }

/* The text s drawn white-on-nothing in Arial Bold, em texels to the em, its cell's top left at (em/2, em/2) of a
   bw x bh picture: the coverage of each texel (0..255), malloc'd, or NULL. */
#ifdef _WIN32
static uint8_t *render_text(const char *s, int len, float em, int bw, int bh)
{
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), bw, -bh, 1, 32, BI_RGB, 0, 0, 0, 0, 0}, {{0, 0, 0, 0}}};
    void *bits = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bm = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    uint8_t *cov = bm ? malloc((size_t)bw * bh) : NULL;
    if (!cov) {
        if (bm)
            DeleteObject(bm);
        if (dc)
            DeleteDC(dc);
        return NULL;
    }
    HGDIOBJ obm = SelectObject(dc, bm);
    HFONT f = CreateFontA(-(int)lrintf(em), 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                          ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
    HGDIOBJ of = SelectObject(dc, f);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    TextOutA(dc, (int)(em / 2), (int)(em / 2), s, len);
    GdiFlush();
    const uint8_t *p = bits;
    for (size_t i = 0; i < (size_t)bw * bh; i++)
        cov[i] = p[i * 4 + 1];
    SelectObject(dc, of);
    DeleteObject(f);
    SelectObject(dc, obm);
    DeleteObject(bm);
    DeleteDC(dc);
    return cov;
}
#else
/* Linux has no GDI: FreeType draws the text in the font fontconfig gives for "Arial:bold" (Liberation Sans Bold
   where it is installed: Arial's widths, near its shapes).  Both libraries are opened when first needed, so the
   program runs without them; the pictures then have no letters, which the log says once. */
#include <dlfcn.h>
#include <ft2build.h>
#include FT_FREETYPE_H

static FT_Face g_face;

static FT_Face text_face(void)
{
    static int tried;
    if (tried)
        return g_face;
    tried = 1;
    char path[1024] = "";
    void *fc = dlopen("libfontconfig.so.1", RTLD_NOW | RTLD_LOCAL);
    if (fc) {                                   /* opaque pointers only: no fontconfig header needed */
        void *(*init)(void) = (void *(*)(void))dlsym(fc, "FcInitLoadConfigAndFonts");
        void *(*parse)(const char *) = (void *(*)(const char *))dlsym(fc, "FcNameParse");
        int (*subst)(void *, void *, int) = (int (*)(void *, void *, int))dlsym(fc, "FcConfigSubstitute");
        void (*dflt)(void *) = (void (*)(void *))dlsym(fc, "FcDefaultSubstitute");
        void *(*match)(void *, void *, int *) = (void *(*)(void *, void *, int *))dlsym(fc, "FcFontMatch");
        int (*get)(void *, const char *, int, char **) = (int (*)(void *, const char *, int, char **))dlsym(fc, "FcPatternGetString");
        void (*destroy)(void *) = (void (*)(void *))dlsym(fc, "FcPatternDestroy");
        void *cfg = init && parse && subst && dflt && match && get && destroy ? init() : NULL;
        void *pat = cfg ? parse("Arial:bold") : NULL;
        if (pat) {
            int res;
            char *file = NULL;
            subst(cfg, pat, 0);                 /* FcMatchPattern */
            dflt(pat);
            void *m = match(cfg, pat, &res);
            if (m && !get(m, "file", 0, &file) && file)     /* FcResultMatch */
                snprintf(path, sizeof path, "%s", file);
            if (m)
                destroy(m);
            destroy(pat);
        }
    }
    static const char *const known[] = {
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf",
        "/usr/share/fonts/TTF/LiberationSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    };
    for (size_t i = 0; !path[0] && i < sizeof known / sizeof *known; i++)
        if (!access(known[i], R_OK))
            snprintf(path, sizeof path, "%s", known[i]);
    void *ft = dlopen("libfreetype.so.6", RTLD_NOW | RTLD_LOCAL);
    FT_Error (*ft_init)(FT_Library *) = ft ? (FT_Error (*)(FT_Library *))dlsym(ft, "FT_Init_FreeType") : NULL;
    FT_Error (*ft_face)(FT_Library, const char *, FT_Long, FT_Face *) =
        ft ? (FT_Error (*)(FT_Library, const char *, FT_Long, FT_Face *))dlsym(ft, "FT_New_Face") : NULL;
    FT_Library lib;
    if (!path[0] || !ft_init || !ft_face || ft_init(&lib) || ft_face(lib, path, 0, &g_face)) {
        g_face = NULL;
        gmv_log("[padicons] no %s: the pictures have no letters\n", !ft ? "libfreetype.so.6" : !path[0] ? "bold sans font" : "usable font");
    } else
        gmv_log("[padicons] letters in %s\n", path);
    return g_face;
}

static uint8_t *render_text(const char *s, int len, float em, int bw, int bh)
{
    FT_Face face = text_face();
    static FT_Error (*size)(FT_Face, FT_UInt, FT_UInt);
    static FT_Error (*load)(FT_Face, FT_ULong, FT_Int32);
    if (face && !size) {
        void *ft = dlopen("libfreetype.so.6", RTLD_NOW | RTLD_LOCAL);     /* already loaded: a reference */
        size = (FT_Error (*)(FT_Face, FT_UInt, FT_UInt))dlsym(ft, "FT_Set_Pixel_Sizes");
        load = (FT_Error (*)(FT_Face, FT_ULong, FT_Int32))dlsym(ft, "FT_Load_Char");
    }
    if (!face || !size || !load || size(face, 0, (FT_UInt)lrintf(em)))
        return NULL;
    uint8_t *cov = calloc((size_t)bw * bh, 1);
    if (!cov)
        return NULL;
    int pen = (int)(em / 2), base = (int)(em / 2) + (int)(face->size->metrics.ascender >> 6);
    for (int i = 0; i < len; i++) {
        if (load(face, (unsigned char)s[i], FT_LOAD_RENDER))
            continue;
        FT_GlyphSlot g = face->glyph;
        if (g->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY)
            for (unsigned y = 0; y < g->bitmap.rows; y++)
                for (unsigned x = 0; x < g->bitmap.width; x++) {
                    int tx = pen + g->bitmap_left + (int)x, ty = base - g->bitmap_top + (int)y;
                    uint8_t v = g->bitmap.buffer[(int)y * g->bitmap.pitch + (int)x];
                    if (tx >= 0 && ty >= 0 && tx < bw && ty < bh && v > cov[(size_t)ty * bw + tx])
                        cov[(size_t)ty * bw + tx] = v;
                }
        pen += (int)(g->advance.x >> 6);
    }
    return cov;
}
#endif

/* Text in Arial Bold, its ink centred on (cx,cy), its capitals cap texels high and at most maxw wide. */
static void draw_text(canvas *cv, const char *s, float cx, float cy, float cap, float maxw, rgb col)
{
    int len = (int)strlen(s);
    float em = cap / 0.716f;                    /* Arial's capital height */
    for (int pass = 0; pass < 2; pass++) {
        int bw = (int)(em * (len + 2)), bh = (int)(em * 2);
        uint8_t *p = render_text(s, len, em, bw, bh);
        if (!p)
            return;
        int x0 = bw, y0 = bh, x1 = -1, y1 = -1;
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++)
                if (p[(size_t)y * bw + x]) {
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
        if (x1 >= 0 && pass == 0 && maxw > 0 && x1 - x0 + 1 > maxw) {
            em *= maxw / (float)(x1 - x0 + 1);  /* too wide: once more, smaller */
        } else if (x1 >= 0) {
            int ox = (int)lrintf(cx - (x1 - x0 + 1) / 2.0f) - x0, oy = (int)lrintf(cy - (y1 - y0 + 1) / 2.0f) - y0;
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++) {
                    int v = p[(size_t)y * bw + x];
                    if (v)
                        cv_put(cv, x + ox, y + oy, col.r, col.g, col.b, v / 255.0f);
                }
            pass = 2;
        }
        free(p);
        if (pass == 2)
            break;
    }
}

/* ---- the pictures ---- */

static const rgb XB_COL[4] = {{40, 124, 220}, {98, 183, 52}, {226, 52, 44}, {245, 190, 35}};     /* X A B Y */
static const rgb PS_COL[4] = {{224, 140, 222}, {124, 178, 238}, {244, 98, 98}, {64, 212, 164}};  /* square cross circle triangle */
static const rgb WHITE = {236, 236, 236}, INK = {24, 24, 24};

/* A face button's sign, size texels high, centred on (cx,cy). */
static void draw_sign(canvas *cv, int style, int button, float cx, float cy, float size, rgb col)
{
    if (style == 1) {
        static const char *const xb[4] = {"X", "A", "B", "Y"};
        draw_text(cv, xb[button], cx, cy, size * 0.92f, 0, col);
        return;
    }
    float h = size / 2, w = size * 0.15f;
    shape s = {0};
    switch (button) {
    case B_X:                                   /* square */
        s = (shape){3, cx, cy, h * 0.8f, h * 0.8f, w * 0.4f, 0, w};
        fill_shape(cv, &s, col);
        break;
    case B_A:                                   /* cross */
        s = (shape){2, cx - h * 0.82f, cy - h * 0.82f, cx + h * 0.82f, cy + h * 0.82f, 0, 0, w};
        fill_shape(cv, &s, col);
        s = (shape){2, cx - h * 0.82f, cy + h * 0.82f, cx + h * 0.82f, cy - h * 0.82f, 0, 0, w};
        fill_shape(cv, &s, col);
        break;
    case B_B:                                   /* circle */
        s = (shape){1, cx, cy, h * 0.86f, 0, 0, 0, w};
        fill_shape(cv, &s, col);
        break;
    default:                                    /* triangle */
        s = (shape){5, cx, cy + h * 0.14f, h * 1.02f, 0, 0, 0, w};
        fill_shape(cv, &s, col);
        break;
    }
}

/* The dark round button the list's face buttons and Back/Start are redrawn as. */
static void draw_button(canvas *cv, float cx, float cy, float r)
{
    shape s = {0, cx, cy + r * 0.03f, r, 0, 0, 0, 0};
    fill_shape(cv, &s, (rgb){8, 8, 10});
    s = (shape){0, cx, cy, r * 0.97f, 0, 0, 0, 0};
    fill_shape(cv, &s, (rgb){20, 20, 23});
    s = (shape){0, cx, cy, r * 0.86f, 0, 0, 0, 0};
    fill_shape2(cv, &s, (rgb){74, 74, 80}, (rgb){30, 30, 34}, cy - r * 0.86f, cy + r * 0.86f);
}

static int is_yellow(const uint8_t *p) { return p[3] > 160 && p[0] > 150 && p[1] > 110 && p[2] < 110; }

static void draw_pic(canvas *cv, int style, int i)
{
    const int b = g_pics[i].button, x0 = g_pics[i].x0, y0 = g_pics[i].y0, x1 = g_pics[i].x1, y1 = g_pics[i].y1;
    const float cx = (x0 + x1) / 2.0f, cy = (y0 + y1) / 2.0f, r = fminf(x1 - x0, y1 - y0) / 2.0f - 3;
    const int ps = style >= 2;
    switch (g_pics[i].kind) {
    case K_FACE:
        cv_clear(cv, x0, y0, x1, y1);
        draw_button(cv, cx, cy, r);
        draw_sign(cv, style, b, cx, cy, r * (ps ? 0.98f : 0.95f), ps ? PS_COL[b] : XB_COL[b]);
        break;
    case K_DOT:
        draw_sign(cv, style, b, cx, cy, r * (ps ? 1.0f : 1.0f), INK);
        break;
    case K_PAD:
        draw_sign(cv, style, b, cx, cy, r * 0.82f, ps ? PS_COL[b] : XB_COL[b]);
        break;
    case K_SHOULDER: {
        static const char *const xb[4] = {"LB", "RB", "LT", "RT"}, *const psn[4] = {"L1", "R1", "L2", "R2"};
        float h = (float)(y1 - y0);
        draw_text(cv, (ps ? psn : xb)[b - B_LB], cx, y0 + h * 0.70f, h * 0.24f, (x1 - x0) * 0.6f, WHITE);
        break;
    }
    case K_MENU:
        cv_clear(cv, x0, y0, x1, y1);
        draw_button(cv, cx, cy, r);
        if (ps)
            draw_text(cv, b == B_START ? "OPTIONS" : style == 3 ? "CREATE" : "SHARE", cx, cy, r * 0.27f, r * 1.45f, WHITE);
        else if (b == B_START) {                /* Menu: three lines */
            for (int k = -1; k <= 1; k++) {
                shape s = {2, cx - r * 0.34f, cy + k * r * 0.24f, cx + r * 0.34f, cy + k * r * 0.24f, 0, 0, r * 0.1f};
                fill_shape(cv, &s, WHITE);
            }
        } else {                                /* View: two windows, the front one filled */
            float hx = r * 0.25f, hy = r * 0.19f, o = r * 0.11f, w = r * 0.075f;
            shape s = {3, cx - o, cy - o, hx, hy, w, 0, w};
            fill_shape(cv, &s, WHITE);
            s = (shape){4, cx + o, cy + o, hx + w * 1.5f, hy + w * 1.5f, w * 2, 0, 0};
            fill_shape(cv, &s, (rgb){50, 50, 55});
            s = (shape){4, cx + o, cy + o, hx + w / 2, hy + w / 2, w, 0, 0};
            fill_shape(cv, &s, WHITE);
        }
        break;
    case K_STICK: {
        if (!ps)
            break;
        /* the yellow disc's radius: its farthest yellow texel straight up from the centre (the letter
           may cross that line; the dark ring and the transparent edge are outside it) */
        int icx = (int)cx, icy = (int)cy, rr = 0;
        for (int d = 1; icy - d >= y0; d++)
            if (is_yellow(cv_at(cv, icx, icy - d)))
                rr = d;
        if (rr < 20)
            break;
        uint8_t top[4], bot[4];
        memcpy(top, cv_at(cv, icx, icy - rr * 8 / 10), 4);
        memcpy(bot, cv_at(cv, icx, icy + rr * 8 / 10), 4);
        shape s = {0, cx, cy, rr - 1.0f, 0, 0, 0, 0};
        fill_shape2(cv, &s, (rgb){top[0], top[1], top[2]}, (rgb){bot[0], bot[1], bot[2]}, cy - rr * 0.8f, cy + rr * 0.8f);
        draw_text(cv, b == B_LS ? "L3" : "R3", cx, cy, rr * 0.62f, rr * 1.4f, INK);
        break;
    }
    }
}

/* Build a style's blocks for every cluster and level from the original's. */
static int build_style(int style)
{
    for (int c = 0; c < NCLUS; c++) {
        canvas cv = {g_clus[c].x, g_clus[c].y, g_clus[c].w, g_clus[c].h, NULL, NULL};
        cv.px = malloc((size_t)cv.w * cv.h * 4);
        cv.mark = calloc((size_t)cv.w * cv.h, 1);
        if (!cv.px || !cv.mark) {
            free(cv.px), free(cv.mark);
            return 0;
        }
        /* level 0, decoded */
        int bw = clus_bw(c, 0);
        for (int by = 0; by < clus_bh(c, 0); by++)
            for (int bx = 0; bx < bw; bx++) {
                uint8_t px[16][4];
                dxt5_decode(g_blocks[0][c][0] + ((size_t)by * bw + bx) * 16, px);
                for (int i = 0; i < 16; i++)
                    memcpy(cv.px + ((size_t)(by * 4 + i / 4) * cv.w + bx * 4 + i % 4) * 4, px[i], 4);
            }
        for (int i = 0; i < NPICS; i++)
            if (g_pics[i].x0 >= cv.x && g_pics[i].x1 <= cv.x + cv.w && g_pics[i].y0 >= cv.y && g_pics[i].y1 <= cv.y + cv.h)
                draw_pic(&cv, style, i);

        /* each level: the blocks with a drawn texel re-encoded, the rest the original's bytes */
        for (int lv = 0; lv < AT_LEVELS; lv++) {
            int w = cv.w >> lv, h = cv.h >> lv;
            if (lv) {                           /* box-filter the level above, alpha-weighted */
                int pw = w * 2;
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++) {
                        const uint8_t *s[4] = {cv.px + ((size_t)(2 * y) * pw + 2 * x) * 4,
                                               cv.px + ((size_t)(2 * y) * pw + 2 * x + 1) * 4,
                                               cv.px + ((size_t)(2 * y + 1) * pw + 2 * x) * 4,
                                               cv.px + ((size_t)(2 * y + 1) * pw + 2 * x + 1) * 4};
                        int m = cv.mark[(size_t)(2 * y) * pw + 2 * x] | cv.mark[(size_t)(2 * y) * pw + 2 * x + 1] |
                                cv.mark[(size_t)(2 * y + 1) * pw + 2 * x] | cv.mark[(size_t)(2 * y + 1) * pw + 2 * x + 1];
                        int sa = s[0][3] + s[1][3] + s[2][3] + s[3][3];
                        uint8_t out[4];
                        for (int k = 0; k < 3; k++) {
                            int v = sa ? (s[0][k] * s[0][3] + s[1][k] * s[1][3] + s[2][k] * s[2][3] + s[3][k] * s[3][3] + sa / 2) / sa
                                       : (s[0][k] + s[1][k] + s[2][k] + s[3][k] + 2) / 4;
                            out[k] = (uint8_t)v;
                        }
                        out[3] = (uint8_t)((sa + 2) / 4);
                        memcpy(cv.px + ((size_t)y * w + x) * 4, out, 4);   /* in place: (x,y) is read before written */
                        cv.mark[(size_t)y * w + x] = (uint8_t)m;
                    }
            }
            size_t n = clus_bytes(c, lv);
            uint8_t *dst = malloc(n);
            if (!dst) {
                free(cv.px), free(cv.mark);
                return 0;
            }
            memcpy(dst, g_blocks[0][c][lv], n);
            int lbw = clus_bw(c, lv);
            for (int by = 0; by < clus_bh(c, lv); by++)
                for (int bx = 0; bx < lbw; bx++) {
                    int drawn = 0;
                    for (int i = 0; i < 16 && !drawn; i++)
                        drawn = cv.mark[(size_t)(by * 4 + i / 4) * w + bx * 4 + i % 4];
                    if (!drawn)
                        continue;
                    uint8_t px[16][4], *blk = dst + ((size_t)by * lbw + bx) * 16;
                    dxt5_decode(blk, px);
                    for (int i = 0; i < 16; i++) {
                        size_t o = (size_t)(by * 4 + i / 4) * w + bx * 4 + i % 4;
                        if (cv.mark[o])
                            memcpy(px[i], cv.px + o * 4, 4);
                    }
                    dxt5_encode((const uint8_t(*)[4])px, blk);
                }
            free(g_blocks[style][c][lv]);
            g_blocks[style][c][lv] = dst;
        }
        free(cv.px), free(cv.mark);
    }
    return 1;
}

static uint32_t atlas_hash(const uint8_t *d, size_t n)
{
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < n; i += 37)
        h = (h ^ d[i]) * 0x01000193u;
    return h;
}

#ifndef PADICONS_TEST
/* ---- the game's side: the atlas textures and the switch ---- */

#define MAX_TEX 4
static struct { GLuint id; unsigned levels; int shown; } g_tex[MAX_TEX];   /* shown: the style it shows */
static int g_on = -1;

int gmv_padicons_on(void)
{
    if (g_on < 0) {
        /* in from the start whenever the file asks for it or the options menu can, so that the menu's line
           and the in-game ORIGINAL_MAC_EXPERIENCE switch can turn it on and off; gmv_config_live decides what
           is shown */
        const char *e = gmv_envkey("GMV_PAD_ICONS");
        if (!e)
            e = gmv_config_file_value("PAD_ICONS");
        g_on = (e && *e && *e != '0') || gmv_config_tab();
        if (g_on) {
            const char *now = gmv_config_live("GMV_PAD_ICONS");
            gmv_log("[padicons] the options menu's gamepad pictures can follow the pad in use (Xbox, DualShock 4, "
                    "DualSense); %s for now\n", now && *now && *now != '0' ? "on" : "off");
        }
    }
    return g_on;
}

static GLuint bound_2d(void)
{
    GLint b = 0;
    glGetIntegerv(0x8069, &b);                  /* GL_TEXTURE_BINDING_2D */
    return (GLuint)b;
}

void gmv_padicons_upload(int level, unsigned fmt, int w, int h, int size, const void *data)
{
    if (!data || fmt != AT_DXT5 || level >= AT_LEVELS || w != AT_W >> level || h != AT_H >> level)
        return;
    GLuint id = bound_2d();
    if (!level) {
        if (size != AT_W * AT_H)
            return;
        uint32_t hs = atlas_hash(data, (size_t)size);
        int k = -1;
        for (int i = 0; i < MAX_TEX; i++)
            if (g_tex[i].id == id || (k < 0 && !g_tex[i].id))
                k = i;
        if (hs != AT_HASH) {
            if (k >= 0 && g_tex[k].id == id)
                g_tex[k].id = 0;                /* the name now holds another picture */
            gmv_trace("[padicons] a 4096x2048 DXT5 texture (%u, sample %08x) is not the gamepad atlas\n", id, hs);
            return;
        }
        if (k < 0)
            return;
        g_tex[k].id = id;
        g_tex[k].levels = 1;
        g_tex[k].shown = 0;
        if (!(g_orig_levels & 1))
            keep_level(0, data);
        gmv_trace("[padicons] the gamepad atlas (splash_gamepad) is texture %u\n", id);
        return;
    }
    for (int i = 0; i < MAX_TEX; i++)
        if (g_tex[i].id && g_tex[i].id == id) {
            if (!(g_orig_levels & (1u << level)))
                keep_level(level, data);
            g_tex[i].levels |= 1u << level;
        }
}

void gmv_padicons_deleted(int n, const unsigned *names)
{
    for (int j = 0; names && j < n; j++)
        for (int i = 0; i < MAX_TEX; i++)
            if (g_tex[i].id && g_tex[i].id == names[j])
                g_tex[i].id = 0;
}

void gmv_padicons_sync(void)
{
    int any = 0;
    for (int i = 0; i < MAX_TEX; i++)
        any |= g_tex[i].id && g_tex[i].levels == (1u << AT_LEVELS) - 1;
    if (!any || g_orig_levels != (1u << AT_LEVELS) - 1)
        return;
    const char *e = gmv_config_live("GMV_PAD_ICONS");
    int want = e && *e && *e != '0' ? gmv_joy_style() : 0;
    if (want < 0 || want >= NSTYLE)
        want = 0;
    static int failed[NSTYLE];
    for (int i = 0; i < MAX_TEX; i++) {
        if (!g_tex[i].id || g_tex[i].levels != (1u << AT_LEVELS) - 1 || g_tex[i].shown == want)
            continue;
        if (!g_blocks[want][0][0]) {
            if (failed[want])
                continue;
            LARGE_INTEGER f, t0, t1;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&t0);
            if (!build_style(want)) {
                failed[want] = 1;
                gmv_log("[padicons] out of memory drawing the %s pictures\n", g_style_name[want]);
                continue;
            }
            QueryPerformanceCounter(&t1);
            gmv_trace("[padicons] drew the %s pictures in %.1f ms\n", g_style_name[want],
                    (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart);
        }
        static void(APIENTRY * sub)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *);
        static void(APIENTRY * bindbuf)(GLenum, GLuint);
        if (!sub) {
            sub = (void(APIENTRY *)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *))(void *)
                gl_real("glCompressedTexSubImage2D");
            bindbuf = (void(APIENTRY *)(GLenum, GLuint))(void *)gl_real("glBindBuffer");
        }
        if (!sub)
            return;
        GLint unpack = 0;
        glGetIntegerv(0x88EF, &unpack);         /* GL_PIXEL_UNPACK_BUFFER_BINDING */
        if (unpack && bindbuf)
            bindbuf(0x88EC, 0);
        GLuint was = bound_2d();
        glBindTexture(GL_TEXTURE_2D, g_tex[i].id);
        for (int c = 0; c < NCLUS; c++)
            for (int lv = 0; lv < AT_LEVELS; lv++)
                sub(GL_TEXTURE_2D, lv, g_clus[c].x >> lv, g_clus[c].y >> lv, g_clus[c].w >> lv, g_clus[c].h >> lv, AT_DXT5,
                    (GLsizei)clus_bytes(c, lv), g_blocks[want][c][lv]);
        glBindTexture(GL_TEXTURE_2D, was);
        if (unpack && bindbuf)
            bindbuf(0x88EC, (GLuint)unpack);
        g_tex[i].shown = want;
        gmv_trace("[padicons] texture %u now shows %s gamepad pictures\n", g_tex[i].id, g_style_name[want]);
    }
}
#endif
