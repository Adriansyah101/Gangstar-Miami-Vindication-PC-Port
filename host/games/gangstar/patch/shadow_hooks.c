/* shadow_hooks.c -- DYNAMIC_SHADOWS: the game side of the shadows */
#include "compat.h"
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

#define SM_TOD_SITE   0x001420d3u   /* call 0x142892 (time-of-day update; arg: the time object) */
#define SM_BB_VT_RENDER    0x00676a90u
#define SM_BB_VT_THUNK     0x00676ae8u

/* ---- DYNAMIC_SHADOWS: the game's own shadows (the GL shim draws the new ones) ----
   A shadow-node is a mesh of the model, the same whatever the hour: a vehicle's at +0x270 (found by 0x11455a),
   a character's at +0x2f0 (0x74aea, on this + 0x2e8).  Each update (0x112bc4, 0x76090) sets its visibility each
   tick, while it is visible, from the profile's flag and gTweakEnableShadow; both tests now go to gmv_ds_node.
   While the new shadows run, the node is hidden and kept in a list; once they stop, the list's nodes are shown
   again.  A node on the list may have gone with its owner since, so the owner must still be one the world or a
   pool holds, and the node's page and vtable are checked too (a model can be changed under a living owner).
   Those updates also say what casts: every scene node under the owner's own (obj+0x10) goes in a table, which the
   draws look up by the node being drawn (gmv_ds_caster).  Each table lives DS_TREE_FRAMES and the last one still
   counts, so a part hung on later is at most that late and an owner updated less often does not flicker. */
#define DS_TREE_N      65536u        /* a power of two */
#define DS_TREE_MAX    32768
#define DS_TREE_FRAMES 30u
#define DS_KIDS        0xe0          /* a node's children: see SF_KIDS */
#define DS_NODE_CAR    0x00112c3au
#define DS_NODE_PERSON 0x000775dbu
#define DS_NODES       1024
#define DS_PROFILE_PP  0x0063dfbcu   /* -> the profile; +8: the game's shadows */
#define DS_TWEAK       0x00641120u   /* gTweakEnableShadow */
static struct { unsigned char *n; void *vt; uint32_t owner; int copy; } ds_nodes[DS_NODES];
static int ds_nodes_n, ds_nodes_at;

void ds_show_all(void)
{
    int shown = 0, gone = 0, copies = 0, no_mem = 0, other = 0;
    for (int i = 0; i < ds_nodes_n; i++) {
        unsigned char *n = ds_nodes[i].n;
        MEMORY_BASIC_INFORMATION mbi;
        if (!pd_object_known(ds_nodes[i].owner)) {
            gone++;
            copies += ds_nodes[i].copy;
        } else if (!VirtualQuery(n, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            no_mem++;
        } else if (*(void **)n != ds_nodes[i].vt) {
            other++;
        } else {
            ((sm_vfn_i)sm_vt(n)[0x48 / 4])(n, 1);
            shown++;
        }
    }
    if (shown < ds_nodes_n)
        gmv_trace("[shadow] the game's shadow back on %d of %d nodes; the rest: %d owners no longer in the world or a pool (%d of "
                "them our copies, the pool's let go), %d with no memory left, %d no longer the same node\n",
                shown, ds_nodes_n, gone, copies, no_mem, other);
    else if (ds_nodes_n)
        gmv_trace("[shadow] the game's shadow back on all %d nodes\n", ds_nodes_n);
    ds_nodes_n = ds_nodes_at = 0;
}

static struct { uint32_t node, gen; } ds_tree[2][DS_TREE_N];
static uint32_t ds_tree_gen = 1;
static unsigned ds_tree_frame;
static int ds_tree_cur, ds_tree_used;

/* Whether the node is there; if not and add is set, it is put there. */
int ds_tree_has(int t, uint32_t node, int add)
{
    uint32_t gen = ds_tree_gen - (uint32_t)(t != ds_tree_cur);
    for (uint32_t h = (node >> 4) * 2654435761u, i = 0; i < DS_TREE_N; i++, h++) {
        h &= DS_TREE_N - 1;
        if (ds_tree[t][h].gen != gen) {
            if (add) {
                ds_tree[t][h].node = node, ds_tree[t][h].gen = gen;
                ds_tree_used++;
            }
            return 0;
        }
        if (ds_tree[t][h].node == node)
            return 1;
    }
    return 0;
}

static void ds_tree_walk(uint32_t node, int depth)
{
    if (!node || depth > 16 || ds_tree_used >= DS_TREE_MAX)
        return;
    ds_tree_has(ds_tree_cur, node, 1);
    uint32_t head = node + DS_KIDS;
    int n = 0;
    for (uint32_t l = *(const uint32_t *)(uintptr_t)head; l && l != head && n < 256; l = *(const uint32_t *)(uintptr_t)l, n++)
        ds_tree_walk(l - 4, depth + 1);
}

static void ds_tree_owner(uint32_t obj)
{
    unsigned frame = gmv_gl_frame_no();
    if (frame - ds_tree_frame >= DS_TREE_FRAMES) {
        ds_tree_frame = frame;
        ds_tree_gen++;                              /* so the old table's entries are its gen - 1, as has() reads them */
        ds_tree_cur ^= 1;
        ds_tree_used = 0;
    }
    uint32_t root = *(const uint32_t *)(uintptr_t)(obj + 0x10);
    if (root && !ds_tree_has(ds_tree_cur, root, 0))
        ds_tree_walk(root, 0);
}

__attribute__((cdecl, force_align_arg_pointer))
void gmv_ds_node(unsigned char *n, uint32_t obj)
{
    int vis = n[0x108] & 1;
    if (!gmv_ds_active()) {
        if (ds_nodes_n)
            ds_show_all();
        if (vis) {
            int on = (*(unsigned char **)(uintptr_t)DS_PROFILE_PP)[8] && *(unsigned char *)(uintptr_t)DS_TWEAK;
            ((sm_vfn_i)sm_vt(n)[0x48 / 4])(n, on);
        }
        return;
    }
    ds_tree_owner(obj);
    if (!vis)
        return;
    ((sm_vfn_i)sm_vt(n)[0x48 / 4])(n, 0);
    for (int i = 0; i < ds_nodes_n; i++)
        if (ds_nodes[i].n == n) {
            ds_nodes[i].vt = *(void **)n;           /* the game showed it again, or it is a new node in the old one's place */
            ds_nodes[i].owner = obj;
            ds_nodes[i].copy = pd_is_extra(obj);
            return;
        }
    ds_nodes[ds_nodes_at].n = n;
    ds_nodes[ds_nodes_at].vt = *(void **)n;
    ds_nodes[ds_nodes_at].owner = obj;
    ds_nodes[ds_nodes_at].copy = pd_is_extra(obj);
    ds_nodes_at = (ds_nodes_at + 1) % DS_NODES;
    if (ds_nodes_n < DS_NODES)
        ds_nodes_n++;
}

/* eax = the node, esi = its owner; the rest of the test at the site is skipped. */
__attribute__((naked)) static void ds_node_stub(void)
{
    __asm__("pushl %esi\n\tpushl %eax\n\tcall " GMV_ASM_SYM(gmv_ds_node) "\n\taddl $8, %esp\n\tret");
}

/* testb $1,0x108(%eax); je +0x3a: becomes the call and a jump to that same place. */
static void ds_node_hook(uint32_t site)
{
    static const unsigned char want[] = {0xf6, 0x80, 0x08, 0x01, 0x00, 0x00, 0x01, 0x74, 0x3a};
    if (memcmp((const void *)(uintptr_t)site, want, sizeof want)) {
        gmv_log("[gamepatch] shadow test at 0x%08x not the expected bytes; the game's shadows stay there\n", site);
        return;
    }
    unsigned char b[7] = {0xe8, 0, 0, 0, 0, 0xeb, 0x3c};
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)ds_node_stub - (site + 5));
    memcpy(b + 1, &rel, 4);
    patch_bytes(site, b, sizeof b);
}

