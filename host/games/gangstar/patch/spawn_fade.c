/* spawn_fade.c -- SPAWN_FADE */
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

/* ---- SPAWN_FADE: the traffic and pedestrians fade in and out (an enhancement, off by default and
   under ORIGINAL_MAC_EXPERIENCE) ----
   Coming: the spawner (0x082504, called at 0x082bd1) takes its object from the spawn pool (see the
   densities above), so the slot it empties names the object, which then fades in over SF_MS.
   Going: the despawn check 0x1a8f6a (each frame, for each object with flags 0x890) takes an object
   out of the world by calling 0x020ed0 at 0x1a93e8, whose answer is the next object to look at; an
   answer of the object itself means it stays (the check's other ways out answer that).  So that call
   first starts the object fading out and answers the object until it has gone; the call after that
   takes it out.  If the check stops asking (the object came back in range), it fades back in.
   Drawing: glitch's CSceneManager draws its lists of scene nodes in renderList<...> and
   renderListAutoBatching<...> (0x0b84c0, 0x0b7f40, 0x0b8a90, 0x0b91cc, 0x0b96a8, reached through the
   jumps at 0x5eda02 + 6k), which put the node being drawn at the manager's +0x9c before calling its
   render (vfunc 0x1c).  A draw whose node hangs under a fading object's own node (obj+0x10, its lights
   included) takes that object's fade.  Otherwise, while a list is being drawn, a draw whose node stands where a fading object
   stands (the node's absolute transformation +0x10, translation at +0x40, against the object's
   own node, obj+0x10) is drawn through shimsgl's dot pattern.  A vehicle's parts and its driver lie
   within SF_CAR of it, a person's within SF_PERSON. */
#define SF_MS          1000.0
#define SF_N           256
#define SF_CAR         250.0f     /* wheels and driver; not much more, or a car parked alongside fades too */
#define SF_PERSON      120.0f
#define SF_LET_GO_MS   250.0      /* not asked to go for this long: it stays, so it comes back */
#define SF_REMOVE_SITE 0x001a93e8u
#define SF_REMOVE_FN   0x00020ed0u
#define SF_CATS        0x0063e174u   /* -> the type -> category table (8 = a person) */
#define SF_STUB0       0x005eda02u
#define SF_STUB_PTR0   0x0063ec78u
#define SF_BATCH_LIMIT 0xdc          /* driver: the biggest node the batching lists gather */
static const uint32_t sf_lists[5] = { 0x000b84c0u, 0x000b7f40u, 0x000b8a90u, 0x000b91ccu, 0x000b96a8u };

typedef struct {
    uint32_t obj;
    int out;                  /* fading out */
    double t0, seen;          /* when the fade (re)started; when the despawn check last asked */
    float a0, a;              /* how far shown at t0; now */
    float x, y, z, r;         /* where its node stands; how near a drawn node must be */
    int alive;                /* found in the world's lists at the last update */
    int list;                 /* the world list it was in when its fade began (0-2), -1 = none of them */
} sf_ent;
static sf_ent sf_e[SF_N];
int sf_n, sf_live, sf_depth, sf_lost;
static const uint32_t sf_wlists[3] = {0x418, 0x424, 0x430};   /* the world's object lists */
static int sf_wlen[3];            /* for the log: the longest each was when walked */
static uint32_t sf_mgr[8];
static int sf_k[8];
static uint32_t sf_snap[PD_POOL_N];
static int sf_made, sf_gone;      /* for the log */
static int sf_add_hit, sf_add_tree, sf_add_near, sf_add_out, sf_add_k[5];   /* additive draws (lights) while something fades */

/* Every scene node under a fading object's own node (obj+0x10), with the object's alpha.  The car's
   lights are models hung on its dummy_light / dummy_headlight nodes (0x115b6a), standing too far from
   the car's middle to be caught by SF_CAR.  A node's children: a list whose head is at node+0xe0, each
   child linked through its +4 (as 0x4c0c30, the find-by-name, walks it). */
#define SF_TREE_N     16384           /* a power of two */
#define SF_TREE_MAX   8192            /* nodes gathered at most (~120 per car, 25 fading = 3000 seen) */
#define SF_KIDS       0xe0
static struct { uint32_t node, gen; float a; } sf_tree[SF_TREE_N];
static uint32_t sf_gen;
static int sf_tree_used;

