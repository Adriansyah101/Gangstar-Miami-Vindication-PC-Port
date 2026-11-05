/* tab_swf.c -- GAMEPATCH_TAB: the SOUND page's rows and the page's credit, added to the menu file as it loads */
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

#define OM_RES_SIZE    0x000039dau   /* (app, &index, &w, &h); their count to app + OM_RES_COUNT    */
#define OM_OPENER_SITE 0x002f9854u   /* RenderFX set-up: register_file_opener_callback(opener)      */
#define OM_OPENER_SET  0x002ad250u
#define OM_GS_MALLOC   0x00321530u   /* gameswf's allocator: (bytes, 0), and its free (p, bytes)    */
#define OM_GS_FREE     0x00321500u
#define OM_FILE_INIT   0x003054a0u   /* tu_file(file, data, read, write, seek, seek_to_end, tell, eof, close) */
#define OM_FILE_CLOSE  0x00306170u
#define OM_FILE_BYTES  0x28
#define OM_INFLATE_INIT 0x00363020u  /* the game's zlib 1.2.3: inflateInit_(stream, "1.2.3", 0x38)  */
#define OM_INFLATE     0x003636f0u
#define OM_INFLATE_END 0x00362d80u

/* ---- the SOUND page's Custom Radio rows and the GAMEPATCH page's credit, added to the menu file as it loads ----
   widgetVolumes (sprite 797) places three rows 98.5 units apart: a title (txtSFX, txtVFX, txtMusic:
   one edittext each, PlaceObject3 with its outline filter) and a bar (barSFX, barVFX, barMusic:
   sprite 794 with the slider btnVolume and the speaker btnMute).  With CUSTOM_RADIO's rows the page
   has five, 88 units apart and 80 higher (the user found the four too low):
     4. "Custom Radio Volume": a copy of the Music row (txtRadio, barRadio);
     5. "Custom Radio": its title (txtRadioOn) and the options' ◀ value ▶ (widgetActionKeyTitle's
        bar, btnLeft, btnRight and txtValue, as the GAMEPATCH lines have them), named crValueBar,
        btnCrLeft, btnCrRight and crTextValue.
   The titles are edittexts of their own (the Music title's, wider, with their text in it).  With
   the options tab, widgetActionKeyTitle (sprite 759) also gets OM_CREDIT (txtPatchBy) at the
   screen's top right, which only the GAMEPATCH page shows (the author's credit). */
#define CR_ROW_DEPTH   40                       /* our placements in sprite 797: 40.. */
#define CR_SEL_X       3640                     /* row 5's ◀ value ▶: centre (under the sliders' centre) */
#define CR_SEL_Y       (CR_BAR0 + 4 * CR_STEP + 342)
#define CR_SEL_SCALE   0.6
#define CR_CREDIT_DEPTH 60                      /* in sprite 759 */
/* Our text boxes: wider than txtMusic's (-40..3120 twips, centre 1540), but gameswf lays a text out
   from the box's left edge as if it were at txtMusic's -40, so a box that starts further left shows
   its text too far right.  Ours start at -40 too, 8360 wide, and their placements move left by
   the difference of the centres. */
#define CR_TEXT_X0  (-40)
#define CR_TEXT_X1  8320
#define CR_TEXT_DX  (1540 - (CR_TEXT_X0 + CR_TEXT_X1) / 2)

static const char *const cr_sel_names[4] = {"crValueBar", "btnCrLeft", "btnCrRight", "crTextValue"};

