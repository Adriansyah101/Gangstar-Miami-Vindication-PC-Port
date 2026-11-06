/* fix_vertex.c -- the vertex-format converter's missing guard, and its diagnostics */
#include "compat.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* The in-world movement crash (the vertex-format converter at 0x563c90)

   0x563c90 copies vertex attributes from a source layout to a destination layout, one semantic
   at a time.  For each semantic it reads a destination slot  al = destMap[sem]  and a source
   slot  cl = srcMap[sem]; a value of 0xFF means "this attribute is absent".  It then indexes the
   descriptor arrays as  descArr + 0x14 + slot*16.

   The engine has TWO paths for the source read:
     * dest-absent  (al == 0xFF, jumps to 0x5640f0): this path DOES guard  cl == 0xFF  before
       touching the source descriptor, and on an absent source attribute it simply advances to
       the next semantic (0x564070).
     * dest-present (al != 0xFF, falls through at 0x563e53): this path does NOT guard cl, so if
       the destination format wants an attribute the source mesh lacks (cl == 0xFF) it computes
       srcDesc = srcDescArr + 0x14 + 255*16 -- far out of bounds of a mesh that declares one
       attribute -- reads garbage, derives a wild stride, and writes a vertex to a random address
       (observed: write to 0x006222ad, faulting at 0x00564e5a).  This fires when the animated
       character's skinned mesh is rebuilt, i.e. the moment the player starts moving.

   The fix applies the engine's own  cl == 0xFF  guard to the dest-present path.  We overwrite the
   8-byte  `cmp al,0xff; je 0x5640f0`  at 0x563e29 with a jump to a trampoline that performs the
   original al test AND the missing cl test, routing cl == 0xFF to 0x564070 -- exactly the
   loop-continue the engine itself uses for an absent source attribute at 0x5640f0.  al and cl are
   both still live at the patch site, so the trampoline needs no extra state.

   Toggle: GMV_VTXFIX=0 disables the fix (to reproduce the original crash for comparison). */

#define VTX_SITE   0x00563e29u   /* cmp al,0xff; je 0x5640f0            (8 bytes we replace)      */
#define VTX_CONT   0x00563e31u   /* fall-through: both attributes present                        */
#define VTX_DESTA  0x005640f0u   /* original al==0xFF target (itself already guards cl)          */
#define VTX_SKIP   0x00564070u   /* loop-continue used for an absent attribute                   */

/* ---- diagnostic: record which semantics hit the dest-present / src-absent mismatch ----

   This is the configuration the shipped engine never expects (see the header comment): the
   destination vertex format wants an attribute the source mesh does not provide (al != 0xFF but
   cl == 0xFF).  The cl-guard stops it from crashing by skipping the attribute, but skipping
   leaves that destination vertex data unwritten, which is why the affected 3D meshes render
   black.  To find the real cause we log, the first time each semantic is seen, the semantic id,
   the destination slot, and the four converter arguments (both slot-maps and both draw objects),
   then only count the repeats so the log never floods during play.  GMV_VTXLOG=0 silences it.
   Called from the trampoline via `cdecl` with the converter's ebp so it can read the args.      */
static unsigned g_vtx_miss_total;
#if GMV_DEV
#define VTX_NSEM 32
static unsigned g_vtx_miss_count[VTX_NSEM];
static int      g_vtx_log_on = -1;              /* -1 = not resolved yet */

/* Format `n` bytes at `p` as "ab cd ef ..." into `out`, or "<unreadable>" if the memory faults. */
static void hexbytes(char *out, const void *p, int n)
{
    if (!p || IsBadReadPtr(p, (UINT_PTR)n)) { strcpy(out, "<unreadable>"); return; }
    static const char hx[] = "0123456789abcdef";
    const unsigned char *b = (const unsigned char *)p;
    char *o = out;
    for (int i = 0; i < n; i++) {
        if (i) *o++ = ' ';
        *o++ = hx[b[i] >> 4];
        *o++ = hx[b[i] & 15];
    }
    *o = 0;
}

/* Dump a draw object's descriptor array: *obj is the array base; header is 0x14 bytes, then
   16-byte descriptors (field +0x8 = count, +0xa = type, +0xe = stride).  Prints the first six. */
