/* hudbars.c -- the HUD's health/armour bars without 38 full UI searches per update (same result).

   PERF_PROFILE's UI-lookup probe showed the game's dotted-path UI search (find 0x2fd160, breadth-
   first search 0x2fcf60/0x2fcba0) running 5000-9000 times a second while driving, ~1100-1500 tree
   entries each.  Nearly all of it is two HUD bar functions:
     0x17e870 bar_refresh(bar)        and   0x17e98c bar_set(bar, value)  (stores value at bar+0x14)
   which pick a name by bar+0x18 (0 "IG_HUD_LIFES.character_life", 1 "...car_life",
   2 "...ammor_life", else nothing) and then, for i = 0..37,
     sprintf(path, "%s.bar.w%d", name, i);  SetVisible(ui = bar+8, path, value <= i);
   SetVisible (0x2fdf80) is node = find(ui, path); if (node) node->+0xab = flag.

   The search is a FIFO walk from root ui+0x40 of (node, path position) entries: a node whose name
   (tu_string at +0x44: length byte + chars at +0x45, or 0xff with length at +0x48 and chars at
   +0x50; the length counts the NUL) equals the current segment moves the position on, or ends the
   search at the last segment; then if node->vtbl[2](node, 2) (a sprite) its children (+0xb8 array,
   +0xbc count) are queued with that position.  When that finds nothing, find tries other roots.

   The 38 paths share every segment but the last, so their walks queue exactly the same entries
   in the same order until each one meets its own "wN".  One walk can therefore answer all 38: the
   first entry at the last position named "wN" is path N's answer, as its own walk would return.
   SetVisible's write (+0xab) is not read by the walk, so the writes can follow afterwards.  If any
   path is not found from the first root (the original then tries other roots), the original loop
   runs instead.  The search keeps only scratch arrays in ui (+0x18, +0x28), reset per search.

   The police stars (5 searches per refresh, nearly every frame) are done the same way; see
   ps_refresh below.

   Installed as a jmp at the functions' entries.  GMV_FAST_HUDBARS=0 (dev) keeps the originals;
   GMV_HUDBARS_VERIFY=1 (dev) also runs the original search per path and logs any difference. */

#include "compat.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "shims.h"
#include "gangstar.h"

#define HB_REFRESH 0x0017e870u
#define HB_SET     0x0017e98cu
#define HB_FIND    0x002fd160u
#define HB_SETVIS  0x002fdf80u
#define HB_N       38
#define HB_QMAX    (1u << 20)                /* queue entries before giving up (a cycle) */

typedef void *(*hb_find_fn)(void *ui, const char *path);
typedef void (*hb_setvis_fn)(void *ui, const char *path, int on);
typedef char (*hb_is_fn)(void *node, int kind);

static const char *const hb_names[3] = {
    "IG_HUD_LIFES.character_life", "IG_HUD_LIFES.car_life", "IG_HUD_LIFES.ammor_life"
};

typedef struct { unsigned char *node; int pos; } hb_ent;
static hb_ent *g_q;
static size_t g_qcap;
static volatile LONG g_busy;
static int g_on, g_verify;
static unsigned g_mismatch;
typedef struct { unsigned updates, fast, fallback, busy; unsigned long long walked; } hb_stats;
static hb_stats g_st, g_sst;                  /* bars, police stars */
static size_t g_lastwalk;                     /* entries the last walk dequeued */
static const char *g_wnames[HB_N];            /* "w0".."w37" */
static char g_wbuf[HB_N][4];

/* The node's name, as the search reads it. */
static int hb_name(const unsigned char *n, const char **s)
{
    if (n[0x44] == 0xff) {
        *s = *(const char *const *)(n + 0x50);
        return *(const int *)(n + 0x48) - 1;
    }
    *s = (const char *)n + 0x45;
    return (int)(signed char)n[0x44] - 1;
}

/* One walk for the n paths "<pre>.<last[i]>" from root (all n last segments different, at most
   HB_N); res[i] = path i's answer.  1 when every one was found. */