static void sf_tree_put(uint32_t node, float a)
{
    for (uint32_t h = (node >> 4) * 2654435761u, i = 0; i < SF_TREE_N; i++, h++) {
        h &= SF_TREE_N - 1;
        if (sf_tree[h].gen != sf_gen) {
            sf_tree[h].node = node, sf_tree[h].gen = sf_gen, sf_tree[h].a = a;
            sf_tree_used++;
            return;
        }
        if (sf_tree[h].node == node) {
            if (a < sf_tree[h].a)
                sf_tree[h].a = a;
            return;
        }
    }
}

static int sf_tree_get(uint32_t node, float *a)
{
    for (uint32_t h = (node >> 4) * 2654435761u, i = 0; i < SF_TREE_N; i++, h++) {
        h &= SF_TREE_N - 1;
        if (sf_tree[h].gen != sf_gen)
            return 0;
        if (sf_tree[h].node == node) {
            *a = sf_tree[h].a;
            return 1;
        }
    }
    return 0;
}

static void sf_tree_walk(uint32_t node, float a, int depth)
{
    if (!node || depth > 16 || sf_tree_used >= SF_TREE_MAX)
        return;
    sf_tree_put(node, a);
    uint32_t head = node + SF_KIDS;
    int n = 0;
    for (uint32_t l = *(const uint32_t *)(uintptr_t)head; l && l != head && n < 256; l = *(const uint32_t *)(uintptr_t)l, n++)
        sf_tree_walk(l - 4, a, depth + 1);
}