static void shader_attrs(char *out, uint32_t s)
{
    /* The shader's attributes: 8 bytes each at [s+0x1c], the semantic at +4; count at s+0x34. */
    strcpy(out, "<unreadable>");
    if (!s || IsBadReadPtr((void *)(uintptr_t)s, 0x38))
        return;
    uint32_t arr = *(const uint32_t *)(uintptr_t)(s + 0x1c);
    unsigned n = *(const unsigned char *)(uintptr_t)(s + 0x34);
    if (n > 24)
        n = 24;
    if (n && (!arr || IsBadReadPtr((void *)(uintptr_t)arr, n * 8)))
        return;
    char *o = out;
    *o = 0;
    for (unsigned i = 0; i < n; i++)
        o += sprintf(o, "%s%u", i ? "," : "",
                     (unsigned)*(const unsigned short *)(uintptr_t)(arr + i * 8 + 4));
}

static void dump_shader(const char *tag, uint32_t s)
{
    char vs[32], fs[220], at[128];
    uint32_t key = 0, mask = 0, vp = 0, fp = 0;
    if (s && !IsBadReadPtr((void *)(uintptr_t)s, 0x54)) {
        key  = *(const uint32_t *)(uintptr_t)(s + 0x3c);
        mask = *(const uint32_t *)(uintptr_t)(s + 0x30);
        vp   = *(const uint32_t *)(uintptr_t)(s + 0x4c);
        fp   = *(const uint32_t *)(uintptr_t)(s + 0x50);
    }
    hexbytes(vs, (const void *)(uintptr_t)vp, 4);
    hexbytes(fs, (const void *)(uintptr_t)fp, 0x44);
    shader_attrs(at, s);
    gmv_logfile("          %s shader 0x%08x baker key 0x%08x attribute mask 0x%08x semantics %s\n", tag, s, key, mask, at);
    gmv_logfile("              vertex state   %s\n", vs);
    gmv_logfile("              fragment state %s\n", fs);
}

static void dump_descarr(const char *tag, uint32_t objp)
{
    if (!objp || IsBadReadPtr((void *)(uintptr_t)objp, 4)) {
        gmv_logfile("          %s obj 0x%08x <bad obj ptr>\n", tag, objp);
        return;
    }
    uint32_t base = *(const uint32_t *)(uintptr_t)objp;
    char hx[160];
    hexbytes(hx, (const void *)(uintptr_t)base, 0x14);
    gmv_logfile("          %s obj 0x%08x base 0x%08x  hdr %s\n", tag, objp, base, hx);
    for (int s = 0; s < 6; s++) {
        hexbytes(hx, (const void *)(uintptr_t)(base + 0x14 + (uint32_t)s * 16), 16);
        gmv_logfile("              [slot %d] %s\n", s, hx);
    }
}