/* Write a matrix (no rotation if !r) at bit; the bit after it. */
static void om_matrix_write(unsigned char *d, size_t *bit, int32_t sx, int32_t sy, int r, int32_t r0, int32_t r1,
                            int32_t tx, int32_t ty)
{
    int has_s = sx != 0x10000 || sy != 0x10000;
    om_put_bits(d, bit, (uint32_t)has_s, 1);
    if (has_s) {
        int k = om_sbits_n(sx) > om_sbits_n(sy) ? om_sbits_n(sx) : om_sbits_n(sy);
        om_put_bits(d, bit, (uint32_t)k, 5);
        om_put_bits(d, bit, (uint32_t)sx, k);
        om_put_bits(d, bit, (uint32_t)sy, k);
    }
    om_put_bits(d, bit, (uint32_t)r, 1);
    if (r) {
        int k = om_sbits_n(r0) > om_sbits_n(r1) ? om_sbits_n(r0) : om_sbits_n(r1);
        om_put_bits(d, bit, (uint32_t)k, 5);
        om_put_bits(d, bit, (uint32_t)r0, k);
        om_put_bits(d, bit, (uint32_t)r1, k);
    }
    int k = om_sbits_n(tx) > om_sbits_n(ty) ? om_sbits_n(tx) : om_sbits_n(ty);
    om_put_bits(d, bit, (uint32_t)k, 5);
    om_put_bits(d, bit, (uint32_t)tx, k);
    om_put_bits(d, bit, (uint32_t)ty, k);
}

/* The parts of a PlaceObject2/3 tag (code, data d of n bytes) that has a character, a matrix and
   a name: where its matrix and its name start and end, and the matrix.  0 if it is not one. */
typedef struct {
    size_t ms, me, ns, ne;
    int32_t sx, sy, r0, r1, tx, ty;
    int r;
} om_place3;

static int om_place3_read(int code, const unsigned char *d, size_t n, om_place3 *o)
{
    size_t p = 0, bit;
    unsigned fl, fl2 = 0;
    int k;
    memset(o, 0, sizeof *o);
    o->sx = o->sy = 0x10000;
    if (n < 4)
        return 0;
    fl = d[p++];
    if (code == 70)
        fl2 = d[p++];
    p += 2;                                     /* depth */
    if (code == 70 && ((fl2 & 8) || ((fl2 & 0x10) && (fl & 2)))) {
        const unsigned char *z = memchr(d + p, 0, n - p);
        if (!z)
            return 0;
        p = (size_t)(z - d) + 1;
    }
    if (!(fl & 2) || !(fl & 4) || !(fl & 0x20) || p + 3 > n)
        return 0;
    p += 2;                                     /* character */
    o->ms = p;
    bit = p * 8;
    if (om_bits(d, &bit, 1)) {
        k = (int)om_bits(d, &bit, 5);
        o->sx = om_sbits(d, &bit, k);
        o->sy = om_sbits(d, &bit, k);
    }
    if ((o->r = (int)om_bits(d, &bit, 1)) != 0) {
        k = (int)om_bits(d, &bit, 5);
        o->r0 = om_sbits(d, &bit, k);
        o->r1 = om_sbits(d, &bit, k);
    }
    k = (int)om_bits(d, &bit, 5);
    o->tx = om_sbits(d, &bit, k);
    o->ty = om_sbits(d, &bit, k);
    o->me = p = (bit + 7) / 8;
    if (fl & 8) {
        int add, mult;
        bit = p * 8;
        add = (int)om_bits(d, &bit, 1);
        mult = (int)om_bits(d, &bit, 1);
        k = (int)om_bits(d, &bit, 4);
        bit += (size_t)k * 4 * (add + mult);
        p = (bit + 7) / 8;
    }
    if (fl & 0x10)
        p += 2;                                 /* ratio */
    if (p >= n || !memchr(d + p, 0, n - p))
        return 0;
    o->ns = p;
    o->ne = p + strlen((const char *)d + p) + 1;
    return 1;
}

/* A copy of that tag, written at o: at depth, of character cid (-1: its own), with the matrix m4
   {sx, sy, tx, ty} (NULL: its own, dx twips right and dy lower) and named name; its length, or 0
   if the tag is not one it can copy. */
