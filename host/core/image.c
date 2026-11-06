/* image.c -- puts the game image at its own addresses and runs the low range from its copy (see image.h). */
#include <stdio.h>
#include <string.h>

#include "image.h"
#include "layout.h"

#define FAIL(...) do { snprintf(err, errsz, __VA_ARGS__); return -1; } while (0)

uint32_t crc32_bytes(const void *p, size_t n)
{
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    const uint8_t *b = p;
    uint32_t c = 0xFFFFFFFFu;
    while (n--)
        c = table[(c ^ *b++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static uint32_t rd32(const void *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static void wr32(void *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

int gmvp_parse(gmvp *t, const uint8_t *data, uint32_t size, const macho *m, char *err, size_t errsz)
{
    if (size < 28 || memcmp(data, "GMVP", 4))
        FAIL("not a GMVP patch table");
    if (rd32(data + 4) != 1)
        FAIL("patch table version %u, host understands 1", rd32(data + 4));
    t->macho_size = rd32(data + 8);
    t->text_crc32 = rd32(data + 12);
    t->low_start = rd32(data + 16);
    t->low_end = rd32(data + 20);
    t->npatch = rd32(data + 24);
    t->patch = (const gmvp_patch *)(data + 28);
    if (t->npatch > (size - 28) / sizeof(gmvp_patch) || 28 + t->npatch * sizeof(gmvp_patch) != size)
        FAIL("patch table size does not match its count");
    if (t->macho_size != m->size)
        FAIL("patch table is for a %u-byte binary, this one is %u bytes", t->macho_size, m->size);
    const macho_seg *text = macho_seg_named(m, "__TEXT");
    if (!text)
        FAIL("no __TEXT segment");
    uint32_t crc = crc32_bytes(m->data + text->fileoff, text->filesize);
    if (crc != t->text_crc32)
        FAIL("patch table is for a different binary (__TEXT crc %08x, table %08x)", crc, t->text_crc32);
    if (t->low_start != GMV_LOW_START || t->low_end < GMV_RESERVE_START ||
        t->low_end - GMV_LOW_START > GMV_LOWCOPY_CAP)
        FAIL("low range 0x%x..0x%x does not fit the host layout", t->low_start, t->low_end);
    for (uint32_t i = 0; i < t->npatch; i++) {
        if (i && t->patch[i].addr <= t->patch[i - 1].addr)
            FAIL("patch table not sorted at entry %u", i);
        if (t->patch[i].kind < GMVP_REL32_OUT || t->patch[i].kind > GMVP_PTR_IN)
            FAIL("unknown patch kind %u at 0x%x", t->patch[i].kind, t->patch[i].addr);
    }
    return 0;
}

uint32_t image_code_addr(const gmvp *t, uint32_t addr)
{
    return addr - t->low_start < t->low_end - t->low_start ? addr + GMV_LOW_DELTA : addr;
}

static int in_low(const gmvp *t, uint32_t a)
{
    return a - t->low_start < t->low_end - t->low_start;
}

/* __text, __const_coal, __symbol_stub, __stub_helper: sections flagged as holding code */
static int is_code(const macho *m, uint32_t a)
{
    const macho_sect *s = macho_sect_at(m, a);
    return s && (s->flags & 0x80000400u);
}

/* Checks one patch against the bytes now in memory; returns NULL if it fits. */
static const char *check_patch(const macho *m, const gmvp *t, const gmvp_patch *p)
{
    const macho_sect *text = macho_sect_named(m, "__TEXT", "__text");
    uint8_t *copy = (uint8_t *)(uintptr_t)(p->addr + GMV_LOW_DELTA);
    uint8_t *orig = (uint8_t *)(uintptr_t)p->addr;
    switch (p->kind) {
    case GMVP_REL32_OUT: {
        if (!in_low(t, p->addr) || !in_low(t, p->addr + 3))
            return "REL32_OUT field outside the low range";
        uint8_t op1 = copy[-1], op2 = copy[-2];
        if (op1 != 0xE8 && op1 != 0xE9 && !(op2 == 0x0F && (op1 & 0xF0) == 0x80))
            return "REL32_OUT field not after a call/jmp/jcc opcode";
        uint32_t tgt = p->addr + 4 + rd32(copy);
        if (in_low(t, tgt))
            return "REL32_OUT branch does not leave the low range";
        if (!is_code(m, tgt))
            return "REL32_OUT target is not code";
        return NULL;
    }
    case GMVP_PIC_POP:
        if (!in_low(t, p->addr) || !in_low(t, p->addr + 5))
            return "PIC_POP outside the low range";
        if (p->reg > 7 || p->reg == 4)
            return "PIC_POP bad register";
        if (copy[0] != 0xE8 || rd32(copy + 1) != 0 || copy[5] != 0x58 + p->reg)
            return "PIC_POP bytes are not call $+5; pop r";
        return NULL;
    case GMVP_REL32_IN: {
        if (in_low(t, p->addr) || p->addr < GMV_RESERVE_START)
            return "REL32_IN field inside the low range";
        if (orig[-1] != 0xE8 && orig[-1] != 0xE9)
            return "REL32_IN field not after a call/jmp opcode";
        uint32_t tgt = p->addr + 4 + rd32(orig);
        if (!in_low(t, tgt) || tgt < text->addr)
            return "REL32_IN target not in low-range code";
        return NULL;
    }
    case GMVP_PTR_IN: {
        if (in_low(t, p->addr) || p->addr < GMV_RESERVE_START || p->addr + 4 > GMV_MACHO_END)
            return "PTR_IN slot outside the mapped image";
        uint32_t v = rd32(orig);
        if (!in_low(t, v) || v < text->addr)
            return "PTR_IN slot does not hold low-range code";
        return NULL;
    }
    }
    return "unknown kind";
}

int image_copy(const macho *m, const gmvp *t, image_stats *st, char *err, size_t errsz)
{
    memset(st, 0, sizeof *st);
    for (int i = 0; i < m->nsegs; i++) {
        const macho_seg *s = &m->segs[i];
        if (!strcmp(s->name, "__PAGEZERO"))
            continue;
        if (s->vmaddr < GMV_LOW_START || s->vmaddr + s->vmsize > GMV_MACHO_END ||
            s->vmaddr + s->vmsize < s->vmaddr)
            FAIL("segment %s 0x%x+0x%x does not fit the reserved range", s->name, s->vmaddr, s->vmsize);
    }
    /* the low copy region and everything above the PE header page start out zero */
    for (int i = 0; i < m->nsegs; i++) {
        const macho_seg *s = &m->segs[i];
        if (!strcmp(s->name, "__PAGEZERO") || !s->filesize)
            continue;
        uint32_t lo = s->vmaddr, hi = s->vmaddr + s->filesize;
        const uint8_t *src = m->data + s->fileoff;
        if (lo < t->low_end) {                  /* into the copy */
            uint32_t e = hi < t->low_end ? hi : t->low_end;
            memcpy((void *)(uintptr_t)(lo + GMV_LOW_DELTA), src, e - lo);
            st->copied_low_bytes += e - lo;
        }
        if (hi > GMV_RESERVE_START) {           /* at its own address */
            uint32_t b = lo > GMV_RESERVE_START ? lo : GMV_RESERVE_START;
            memcpy((void *)(uintptr_t)b, src + (b - lo), hi - b);
            st->mapped_bytes += hi - b;
        }
    }
    return 0;
}

static uint32_t count_diff(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    uint32_t d = 0;
    if (!memcmp(a, b, n))
        return 0;
    for (uint32_t i = 0; i < n; i++)
        d += a[i] != b[i];
    return d;
}

uint32_t image_compare(const macho *m, const gmvp *t)
{
    uint32_t diff = 0;
    for (int i = 0; i < m->nsegs; i++) {
        const macho_seg *s = &m->segs[i];
        if (!strcmp(s->name, "__PAGEZERO"))
            continue;
        uint32_t lo = s->vmaddr, hi = s->vmaddr + s->filesize;
        const uint8_t *src = m->data + s->fileoff;
        if (lo < t->low_end) {
            uint32_t e = hi < t->low_end ? hi : t->low_end;
            diff += count_diff((const uint8_t *)(uintptr_t)(lo + GMV_LOW_DELTA), src, e - lo);
        }
        if (hi > GMV_RESERVE_START) {
            uint32_t b = lo > GMV_RESERVE_START ? lo : GMV_RESERVE_START;
            diff += count_diff((const uint8_t *)(uintptr_t)b, src + (b - lo), hi - b);
        }
        /* zero-fill tails must still be zero */
        for (uint32_t a = (hi > GMV_RESERVE_START ? hi : GMV_RESERVE_START); a < s->vmaddr + s->vmsize; a++)
            diff += *(const uint8_t *)(uintptr_t)a != 0;
    }
    return diff;
}

int image_patch(const macho *m, const gmvp *t, image_stats *st, char *err, size_t errsz)
{
    /* verify every patch before changing a single byte */
    uint32_t prev_end = 0;
    for (uint32_t i = 0; i < t->npatch; i++) {
        const gmvp_patch *p = &t->patch[i];
        const char *why = check_patch(m, t, p);
        if (why)
            FAIL("patch %u at 0x%06x: %s", i, p->addr, why);
        if (p->addr < prev_end)
            FAIL("patch %u at 0x%06x overlaps the previous one", i, p->addr);
        prev_end = p->addr + (p->kind == GMVP_PIC_POP ? 6 : 4);
    }
    for (uint32_t i = 0; i < t->npatch; i++) {
        const gmvp_patch *p = &t->patch[i];
        uint8_t *copy = (uint8_t *)(uintptr_t)(p->addr + GMV_LOW_DELTA);
        uint8_t *orig = (uint8_t *)(uintptr_t)p->addr;
        switch (p->kind) {
        case GMVP_REL32_OUT:                    /* the copy moved, the target did not */
            wr32(copy, rd32(copy) - GMV_LOW_DELTA);
            break;
        case GMVP_PIC_POP:                      /* hand out the original PIC base */
            copy[0] = 0xB8 + p->reg;
            wr32(copy + 1, p->addr + 5);
            copy[5] = 0x90;
            break;
        case GMVP_REL32_IN:                     /* the target moved into the copy */
            wr32(orig, rd32(orig) + GMV_LOW_DELTA);
            break;
        case GMVP_PTR_IN:
            wr32(orig, rd32(orig) + GMV_LOW_DELTA);
            break;
        }
        st->applied[p->kind]++;
    }
    return 0;
}
