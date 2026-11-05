/* tab_arrows.c -- GAMEPATCH_TAB: the lines' value arrows, added to the menu file as it loads */
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

#define OM_SEL_DEPTH   20            /* the lines' own placements go up to 14                       */
#define OM_BOX_W       4320          /* a line's key boxes (wCostKey, sprite 763) before its scale  */
#define OM_BOX_H       780

/* ---- the lines' ◀ value ▶, added to the menu file as it loads ----
   gameswf opens a movie through the file opener the game registers (at 0x2f9854) and reads the
   swf from the stream (tu_file) it answers.  Our opener reads newmenu.fua's whole stream,
   inflates it with the game's zlib and answers a stream over the result, in which each of
   widgetControlHelps' six line sprites (wCusLine1-6) has four more placements, over its key
   boxes: the category header's bar, btnLeft, btnRight and txtValue (widgetActionKeyTitle's own
   characters and layout, scaled to the boxes' height), named wValueBar, btnValueLeft,
   btnValueRight and textValue.  A sprite with a child named hitzone is a button wherever it is
   (0x2cb730), so their clicks come to the list widget like the header's.  Everything else in the
   file stays as it was; the CONTROLS page hides the four again. */
uint32_t om_bits(const unsigned char *p, size_t *bit, int n)
{
    uint32_t v = 0;
    for (; n > 0; n--, (*bit)++)
        v = v << 1 | (p[*bit >> 3] >> (7 - (*bit & 7)) & 1);
    return v;
}

int32_t om_sbits(const unsigned char *p, size_t *bit, int n)
{
    uint32_t v = om_bits(p, bit, n);
    return n > 0 && n < 32 && v >> (n - 1) ? (int32_t)(v - (1u << n)) : (int32_t)v;
}

void om_put_bits(unsigned char *p, size_t *bit, uint32_t v, int n)
{
    for (int i = n - 1; i >= 0; i--, (*bit)++)
        if (v >> i & 1)
            p[*bit >> 3] |= 0x80 >> (*bit & 7);
}

int om_sbits_n(int32_t v)                /* the bits a signed field needs */
{
    int n = 1;
    while (n < 31 && (v < -(1 << (n - 1)) || v >= (1 << (n - 1))))
        n++;
    return n;
}

/* A PlaceObject2's fields (tag data d of n bytes); 0 if they run past it. */
static int om_place_read(const unsigned char *d, size_t n, om_place *o)
{
    unsigned fl = n ? d[0] : 0;
    size_t p = 3;
    memset(o, 0, sizeof *o);
    if (n < 3)
        return 0;
    o->depth = d[1] | d[2] << 8;
    o->sx = o->sy = 0x10000;
    if (fl & 2) {
        if (p + 2 > n)
            return 0;
        o->cid = d[p] | d[p + 1] << 8;
        p += 2;
    }
    if (fl & 4) {
        size_t bit = p * 8;
        int k;
        if (p + 1 > n)
            return 0;
        o->matrix = 1;
        if (om_bits(d, &bit, 1)) {
            k = (int)om_bits(d, &bit, 5);
            o->sx = om_sbits(d, &bit, k);
            o->sy = om_sbits(d, &bit, k);
        }
        if (om_bits(d, &bit, 1)) {
            k = (int)om_bits(d, &bit, 5);
            om_sbits(d, &bit, k);
            om_sbits(d, &bit, k);
            o->rotated = 1;
        }
        k = (int)om_bits(d, &bit, 5);
        o->tx = om_sbits(d, &bit, k);
        o->ty = om_sbits(d, &bit, k);
        p = (bit + 7) / 8;
    }
    if (fl & 8) {                               /* colour transform with alpha */
        size_t bit = p * 8;
        int add = (int)om_bits(d, &bit, 1), mult = (int)om_bits(d, &bit, 1), k = (int)om_bits(d, &bit, 4);
        bit += (size_t)k * 4 * (add + mult);
        p = (bit + 7) / 8;
    }
    if (fl & 0x10)
        p += 2;                                 /* ratio */
    if (p > n)
        return 0;
    if (fl & 0x20) {
        if (!memchr(d + p, 0, n - p))
            return 0;
        o->name = (const char *)d + p;
    }
    return 1;
}