static size_t om_place_clone(unsigned char *o, int code, const unsigned char *d, size_t n, int depth, int cid,
                             const int32_t *m4, int32_t dx, int32_t dy, const char *name)
{
    om_place3 m;
    unsigned char body[512];
    size_t p, bit;
    if (n > 400 || !om_place3_read(code, d, n, &m))
        return 0;
    memset(body, 0, sizeof body);
    memcpy(body, d, m.ms);                      /* flags, depth, class name, character */
    body[(code == 70) + 1] = (unsigned char)depth;
    body[(code == 70) + 2] = (unsigned char)(depth >> 8);
    if (cid >= 0) {
        body[m.ms - 2] = (unsigned char)cid;
        body[m.ms - 1] = (unsigned char)(cid >> 8);
    }
    bit = m.ms * 8;
    if (m4)
        om_matrix_write(body, &bit, m4[0], m4[1], 0, 0, 0, m4[2], m4[3]);
    else
        om_matrix_write(body, &bit, m.sx, m.sy, m.r, m.r0, m.r1, m.tx + dx, m.ty + dy);
    p = (bit + 7) / 8;
    memcpy(body + p, d + m.me, m.ns - m.me);    /* colour transform, ratio */
    p += m.ns - m.me;
    memcpy(body + p, name, strlen(name) + 1);
    p += strlen(name) + 1;
    memcpy(body + p, d + m.ne, n - m.ne);       /* clip depth, filters, blend mode, ... */
    p += n - m.ne;
    if (p < 63) {
        o[0] = (unsigned char)(code << 6 | p);
        o[1] = (unsigned char)((code << 6 | p) >> 8);
        memcpy(o + 2, body, p);
        return p + 2;
    }
    o[0] = (unsigned char)(code << 6 | 0x3f);
    o[1] = (unsigned char)((code << 6 | 0x3f) >> 8);
    {
        uint32_t len = (uint32_t)p;
        memcpy(o + 2, &len, 4);
    }
    memcpy(o + 6, body, p);
    return p + 6;
}

/* The highest character id the file defines. */
static int swf_max_cid(const unsigned char *s, size_t start, size_t n)
{
    static const unsigned char defs[] = {2, 6, 7, 10, 11, 14, 20, 21, 22, 32, 33, 34, 35, 36, 37, 39, 46, 48, 60,
                                         75, 83, 84, 90, 91};
    int code, top = 0;
    size_t d, l;
    for (size_t p = start, next; (next = om_tag(s, p, n, &code, &d, &l)) != 0 && code; p = next)
        if (l >= 2 && memchr(defs, code, sizeof defs)) {
            int cid = s[d] | s[d + 1] << 8;
            if (cid > top)
                top = cid;
        }
    return top;
}

/* A DefineEditText like the SOUND page's titles (Arial 23, centred, read-only HTML) holding text,
   x0..x1 twips wide, in colour rgb ("rrggbb"), written at o; its length. */
static size_t swf_edittext(unsigned char *o, int cid, int32_t x0, int32_t x1, const char *rgb, const char *text)
{
    static const unsigned char mid[] = {0xed, 0x23, 43, 0, 0xcc, 0x01};        /* flags; font 43, 23 px */
    static const unsigned char layout[] = {2, 0, 0, 0, 0, 0, 0, 0x28, 0, 0};   /* centred, leading 2, no name */
    unsigned char d[600] = {0};
    size_t bit = 2 * 8, p;
    int32_t y0 = -40, y1 = 556;
    int k = 1;
    unsigned v = (unsigned)strtoul(rgb, NULL, 16);
    while (om_sbits_n(x0) > k || om_sbits_n(x1) > k || om_sbits_n(y0) > k || om_sbits_n(y1) > k)
        k++;
    d[0] = (unsigned char)cid;
    d[1] = (unsigned char)(cid >> 8);
    om_put_bits(d, &bit, (uint32_t)k, 5);
    om_put_bits(d, &bit, (uint32_t)x0, k);
    om_put_bits(d, &bit, (uint32_t)x1, k);
    om_put_bits(d, &bit, (uint32_t)y0, k);
    om_put_bits(d, &bit, (uint32_t)y1, k);
    p = (bit + 7) / 8;
    memcpy(d + p, mid, sizeof mid);
    p += sizeof mid;
    d[p++] = (unsigned char)(v >> 16);
    d[p++] = (unsigned char)(v >> 8);
    d[p++] = (unsigned char)v;
    d[p++] = 0xff;
    memcpy(d + p, layout, sizeof layout);
    p += sizeof layout;
    p += (size_t)snprintf((char *)d + p, sizeof d - p - 1,
                          "<p align=\"center\"><font face=\"Arial\" size=\"23\" color=\"#%s\" letterSpacing=\"0.000000\" "
                          "kerning=\"1\">%s</font></p>", rgb, text) + 1;
    o[0] = (unsigned char)(37 << 6 | 0x3f);
    o[1] = (unsigned char)((37 << 6 | 0x3f) >> 8);
    {
        uint32_t len = (uint32_t)p;
        memcpy(o + 2, &len, 4);
    }
    memcpy(o + 6, d, p);
    return p + 6;
}