static int hb_walk(unsigned char *root, const char *pre, int n, const char *const *last,
                   unsigned char **res)
{
    const char *seg[8];
    int segl[8], m = 0;
    for (const char *p = pre;;) {
        const char *d = strchr(p, '.');
        int l = d ? (int)(d - p) : (int)strlen(p);
        if (m == 8)
            return 0;
        seg[m] = p, segl[m++] = l;
        if (!d)
            break;
        p = d + 1;
    }
    int lastl[HB_N];
    if (n < 1 || n > HB_N)
        return 0;
    char c0 = last[0][0];                     /* a quick first test when all share it */
    for (int i = 0; i < n; i++) {
        lastl[i] = (int)strlen(last[i]);
        if (last[i][0] != c0 || !lastl[i])
            c0 = 0;
        res[i] = NULL;
    }
    if (!root)
        return 0;
    int left = n;
    size_t head = 0, tail = 0;
    g_q[tail++] = (hb_ent){ root, 0 };
    while (head < tail) {
        hb_ent e = g_q[head++];
        unsigned char *nd = e.node;
        if (!nd)
            return 0;
        const char *nm;
        int nl = hb_name(nd, &nm), pos = e.pos;
        if (pos < m) {
            if (nl == segl[pos] && !memcmp(nm, seg[pos], (size_t)nl))
                pos++;
        } else if (nl > 0 && (!c0 || nm[0] == c0)) {
            for (int i = 0; i < n; i++)
                if (nl == lastl[i] && !memcmp(nm, last[i], (size_t)nl)) {
                    if (!res[i]) {
                        res[i] = nd;
                        if (!--left) {
                            g_lastwalk = head;
                            return 1;
                        }
                    }
                    break;
                }
        }
        hb_is_fn is = (hb_is_fn)(*(void ***)nd)[2];
        if (!is(nd, 2))
            continue;
        for (int j = 0; j < *(const int *)(nd + 0xbc); j++) {
            if (tail == g_qcap) {
                size_t cap = g_qcap * 2;
                hb_ent *q = cap <= HB_QMAX ? realloc(g_q, cap * sizeof *q) : NULL;
                if (!q)
                    return 0;
                g_q = q, g_qcap = cap;
            }
            g_q[tail++] = (hb_ent){ (*(unsigned char ***)(nd + 0xb8))[j], pos };
        }
    }
    g_lastwalk = head;
    return 0;
}

static void hb_update(unsigned char *bar)
{
    uint32_t type = *(const uint32_t *)(bar + 0x18);
    if (type > 2)
        return;
    const char *name = hb_names[type];
    void *ui = *(void **)(bar + 8);
    int value = *(const int *)(bar + 0x14);
    if (GMV_DEV)
        g_st.updates++;
    if (InterlockedCompareExchange(&g_busy, 1, 0) == 0) {
        unsigned char *res[HB_N];
        char pre[64];
        snprintf(pre, sizeof pre, "%s.bar", name);
        int ok = hb_walk(*(unsigned char **)((char *)ui + 0x40), pre, HB_N, g_wnames, res);
        if (GMV_DEV)
            g_st.walked += g_lastwalk;
        if (ok && GMV_DEV && g_verify) {
            for (int i = 0; i < HB_N; i++) {
                char path[64];
                snprintf(path, sizeof path, "%s.bar.w%d", name, i);
                unsigned char *o = ((hb_find_fn)(uintptr_t)HB_FIND)(ui, path);
                if (o != res[i] && g_mismatch++ < 20)
                    gmv_log("[hudbars] MISMATCH #%u: \"%s\" original %p, one walk %p\n", g_mismatch, path,
                            (void *)o, (void *)res[i]);
                res[i] = o;
            }
        }
        InterlockedExchange(&g_busy, 0);
        if (ok) {
            if (GMV_DEV)
                g_st.fast++;
            for (int i = 0; i < HB_N; i++)
                if (res[i])
                    res[i][0xab] = value <= i;
            return;
        }
        if (GMV_DEV)
            g_st.fallback++;
    } else if (GMV_DEV) {
        g_st.busy++;
    }
    /* The original loop. */
    for (int i = 0; i < HB_N; i++) {
        char path[64];
        snprintf(path, sizeof path, "%s.bar.w%d", name, i);
        ((hb_setvis_fn)(uintptr_t)HB_SETVIS)(*(void **)(bar + 8), path, *(const int *)(bar + 0x14) <= i);
    }
}

__attribute__((cdecl, force_align_arg_pointer))
static void hb_refresh(unsigned char *bar)
{
    hb_update(bar);
}

__attribute__((cdecl, force_align_arg_pointer))
static void hb_set(unsigned char *bar, int value)
{
    *(int *)(bar + 0x14) = value;
    hb_update(bar);
}

