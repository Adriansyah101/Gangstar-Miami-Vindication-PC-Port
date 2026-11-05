/* vbostatic.c -- STATIC_VBO (a port performance fix, [Fixes], on by default and under
   ORIGINAL_MAC_EXPERIENCE): the game's models that stay unchanged are kept in GL buffer objects.
   Tested 2026-10-30: time inside the draws 185 -> 77 ms a second (10.9 -> 4.7 us a draw), no crash, no
   visible change.  The per-site counts, the event lines and the fault dump are dev only.

   The game's glitch buffers (IBuffer) and where they live.

   glitch keeps a buffer's data in plain memory when its usage is 4 and draws it from client arrays;
   usages 0-2 (GL_STATIC/DYNAMIC/STREAM_DRAW) get a GL buffer object, filled at the first bind and
   refilled with glBufferSubData when the buffer is marked changed (byte +0x12 bit 2, which its map
   0x517b10 sets for any access but the GL-mapped ones).  The Collada mesh loaders create theirs with
   usage 4, so the city is drawn from client arrays and the driver copies it every draw.

   PERF_PROFILE: the IBuffer constructor call in createBuffer (0x501af1 -> 0x4c6ae0) is re-aimed to
   count the buffers each game site creates by type and usage, and map 0x517b10 is hooked to count
   the accesses asked for, on buffers with and without a GL buffer behind them.

   GMV_VBO_STATIC=1 (the experiment).  First try (every usage-4 buffer made static at creation): the
   batch mesh's per-draw index ranges (0x468f82, ~5000 a second) piled up until the driver crashed, and
   HUD parts went missing / artefacts showed -- some buffers are written behind the engine's back.
   Now a vertex or index buffer is only watched at creation (kept usage 4):
   - between frames its data is checksummed, a few MB a frame in turn; one unchanged for VS_QUIET
     checks in a row and not mapped gets usage 0 and the changed bit, so the engine's next bind makes
     its GL buffer from the data;
   - map on a converted buffer puts it back to usage 4 (its GL buffer deleted) before the map runs, so
     the write lands in the data the draws then use;
   - a converted buffer whose checksum changes all the same (written without map) is put back too
     (these rechecks run at VS_BUDGET_STATIC a frame, so one can show stale for up to ~0.5 s);
   - a buffer put back is never converted again; the destructors (CBuffer vtable 0x680a48 slots 0/1)
     drop it from the watch.
   Second try crashed in the menu (the driver reading freed client memory): bindImpl (slot 3, 0x504580)
   frees the buffer's data after the GL upload when its caller asks (setData(size, 0, own) 0x5177f0),
   and other objects still point into that memory.  Watched buffers are now always bound keeping it
   (it never happened in the runs after).  The menu crash itself (frame 3420 each time): a buffer made
   with size 0 (0x315f7a) gets its data through setData 0x5177f0, not map; on a converted buffer glitch
   only BufferSubData's the new data into the GL buffer made at the old size, so the draw indexed past
   the vertices.  setData is hooked too: a converted buffer goes back to client memory first.
   Not watched: the batch ranges (0x468f82), any site past VS_SITE_MAX buffers, and the sites seen
   writing their buffers behind the engine's back (vs_skip), which the checksum only catches late.

   Buffer fields: +8 data, +0xc size, +0x10 type (0 vertex, 1 index), +0x11 usage, +0x12 flags (2 =
   changed, 8 = has GL storage, 0x20 = GL-mapped), +0x13 map state, +0x14 driver (+0x224 + type*4 =
   its bound-buffer cache), +0x18 GL name. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"
#include <GL/gl.h>
#include <emmintrin.h>

#include "log.h"
#include "shims.h"
#include "gangstar.h"

/* Never converted: CBatchMesh's per-draw index ranges (wrapping other buffers' memory, thousands a
   second), and the sites whose buffers were seen changed without map or setData (2026-10-30 runs). */
static const uint32_t vs_skip[] = {0x468f82u, 0x315f7au, 0x3162b6u, 0x316854u, 0x469f4fu};