#endif

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_vtx_missing(uint32_t sem, uint32_t destslot, uint32_t frameEbp)
{
#if !GMV_DEV
    (void)sem;
    (void)destslot;
    (void)frameEbp;
    unsigned n = ++g_vtx_miss_total;
    if (n == 1 || n == 10 || n == 100 || n == 1000 || n == 10000 || n == 100000)
        gmv_logfile("[vtxmiss] cumulative occurrences so far: %u\n", n);
#else
    if (g_vtx_log_on < 0) {
        const char *e = gmv_dbgenv("GMV_VTXLOG");
        g_vtx_log_on = (e && e[0] == '0') ? 0 : 1;
    }
    g_vtx_miss_total++;
    if (g_vtx_log_on) {                          /* magnitude: is this isolated or systemic? */
        unsigned t = g_vtx_miss_total;
        if (t == 1 || t == 10 || t == 100 || t == 1000 || t == 10000 || t == 100000)
            gmv_logfile("[vtxmiss] cumulative occurrences so far: %u\n", t);
    }
    unsigned idx  = (sem < VTX_NSEM) ? sem : (VTX_NSEM - 1);
    unsigned seen = g_vtx_miss_count[idx]++;
    if (!g_vtx_log_on || seen != 0)
        return;                                 /* detailed dump only on first sighting of a semantic */

    uint32_t dmap = 0, dobj = 0, smap = 0, sobj = 0;
    const uint32_t *fp = (const uint32_t *)(uintptr_t)frameEbp;
    if (fp && !IsBadReadPtr((void *)fp, 0x20 * sizeof(uint32_t))) {
        dmap = fp[2];                           /* [ebp+0x08] dest slot-map  */
        dobj = fp[3];                           /* [ebp+0x0c] dest draw obj  */
        smap = fp[6];                           /* [ebp+0x18] src  slot-map  */
        sobj = fp[7];                           /* [ebp+0x1c] src  draw obj  */
    }
    /* Dump both slot-maps (byte per semantic, 0xFF = absent) and the destination descriptor for
       this semantic (*destObj + 0x14 + destslot*16, 16 bytes: field+0x8=count, +0xa=type, +0xe=stride).
       This is the full picture needed to decide why dest requests an attribute the source lacks.  */
    char dmapsb[96], smapsb[96], ddesc[64];
    hexbytes(dmapsb, (const void *)(uintptr_t)dmap, 22);
    hexbytes(smapsb, (const void *)(uintptr_t)smap, 22);
    strcpy(ddesc, "<n/a>");
    if (dobj && !IsBadReadPtr((void *)(uintptr_t)dobj, 4)) {
        uint32_t base = *(const uint32_t *)(uintptr_t)dobj;
        uint32_t ddp  = base + 0x14 + (destslot & 0xffu) * 16;
        hexbytes(ddesc, (const void *)(uintptr_t)ddp, 16);
    }
    gmv_logfile("[vtxmiss] semantic %-2u dest slot %u present, src slot 0xFF (absent)\n", sem, destslot & 0xffu);
    gmv_logfile("          destMap[0..21] = %s\n", dmapsb);
    gmv_logfile("          srcMap [0..21] = %s\n", smapsb);
    gmv_logfile("          destDesc(16B)  = %s\n", ddesc);
    /* destObj is the READ (input) side -- it has the attribute; srcObj is the WRITE (output) side
       that lacks a slot for it.  Dump both so we can see the real shape of the output format. */
    dump_descarr("destObj READ/input ", dobj);
    dump_descarr("srcObj  WRITE/output", sobj);
    /* Two shaders share a baker when their keys match (0x4ca6d0); the baker's output layout comes
       from the first of them.  [ebp-0x1c] is the draw's shader, shader+0x14 its baker, baker+8
       the shader that created it. */
    if (fp) {
        uint32_t s = fp[-7], b = 0, first = 0;
        if (s && !IsBadReadPtr((void *)(uintptr_t)s, 0x18))
            b = *(const uint32_t *)(uintptr_t)(s + 0x14);
        if (b && !IsBadReadPtr((void *)(uintptr_t)b, 0xc))
            first = *(const uint32_t *)(uintptr_t)(b + 8);
        gmv_logfile("          baker 0x%08x\n", b);
        dump_shader("this draw's", s);
        dump_shader("baker's    ", first);
    }
#endif
}

#if GMV_DEV
/* ---- diagnostic: which shaders share a baker (the cause of [vtxmiss]) ----

   A shader's first draw asks 0x4ca6d0 or 0x518750 (shader vt+0x20) for a baker.  They keep one baker
   per key in the map at [0x63d57c], and the key is only a hash of the shader's states.  The baker's
   output layout comes from the shader that created it, so a shader handed another shader's baker can
   want an attribute the output has no slot for.  [vtxmiss] needs that draw to happen in the right
   order; this logs the sharing itself, once per shader, in either order.  GMV_BAKERSHARE=0 opts out. */