/* Police stars.  0x180412 stars_refresh(hud) (also reached by tail jumps from 0x17b110, 0x18052d
   set count, 0x18053f set flag) does, reading hud's fields afresh each time, for i = 0..4:
     sprintf(path, "ingame_HUD_PoliceStars.stars%d", i);
     if (count (+0x14) >= i + 1)
         flag (+0x13) ? SetVisible(ui, "ingame_HUD_PoliceStars", 1)
                      : (SetVisible(ui, path, 1), play(ui, path, "blink", 0) [0x2fe1d0]);
     else
         SetVisible(ui, path, 0);
   The shown stars keep the original calls (the "blink" play runs the movie's code, which may change
   the UI tree).  The hidden ones are the last 5 - count of the loop and only write +0xab, so one
   walk made where the first of them would search answers them all, as for the bars. */
#define PS_REFRESH 0x00180412u
#define PS_PLAY    0x002fe1d0u
#define PS_BLINK   0x0061feebu               /* the game's "blink" */
#define PS_GROUP   "ingame_HUD_PoliceStars"
#define PS_N       5

typedef void (*ps_play_fn)(void *ui, const char *path, const char *label, int arg);
static const char *const g_snames[PS_N] = { "stars0", "stars1", "stars2", "stars3", "stars4" };
static int g_son;

__attribute__((cdecl, force_align_arg_pointer))
static void ps_refresh(unsigned char *hud)
{
    hb_setvis_fn setvis = (hb_setvis_fn)(uintptr_t)HB_SETVIS;
    char path[64];
    int i = 0;
    if (GMV_DEV)
        g_sst.updates++;
    for (; i < PS_N; i++) {
        snprintf(path, sizeof path, PS_GROUP ".stars%d", i);
        if (*(const int *)(hud + 0x14) < i + 1)
            break;
        if (hud[0x13]) {
            setvis(*(void **)(hud + 8), PS_GROUP, 1);
        } else {
            setvis(*(void **)(hud + 8), path, 1);
            ((ps_play_fn)(uintptr_t)PS_PLAY)(*(void **)(hud + 8), path, (const char *)(uintptr_t)PS_BLINK, 0);
        }
    }
    if (i == PS_N)
        return;
    /* stars i..4 are hidden */
    void *ui = *(void **)(hud + 8);
    int n = PS_N - i;
    if (InterlockedCompareExchange(&g_busy, 1, 0) == 0) {
        unsigned char *res[PS_N];
        int ok = hb_walk(*(unsigned char **)((char *)ui + 0x40), PS_GROUP, n, g_snames + i, res);
        if (GMV_DEV)
            g_sst.walked += g_lastwalk;
        if (ok && GMV_DEV && g_verify) {
            for (int j = 0; j < n; j++) {
                snprintf(path, sizeof path, PS_GROUP ".stars%d", i + j);
                unsigned char *o = ((hb_find_fn)(uintptr_t)HB_FIND)(ui, path);
                if (o != res[j] && g_mismatch++ < 20)
                    gmv_log("[hudbars] MISMATCH #%u: \"%s\" original %p, one walk %p\n", g_mismatch, path,
                            (void *)o, (void *)res[j]);
                res[j] = o;
            }
        }
        InterlockedExchange(&g_busy, 0);
        if (ok) {
            if (GMV_DEV)
                g_sst.fast++;
            for (int j = 0; j < n; j++)
                if (res[j])
                    res[j][0xab] = 0;
            return;
        }
        if (GMV_DEV)
            g_sst.fallback++;
    } else if (GMV_DEV) {
        g_sst.busy++;
    }
    for (; i < PS_N; i++) {
        snprintf(path, sizeof path, PS_GROUP ".stars%d", i);
        setvis(*(void **)(hud + 8), path, 0);
    }
}

#if GMV_DEV
/* One line every 5 s with the PERF_PROFILE summary (dev build). */
static void hb_report1(const char *what, const char *unit, hb_stats *s, double seconds)
{
    if (!s->updates)
        return;
    gmv_logfile("[perf]   %s: %.0f %s a second, %u by one walk (%.0f entries each), %u by the original "
                "calls (%u not found from the first root, %u another thread's)%s%s\n",
                what, s->updates / seconds, unit, s->fast,
                (double)s->walked / (s->fast + s->fallback ? s->fast + s->fallback : 1),
                s->fallback + s->busy, s->fallback, s->busy,
                g_verify ? ", verified against the original: " : "",
                g_verify ? (g_mismatch ? "MISMATCHES (see above)" : "all identical") : "");
    memset(s, 0, sizeof *s);
}

void gmv_hudbars_report(double seconds)
{
    if (g_on)
        hb_report1("hud bars", "updates", &g_st, seconds);
    if (g_son)
        hb_report1("police stars", "refreshes", &g_sst, seconds);
}
#endif