#define VS_CTOR_SITE 0x00501af1u                        /* createBuffer: call IBuffer ctor */
#define VS_CTOR 0x004c6ae0u
#define VS_MAP 0x00517b10u                              /* IBuffer::map(buffer, access) */
#define VS_VTBL 0x00680a48u                             /* CBuffer vtable: [0] 0x5036c0, [1] 0x5035b0 */
static const unsigned char vs_map_pro[7] = {0x55, 0x89, 0xe5, 0x53, 0x8b, 0x4d, 0x08};
#define VS_SETDATA 0x005177f0u                          /* IBuffer::setData(buffer, size, data, own) */
static const unsigned char vs_set_pro[6] = {0x55, 0x89, 0xe5, 0x83, 0xec, 0x28};
typedef void (*setdata_fn)(void *buf, unsigned size, void *data, int own);
static setdata_fn g_setdata;                            /* trampoline */

#define VS_QUIET 4                                      /* unchanged checks before converting */
#define VS_BUDGET (3u << 20)                            /* bytes of watched buffers checksummed a frame */
/* The converted ones are rechecked more slowly: map and setData already put them back, so this only
   catches a site writing behind the engine's back (none left after vs_skip in the 2026-10-30 runs).
   At 3 MB a frame the rechecks took ~9% of a tick; 512 KB goes round ~30 MB in about half a second. */
#define VS_BUDGET_STATIC (512u << 10)

typedef void (*ctor_fn)(void *obj, void *drv, int type, int usage, unsigned size, void *data, int keep);
typedef void *(*map_fn)(void *buf, int access);
typedef void (*dtor_fn)(void *obj);
typedef void (*bind_fn)(void *buf, int drop);
static map_fn g_map;                                    /* trampoline */
static dtor_fn g_dtor0, g_dtor1;
static bind_fn g_bind;
#define VS_BIND 0x00504580u                             /* CBuffer vtable slot 3: bindImpl(buf, drop data) */

static int g_static;                                    /* STATIC_VBO */
static volatile LONG g_lockv;
static DWORD g_gl_tid;                                  /* the thread that presents (GL context) */

#define VS_SITES 64
#define VS_SITE_MAX 2000                                /* a site past this many makes throwaway buffers */
static struct site {
    uint32_t ret;
    unsigned n, by_usage[6], by_type[4];
    unsigned conv, rev_map, rev_hash;
    unsigned long long bytes, conv_bytes;
} g_site[VS_SITES];
static int g_nsite;
static unsigned g_more;                                 /* sites past the table */

#if GMV_DEV
static volatile LONG g_map_n[8][2];                     /* access (0-7) x has GL storage */
#endif

/* ---- watched buffers: a dense array and an open-addressing index by object ---- */
enum { W_WATCH, W_STATIC, W_NEVER };
typedef struct {
    uint8_t *obj;
    uint32_t s1, s2;                                    /* checksum at the last check */
    uint16_t quiet;
    uint8_t state, site;
} went;
static went *g_w;
static int g_nw, g_wcap;
static int32_t *g_ix;                                   /* -1 empty */
static uint32_t g_ixcap;                                /* power of 2 */
static int g_cursor, g_scursor;                         /* next watched / converted entry to check */
static uint8_t **g_byname;                              /* GL name -> the buffer last bound with it (GL thread) */
static uint32_t g_bynamecap;
static struct { unsigned conv, rev_map, rev_hash, rev_set, leaked, gone, kept; unsigned long long conv_bytes; } g_tot;

static void vs_lock(void)
{
    while (InterlockedCompareExchange(&g_lockv, 1, 0))
        YieldProcessor();
}

static void vs_unlock(void)
{
    InterlockedExchange(&g_lockv, 0);
}

static uint32_t vs_slot(const void *obj)
{
    uint32_t h = (uint32_t)(uintptr_t)obj;
    h ^= h >> 16;
    h *= 0x45d9f3bu;
    h ^= h >> 16;
    return h & (g_ixcap - 1);
}

static int vs_find(const void *obj)
{
    if (!g_ixcap)
        return -1;
    for (uint32_t i = vs_slot(obj);; i = (i + 1) & (g_ixcap - 1)) {
        int32_t k = g_ix[i];
        if (k < 0)
            return -1;
        if (g_w[k].obj == obj)
            return k;
    }
}