static double sf_now(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

int sf_menu = -1;           /* the options menu's FADE IN TRAFFIC, -1 = not touched */

static int sf_setting(void)
{
    if (sf_menu >= 0)
        return sf_menu;
    const char *e = gmv_getenv("GMV_SPAWN_FADE");
    return e && *e && *e != '0';
}

static float sf_alpha(const sf_ent *e, double now)
{
    double a = e->a0 + (e->out ? -1 : 1) * (now - e->t0) / SF_MS;
    return (float)(a < 0 ? 0 : a > 1 ? 1 : a);
}

static sf_ent *sf_find(uint32_t obj)
{
    for (int i = 0; i < sf_n; i++)
        if (sf_e[i].obj == obj)
            return &sf_e[i];
    return NULL;
}

static void sf_drop(sf_ent *e)
{
    *e = sf_e[--sf_n];
}

static void sf_place(sf_ent *e)
{
    uint32_t node = *(const uint32_t *)(uintptr_t)(e->obj + 0x10);
    const uint8_t *cats = (const uint8_t *)(uintptr_t)*(const uint32_t *)(uintptr_t)SF_CATS;
    if (node) {
        const float *t = (const float *)(uintptr_t)(node + 0x40);
        e->x = t[0], e->y = t[1], e->z = t[2];
    }
    e->r = cats && cats[*(const uint8_t *)(uintptr_t)(e->obj + 0x24)] == 8 ? SF_PERSON : SF_CAR;
}

/* Start obj fading (out = 1) from wherever it stands now, or from a0 if it isn't fading yet */
static sf_ent *sf_start(uint32_t obj, int out, float a0, double now)
{
    sf_ent *e = sf_find(obj);
    if (e) {
        if (e->out != out)
            e->a0 = sf_alpha(e, now), e->t0 = now, e->out = out;
        return e;
    }
    if (sf_n == SF_N)
        return NULL;
    e = &sf_e[sf_n++];
    memset(e, 0, sizeof *e);
    e->obj = obj, e->out = out, e->a0 = e->a = a0, e->t0 = e->seen = now;
    /* An object in a world list points back at it (+0x20, set by the list add 0x2056a) */
    uint32_t world = *(const uint32_t *)(uintptr_t)SV_WORLD, in = *(const uint32_t *)(uintptr_t)(obj + 0x20);
    e->list = -1;
    for (int l = 0; world && l < 3; l++)
        if (in == world + sf_wlists[l])
            e->list = l;
    sf_place(e);
    static int logged;
    if (logged < 3) {
        uint32_t node = *(const uint32_t *)(uintptr_t)(obj + 0x10);
        const float *t = node ? (const float *)(uintptr_t)(node + 0x40) : NULL, *p = node ? (const float *)(uintptr_t)(node + 0xa4) : NULL;
        logged++;
        gmv_trace("[spawnfade] %s %08x (vtable %08x, type %u): node %08x at %.0f %.0f %.0f (its own position %.0f %.0f %.0f)\n",
                out ? "going" : "coming", obj, *(const uint32_t *)(uintptr_t)obj, *(const uint8_t *)(uintptr_t)(obj + 0x24),
                node, t ? t[0] : 0, t ? t[1] : 0, t ? t[2] : 0, p ? p[0] : 0, p ? p[1] : 0, p ? p[2] : 0);
    }
    return e;
}

/* Once per list drawn: the fades' progress and where their objects stand */
static void sf_update(void)
{
    sf_live = sf_setting();
    if (!sf_live) {
        sf_n = 0;
        return;
    }
    /* An object can leave the world other ways than the despawn check (a mission's start clears the
       streets, a replay puts the world back): one no longer in any of the world's lists (+0x418, +0x424,
       +0x430, each a head at +4 linking objects through +0xc) is let go before it is read.  The world
       only changes between frames, so this runs at the frame's first list (it took ~2.7% of a tick
       when run at every list, still ~2.4% once a frame over all three: the lists are long); only the
       lists the fading objects were in are walked (one that moved to another list is let go too); a
       bit table keyed by the object's address skips most sf_find calls. */
    static unsigned walked = ~0u;
    unsigned frame = gmv_gl_frame_no();
    if (sf_n && frame != walked) {
        walked = frame;
        uint32_t world = *(const uint32_t *)(uintptr_t)SV_WORLD;
        uint32_t bits[64] = {0};                        /* 2048 bits */
        unsigned want = 0;
        for (int i = 0; i < sf_n; i++) {
            uint32_t h = (sf_e[i].obj >> 3) & 2047;
            bits[h >> 5] |= 1u << (h & 31);
            sf_e[i].alive = 0;
            want |= sf_e[i].list < 0 ? 7u : 1u << sf_e[i].list;
        }
        for (int l = 0; world && l < 3; l++) {
            if (!(want & (1u << l)))
                continue;
            uint32_t o = *(const uint32_t *)(uintptr_t)(world + sf_wlists[l] + 4);
            int n = 0;
            for (; o && n < 8192; o = *(const uint32_t *)(uintptr_t)(o + 0xc), n++) {
                uint32_t h = (o >> 3) & 2047;
                if (!(bits[h >> 5] & (1u << (h & 31))))
                    continue;
                sf_ent *e = sf_find(o);
                if (e)
                    e->alive = 1;
            }
            if (n > sf_wlen[l])
                sf_wlen[l] = n;
        }
        for (int i = 0; i < sf_n; i++)
            if (!sf_e[i].alive) {
                sf_lost++;
                sf_drop(&sf_e[i]), i--;
            }
    }
    double now = sf_now();
    for (int i = 0; i < sf_n; i++) {
        sf_ent *e = &sf_e[i];
        if (e->out && now - e->seen > SF_LET_GO_MS)
            e->a0 = sf_alpha(e, now), e->t0 = now, e->out = 0;
        e->a = sf_alpha(e, now);
        if (!e->out && e->a >= 1) {
            sf_drop(e), i--;
            continue;
        }
        sf_place(e);
    }
    if (sf_depth == 0) {
        sf_gen++;
        sf_tree_used = 0;
        for (int i = 0; i < sf_n; i++)
            sf_tree_walk(*(const uint32_t *)(uintptr_t)(sf_e[i].obj + 0x10), sf_e[i].a, 0);
    }
}

static uint32_t *sf_pool(int *n)
{
    *n = pd_pool_on ? PD_POOL_N : 256;
    return pd_pool_on ? pd_pool : (uint32_t *)(uintptr_t)PD_POOL_OLD;
}

void sf_before_make(void)
{
    if (!sf_live)
        return;
    int n;
    const uint32_t *p = sf_pool(&n);
    memcpy(sf_snap, p, n * sizeof *p);
}

void sf_after_make(void)
{
    if (!sf_live)
        return;
    int n;
    const uint32_t *p = sf_pool(&n);
    double now = sf_now();
    for (int i = 0; i < n; i++)
        if (sf_snap[i] && !p[i] && sf_start(sf_snap[i], 0, 0, now))
            sf_made++;
}

__attribute__((cdecl, force_align_arg_pointer, noinline))
static uint32_t gmv_sf_remove(uint32_t world, uint32_t obj)
{
    sf_ent *e = sf_find(obj);
    if (sf_live) {
        double now = sf_now();
        if (!e && (e = sf_start(obj, 1, 1, now)) != NULL)
            sf_gone++;
        else if (e && !e->out)
            sf_start(obj, 1, 1, now);
        if (e) {
            e->seen = now;
            if (sf_alpha(e, now) > 0)
                return obj;                     /* stays until it has faded out */
        }
    }
    if (e)
        sf_drop(e);
    return ((uint32_t (*)(uint32_t, uint32_t))(uintptr_t)SF_REMOVE_FN)(world, obj);
}

__attribute__((cdecl, force_align_arg_pointer, noinline))
static void sf_list(int k, uint32_t mgr, uint32_t pass, uint32_t list, uint32_t a, uint32_t b)
{
    sf_update();
    if (sf_depth < 8)
        sf_mgr[sf_depth] = mgr, sf_k[sf_depth] = k;
    sf_depth++;
    /* The batching lists (k 3, 4) let the driver gather nodes that share a material (all the cars'
       lights) into one draw, sent while a later node is current.  A node whose size (vfunc 0x20) is
       over the driver's +0xdc is drawn on its own, so while anything fades every node is "too big". */
    int32_t *lim = NULL, keep = 0;
    uint32_t drv = *(const uint32_t *)(uintptr_t)(mgr + 0x14);
    if (k >= 3 && sf_n && drv) {
        lim = (int32_t *)(uintptr_t)(drv + SF_BATCH_LIMIT);
        keep = *lim;
        *lim = -1;
    }
    ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))(uintptr_t)sf_lists[k])(mgr, pass, list, a, b);
    if (lim)
        *lim = keep;
    sf_depth--;
    static double next;
    double now = sf_now();
    if (now >= next && (sf_made || sf_gone)) {
        gmv_trace("[spawnfade] %d faded in, %d faded out in the last 30 s; %d fading now; additive draws while "
                "fading: %d in a fading object's tree, %d dotted by distance, %d near one but not, %d outside the "
                "lists, by list %d %d %d %d %d; tree nodes now %d; %d left the world some other way; world lists "
                "walked up to %d / %d / %d long\n",
                sf_made, sf_gone, sf_n, sf_add_tree, sf_add_hit, sf_add_near, sf_add_out, sf_add_k[0], sf_add_k[1],
                sf_add_k[2], sf_add_k[3], sf_add_k[4], sf_tree_used, sf_lost, sf_wlen[0], sf_wlen[1], sf_wlen[2]);
        sf_made = sf_gone = sf_lost = sf_add_tree = sf_add_hit = sf_add_near = sf_add_out = 0;
        memset(sf_wlen, 0, sizeof sf_wlen);
        memset(sf_add_k, 0, sizeof sf_add_k);
        next = now + 30000;
    }
}