/* The tag at p of the tags ending at end: its code, data offset and length; the next tag's
   offset, or 0 if it runs past the end. */
size_t om_tag(const unsigned char *s, size_t p, size_t end, int *code, size_t *data, size_t *len)
{
    uint32_t l;
    if (p + 2 > end)
        return 0;
    *code = (s[p] | s[p + 1] << 8) >> 6;
    l = s[p] & 0x3f;
    p += 2;
    if (l == 0x3f) {
        if (p + 4 > end)
            return 0;
        memcpy(&l, s + p, 4);
        p += 4;
    }
    if (l > end - p)
        return 0;
    *data = p;
    *len = l;
    return p + l;
}

/* The DefineSprite of character cid among the file's tags from start: its data and length. */
int om_sprite(const unsigned char *s, size_t start, size_t n, int cid, size_t *data, size_t *len)
{
    int code;
    for (size_t p = start, next; (next = om_tag(s, p, n, &code, data, len)) != 0 && code != 0; p = next)
        if (code == 39 && *len >= 4 && (s[*data] | s[*data + 1] << 8) == cid)
            return 1;
    return 0;
}

/* The placement named name in a sprite's first frame, or in the root's if data is the tags' start. */
int om_find_place(const unsigned char *s, size_t from, size_t end, const char *name, om_place *o)
{
    int code;
    size_t d, l;
    for (size_t p = from, next; (next = om_tag(s, p, end, &code, &d, &l)) != 0 && code != 0 && code != 1; p = next)
        if (code == 26 && om_place_read(s + d, l, o) && o->cid && o->matrix && !o->rotated &&
            (name ? o->name && !strcmp(o->name, name) : !o->name))
            return 1;
    return 0;
}

size_t om_place_write(unsigned char *o, int depth, const om_place *m, const char *name)
{
    unsigned char d[64] = {0};
    size_t bit = 5 * 8, p, k = strlen(name) + 1;
    int ns = om_sbits_n(m->sx) > om_sbits_n(m->sy) ? om_sbits_n(m->sx) : om_sbits_n(m->sy);
    int nt = om_sbits_n(m->tx) > om_sbits_n(m->ty) ? om_sbits_n(m->tx) : om_sbits_n(m->ty);
    d[0] = 0x26;                                /* a character, a matrix and a name */
    d[1] = (unsigned char)depth;
    d[2] = (unsigned char)(depth >> 8);
    d[3] = (unsigned char)m->cid;
    d[4] = (unsigned char)(m->cid >> 8);
    om_put_bits(d, &bit, 1, 1);
    om_put_bits(d, &bit, (uint32_t)ns, 5);
    om_put_bits(d, &bit, (uint32_t)m->sx, ns);
    om_put_bits(d, &bit, (uint32_t)m->sy, ns);
    om_put_bits(d, &bit, 0, 1);
    om_put_bits(d, &bit, (uint32_t)nt, 5);
    om_put_bits(d, &bit, (uint32_t)m->tx, nt);
    om_put_bits(d, &bit, (uint32_t)m->ty, nt);
    p = (bit + 7) / 8;
    memcpy(d + p, name, k);
    p += k;                                     /* under 63: the short tag header */
    o[0] = (unsigned char)(26 << 6 | p);
    o[1] = (unsigned char)((26 << 6 | p) >> 8);
    memcpy(o + 2, d, p);
    return p + 2;
}

/* The uncompressed menu file s (n bytes, header included) with the lines' ◀ value ▶, or NULL if
   it is not the one expected. */