static int vs_rehash(uint32_t cap)
{
    int32_t *ix = malloc(cap * sizeof *ix);
    if (!ix)
        return 0;
    for (uint32_t i = 0; i < cap; i++)
        ix[i] = -1;
    free(g_ix);
    g_ix = ix;
    g_ixcap = cap;
    for (int k = 0; k < g_nw; k++) {
        uint32_t i = vs_slot(g_w[k].obj);
        while (g_ix[i] >= 0)
            i = (i + 1) & (cap - 1);
        g_ix[i] = k;
    }
    return 1;
}

static void vs_add(uint8_t *obj, int site)
{
    if (vs_find(obj) >= 0)
        return;
    if (g_nw == g_wcap) {
        int c = g_wcap ? g_wcap * 2 : 1024;
        went *w = realloc(g_w, (size_t)c * sizeof *w);
        if (!w)
            return;
        g_w = w;
        g_wcap = c;
    }
    if ((uint32_t)(g_nw + 1) * 2 > g_ixcap && !vs_rehash(g_ixcap ? g_ixcap * 2 : 4096))
        return;
    went *e = &g_w[g_nw];
    memset(e, 0, sizeof *e);
    e->obj = obj;
    e->site = (uint8_t)site;
    uint32_t i = vs_slot(obj);
    while (g_ix[i] >= 0)
        i = (i + 1) & (g_ixcap - 1);
    g_ix[i] = g_nw++;
}

/* Remove slot i of the index (backward-shift deletion). */
static void vs_ix_del(uint32_t i)
{
    uint32_t m = g_ixcap - 1;
    g_ix[i] = -1;
    for (uint32_t j = (i + 1) & m; g_ix[j] >= 0; j = (j + 1) & m) {
        int32_t k = g_ix[j];
        uint32_t h = vs_slot(g_w[k].obj);
        /* move k back to i if its home is not in (i, j] cyclically */
        if (((j - h) & m) >= ((j - i) & m)) {
            g_ix[i] = k;
            g_ix[j] = -1;
            i = j;
        }
    }
}

static void vs_event(const char *what, const uint8_t *b, int site);

static void vs_remove(const void *obj)
{
    int k = vs_find(obj);
    if (k < 0)
        return;
    if (g_w[k].state == W_STATIC)
        vs_event("destroyed static", obj, g_w[k].site);
    uint32_t i = vs_slot(obj);
    while (g_ix[i] != k)
        i = (i + 1) & (g_ixcap - 1);
    vs_ix_del(i);
    int last = g_nw - 1;
    if (k != last) {                                    /* the last entry moves into k */
        uint32_t j = vs_slot(g_w[last].obj);
        while (g_ix[j] != last)
            j = (j + 1) & (g_ixcap - 1);
        g_w[k] = g_w[last];
        g_ix[j] = k;
    }
    g_nw--;
    g_tot.gone++;
}

/* Fletcher-style sums in 4 lanes of 32 bits, 16 bytes a step (SSE2) */
__attribute__((target("sse2")))
static void vs_sum(const uint8_t *p, uint32_t n, uint32_t *a, uint32_t *b)
{
    __m128i v1 = _mm_setzero_si128(), v2 = _mm_setzero_si128();
    uint32_t k = n / 16;
    for (uint32_t i = 0; i < k; i++) {
        v1 = _mm_add_epi32(v1, _mm_loadu_si128((const __m128i *)(p + i * 16)));
        v2 = _mm_add_epi32(v2, v1);
    }
    uint32_t l1[4], l2[4];
    _mm_storeu_si128((__m128i *)l1, v1);
    _mm_storeu_si128((__m128i *)l2, v2);
    uint32_t s1 = 0x12345678u ^ n, s2 = 0;
    for (int j = 0; j < 4; j++) {
        s1 = s1 * 31 + l1[j];
        s2 = s2 * 31 + l2[j];
    }
    for (uint32_t i = k * 16; i < n; i++) {
        s1 += p[i];
        s2 += s1;
    }
    *a = s1;
    *b = s2;
}