/* A placement tag of a sprite's first frame, rewritten at o (its length) or left as it is (0). */
typedef size_t (*swf_fix_fn)(unsigned char *o, int code, const unsigned char *d, size_t n);

/* The swf s with sprite cid's first frame changed: each placement through fix (if any), the an
   bytes of add before its first ShowFrame, and the dn bytes of defs (top-level definitions) before
   the sprite.  NULL if the sprite is not there. */
static unsigned char *swf_sprite_edit(const unsigned char *s, size_t n, int cid, const unsigned char *defs, size_t dn,
                                      const unsigned char *add, size_t an, swf_fix_fn fix, size_t *out_n)
{
    size_t start = 8 + (5 + 4 * (s[8] >> 3) + 7) / 8 + 4, q, d, l;
    int code, done = 0;
    unsigned char *out = malloc(n + dn + an + 8192);
    if (!out)
        return NULL;
    memcpy(out, s, start);
    q = start;
    for (size_t p = start, next; p < n; p = next) {
        if (!(next = om_tag(s, p, n, &code, &d, &l))) {
            free(out);
            return NULL;
        }
        if (code != 39 || done || l < 4 || (s[d] | s[d + 1] << 8) != cid) {
            memcpy(out + q, s + p, next - p);
            q += next - p;
        } else {
            size_t at;
            int added = 0, c2;
            uint32_t len;
            memcpy(out + q, defs, dn);
            q += dn;
            out[q] = (unsigned char)(39 << 6 | 0x3f);
            out[q + 1] = (unsigned char)((39 << 6 | 0x3f) >> 8);
            at = q + 2;
            q += 6;
            memcpy(out + q, s + d, 4);          /* id, frame count */
            q += 4;
            for (size_t t = d + 4, tn, td, tl; t < d + l; t = tn) {
                size_t w;
                if (!(tn = om_tag(s, t, d + l, &c2, &td, &tl))) {
                    free(out);
                    return NULL;
                }
                if (c2 == 1 && !added) {
                    memcpy(out + q, add, an);
                    q += an;
                    added = 1;
                }
                w = !added && fix && (c2 == 26 || c2 == 70) ? fix(out + q, c2, s + td, tl) : 0;
                if (w)
                    q += w;
                else {
                    memcpy(out + q, s + t, tn - t);
                    q += tn - t;
                }
                if (c2 == 0)
                    break;
            }
            len = (uint32_t)(q - at - 4);
            memcpy(out + at, &len, 4);
            done = added;
            if (!done) {
                free(out);
                return NULL;
            }
        }
        if (code == 0)
            break;
    }
    if (!done) {
        free(out);
        return NULL;
    }
    {
        uint32_t total = (uint32_t)q;
        memcpy(out + 4, &total, 4);
    }
    *out_n = q;
    return out;
}

/* The placement named name in sprite cid's first frame: its tag code, data and length. */
static int swf_find_named(const unsigned char *s, size_t n, int cid, const char *name, int *code, size_t *pd,
                          size_t *pl)
{
    size_t start = 8 + (5 + 4 * (s[8] >> 3) + 7) / 8 + 4, d, l;
    int c;
    if (!om_sprite(s, start, n, cid, &d, &l))
        return 0;
    for (size_t p = d + 4, next, td, tl; (next = om_tag(s, p, d + l, &c, &td, &tl)) != 0 && c != 1 && c; p = next) {
        om_place3 o;
        if ((c == 26 || c == 70) && om_place3_read(c, s + td, tl, &o) && !strcmp((const char *)s + td + o.ns, name)) {
            *code = c;
            *pd = td;
            *pl = tl;
            return 1;
        }
    }
    return 0;
}

