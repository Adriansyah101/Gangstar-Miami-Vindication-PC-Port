#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "macho.h"

#define LC_SEGMENT          0x1
#define LC_SYMTAB           0x2
#define LC_UNIXTHREAD       0x5
#define LC_DYSYMTAB         0xB
#define LC_LOAD_DYLIB       0xC
#define LC_LOAD_WEAK_DYLIB  0x80000018
#define LC_REEXPORT_DYLIB   0x8000001F
#define LC_DYLD_INFO        0x22
#define LC_DYLD_INFO_ONLY   0x80000022

#define MH_MAGIC            0xFEEDFACE
#define MH_EXECUTE          2
#define MH_PIE              0x200000
#define CPU_TYPE_I386       7

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static void copy_name(char dst[17], const uint8_t *src)
{
    memcpy(dst, src, 16);
    dst[16] = 0;
}

#define FAIL(...) do { snprintf(err, errsz, __VA_ARGS__); return -1; } while (0)

static int in_file(const macho *m, uint32_t off, uint32_t len)
{
    return off <= m->size && len <= m->size - off;
}

static uint32_t name_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

/* symbol-table entry: strx, type, sect, desc, value */
static const uint8_t *nlist(const macho *m, uint32_t i)
{
    return m->data + m->symoff + 12 * i;
}

static const char *sym_name(const macho *m, uint32_t i)
{
    uint32_t strx = rd32(nlist(m, i));
    return strx < m->strsize ? (const char *)m->data + m->stroff + strx : "";
}

static int build_hash(macho *m)
{
    uint32_t cap = 1;
    while (cap < m->nsyms * 2)
        cap <<= 1;
    m->hash = calloc(cap, sizeof *m->hash);     /* slot holds symbol index + 1 */
    if (!m->hash)
        return -1;
    m->hash_mask = cap - 1;
    for (uint32_t i = 0; i < m->nsyms; i++) {
        uint8_t type = nlist(m, i)[4];
        if ((type & 0x0F) != 0x0F)              /* N_SECT | N_EXT */
            continue;
        const char *name = sym_name(m, i);
        uint32_t h = name_hash(name) & m->hash_mask;
        while (m->hash[h]) {
            if (!strcmp(sym_name(m, m->hash[h] - 1), name))
                break;                          /* keep the first definition */
            h = (h + 1) & m->hash_mask;
        }
        if (!m->hash[h])
            m->hash[h] = i + 1;
    }
    return 0;
}

uint32_t macho_lookup(const macho *m, const char *name)
{
    uint32_t h = name_hash(name) & m->hash_mask;
    while (m->hash[h]) {
        uint32_t i = m->hash[h] - 1;
        if (!strcmp(sym_name(m, i), name))
            return rd32(nlist(m, i) + 8);
        h = (h + 1) & m->hash_mask;
    }
    return 0;
}