/* The first VS_EVENTS conversions, put-backs and destructions of static buffers, logged as they happen. */
#define VS_EVENTS 300
static void vs_event(const char *what, const uint8_t *b, int site)
{
#if GMV_DEV
    static volatile LONG n;
    if (!gmv_perf_on() || InterlockedIncrement(&n) > VS_EVENTS)
        return;
    uint32_t data, size, name;
    memcpy(&data, b + 8, 4);
    memcpy(&size, b + 0xc, 4);
    memcpy(&name, b + 0x18, 4);
    gmv_log("[vbostatic] %s %p (site 0x%06x, %s, usage %u, flags 0x%02x, data 0x%08x, %u bytes, GL %u)\n", what,
            (const void *)b, site >= 0 ? g_site[site].ret : 0, b[0x10] ? "index" : "vertex", b[0x11], b[0x12], data,
            size, name);
#else
    (void)what, (void)b, (void)site;
#endif
}

/* Back to client memory (under the lock). */
static void vs_revert(uint8_t *b)
{
    uint8_t f = b[0x12];
    b[0x11] = 4;
    if ((f & 8) && !(f & 0x20)) {
        uint32_t name;
        memcpy(&name, b + 0x18, 4);
        if (name && GetCurrentThreadId() == g_gl_tid) {
            static void(APIENTRY * del)(GLsizei, const GLuint *);
            if (!del) del = (void(APIENTRY *)(GLsizei, const GLuint *))(void *)gl_real("glDeleteBuffers");
            uint8_t *drv;
            memcpy(&drv, b + 0x14, 4);
            uint32_t *cache = (uint32_t *)(drv + 0x224 + 4 * (b[0x10] & 3));
            if (*cache == name)
                *cache = 0;                             /* GL unbinds a deleted buffer */
            if (del) {
                del(1, &name);
                memset(b + 0x18, 0, 4);
            } else
                g_tot.leaked++;
        } else if (name)
            g_tot.leaked++;                             /* not the GL thread: left (never deleted) */
        b[0x12] = (uint8_t)(f & ~(8 | 2));
    } else
        b[0x12] = (uint8_t)(f & ~2);
}

static void vs_ctor(void *obj, void *drv, int type, int usage, unsigned size, void *data, int keep)
{
    /* createBuffer's frame: push ebp; mov ebp, esp; sub esp, 0x38 -- its return address sits 0x3c
       above the first argument of this call. */
    uintptr_t at = (uintptr_t)&obj + 0x3c;
    uint32_t ret = *(const volatile uint32_t *)at;
    ((ctor_fn)(uintptr_t)VS_CTOR)(obj, drv, type, usage, size, data, keep);
    vs_lock();
    int si = -1;
    for (int i = 0; i < g_nsite; i++)
        if (g_site[i].ret == ret) { si = i; break; }
    if (si < 0 && g_nsite < VS_SITES) {
        si = g_nsite++;
        g_site[si].ret = ret;
    }
    if (si >= 0) {
        struct site *s = &g_site[si];
        s->n++;
        s->by_usage[(unsigned)usage < 6 ? usage : 5]++;
        s->by_type[(unsigned)type < 4 ? type : 3]++;
        s->bytes += size;
        int skip = 0;
        for (size_t i = 0; i < sizeof vs_skip / sizeof *vs_skip; i++)
            skip |= ret == vs_skip[i];
        if (g_static && usage == 4 && (type == 0 || type == 1) && !skip && s->n <= VS_SITE_MAX)
            vs_add(obj, si);
    } else
        g_more++;
    vs_unlock();
}

static void *vs_map(void *buf, int access)
{
    uint8_t *b = buf;
#if GMV_DEV
    InterlockedIncrement(&g_map_n[access & 7][(b[0x12] & 8) ? 1 : 0]);
#endif
    if (g_static) {
        vs_lock();
        int k = vs_find(b);
        if (k >= 0) {
            went *e = &g_w[k];
            if (e->state == W_STATIC) {
                vs_event("put back (map)", b, e->site);
                vs_revert(b);
                e->state = W_NEVER;
                g_tot.rev_map++;
                g_site[e->site].rev_map++;
            } else
                e->quiet = 0;
        }
        vs_unlock();
    }
    return g_map(buf, access);
}