/* The SOUND page's three rows to their new places (88 units apart, 80 higher). */

/* The swf s (n bytes, uncompressed, header included) with the SOUND page's Custom Radio rows, or
   NULL if the page is not the one expected. */
static unsigned char *cr_swf_patch(const unsigned char *s, size_t n, size_t *out_n)
{
    static const char *const head_names[4] = {NULL, "btnLeft", "btnRight", "txtValue"};
    size_t start, d, l, td, tl, bd, bl, q, dn, w1, w2, w3;
    int tc, bc, code, top;
    om_place vol, head, sel[4];
    unsigned char defs[2 * 700], add[8 * 300];
    if (n < 32 || memcmp(s, "FWS", 3))
        return NULL;
    start = 8 + (5 + 4 * (s[8] >> 3) + 7) / 8 + 4;
    if (!om_find_place(s, start, n, "widgetVolumes", &vol) || !om_find_place(s, start, n, "widgetActionKeyTitle", &head) ||
        !om_sprite(s, start, n, head.cid, &d, &l))
        return NULL;
    for (int e = 0; e < 4; e++)
        if (!om_find_place(s, d + 4, d + l, head_names[e], &sel[e]))
            return NULL;
    if (!swf_find_named(s, n, vol.cid, "txtMusic", &tc, &td, &tl) || !swf_find_named(s, n, vol.cid, "barMusic", &bc, &bd, &bl) ||
        swf_find_named(s, n, vol.cid, "txtRadio", &code, &d, &l))
        return NULL;                            /* (not twice) */
    top = swf_max_cid(s, start, n);
    if (top + 2 > 0xffff)
        return NULL;
    dn = swf_edittext(defs, top + 1, CR_TEXT_X0, CR_TEXT_X1, "42d8fb", "Custom Radio Volume");
    dn += swf_edittext(defs + dn, top + 2, CR_TEXT_X0, CR_TEXT_X1, "42d8fb", "Custom Radio");
    w1 = om_place_clone(add, tc, s + td, tl, CR_ROW_DEPTH, top + 1, NULL, CR_TEXT_DX, CR_TITLE0 + 3 * CR_STEP - 4150 + CR_OFF,
                        "txtRadio");
    w2 = w1 ? om_place_clone(add + w1, bc, s + bd, bl, CR_ROW_DEPTH + 1, -1, NULL, 0, CR_BAR0 + 3 * CR_STEP - 4671 + CR_OFF, "barRadio")
            : 0;
    w3 = w2 ? om_place_clone(add + w1 + w2, tc, s + td, tl, CR_ROW_DEPTH + 2, top + 2, NULL, CR_TEXT_DX,
                             CR_TITLE0 + 4 * CR_STEP - 4150 + CR_OFF, "txtRadioOn") : 0;
    if (!w3)
        return NULL;
    q = w1 + w2 + w3;
    {
        const double g = CR_SEL_SCALE;
        const double ox = CR_SEL_X - g * (OM_SEL_X0 + OM_SEL_X1) / 2, oy = CR_SEL_Y - g * (OM_SEL_Y0 + OM_SEL_Y1) / 2;
        for (int e = 0; e < 4; e++) {
            om_place m = sel[e];
            m.sx = (int32_t)lround(g * sel[e].sx);
            m.sy = (int32_t)lround(g * sel[e].sy);
            m.tx = (int32_t)lround(ox + g * sel[e].tx);
            m.ty = (int32_t)lround(oy + g * sel[e].ty) + CR_OFF;
            q += om_place_write(add + q, CR_ROW_DEPTH + 3 + e, &m, cr_sel_names[e]);
        }
    }
    return swf_sprite_edit(s, n, vol.cid, defs, dn, add, q, NULL, out_n);
}