static int hb_call_to(uint32_t call, uint32_t to)
{
    int32_t rel;
    memcpy(&rel, (const void *)(uintptr_t)(call + 1), 4);
    return *(const unsigned char *)(uintptr_t)call == 0xe8 && call + 5 + (uint32_t)rel == to;
}

static int hb_check(uint32_t fn, uint32_t loop_cmp, const unsigned char *cmp3, uint32_t call)
{
    return !memcmp((const void *)(uintptr_t)fn, "\x55\x89\xe5\x53\x57\x56", 6)
        && !memcmp((const void *)(uintptr_t)loop_cmp, cmp3, 3) && hb_call_to(call, HB_SETVIS);
}

/* The police stars function as read: loop to 5, the fields +0x13/+0x14, its calls and strings. */
static int ps_check(void)
{
    const unsigned char *p = (const unsigned char *)(uintptr_t)0x00180458u;
    return hb_check(PS_REFRESH, 0x00180500u, (const unsigned char *)"\x83\xfe\x05", 0x001804abu)
        && hb_call_to(0x001804fbu, HB_SETVIS) && hb_call_to(0x001804ccu, PS_PLAY)
        && !memcmp(p, "\x80\x79\x13\x00\x8b\x59\x14", 7)
        && !memcmp((const void *)(uintptr_t)0x0061fecc, PS_GROUP ".stars%d", sizeof PS_GROUP ".stars%d")
        && !memcmp((const void *)(uintptr_t)0x0061eab8, PS_GROUP, sizeof PS_GROUP)
        && !memcmp((const void *)(uintptr_t)PS_BLINK, "blink", 6);
}

static int hb_jmp(uint32_t fn, void *to)
{
    unsigned char j[5] = { 0xe9 };
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)to - (fn + 5));
    memcpy(j + 1, &rel, 4);
    DWORD old;
    if (!VirtualProtect((void *)(uintptr_t)fn, 5, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy((void *)(uintptr_t)fn, j, 5);
    VirtualProtect((void *)(uintptr_t)fn, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)(uintptr_t)fn, 5);
    return 1;
}

void gmv_apply_hudbars(void)
{
    const char *e = gmv_dbgenv("GMV_FAST_HUDBARS");
    if (e && *e == '0') {
        gmv_log("[hudbars] FAST_HUDBARS=0: the original bar and police star updates kept\n");
        return;
    }
    for (int i = 0; i < HB_N; i++) {
        snprintf(g_wbuf[i], sizeof g_wbuf[i], "w%d", i);
        g_wnames[i] = g_wbuf[i];
    }
    int bars = hb_check(HB_REFRESH, 0x0017e970u, (const unsigned char *)"\x83\xfe\x26", 0x0017e96au)
            && hb_check(HB_SET, 0x0017ea90u, (const unsigned char *)"\x83\xff\x26", 0x0017ea8au);
    int stars = ps_check();
    if (!bars)
        gmv_log("[hudbars] the HUD bar functions are not as expected; bars skipped\n");
    if (!stars)
        gmv_log("[hudbars] the police stars function is not as expected; stars skipped\n");
    if (!bars && !stars)
        return;
    g_qcap = 4096;
    g_q = malloc(g_qcap * sizeof *g_q);
    if (!g_q) {
        gmv_log("[hudbars] out of memory; skipped\n");
        return;
    }
    if (GMV_DEV) {
        const char *v = gmv_dbgenv("GMV_HUDBARS_VERIFY");
        g_verify = v && *v && *v != '0';
    }
    if (bars) {
        if (!hb_jmp(HB_REFRESH, (void *)hb_refresh) || !hb_jmp(HB_SET, (void *)hb_set)) {
            gmv_log("[hudbars] write failed (%lu); bars skipped\n", GetLastError());
        } else {
            g_on = 1;
            gmv_log("[hudbars] HUD bars updated by one UI walk (0x%06x, 0x%06x)\n", HB_REFRESH, HB_SET);
        }
    }
    if (stars) {
        if (!hb_jmp(PS_REFRESH, (void *)ps_refresh)) {
            gmv_log("[hudbars] write failed (%lu); stars skipped\n", GetLastError());
        } else {
            g_son = 1;
            gmv_log("[hudbars] hidden police stars set by one UI walk (0x%06x)\n", PS_REFRESH);
        }
    }
    if (g_verify && (g_on || g_son))
        gmv_log("[hudbars] HUDBARS_VERIFY: every walk checked against the original search\n");
}