#define BAKER_MAP 0x0063d57cu

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_baker_ask(uint32_t shader, uint32_t key)
{
    static unsigned asked, shared, other_attrs;
    asked++;
    uint32_t map = *(const uint32_t *)(uintptr_t)BAKER_MAP;
    uint32_t head = map + 4, at = head;
    uint32_t node = *(const uint32_t *)(uintptr_t)(map + 8);
    while (node) {                              /* the game's own lower_bound: +8 left, +0xc right */
        if (*(const uint32_t *)(uintptr_t)(node + 0x10) >= key) {
            at = node;
            node = *(const uint32_t *)(uintptr_t)(node + 8);
        } else
            node = *(const uint32_t *)(uintptr_t)(node + 0xc);
    }
    uint32_t baker = 0, first = 0;
    if (at != head && *(const uint32_t *)(uintptr_t)(at + 0x10) == key)
        baker = *(const uint32_t *)(uintptr_t)(at + 0x14);
    if (baker && !IsBadReadPtr((void *)(uintptr_t)baker, 0xc))
        first = *(const uint32_t *)(uintptr_t)(baker + 8);
    if (first && first != shader) {
        char a[128], b[128];
        shader_attrs(a, shader);
        shader_attrs(b, first);
        int differ = strcmp(a, b) != 0;
        shared++;
        other_attrs += differ;
        if (shared <= 32) {
            gmv_logfile("[bakershare] key 0x%08x: shader 0x%08x gets baker 0x%08x made for shader 0x%08x; attributes %s\n",
                        key, shader, baker, first, differ ? "DIFFER" : "match");
            dump_shader("asking ", shader);
            dump_shader("creator", first);
        }
    }
    if (asked == 1 || asked % 20 == 0)
        gmv_logfile("[bakershare] %u shaders asked for a baker: %u got another shader's, %u of those with other attributes\n",
                    asked, shared, other_attrs);
}

static void hook_baker_maker(uint32_t fn)
{
    static const unsigned char expect[6] = { 0x55, 0x89, 0xe5, 0x57, 0x56, 0x53 };
    if (memcmp((const void *)(uintptr_t)fn, expect, sizeof expect) != 0) {
        gmv_log("[gamepatch] fn 0x%08x not the expected bytes; baker log skipped\n", fn);
        return;
    }
    unsigned char *tr = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr)
        return;
    size_t o = 0;
    tr[o++] = 0x60;                                                 /* pushad                         */
    tr[o++] = 0xff; tr[o++] = 0x74; tr[o++] = 0x24; tr[o++] = 0x28; /* push [esp+0x28]   ; key         */
    tr[o++] = 0xff; tr[o++] = 0x74; tr[o++] = 0x24; tr[o++] = 0x28; /* push [esp+0x28]   ; shader      */
    tr[o++] = 0xb8;                                                 /* mov eax, &gmv_baker_ask         */
    { uint32_t fnp = (uint32_t)(uintptr_t)&gmv_baker_ask; memcpy(tr + o, &fnp, 4); o += 4; }
    tr[o++] = 0xff; tr[o++] = 0xd0;                                 /* call eax                        */
    tr[o++] = 0x83; tr[o++] = 0xc4; tr[o++] = 0x08;                 /* add esp,8                       */
    tr[o++] = 0x61;                                                 /* popad                           */
    memcpy(tr + o, expect, sizeof expect); o += sizeof expect;      /* the 6 displaced bytes           */
    emit_jmp(tr, &o, fn + sizeof expect);
    FlushInstructionCache(GetCurrentProcess(), tr, o);

    unsigned char site[6];
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)tr - (fn + 5));
    site[0] = 0xe9;
    memcpy(site + 1, &rel, 4);
    site[5] = 0x90;
    if (!patch_bytes(fn, site, sizeof site))
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); baker log skipped\n", fn, GetLastError());
}

void apply_bakershare_log(void)
{
    const char *e = gmv_dbgenv("GMV_BAKERSHARE");
    if (e && e[0] == '0')
        return;
    hook_baker_maker(0x004ca6d0u);
    hook_baker_maker(0x00518750u);
    gmv_log("[gamepatch] baker log at 0x004ca6d0 and 0x00518750: shaders sharing a baker logged as [bakershare]\n");
}
#endif