void apply_sun_moon(void)
{
    const char *e = gmv_getenv("GMV_SUN_MOON"), *ds = gmv_getenv("GMV_DYNAMIC_SHADOWS");
    int ds_key = ds && *ds && *ds != '0';
    sm_mode_set(!e || !*e || *e == '0' ? 0 : *e == '2' ? 2 : 1);
    if (gmv_config_mac())
        return;                                 /* the Mac's sky: the hooks come with the switch */
    if (tab_live() || ds_key) {
        ds_node_hook(DS_NODE_CAR);
        ds_node_hook(DS_NODE_PERSON);
    }
    if (!g_sm_on && !tab_live() && !ds_key) {
        if (!bloom_windows_on())
            return;
        if (!sm_is_call(SM_TOD_SITE, SM_TOD_FN))
            gmv_log("[gamepatch] time-of-day call not the expected bytes; BLOOM_WINDOWS has no clock\n");
        else if (sm_redirect(SM_TOD_SITE, (void *)gmv_sun_moon_tod))
            gmv_log("[gamepatch] the game's clock read for BLOOM_WINDOWS\n");
        return;
    }
    if (!sm_is_call(SM_TOD_SITE, SM_TOD_FN) || !sm_is_call(sm_track_sites[0], SM_TRACK_FN) ||
        !sm_is_call(sm_track_sites[1], SM_TRACK_FN)) {
        gmv_log("[gamepatch] time-of-day calls not the expected bytes; sun and moon skipped\n");
        return;
    }
    if (!sm_redirect(sm_track_sites[0], (void *)gmv_sun_moon_track) ||
        !sm_redirect(sm_track_sites[1], (void *)gmv_sun_moon_track) ||
        !sm_redirect(SM_TOD_SITE, (void *)gmv_sun_moon_tod))
        return;
    g_sm_hooked = 1;
    sm_discs_behind_farcity();
    uint32_t r = (uint32_t)(uintptr_t)gmv_sun_moon_render, t = (uint32_t)(uintptr_t)gmv_sun_moon_render_sn;
    int glows = *(uint32_t *)(uintptr_t)SM_BB_VT_RENDER == SM_BB_RENDER &&
                *(uint32_t *)(uintptr_t)SM_BB_VT_THUNK == SM_BB_RENDER_THUNK &&
                patch_bytes(SM_BB_VT_RENDER, (const unsigned char *)&r, 4) &&
                patch_bytes(SM_BB_VT_THUNK, (const unsigned char *)&t, 4);
    gmv_log("[gamepatch] sun and moon %s%s (SUN_MOON=0, 1 or 2: the Mac's, enhanced, the iPhone's)\n",
            g_sky_glow_iphone ? "in the sky as on the iPhone" : g_sm_on ? "in the sky, enhanced"
                              : "ready for the options menu, off for now",
            glows ? "" : "; billboard render not the expected entries, so the glows show through the world");
}