int macho_parse(macho *m, const uint8_t *data, uint32_t size, char *err, size_t errsz)
{
    memset(m, 0, sizeof *m);
    m->data = data;
    m->size = size;
    if (size < 28)
        FAIL("file too small");
    if (rd32(data) != MH_MAGIC)
        FAIL("not a thin 32-bit Mach-O (magic %08x)", rd32(data));
    m->cputype = rd32(data + 4);
    m->filetype = rd32(data + 12);
    m->flags = rd32(data + 24);
    uint32_t ncmds = rd32(data + 16), sizeofcmds = rd32(data + 20);
    if (m->cputype != CPU_TYPE_I386)
        FAIL("not i386 (cputype %u)", m->cputype);
    if (m->filetype != MH_EXECUTE)
        FAIL("not an executable (filetype %u)", m->filetype);
    if (m->flags & MH_PIE)
        FAIL("PIE executable: the host expects a fixed-address image");
    if (!in_file(m, 28, sizeofcmds))
        FAIL("load commands run past the file");

    uint32_t off = 28, end = 28 + sizeofcmds;
    int have_dyld = 0, have_symtab = 0, have_dysymtab = 0;
    for (uint32_t c = 0; c < ncmds; c++) {
        if (off + 8 > end)
            FAIL("load command %u truncated", c);
        uint32_t cmd = rd32(data + off), csize = rd32(data + off + 4);
        if (csize < 8 || csize > end - off)
            FAIL("load command %u has bad size %u", c, csize);
        const uint8_t *p = data + off;
        if (cmd == LC_SEGMENT) {
            if (m->nsegs == MACHO_MAX_SEGS)
                FAIL("too many segments");
            macho_seg *s = &m->segs[m->nsegs++];
            copy_name(s->name, p + 8);
            s->vmaddr = rd32(p + 24);
            s->vmsize = rd32(p + 28);
            s->fileoff = rd32(p + 32);
            s->filesize = rd32(p + 36);
            s->initprot = rd32(p + 44);
            uint32_t nsects = rd32(p + 48);
            if (s->filesize > s->vmsize || !in_file(m, s->fileoff, s->filesize))
                FAIL("segment %s has a bad file range", s->name);
            if (56 + 68 * nsects > csize)
                FAIL("segment %s sections truncated", s->name);
            for (uint32_t i = 0; i < nsects; i++) {
                if (m->nsects == MACHO_MAX_SECTS)
                    FAIL("too many sections");
                const uint8_t *q = p + 56 + 68 * i;
                macho_sect *t = &m->sects[m->nsects++];
                copy_name(t->name, q);
                copy_name(t->segname, q + 16);
                t->addr = rd32(q + 32);
                t->size = rd32(q + 36);
                t->offset = rd32(q + 40);
                t->flags = rd32(q + 56);
                t->reserved1 = rd32(q + 60);
                t->reserved2 = rd32(q + 64);
                if (t->addr < s->vmaddr || t->addr + t->size > s->vmaddr + s->vmsize)
                    FAIL("section %s,%s lies outside its segment", t->segname, t->name);
            }
        } else if (cmd == LC_UNIXTHREAD) {
            /* flavor, count, i386_thread_state: eax ebx ecx edx edi esi ebp esp ss eflags eip */
            if (csize < 16 + 11 * 4)
                FAIL("LC_UNIXTHREAD truncated");
            m->entry = rd32(p + 16 + 10 * 4);
        } else if (cmd == LC_DYLD_INFO || cmd == LC_DYLD_INFO_ONLY) {
            m->bind_off = rd32(p + 16);
            m->bind_size = rd32(p + 20);
            m->weak_off = rd32(p + 24);
            m->weak_size = rd32(p + 28);
            m->lazy_off = rd32(p + 32);
            m->lazy_size = rd32(p + 36);
            /* Rebase info only matters when an image slides; this host always maps the image at
               its own addresses (layout.h), so it is ignored. */
            have_dyld = 1;
        } else if (cmd == LC_SYMTAB) {
            m->symoff = rd32(p + 8);
            m->nsyms = rd32(p + 12);
            m->stroff = rd32(p + 16);
            m->strsize = rd32(p + 20);
            have_symtab = 1;
        } else if (cmd == LC_DYSYMTAB) {
            m->indirectsymoff = rd32(p + 56);
            m->nindirectsyms = rd32(p + 60);
            have_dysymtab = 1;
        } else if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB || cmd == LC_REEXPORT_DYLIB) {
            if (m->ndylibs == MACHO_MAX_DYLIBS)
                FAIL("too many dylibs");
            uint32_t noff = rd32(p + 8);
            if (noff >= csize || !memchr(p + noff, 0, csize - noff))
                FAIL("dylib name malformed");
            m->dylibs[m->ndylibs++] = (const char *)p + noff;
        }
        off += csize;
    }
    if (!have_dyld || !have_symtab || !have_dysymtab || !m->entry)
        FAIL("missing LC_DYLD_INFO, LC_SYMTAB, LC_DYSYMTAB or entry point");
    if (!in_file(m, m->bind_off, m->bind_size) || !in_file(m, m->weak_off, m->weak_size) ||
        !in_file(m, m->lazy_off, m->lazy_size))
        FAIL("dyld info runs past the file");
    if (m->nsyms > 0x1000000 || !in_file(m, m->symoff, 12 * m->nsyms) ||
        !in_file(m, m->stroff, m->strsize) ||
        !in_file(m, m->indirectsymoff, 4 * m->nindirectsyms))
        FAIL("symbol tables run past the file");
    if (build_hash(m))
        FAIL("out of memory");
    return 0;
}

void macho_free(macho *m)
{
    free(m->hash);
    m->hash = NULL;
}

const macho_sect *macho_sect_named(const macho *m, const char *seg, const char *sect)
{
    for (int i = 0; i < m->nsects; i++)
        if (!strcmp(m->sects[i].segname, seg) && !strcmp(m->sects[i].name, sect))
            return &m->sects[i];
    return NULL;
}

const macho_sect *macho_sect_at(const macho *m, uint32_t addr)
{
    for (int i = 0; i < m->nsects; i++)
        if (addr - m->sects[i].addr < m->sects[i].size)
            return &m->sects[i];
    return NULL;
}

const macho_seg *macho_seg_named(const macho *m, const char *seg)
{
    for (int i = 0; i < m->nsegs; i++)
        if (!strcmp(m->segs[i].name, seg))
            return &m->segs[i];
    return NULL;
}

const uint8_t *macho_bytes(const macho *m, uint32_t addr, uint32_t n)
{
    for (int i = 0; i < m->nsegs; i++) {
        const macho_seg *s = &m->segs[i];
        if (addr >= s->vmaddr && addr - s->vmaddr < s->filesize && n <= s->filesize - (addr - s->vmaddr))
            return m->data + s->fileoff + (addr - s->vmaddr);
    }
    return NULL;
}