/* A watched buffer keeps its data at the bind: it is what other objects point into and what a put
   back buffer is drawn from. */
static void vs_bind(void *buf, int drop)
{
    if ((drop & 0xff) && g_static) {
        vs_lock();
        if (vs_find(buf) >= 0) {
            drop = 0;
            g_tot.kept++;
        }
        vs_unlock();
    }
    uint32_t name0;
    memcpy(&name0, (uint8_t *)buf + 0x18, 4);
    g_bind(buf, drop);
    if (g_static) {                                     /* GL name -> buffer, for gmv_vbostatic_client */
        uint32_t name;
        memcpy(&name, (uint8_t *)buf + 0x18, 4);
        if (name && !name0) {
            vs_lock();
            int k = vs_find(buf);
            if (k >= 0)
                vs_event("uploaded", buf, g_w[k].site);
            vs_unlock();
        }
        if (name && name < (1u << 20)) {
            if (name >= g_bynamecap) {
                uint32_t c = g_bynamecap ? g_bynamecap : 4096;
                while (c <= name)
                    c *= 2;
                uint8_t **a = realloc(g_byname, c * sizeof *a);
                if (!a)
                    return;
                memset(a + g_bynamecap, 0, (c - g_bynamecap) * sizeof *a);
                g_byname = a;
                g_bynamecap = c;
            }
            g_byname[name] = buf;
        }
    }
}

/* Where an array or index pointer given while GL buffer `name` was bound lies in that buffer's data, for
   the host's own readers of the vertices (the sky's colours, the sea).  Only for a buffer made static
   here (its data kept and unchanged); NULL otherwise.  GL thread. */
const void *gmv_vbostatic_client(unsigned name, uintptr_t off)
{
    if (!g_static || name >= g_bynamecap || !g_byname[name])
        return NULL;
    uint8_t *b = g_byname[name];
    const void *r = NULL;
    vs_lock();
    int k = vs_find(b);                                 /* b is dereferenced only while it is a live buffer */
    if (k >= 0 && g_w[k].state == W_STATIC && (b[0x12] & 8)) {
        uint32_t nm, size;
        uint8_t *data;
        memcpy(&nm, b + 0x18, 4);
        memcpy(&data, b + 8, 4);
        memcpy(&size, b + 0xc, 4);
        if (nm == name && data && off < size)
            r = data + off;
    }
    vs_unlock();
    return r;
}

/* New data (or a new size) for a buffer: glitch would only BufferSubData it into the GL buffer made at
   the old size, so a converted one goes back to client memory first. */
static void vs_setdata(void *buf, unsigned size, void *data, int own)
{
    if (g_static) {
        uint8_t *b = buf;
        vs_lock();
        int k = vs_find(b);
        if (k >= 0) {
            went *e = &g_w[k];
            if (e->state == W_STATIC) {
                vs_event("put back (new data)", b, e->site);
                vs_revert(b);
                e->state = W_NEVER;
                g_tot.rev_set++;
            } else
                e->quiet = 0;
        }
        vs_unlock();
    }
    g_setdata(buf, size, data, own);
}

static void vs_dtor0(void *obj)
{
    vs_lock();
    vs_remove(obj);
    vs_unlock();
    g_dtor0(obj);
}

static void vs_dtor1(void *obj)
{
    vs_lock();
    vs_remove(obj);
    vs_unlock();
    g_dtor1(obj);
}

/* Checksum one entry: a watched one is converted after VS_QUIET quiet checks, a converted one that
   changed goes back.  Returns the bytes read. */