/* The swf s with OM_CREDIT in widgetActionKeyTitle (the options tab's header), or NULL. */
static unsigned char *om_credit_patch(const unsigned char *s, size_t n, size_t *out_n)
{
    size_t start, td, tl, dn, an, d, l;
    int tc, code, top;
    om_place vol, head;
    unsigned char defs[700], add[300];
    if (n < 32 || memcmp(s, "FWS", 3))
        return NULL;
    start = 8 + (5 + 4 * (s[8] >> 3) + 7) / 8 + 4;
    if (!om_find_place(s, start, n, "widgetVolumes", &vol) || !om_find_place(s, start, n, "widgetActionKeyTitle", &head) ||
        !swf_find_named(s, n, vol.cid, "txtMusic", &tc, &td, &tl) || swf_find_named(s, n, head.cid, "txtPatchBy", &code, &d, &l))
        return NULL;
    top = swf_max_cid(s, start, n);
    if (top + 1 > 0xffff)
        return NULL;
    dn = swf_edittext(defs, top + 1, CR_TEXT_X0, CR_TEXT_X1, "42d8fb", OM_CREDIT);
    {
        /* The titles' size, centred on the stage's (1072, 63.3): the text spans about x 917..1227 of the 1280, right
           of the page's title (widgetTitle, its box 625..1043, the text at its centre).  1227 is where the old, shorter
           credit ended; further right was cut off at the screen's edge in a test.  The box's centre is
           ((CR_TEXT_X0 + CR_TEXT_X1) / 2, 258) twips in its own units. */
        const double g = 1.0, cx = 1072.0 * 20 - head.tx, cy = 63.3 * 20 - head.ty;
        const int32_t m4[4] = {(int32_t)lround(g * 65536), (int32_t)lround(g * 65536),
                               (int32_t)lround(cx - g * (CR_TEXT_X0 + CR_TEXT_X1) / 2), (int32_t)lround(cy - g * 258)};
        if (!(an = om_place_clone(add, tc, s + td, tl, CR_CREDIT_DEPTH, top + 1, m4, 0, 0, "txtPatchBy")))
            return NULL;
    }
    return swf_sprite_edit(s, n, head.cid, defs, dn, add, an, NULL, out_n);
}

/* A gameswf stream over memory: (dst, bytes, data), (pos, data), (data). */
typedef struct { unsigned char *p; int n, at; } om_mem;

__attribute__((cdecl, force_align_arg_pointer))
static int om_mem_read(void *dst, int bytes, om_mem *m)
{
    int k = bytes < m->n - m->at ? bytes : m->n - m->at;
    if (k <= 0)
        return 0;
    memcpy(dst, m->p + m->at, (size_t)k);
    m->at += k;
    return k;
}

__attribute__((cdecl, force_align_arg_pointer))
static int om_mem_write(const void *src, int bytes, om_mem *m)
{
    (void)src, (void)bytes, (void)m;
    return 0;
}

__attribute__((cdecl, force_align_arg_pointer))
static int om_mem_seek(int pos, om_mem *m)
{
    if (pos < 0 || pos > m->n)
        return -1;
    m->at = pos;
    return 0;
}

__attribute__((cdecl, force_align_arg_pointer))
static int om_mem_seek_end(om_mem *m)
{
    m->at = m->n;
    return 0;
}

__attribute__((cdecl, force_align_arg_pointer))
static int om_mem_tell(om_mem *m)
{
    return m->at;
}

__attribute__((cdecl, force_align_arg_pointer))
static int om_mem_eof(om_mem *m)
{
    return m->at >= m->n;
}

__attribute__((cdecl, force_align_arg_pointer))
static int om_mem_close(om_mem *m)
{
    free(m->p);
    free(m);
    return 0;
}