unsigned char *om_swf_patch(const unsigned char *s, size_t n, size_t *out_n)
{
    static const char *const head_names[4] = {NULL, "btnLeft", "btnRight", "txtValue"};
    size_t start, d, l, ld[OM_LINES], ll[OM_LINES], show[OM_LINES], q;
    om_place head, list, sel[4], key[OM_LINES], o;
    int lines[OM_LINES], code;
    unsigned char *out;
    if (n < 32 || memcmp(s, "FWS", 3))
        return NULL;
    start = 8 + (5 + 4 * (s[8] >> 3) + 7) / 8 + 4;
    if (!om_find_place(s, start, n, "widgetActionKeyTitle", &head) || !om_find_place(s, start, n, "widgetControlHelps", &list) ||
        !om_sprite(s, start, n, head.cid, &d, &l))
        return NULL;
    for (int e = 0; e < 4; e++)
        if (!om_find_place(s, d + 4, d + l, head_names[e], &sel[e]))
            return NULL;
    if (!om_sprite(s, start, n, list.cid, &d, &l))
        return NULL;
    for (int i = 0; i < OM_LINES; i++) {
        char name[16];
        snprintf(name, sizeof name, "wCusLine%d", i + 1);
        if (!om_find_place(s, d + 4, d + l, name, &o) || !om_sprite(s, start, n, o.cid, &ld[i], &ll[i]) ||
            !om_find_place(s, ld[i] + 4, ld[i] + ll[i], "wCostKey", &key[i]))
            return NULL;
        lines[i] = o.cid;
        show[i] = 0;                            /* the first ShowFrame; no depth of ours taken */
        for (size_t p = ld[i] + 4, next, td, tl; (next = om_tag(s, p, ld[i] + ll[i], &code, &td, &tl)) != 0 && code; p = next) {
            if (code == 1) {
                show[i] = p;
                break;
            }
            if (code == 26 || code == 70) {     /* (PlaceObject3 has two flag bytes) */
                size_t at = td + (code == 70) + 1;
                int depth = tl >= at - td + 2 ? s[at] | s[at + 1] << 8 : 0;
                if (depth >= OM_SEL_DEPTH && depth < OM_SEL_DEPTH + 4)
                    return NULL;
            }
        }
        if (!show[i])
            return NULL;
    }
    if (!(out = malloc(n + OM_LINES * 4 * 64)))
        return NULL;
    memcpy(out, s, start);
    q = start;
    for (size_t p = start, next; p < n; p = next) {
        int i = 0;
        if (!(next = om_tag(s, p, n, &code, &d, &l))) {
            free(out);
            return NULL;
        }
        if (code == 39)
            while (i < OM_LINES && lines[i] != (s[d] | s[d + 1] << 8))
                i++;
        if (code != 39 || i == OM_LINES) {
            memcpy(out + q, s + p, next - p);
            q += next - p;
        } else {                                /* the line sprite with ours before its ShowFrame */
            double w = OM_BOX_W * key[i].sx / 65536.0, h = OM_BOX_H * key[i].sy / 65536.0;
            double g = (w - 120) / (OM_SEL_X1 - OM_SEL_X0), gy = h / (OM_SEL_Y1 - OM_SEL_Y0);
            double ox, oy;
            size_t at;
            uint32_t len;
            if (gy < g)
                g = gy;
            ox = key[i].tx + w / 2 - g * (OM_SEL_X0 + OM_SEL_X1) / 2;
            oy = key[i].ty + h / 2 - g * (OM_SEL_Y0 + OM_SEL_Y1) / 2;
            out[q] = (unsigned char)(39 << 6 | 0x3f);
            out[q + 1] = (unsigned char)((39 << 6 | 0x3f) >> 8);
            at = q + 2;
            q += 6;
            memcpy(out + q, s + d, show[i] - d);
            q += show[i] - d;
            for (int e = 0; e < 4; e++) {
                om_place m = sel[e];
                m.sx = (int32_t)lround(g * sel[e].sx);
                m.sy = (int32_t)lround(g * sel[e].sy);
                m.tx = (int32_t)lround(ox + g * sel[e].tx);
                m.ty = (int32_t)lround(oy + g * sel[e].ty);
                q += om_place_write(out + q, OM_SEL_DEPTH + e, &m, om_sel_names[e]);
            }
            memcpy(out + q, s + show[i], d + l - show[i]);
            q += d + l - show[i];
            len = (uint32_t)(q - at - 4);
            memcpy(out + at, &len, 4);
        }
        if (code == 0)
            break;
    }
    {
        uint32_t total = (uint32_t)q;
        memcpy(out + 4, &total, 4);
    }
    *out_n = q;
    return out;
}