static uint32_t vs_check(went *e)
{
    uint8_t *b = e->obj, *data;
    uint32_t size;
    memcpy(&data, b + 8, 4);
    memcpy(&size, b + 0xc, 4);
    if (!data || !size || b[0x13])                      /* no data, or mapped right now */
        return 0;
    uint32_t s1, s2;
    vs_sum(data, size, &s1, &s2);
    int same = s1 == e->s1 && s2 == e->s2;
    e->s1 = s1;
    e->s2 = s2;
    if (e->state == W_STATIC) {
        if (!same) {
            vs_event("put back (changed)", b, e->site);
            vs_revert(b);
            e->state = W_NEVER;
            g_tot.rev_hash++;
            g_site[e->site].rev_hash++;
        }
    } else if (!same)
        e->quiet = 0;
    else if (++e->quiet >= VS_QUIET && b[0x11] == 4 && !(b[0x12] & 8)) {
        b[0x11] = 0;                                    /* GL_STATIC_DRAW */
        b[0x12] |= 2;                                   /* the next bind makes the GL buffer */
        e->state = W_STATIC;
        vs_event("made static", b, e->site);
        g_tot.conv++;
        g_tot.conv_bytes += size;
        g_site[e->site].conv++;
        g_site[e->site].conv_bytes += size;
    }
    return size;
}

/* Walk the entries in the given state from *cur on, until budget bytes are checked or all were seen */
static void vs_round(int *cur, int state, uint32_t budget)
{
    uint32_t done = 0;
    for (int visits = 0; g_nw && done < budget && visits < g_nw; visits++) {
        if (*cur >= g_nw)
            *cur = 0;
        went *e = &g_w[(*cur)++];
        if (e->state == state)
            done += vs_check(e);
    }
}

/* Between frames, on the GL thread: checksum the watched buffers in turn, convert the quiet ones and
   (more slowly) put back the converted ones that changed. */
void gmv_vbostatic_frame(void)
{
    if (!g_static)
        return;
    g_gl_tid = GetCurrentThreadId();
    vs_lock();
    vs_round(&g_cursor, W_WATCH, VS_BUDGET);
    vs_round(&g_scursor, W_STATIC, VS_BUDGET_STATIC);
    vs_unlock();
}

