/* diag_loop.c -- diagnostic: the per-frame tick */
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

/* ---- diagnostic: capture the per-frame TICK function (GMV_LOOP_TRACE=1) ------------------------

   The main run loop is `while ([this+5]) this->vtbl[0x24]()` at 0x594ee0 -- vtbl[0x24] is the
   current state's per-frame tick (for gameplay: the function that does update(dt) + render).  To
   learn that tick's address and its phase layout, hook the
   loop entry once and log this, its vtable, and the vtbl entries around the render/update phases
   (0x1c render, 0x20, 0x24 tick) so the tick can be disassembled statically.  Pure diagnostic. */
#define FRAMELOOP_FUNC 0x00594ee0u

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_frametick_log(void *thisp)
{
    static int done;
    if (done || !thisp || IsBadReadPtr(thisp, 4)) return;
    void **vt = *(void ***)thisp;
    if (IsBadReadPtr(vt, 0x40)) return;
    done = 1;
    gmv_log("[loop] frameloop this=%p vtbl=%p  vtbl[0x1c]=%p vtbl[0x20]=%p vtbl[0x24]=%p vtbl[0x28]=%p\n",
            thisp, (void *)vt, vt[0x1c/4], vt[0x20/4], vt[0x24/4], vt[0x28/4]);
}

void apply_frametick_probe(void)
{
    const char *e = gmv_dbgenv("GMV_LOOP_TRACE");
    if (!(e && *e && *e != '0'))
        return;
    static const unsigned char expect[7] = { 0x55, 0x89, 0xe5, 0x53, 0x83, 0xec, 0x14 };
    if (memcmp((const void *)(uintptr_t)FRAMELOOP_FUNC, expect, sizeof expect) != 0) {
        gmv_log("[gamepatch] frameloop 0x%08x not the expected bytes; probe skipped\n", FRAMELOOP_FUNC);
        return;
    }
    unsigned char *tr = VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return;
    size_t o = 0;
    tr[o++] = 0xff; tr[o++] = 0x74; tr[o++] = 0x24; tr[o++] = 0x04;  /* push dword [esp+4] this (arg0) */
    tr[o++] = 0xb8; { uint32_t fnp = (uint32_t)(uintptr_t)&gmv_frametick_log; memcpy(tr + o, &fnp, 4); o += 4; }
    tr[o++] = 0xff; tr[o++] = 0xd0;                                 /* call eax                        */
    tr[o++] = 0x83; tr[o++] = 0xc4; tr[o++] = 0x04;                 /* add esp,4                       */
    tr[o++] = 0x55; tr[o++] = 0x89; tr[o++] = 0xe5; tr[o++] = 0x53; /* displaced 7: push ebp;mov;push ebx */
    tr[o++] = 0x83; tr[o++] = 0xec; tr[o++] = 0x14;                 /* sub esp,0x14                    */
    tr[o++] = 0xff; tr[o++] = 0x25; size_t contref = o; o += 4;
    size_t contslot = o;
    { uint32_t c = FRAMELOOP_FUNC + 7; memcpy(tr + o, &c, 4); o += 4; }
    { uint32_t a = (uint32_t)(uintptr_t)(tr + contslot); memcpy(tr + contref, &a, 4); }
    FlushInstructionCache(GetCurrentProcess(), tr, o);
    unsigned char site[7];
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)tr - (FRAMELOOP_FUNC + 5));
    site[0] = 0xe9; memcpy(site + 1, &rel, 4); site[5] = 0x90; site[6] = 0x90;
    if (!patch_bytes(FRAMELOOP_FUNC, site, sizeof site)) {
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); frametick probe skipped\n", FRAMELOOP_FUNC, GetLastError());
        return;
    }
    gmv_log("[gamepatch] frame-tick probe installed at 0x%08x (trampoline @ %p)\n", FRAMELOOP_FUNC, (void *)tr);
}