/* ---- dyld bind opcode streams ------------------------------------------------------- */

static int uleb(const uint8_t **p, const uint8_t *end, uint32_t *out)
{
    uint64_t v = 0;
    int shift = 0;
    while (*p < end) {
        uint8_t b = *(*p)++;
        if (shift < 64)
            v |= (uint64_t)(b & 0x7F) << shift;
        shift += 7;
        if (!(b & 0x80)) {
            *out = (uint32_t)v;
            return 0;
        }
    }
    return -1;
}

static int sleb(const uint8_t **p, const uint8_t *end, int32_t *out)
{
    int64_t v = 0;
    int shift = 0;
    while (*p < end) {
        uint8_t b = *(*p)++;
        if (shift < 64)
            v |= (int64_t)(b & 0x7F) << shift;
        shift += 7;
        if (!(b & 0x80)) {
            if ((b & 0x40) && shift < 64)
                v -= (int64_t)1 << shift;
            *out = (int32_t)v;
            return 0;
        }
    }
    return -1;
}

int macho_binds(const macho *m, int which, macho_bind_cb cb, void *ctx, char *err, size_t errsz)
{
    uint32_t off = which == BIND_LAZY ? m->lazy_off : which == BIND_WEAK ? m->weak_off : m->bind_off;
    uint32_t size = which == BIND_LAZY ? m->lazy_size : which == BIND_WEAK ? m->weak_size : m->bind_size;
    const uint8_t *p = m->data + off, *end = p + size;
    const char *name = "";
    int ordinal = 0, flags = 0;
    int32_t addend = 0;
    uint32_t addr = 0, v, count, skip;
    int have_addr = 0;

#define DO_BIND() do { \
        if (!have_addr) FAIL("bind before a segment was set"); \
        if (cb(ctx, addr, name, ordinal, addend, flags)) return 0; \
    } while (0)

    while (p < end) {
        uint8_t b = *p++, op = b & 0xF0, imm = b & 0x0F;
        switch (op) {
        case 0x00:                              /* DONE: lazy streams are a list of runs */
            if (which != BIND_LAZY)
                return 0;
            break;
        case 0x10:                              /* SET_DYLIB_ORDINAL_IMM */
            ordinal = imm;
            break;
        case 0x20:                              /* SET_DYLIB_ORDINAL_ULEB */
            if (uleb(&p, end, &v))
                FAIL("truncated ordinal");
            ordinal = (int)v;
            break;
        case 0x30:                              /* SET_DYLIB_SPECIAL_IMM */
            ordinal = imm ? (int)(int8_t)(0xF0 | imm) : 0;
            break;
        case 0x40: {                            /* SET_SYMBOL_TRAILING_FLAGS_IMM */
            const uint8_t *z = memchr(p, 0, end - p);
            if (!z)
                FAIL("unterminated symbol name");
            name = (const char *)p;
            flags = imm;
            p = z + 1;
            if (which == BIND_WEAK && (imm & BINDF_STRONG_DEF))
                if (cb(ctx, 0, name, 0, 0, BINDF_STRONG_DEF))
                    return 0;
            break;
        }
        case 0x50:                              /* SET_TYPE_IMM */
            if (imm != 1)
                FAIL("bind type %u (only plain pointers are supported)", imm);
            break;
        case 0x60:                              /* SET_ADDEND_SLEB */
            if (sleb(&p, end, &addend))
                FAIL("truncated addend");
            break;
        case 0x70:                              /* SET_SEGMENT_AND_OFFSET_ULEB */
            if (imm >= m->nsegs || uleb(&p, end, &v))
                FAIL("bad segment/offset");
            addr = m->segs[imm].vmaddr + v;
            have_addr = 1;
            break;
        case 0x80:                              /* ADD_ADDR_ULEB */
            if (uleb(&p, end, &v))
                FAIL("truncated address delta");
            addr += v;
            break;
        case 0x90:                              /* DO_BIND */
            DO_BIND();
            addr += 4;
            break;
        case 0xA0:                              /* DO_BIND_ADD_ADDR_ULEB */
            DO_BIND();
            if (uleb(&p, end, &v))
                FAIL("truncated address delta");
            addr += 4 + v;
            break;
        case 0xB0:                              /* DO_BIND_ADD_ADDR_IMM_SCALED */
            DO_BIND();
            addr += 4 + imm * 4u;
            break;
        case 0xC0:                              /* DO_BIND_ULEB_TIMES_SKIPPING_ULEB */
            if (uleb(&p, end, &count) || uleb(&p, end, &skip))
                FAIL("truncated repeat");
            for (uint32_t i = 0; i < count; i++) {
                DO_BIND();
                addr += 4 + skip;
            }
            break;
        default:
            FAIL("bad bind opcode 0x%02x", b);
        }
    }
#undef DO_BIND
    return 0;
}