/* newmenu.fua's stream f read to the end and uncompressed: the swf with its header, or NULL. */
static unsigned char *om_swf_read(uint32_t *f, unsigned char **raw, size_t *raw_n, size_t *n)
{
    typedef int (*read_fn)(void *, int, void *);
    size_t cap = 1 << 22, got = 0;
    unsigned char *b = malloc(cap), *swf;
    uint32_t total, zs[14] = {0};               /* a z_stream */
    int r;
    *raw = NULL;
    for (;;) {
        int k;
        if (!b)
            return NULL;
        if (got == cap) {
            unsigned char *more = realloc(b, cap * 2);
            if (!more) {
                free(b);
                return NULL;
            }
            b = more;
            cap *= 2;
        }
        if ((k = ((read_fn)(uintptr_t)f[2])(b + got, (int)(cap - got), (void *)(uintptr_t)f[0])) <= 0)
            break;
        got += (size_t)k;
    }
    *raw = b;
    *raw_n = got;
    if (got < 8 || (memcmp(b, "CWS", 3) && memcmp(b, "FWS", 3)))
        return NULL;
    memcpy(&total, b + 4, 4);
    if (b[0] == 'F')
        return total == got ? (*n = got, b) : NULL;
    if (total <= 8 || total > (64u << 20) || !(swf = malloc(total)))
        return NULL;
    memcpy(swf, b, 8);
    swf[0] = 'F';
    zs[0] = (uint32_t)(uintptr_t)(b + 8);
    zs[1] = (uint32_t)(got - 8);
    zs[3] = (uint32_t)(uintptr_t)(swf + 8);
    zs[4] = total - 8;
    r = ((int (*)(void *, const char *, int))(uintptr_t)OM_INFLATE_INIT)(zs, "1.2.3", (int)sizeof zs);
    if (r == 0) {
        r = ((int (*)(void *, int))(uintptr_t)OM_INFLATE)(zs, 4);          /* Z_FINISH: Z_STREAM_END */
        ((int (*)(void *))(uintptr_t)OM_INFLATE_END)(zs);
    }
    if (r != 1 || zs[5] != total - 8) {
        free(swf);
        return NULL;
    }
    *n = total;
    return swf;
}