#define SF_WRAP(k) \
    __attribute__((cdecl, force_align_arg_pointer)) \
    static void gmv_sf_list##k(uint32_t mgr, uint32_t pass, uint32_t list, uint32_t a, uint32_t b) \
    { sf_list(k, mgr, pass, list, a, b); }
SF_WRAP(0) SF_WRAP(1) SF_WRAP(2) SF_WRAP(3) SF_WRAP(4)

int gmv_spawn_fade_level(int add)
{
    if (!sf_n)
        return 64;
    if (!sf_depth || sf_depth > 8) {
        if (add)
            sf_add_out++;
        return 64;
    }
    uint32_t node = *(const uint32_t *)(uintptr_t)(sf_mgr[sf_depth - 1] + 0x9c);
    if (!node)
        return 64;
    float a = 1, near2 = 1e30f;
    if (sf_tree_get(node, &a)) {
        if (add) {
            if (sf_k[sf_depth - 1] < 5)
                sf_add_k[sf_k[sf_depth - 1]]++;
            sf_add_tree++;
        }
        return a >= 1 ? 64 : (int)(a * 64);
    }
    const float *t = (const float *)(uintptr_t)(node + 0x40);
    for (int i = 0; i < sf_n; i++) {
        const sf_ent *e = &sf_e[i];
        float dx = t[0] - e->x, dy = t[1] - e->y, dz = t[2] - e->z, d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < e->r * e->r && e->a < a)
            a = e->a;
        if (d2 < near2)
            near2 = d2;
    }
    if (add) {
        if (sf_k[sf_depth - 1] < 5)
            sf_add_k[sf_k[sf_depth - 1]]++;
        if (a < 1)
            sf_add_hit++;
        else if (near2 < 3 * SF_CAR * 3 * SF_CAR)
            sf_add_near++;
    }
    return a >= 1 ? 64 : (int)(a * 64);
}

