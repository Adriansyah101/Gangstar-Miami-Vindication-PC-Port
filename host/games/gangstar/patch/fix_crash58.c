/* fix_crash58.c -- the guard against the game's null-forward crash */
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

/* ---- crash guard: the recurring null-deref at 0x58c1ab (~13 min into play) ----

   The fault is `access violation (read 0x00000000) at eip 0x0058c1ab`.  The function at 0x58c190
   is a tiny vtable-forwarding adapter:

        f(this, a1, a2, a3):
            p = this->field4          ; mov edx,[ebx+4]      (ebx = this)
            p->vtbl[6](p, a2, 0)      ; mov ecx,[edx]  <-- FAULT when p == NULL
            p = this->field4          ; reload
            return p->vtbl[3](p, a3, a1)   ; tail-call

   i.e. it delegates two virtual calls to `this->field4`.  Intermittently, ~13 min in, that
   delegate pointer (this+4) is NULL and the `mov ecx,[edx]` reads *NULL.  The code cluster sits
   near the engine's animation/collada code and is reached through the pedestrian/NPC streaming
   path (the call chain passes through the region after Data::StressedMale::read), consistent with
   a streamed object whose delegate is torn down / not yet set in our port.

   This guard is a trampoline at the function entry that leaves the normal (non-NULL) path
   byte-for-byte identical -- it re-executes the exact prologue bytes it displaced and resumes at
   0x58c196 -- and ONLY intercepts the NULL case, which currently always crashes anyway: it logs
   the object (its own vtable so the class can be identified), the NULL field4, and the caller
   return address, then returns 0 (a benign "empty/false") instead of faulting.  So the common
   case cannot be made worse; the only changed behaviour is on the path that is 100% a crash today.
   PERMANENT (default on): since the NULL-field4 path is a 100% crash in the original, skipping the
   two delegate calls and returning 0 can only ever help -- it is strictly safer than the status quo
   and cannot make the common (non-NULL) path worse.  GMV_CRASH58_GUARD=0 opts OUT (restores the
   original faulting behaviour) for debugging/comparison; any value except "0" keeps it on. */
#define CR58_FUNC 0x0058c190u   /* adapter entry: push ebp; mov ebp,esp; sub esp,0x28; ...        */
#define CR58_CONT 0x0058c196u   /* resume point: first instruction after the 6 bytes we displace  */

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_crash58_log(void *thisp, void *caller, const uint32_t *args, const uint32_t *frame)
{
    const uint32_t *t = (const uint32_t *)thisp;
    uint32_t vtbl = (t && !IsBadReadPtr((void *)t, 4)) ? t[0] : 0;
    uint32_t f4   = (t && !IsBadReadPtr((void *)t, 8)) ? t[1] : 0;
    static unsigned n;
    if (n++ < 64 || (n % 512) == 0)
        gmv_log("[crash58] guarded null forward: this=%p vtbl=0x%08x field4=0x%08x caller=%p (n=%u)\n",
                thisp, vtbl, f4, caller, n);
#if !GMV_DEV
    (void)args;
    (void)frame;
#else
    if (n > 8)
        return;
    /* An animation chunk could not be read: field4 is the file the bank's root names (root+4).
       The caller's [ebp+0xc] is the database: *db+0x10 = the bank's image, image+0x20 = its root. */
    uint32_t db = 0, image = 0, root = 0, namep = 0, chars = 0;
    if (frame && !IsBadReadPtr((void *)frame, 0x10))
        db = frame[3];
    if (db && !IsBadReadPtr((void *)(uintptr_t)db, 4)) {
        uint32_t d = *(const uint32_t *)(uintptr_t)db;
        if (d && !IsBadReadPtr((void *)(uintptr_t)d, 0x14))
            image = *(const uint32_t *)(uintptr_t)(d + 0x10);
    }
    if (image && !IsBadReadPtr((void *)(uintptr_t)image, 0x24))
        root = *(const uint32_t *)(uintptr_t)(image + 0x20);
    if (root && !IsBadReadPtr((void *)(uintptr_t)root, 8))
        namep = *(const uint32_t *)(uintptr_t)(root + 4);
    if (namep && !IsBadReadPtr((void *)(uintptr_t)namep, 4))
        chars = *(const uint32_t *)(uintptr_t)namep;
    const char *fail;
    int err;
    long open_files;
    gmv_fopen_info(&fail, &err, &open_files);
    gmv_log("[crash58] chunk size 0x%x offset 0x%x, bank root 0x%08x name pointer 0x%08x \"%.200s\"\n",
            args ? args[2] : 0, args ? args[3] : 0, root, namep,
            chars && !IsBadReadPtr((void *)(uintptr_t)chars, 1) ? (const char *)(uintptr_t)chars : "");
    gmv_log("[crash58] open streams %ld of %d; last failed open errno %d \"%s\"\n",
            open_files, _getmaxstdio(), err, fail);
#endif
}