__attribute__((cdecl, force_align_arg_pointer))
static uint32_t *gmv_om_open(const char *name)
{
    typedef void (*init_fn)(void *, void *, void *, void *, void *, void *, void *, void *, void *);
    uint32_t *f = om_opener ? (uint32_t *)om_opener(name) : NULL, *mf;
    size_t k = name ? strlen(name) : 0, raw_n = 0, n = 0, out_n = 0;
    unsigned char *raw, *swf, *out = NULL;
    om_mem *m;
    if (!f || f[9] || k < 11 || strcmp(name + k - 11, "newmenu.fua"))
        return f;
    swf = om_swf_read(f, &raw, &raw_n, &n);
    int sel = 0;
    if (swf && om_tab_files) {
        size_t cn = 0;
        unsigned char *cred;
        sel = (out = om_swf_patch(swf, n, &out_n)) != NULL;
        cred = om_credit_patch(out ? out : swf, out ? out_n : n, &cn);    /* OM_CREDIT */
        om_credit = cred != NULL;
        if (cred) {
            free(out);
            out = cred;
            out_n = cn;
        } else
            gmv_log("[gamepatch] options tab: the header is not the one expected; no credit\n");
    }
    if (swf && g_cr_row_want) {                 /* the SOUND page's fourth row (CUSTOM_RADIO) */
        size_t row_n = 0;
        unsigned char *row = cr_swf_patch(out ? out : swf, out ? out_n : n, &row_n);
        g_cr_row = row != NULL;
        if (row) {
            free(out);
            out = row;
            out_n = row_n;
            gmv_log("[gamepatch] the SOUND page gets the Custom Radio rows\n");
        } else
            gmv_log("[gamepatch] the SOUND page is not the one expected; no Custom Radio row\n");
    }
    if (swf != raw)
        free(swf);
    mf = ((uint32_t * (*)(int, int))(uintptr_t)OM_GS_MALLOC)(OM_FILE_BYTES, 0);
    m = malloc(sizeof *m);
    if (!raw || !mf || !m) {                    /* (no memory: the stream from its start, if it can) */
        free(raw);
        free(out);
        free(m);
        if (mf)
            ((void (*)(void *, int))(uintptr_t)OM_GS_FREE)(mf, 0);
        ((int (*)(int, void *))(uintptr_t)f[4])(0, (void *)(uintptr_t)f[0]);
        om_selectors = 0;
        gmv_log("[gamepatch] options tab: could not read the menu file; the list keeps its key boxes\n");
        return f;
    }
    ((void (*)(void *))(uintptr_t)OM_FILE_CLOSE)(f);
    ((void (*)(void *, int))(uintptr_t)OM_GS_FREE)(f, 0);
    om_selectors = sel;
    if (om_tab_files)
        gmv_log(sel ? "[gamepatch] options tab: the list's lines get a value selector\n"
                    : "[gamepatch] options tab: menu file not the one expected; the list keeps its key boxes\n");
    if (out) {
        free(raw);
        m->p = out;
        m->n = (int)out_n;
        gmv_trace("[gamepatch] menu file %u bytes, %u patched\n", (unsigned)n, (unsigned)out_n);
    } else {
        m->p = raw;                             /* the file as it was read */
        m->n = (int)raw_n;
    }
    m->at = 0;
    memset(mf, 0, OM_FILE_BYTES);
    ((init_fn)(uintptr_t)OM_FILE_INIT)(mf, m, (void *)om_mem_read, (void *)om_mem_write, (void *)om_mem_seek,
                                       (void *)om_mem_seek_end, (void *)om_mem_tell, (void *)om_mem_eof,
                                       (void *)om_mem_close);
    return mf;
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_om_set_opener(void *(*opener)(const char *))
{
    if (opener && opener != (void *(*)(const char *))gmv_om_open) {
        om_opener = opener;
        opener = (void *(*)(const char *))gmv_om_open;
    }
    ((void (*)(void *))(uintptr_t)OM_OPENER_SET)((void *)opener);
}

/* Our opener in front of the game's (once): 1 if it is, 0 if the set-up is not the expected one. */
int om_opener_hook(void)
{
    if (sm_is_call(OM_OPENER_SITE, (uint32_t)(uintptr_t)gmv_om_set_opener))
        return 1;
    return sm_is_call(OM_OPENER_SITE, OM_OPENER_SET) && sm_redirect(OM_OPENER_SITE, (void *)gmv_om_set_opener);
}

void om_selectors_visible(void *fx, int line, int on)
{
    for (int e = 0; e < 4; e++)
        om_line_visible(fx, line, om_sel_names[e], on);
}

unsigned om_resolution(int i, unsigned *h)        /* its width; the count to the app */
{
    unsigned w = 0;
    *h = 0;
    ((void (*)(void *, int *, unsigned *, unsigned *))(uintptr_t)OM_RES_SIZE)(om_app(), &i, &w, h);
    return w;
}

int om_density(const char *key, double v)
{
    for (int j = 0; j < 3; j++) {
        if (strcmp(pd_kinds[j].env + 4, key))
            continue;
        if (!pd_on || (v > 1 && !pd_centres_on))
            return 0;                           /* the start-up set-up did not make room for it */
        pd_user[j] = v;
        if (!pd_racing) {
            pd_scale[j] = v;
            if (pd_write() < 0)
                return 0;
        }                                       /* else when the race is over */
        if (pd_built[j] && v > pd_built[j] + 0.001)
            pd_pool_top_up();                   /* more than the pool was built for */
        return 1;
    }
    return 0;
}

static int om_sprint(int on)
{
    static const unsigned char press[3] = { 0x8b, 0x75, 0xf0 }, release[3] = { 0x8b, 0x46, 0x48 };
    static const unsigned char press_jmp[2] = { 0xeb, 0x61 }, release_jmp[2] = { 0xeb, 0x1c };
    const void *p = (const void *)(uintptr_t)WS_PRESS_SITE, *r = (const void *)(uintptr_t)WS_RELEASE_SITE;
    if ((memcmp(p, press, 3) && memcmp(p, press_jmp, 2)) || (memcmp(r, release, 3) && memcmp(r, release_jmp, 2)))
        return 0;
    return on ? patch_bytes(WS_PRESS_SITE, press_jmp, 2) && patch_bytes(WS_RELEASE_SITE, release_jmp, 2)
              : patch_bytes(WS_PRESS_SITE, press, 3) && patch_bytes(WS_RELEASE_SITE, release, 3);
}

int om_set_sprint(const char *key, double v)
{
    (void)key;
    return om_sprint(v != 0);
}

int om_set_minimap(const char *key, double v)
{
    (void)key;
    return minimap_shops_set(v != 0);
}

int om_set_spawn_fade(const char *key, double v)
{
    (void)key;
    return spawn_fade_set(v != 0);
}