/* DYNAMIC_SHADOWS: the absolute transformation of the node being drawn, if it belongs to a vehicle or a person. */
const float *gmv_ds_caster(void)
{
    if (!sf_depth || sf_depth > 8)
        return NULL;
    uint32_t node = *(const uint32_t *)(uintptr_t)(sf_mgr[sf_depth - 1] + 0x9c);
    if (!node || !(ds_tree_has(0, node, 0) || ds_tree_has(1, node, 0)))
        return NULL;
    return (const float *)(uintptr_t)(node + 0x10);
}

/* The options menu's FADE IN TRAFFIC: 1 when put in force, 0 when the hooks are not in.  Going off,
   sf_update lets go of the fades at the next list drawn and the despawn check removes as before. */
int spawn_fade_set(int on)
{
    if (!sf_hooked)
        return 0;
    if (sf_setting() != on)
        gmv_log("[spawnfade] SPAWN_FADE %s\n", on ? "on" : "off");
    sf_menu = on;
    return 1;
}

/* With GAMEPATCH_TAB the hooks go in even while it is off, so the menu can switch it on; while off
   they pass every call straight through.  Under ORIGINAL_MAC_EXPERIENCE they are not in at all. */
void apply_spawn_fade(void)
{
    const char *ds = gmv_getenv("GMV_DYNAMIC_SHADOWS");     /* its casters are found through the same lists */
    if (gmv_config_mac() || (!sf_setting() && !tab_live() && !(ds && *ds && *ds != '0')))
        return;
    static void *const wraps[5] = { (void *)gmv_sf_list0, (void *)gmv_sf_list1, (void *)gmv_sf_list2,
                                    (void *)gmv_sf_list3, (void *)gmv_sf_list4 };
    int ok = sm_is_call(SF_REMOVE_SITE, SF_REMOVE_FN) &&
             (sm_is_call(PD_MAKE_SITE, PD_MAKE_FN) || sm_is_call(PD_MAKE_SITE, (uint32_t)(uintptr_t)gmv_spawn_make));
    for (int k = 0; k < 5 && ok; k++) {
        const unsigned char *s = (const unsigned char *)(uintptr_t)(SF_STUB0 + 6 * k);
        uint32_t ptr;
        memcpy(&ptr, s + 2, 4);
        ok = s[0] == 0xff && s[1] == 0x25 && ptr == SF_STUB_PTR0 + 4 * k;
    }
    if (!ok) {
        gmv_log("[spawnfade] spawner, despawn check or scene lists not the expected bytes; SPAWN_FADE skipped\n");
        return;
    }
    for (int k = 0; k < 5; k++) {
        unsigned char code[6] = { 0xe9, 0, 0, 0, 0, 0x90 };
        int32_t rel = (int32_t)((uint32_t)(uintptr_t)wraps[k] - (SF_STUB0 + 6 * k + 5));
        memcpy(code + 1, &rel, 4);
        if (!patch_bytes(SF_STUB0 + 6 * k, code, sizeof code)) {
            gmv_log("[spawnfade] write failed (%lu); SPAWN_FADE skipped\n", GetLastError());
            return;
        }
    }
    int32_t r = (int32_t)((uint32_t)(uintptr_t)gmv_sf_remove - (SF_REMOVE_SITE + 5));
    patch_bytes(SF_REMOVE_SITE + 1, (const unsigned char *)&r, 4);
    if (sm_is_call(PD_MAKE_SITE, PD_MAKE_FN)) {
        r = (int32_t)((uint32_t)(uintptr_t)gmv_spawn_make - (PD_MAKE_SITE + 5));
        patch_bytes(PD_MAKE_SITE + 1, (const unsigned char *)&r, 4);
    }
    sf_hooked = 1;
    sf_live = sf_setting();
    gmv_log("[spawnfade] SPAWN_FADE%s: cars and pedestrians fade in and out over %.0f ms\n",
            sf_live ? "=1" : " hooked (off for now)", SF_MS);
}

int mac_features_on, mac_out_pending, uc_loaded;