void apply_crash58_guard(void)
{
    const char *e = gmv_getenv("GMV_CRASH58_GUARD");
    if (e && *e == '0')
        return;                         /* explicit opt-out; otherwise the guard is on by default */
    /* Only patch a binary whose entry bytes match exactly (push ebp; mov ebp,esp; sub esp,0x28; ...). */
    static const unsigned char expect[8] = { 0x55, 0x89, 0xe5, 0x83, 0xec, 0x28, 0x89, 0x5d };
    if (memcmp((const void *)(uintptr_t)CR58_FUNC, expect, sizeof expect) != 0) {
        gmv_log("[gamepatch] fn 0x%08x not the expected bytes; crash58 guard skipped\n", CR58_FUNC);
        return;
    }

    unsigned char *tr = VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) {
        gmv_log("[gamepatch] VirtualAlloc for crash58 trampoline failed (%lu); guard skipped\n", GetLastError());
        return;
    }

    /* Entered via `jmp` from the function entry, so the stack is the untouched call frame:
         [esp]=retaddr  [esp+4]=this  [esp+8]=a1  [esp+0xc]=a2  [esp+0x10]=a3   (cdecl, caller cleans) */
    size_t o = 0;
    tr[o++] = 0x8b; tr[o++] = 0x44; tr[o++] = 0x24; tr[o++] = 0x04; /* mov eax,[esp+4]   ; this        */
    tr[o++] = 0x85; tr[o++] = 0xc0;                                 /* test eax,eax                   */
    tr[o++] = 0x74; size_t j1 = o++;                                /* jz  L_null                     */
    tr[o++] = 0x8b; tr[o++] = 0x40; tr[o++] = 0x04;                 /* mov eax,[eax+4]   ; this->field4*/
    tr[o++] = 0x85; tr[o++] = 0xc0;                                 /* test eax,eax                   */
    tr[o++] = 0x74; size_t j2 = o++;                                /* jz  L_null                     */
    /* L_orig: field4 != NULL -> re-run the displaced prologue and resume the real function. */
    tr[o++] = 0x55;                                                 /* push ebp                       */
    tr[o++] = 0x89; tr[o++] = 0xe5;                                 /* mov ebp,esp                    */
    tr[o++] = 0x83; tr[o++] = 0xec; tr[o++] = 0x28;                 /* sub esp,0x28                   */
    tr[o++] = 0xff; tr[o++] = 0x25; size_t contref = o; o += 4;     /* jmp [cont_ptr]  -> CR58_CONT   */
    /* L_null: field4 == NULL (the crash). Log this/caller, then return 0 to the caller. */
    size_t lnull = o;
    tr[o++] = 0x8b; tr[o++] = 0x44; tr[o++] = 0x24; tr[o++] = 0x04; /* mov eax,[esp+4]   ; this        */
    tr[o++] = 0x8b; tr[o++] = 0x14; tr[o++] = 0x24;                 /* mov edx,[esp]     ; caller ret  */
    tr[o++] = 0x89; tr[o++] = 0xe1;                                 /* mov ecx,esp       ; the call's args */
    tr[o++] = 0x55;                                                 /* push ebp          ; arg4: caller frame */
    tr[o++] = 0x51;                                                 /* push ecx          ; arg3        */
    tr[o++] = 0x52;                                                 /* push edx          ; arg2        */
    tr[o++] = 0x50;                                                 /* push eax          ; arg1        */
    tr[o++] = 0xb8;                                                 /* mov eax, &gmv_crash58_log       */
    { uint32_t fnp = (uint32_t)(uintptr_t)&gmv_crash58_log; memcpy(tr + o, &fnp, 4); o += 4; }
    tr[o++] = 0xff; tr[o++] = 0xd0;                                 /* call eax                        */
    tr[o++] = 0x83; tr[o++] = 0xc4; tr[o++] = 0x10;                 /* add esp,16        ; cdecl cleanup*/
    tr[o++] = 0x31; tr[o++] = 0xc0;                                 /* xor eax,eax       ; return 0    */
    tr[o++] = 0xc3;                                                 /* ret                             */
    size_t contslot = o;                                           /* cont_ptr data slot              */
    { uint32_t c = CR58_CONT; memcpy(tr + o, &c, 4); o += 4; }

    tr[j1] = (unsigned char)(lnull - (j1 + 1));                     /* fill the two short jz targets   */
    tr[j2] = (unsigned char)(lnull - (j2 + 1));
    { uint32_t a = (uint32_t)(uintptr_t)(tr + contslot); memcpy(tr + contref, &a, 4); }
    FlushInstructionCache(GetCurrentProcess(), tr, o);

    unsigned char site[6];
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)tr - (CR58_FUNC + 5));
    site[0] = 0xe9;                                                 /* jmp trampoline                 */
    memcpy(site + 1, &rel, 4);
    site[5] = 0x90;                                                 /* pad to 0x58c196                */
    if (!patch_bytes(CR58_FUNC, site, sizeof site)) {
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); crash58 guard skipped\n", CR58_FUNC, GetLastError());
        return;
    }
    gmv_log("[gamepatch] crash58 null-forward guard applied at 0x%08x (trampoline @ %p; permanent, "
            "CRASH58_GUARD=0 to disable); null this->field4 now logged as [crash58] + returns 0\n",
            CR58_FUNC, (void *)tr);
}