static int vs_patch(uint32_t at, const void *bytes, size_t n)
{
    DWORD old;
    if (!VirtualProtect((void *)(uintptr_t)at, n, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy((void *)(uintptr_t)at, bytes, n);
    VirtualProtect((void *)(uintptr_t)at, n, old, &old);
    return 1;
}

void gmv_vbostatic_install(void)
{
    const char *e = gmv_getenv("GMV_STATIC_VBO");
    int want = e ? (*e && *e != '0') : 1;
    if (!want && !gmv_perf_on())
        return;
    const unsigned char *c = (const unsigned char *)(uintptr_t)VS_CTOR_SITE;
    const uint32_t *vt = (const uint32_t *)(uintptr_t)VS_VTBL;
    int32_t rel;
    memcpy(&rel, c + 1, 4);
    if (c[0] != 0xe8 || VS_CTOR_SITE + 5 + (uint32_t)rel != VS_CTOR || vt[0] != 0x5036c0u || vt[1] != 0x5035b0u ||
        vt[3] != VS_BIND || memcmp((const void *)(uintptr_t)VS_MAP, vs_map_pro, sizeof vs_map_pro) ||
        memcmp((const void *)(uintptr_t)VS_SETDATA, vs_set_pro, sizeof vs_set_pro)) {
        gmv_log("[vbostatic] the buffer code is not as expected; skipped\n");
        return;
    }
    unsigned char *tr = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr)
        return;
    memcpy(tr, vs_map_pro, sizeof vs_map_pro);          /* map's first 3 instructions, then back */
    tr[7] = 0xe9;
    int32_t r = (int32_t)(VS_MAP + 7 - (uint32_t)(uintptr_t)(tr + 12));
    memcpy(tr + 8, &r, 4);
    unsigned char *ts = tr + 16;                        /* setData's first 3 instructions, then back */
    memcpy(ts, vs_set_pro, sizeof vs_set_pro);
    ts[6] = 0xe9;
    r = (int32_t)(VS_SETDATA + 6 - (uint32_t)(uintptr_t)(ts + 11));
    memcpy(ts + 7, &r, 4);
    FlushInstructionCache(GetCurrentProcess(), tr, 4096);
    g_map = (map_fn)(void *)tr;
    g_setdata = (setdata_fn)(void *)ts;
    unsigned char js[5] = {0xe9};
    int32_t rs = (int32_t)((uint32_t)(uintptr_t)vs_setdata - (VS_SETDATA + 5));
    memcpy(js + 1, &rs, 4);
    g_dtor0 = (dtor_fn)(uintptr_t)vt[0];
    g_dtor1 = (dtor_fn)(uintptr_t)vt[1];
    g_bind = (bind_fn)(uintptr_t)vt[3];
    uint32_t bnd = (uint32_t)(uintptr_t)vs_bind;
    unsigned char j[5] = {0xe9};
    r = (int32_t)((uint32_t)(uintptr_t)vs_map - (VS_MAP + 5));
    memcpy(j + 1, &r, 4);
    uint32_t d[2] = {(uint32_t)(uintptr_t)vs_dtor0, (uint32_t)(uintptr_t)vs_dtor1};
    r = (int32_t)((uint32_t)(uintptr_t)vs_ctor - (VS_CTOR_SITE + 5));
    if (!vs_patch(VS_VTBL, d, sizeof d) || !vs_patch(VS_VTBL + 12, &bnd, 4) || !vs_patch(VS_MAP, j, 5) ||
        !vs_patch(VS_SETDATA, js, 5) || !vs_patch(VS_CTOR_SITE + 1, &r, 4)) {
        gmv_log("[vbostatic] patch failed (%lu)\n", GetLastError());
        FlushInstructionCache(GetCurrentProcess(), NULL, 0);
        return;
    }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    g_static = want;
    gmv_log("[vbostatic] %s\n", g_static ? "STATIC_VBO=1: the game's models that stay unchanged are kept in GL buffer "
                                           "objects (GL_STATIC_DRAW)" : "STATIC_VBO=0: only counting the game's buffers");
}

#if GMV_DEV
/* PERF_PROFILE, every 5 s: the sites (when buffers were made since), the conversions and the maps. */
void gmv_vbostatic_report(double seconds)
{
    if (seconds <= 0 || !g_map)
        return;
    struct site s[VS_SITES];
    int n, nw, nstatic = 0;
    vs_lock();
    n = g_nsite;
    memcpy(s, g_site, sizeof s);
    nw = g_nw;
    for (int k = 0; k < g_nw; k++)
        nstatic += g_w[k].state == W_STATIC;
    __typeof__(g_tot) tot = g_tot;
    vs_unlock();
    static int last_n = -1;
    static unsigned last_total, last_rev;
    unsigned total = 0;
    for (int i = 0; i < n; i++)
        total += s[i].n;
    if (n != last_n || total != last_total || tot.rev_map + tot.rev_hash != last_rev) {
        last_n = n;
        last_total = total;
        last_rev = tot.rev_map + tot.rev_hash;
        gmv_log("[perf]   glitch buffers so far: %u from %d sites%s\n", total, n, g_more ? " (more not listed)" : "");
        for (int i = 0; i < n; i++)
            gmv_log("[perf]     site 0x%06x: %u (vertex %u, index %u, other %u), usage 0/1/2/4 = %u/%u/%u/%u, "
                    "%.1f MB; made static %u (%.1f MB), put back: mapped %u, changed %u\n", s[i].ret, s[i].n,
                    s[i].by_type[0], s[i].by_type[1], s[i].by_type[2] + s[i].by_type[3], s[i].by_usage[0],
                    s[i].by_usage[1], s[i].by_usage[2], s[i].by_usage[4], (double)s[i].bytes / 1048576.0, s[i].conv,
                    (double)s[i].conv_bytes / 1048576.0, s[i].rev_map, s[i].rev_hash);
    }
    if (g_static)
        gmv_log("[perf]   static buffers: %d watched, %d static now; so far made static %u (%.1f MB), put back "
                "mapped %u / new data %u / changed without either %u, GL buffers left %u, destroyed %u; data kept at "
                "bind %u\n", nw, nstatic, tot.conv, (double)tot.conv_bytes / 1048576.0, tot.rev_map, tot.rev_set,
                tot.rev_hash, tot.leaked, tot.gone, tot.kept);
    LONG m[8][2];
    for (int a = 0; a < 8; a++)
        for (int g = 0; g < 2; g++)
            m[a][g] = InterlockedExchange(&g_map_n[a][g], 0);
    gmv_log("[perf]   buffer maps a second by access 0-5 (client / GL): %.0f/%.0f %.0f/%.0f %.0f/%.0f %.0f/%.0f "
            "%.0f/%.0f %.0f/%.0f\n", m[0][0] / seconds, m[0][1] / seconds, m[1][0] / seconds, m[1][1] / seconds,
            m[2][0] / seconds, m[2][1] / seconds, m[3][0] / seconds, m[3][1] / seconds, m[4][0] / seconds,
            m[4][1] / seconds, m[5][0] / seconds, m[5][1] / seconds);
}

/* From the fault handler (no lock: the faulting thread may hold it): the static buffers, and the GL
   bindings and array pointers when the fault is on the GL thread. */
void gmv_vbostatic_fault(void)
{
    if (!g_static)
        return;
    gmv_log("        [vbostatic] %d watched; static ones:\n", g_nw);
    int shown = 0;
    uint8_t *drv = NULL;
    for (int k = 0; k < g_nw && shown < 40; k++) {
        if (g_w[k].state != W_STATIC)
            continue;
        const uint8_t *b = g_w[k].obj;
        if (IsBadReadPtr(b, 0x1c)) {
            gmv_log("        [vbostatic]   %p unreadable\n", (const void *)b);
            continue;
        }
        uint32_t data, size, name;
        memcpy(&data, b + 8, 4);
        memcpy(&size, b + 0xc, 4);
        memcpy(&name, b + 0x18, 4);
        memcpy(&drv, b + 0x14, 4);
        gmv_log("        [vbostatic]   %p site 0x%06x %s usage %u flags 0x%02x map %u data 0x%08x..0x%08x GL %u%s\n",
                (const void *)b, g_site[g_w[k].site].ret, b[0x10] ? "index" : "vertex", b[0x11], b[0x12], b[0x13],
                data, data + size, name, IsBadReadPtr((const void *)(uintptr_t)data, size ? size : 1) ? " (data unreadable)" : "");
        shown++;
    }
    if (drv && !IsBadReadPtr(drv + 0x224, 8)) {
        uint32_t c[2];
        memcpy(c, drv + 0x224, 8);
        gmv_log("        [vbostatic] glitch's bound buffers: array %u, element %u\n", c[0], c[1]);
    }
    if (GetCurrentThreadId() != g_gl_tid || !wglGetCurrentContext())
        return;
    GLint ab = 0, eb = 0, vb = 0, cb = 0, tb = 0, nb = 0, ct = 0;
    glGetIntegerv(0x8894, &ab);
    glGetIntegerv(0x8895, &eb);
    glGetIntegerv(0x8896, &vb);
    glGetIntegerv(0x8897, &nb);
    glGetIntegerv(0x8898, &cb);
    glGetIntegerv(0x84E1, &ct);
    glGetIntegerv(0x889A, &tb);
    void *vp = NULL, *cp = NULL, *tp = NULL, *np = NULL;
    glGetPointerv(GL_VERTEX_ARRAY_POINTER, &vp);
    glGetPointerv(GL_COLOR_ARRAY_POINTER, &cp);
    glGetPointerv(GL_TEXTURE_COORD_ARRAY_POINTER, &tp);
    glGetPointerv(GL_NORMAL_ARRAY_POINTER, &np);
    gmv_log("        [vbostatic] GL: array buffer %d, element buffer %d; vertex %s buf %d ptr %p, colour %s buf %d ptr %p, "
            "normal %s buf %d ptr %p, texcoord (unit 0x%x) %s buf %d ptr %p\n", ab, eb,
            glIsEnabled(GL_VERTEX_ARRAY) ? "on" : "off", vb, vp, glIsEnabled(GL_COLOR_ARRAY) ? "on" : "off", cb, cp,
            glIsEnabled(GL_NORMAL_ARRAY) ? "on" : "off", nb, np, ct, glIsEnabled(GL_TEXTURE_COORD_ARRAY) ? "on" : "off",
            tb, tp);
}
#endif
