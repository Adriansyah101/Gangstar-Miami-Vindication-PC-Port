/* patch_common.c -- what the patches share: writing to the game's code, and undoing it */
#include "compat.h"
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* Surgical correctness patches applied to the game's own __TEXT at load time.

   These are not enhancements or behavioural changes: each one repairs a latent assumption in
   the shipped i386 code that only holds on the original platform, by mirroring logic the engine
   already contains elsewhere.  Each patch sanity-checks the exact bytes it is about to replace,
   so it is a no-op on any binary that does not match, and each can be turned off with an env var. */

patch_undo *patch_undo_now;     /* the group being put in, if any */
patch_undo mac_undo;            /* every enhancement's hooks but the ones with groups of their own */

#if GMV_DEV
/* [macaudit]: what the game's code and tables held before any patch of ours (ma_snapshot), compared under
   ORIGINAL_MAC_EXPERIENCE (ma_tick).  Patches elsewhere, in its writable data, are noted here as they go in. */
ma_site_t ma_site[MA_SITES];
int ma_sites;

static void ma_note(uint32_t va, size_t n)
{
    if (ma_sites >= MA_SITES || n > sizeof ma_site[0].was || (va >= 0x11000u && va < 0x63d000u) ||
        (va >= 0x64ea40u && va < 0x6847b8u))
        return;
    for (int i = 0; i < ma_sites; i++)
        if (ma_site[i].va == va)
            return;
    ma_site[ma_sites].va = va, ma_site[ma_sites].n = (unsigned)n;
    memcpy(ma_site[ma_sites++].was, (const void *)(uintptr_t)va, n);
}
#endif

int patch_bytes(uint32_t va, const unsigned char *buf, size_t n)
{
    DWORD old;
    void *p = (void *)(uintptr_t)va;
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old))
        return 0;
#if GMV_DEV
    ma_note(va, n);
#endif
    if (patch_undo_now) {
        patch_undo *u = patch_undo_now;
        if (n <= sizeof u->e[0].was && u->n < PATCH_UNDO_MAX) {
            u->e[u->n].va = va, u->e[u->n].n = (unsigned)n;
            memcpy(u->e[u->n++].was, p, n);
        } else
            gmv_log("[gamepatch] patch at 0x%08x cannot be taken out again\n", va);
    }
    memcpy(p, buf, n);
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return 1;
}

/* The group out again, last patch first: the bytes are what they were before it went in. */
void patch_undo_all(patch_undo *u)
{
    patch_undo *keep = patch_undo_now;
    patch_undo_now = NULL;
    while (u->n > 0) {
        u->n--;
        patch_bytes(u->e[u->n].va, u->e[u->n].was, u->e[u->n].n);
    }
    patch_undo_now = keep;
}

/* Emit `jmp rel32` at tr[o], targeting absolute game VA `dst`; advance o past the 5 bytes. */
void emit_jmp(unsigned char *tr, size_t *o, uint32_t dst)
{
    int32_t rel = (int32_t)(dst - ((uint32_t)(uintptr_t)tr + (uint32_t)*o + 5));
    tr[(*o)++] = 0xe9;
    memcpy(tr + *o, &rel, 4);
    *o += 4;
}