void apply_vtxfix(void)
{
    const char *e = gmv_getenv("GMV_VTXFIX");
    if (e && e[0] == '0') {
        gmv_log("[gamepatch] vertex-converter cl-guard DISABLED (VTXFIX=0)\n");
        return;
    }

    /* Only patch a binary that matches byte-for-byte, so this never corrupts an unexpected build. */
    static const unsigned char expect[8] = { 0x3c, 0xff, 0x0f, 0x84, 0xbf, 0x02, 0x00, 0x00 };
    if (memcmp((const void *)(uintptr_t)VTX_SITE, expect, sizeof expect) != 0) {
        gmv_log("[gamepatch] site 0x%08x not the expected bytes; vertex fix skipped\n", VTX_SITE);
        return;
    }

    unsigned char *tr = VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) {
        gmv_log("[gamepatch] VirtualAlloc for trampoline failed (%lu); vertex fix skipped\n", GetLastError());
        return;
    }

    /* Build the trampoline.  Offsets are fixed by this exact byte sequence:
         00  cmp al,0xff
         02  je near L_A                 (displacement patched once L_A is known)
         08  cmp cl,0xff
         0b  je near L_B                 (displacement patched once L_B is known)
         11  jmp VTX_CONT                both attributes present -> resume converter
         16  L_A: jmp VTX_DESTA          dest absent -> engine's own (cl-guarded) path
         1b  L_B: log the mismatch, then skip this semantic (the engine's loop-continue) */
    size_t o = 0;
    tr[o++] = 0x3c; tr[o++] = 0xff;                 /* cmp al,0xff                                */
    tr[o++] = 0x0f; tr[o++] = 0x84;                 /* je near L_A                                */
    size_t je_a = o; o += 4;                        /*   (displacement filled in below)          */
    tr[o++] = 0x80; tr[o++] = 0xf9; tr[o++] = 0xff; /* cmp cl,0xff                                */
    tr[o++] = 0x0f; tr[o++] = 0x84;                 /* je near L_B                                */
    size_t je_b = o; o += 4;                        /*   (displacement filled in below)          */
    emit_jmp(tr, &o, VTX_CONT);                     /* both present: resume the converter        */

    size_t l_a = o;
    emit_jmp(tr, &o, VTX_DESTA);                    /* L_A: original al==0xFF handling           */

    size_t l_b = o;                                 /* L_B: dest present, src absent (the crash) */
    tr[o++] = 0x60;                                 /* pushad                                     */
    tr[o++] = 0x55;                                 /* push ebp   -> frameEbp (3rd arg)          */
    tr[o++] = 0x50;                                 /* push eax   -> destslot (2nd arg)          */
    tr[o++] = 0x53;                                 /* push ebx   -> semantic (1st arg)          */
    tr[o++] = 0xb8;                                 /* mov eax, imm32 = &gmv_vtx_missing         */
    { uint32_t fnp = (uint32_t)(uintptr_t)&gmv_vtx_missing; memcpy(tr + o, &fnp, 4); o += 4; }
    tr[o++] = 0xff; tr[o++] = 0xd0;                 /* call eax                                   */
    tr[o++] = 0x83; tr[o++] = 0xc4; tr[o++] = 0x0c; /* add esp,12   (cdecl arg cleanup)          */
    tr[o++] = 0x61;                                 /* popad                                      */
    emit_jmp(tr, &o, VTX_SKIP);                     /* skip this semantic -> next                */

    /* Fill the two near-je displacements now that the label offsets are known. */
    { int32_t r = (int32_t)(l_a - (je_a + 4)); memcpy(tr + je_a, &r, 4); }
    { int32_t r = (int32_t)(l_b - (je_b + 4)); memcpy(tr + je_b, &r, 4); }
    FlushInstructionCache(GetCurrentProcess(), tr, o);

    unsigned char site[8];
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)tr - (VTX_SITE + 5));
    site[0] = 0xe9;                                 /* jmp trampoline                             */
    memcpy(site + 1, &rel, 4);
    site[5] = site[6] = site[7] = 0x90;             /* pad to the next instruction (0x563e31)     */
    if (!patch_bytes(VTX_SITE, site, sizeof site)) {
        gmv_log("[gamepatch] VirtualProtect/write at 0x%08x failed (%lu); vertex fix skipped\n",
                VTX_SITE, GetLastError());
        return;
    }

    gmv_log("[gamepatch] vertex-converter cl-guard applied at 0x%08x (trampoline @ %p); "
            "dest-present/src-absent cases counted as [vtxmiss]\n",
            VTX_SITE, (void *)tr);
}
