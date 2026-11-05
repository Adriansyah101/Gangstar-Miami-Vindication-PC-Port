/* fix_fade_edge.c -- FADE_EDGE_FIX: full-screen fades cover the screen's top row and left column */
#include "compat.h"
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* ---- original-game bug, fixed: a line of picture along the top and left of a black fade ----

   Symptom: with anti-aliasing on (MSAA, 4x on the Mac), the top row and the left column of the screen keep
   showing the picture while a fade holds it black, as after clicking CONTINUE.

   Mechanism: the game's 2D projection is moved by 0.375 of a pixel (the old trick that puts a whole-number
   coordinate on a pixel's centre), so a rectangle from (0,0) starts 0.375 px inside the top-left corner.  The
   cutscene fader's rectangle comes out 0.375 further out, so it starts on the corner, but the state fades'
   (0x1a5b32's, the generic fade state's) come out at exactly (0,0).  Without samples that still covers the
   centre of the first row and column; with 4x MSAA some of their samples lie outside it, so those pixels are
   left part picture.  The Mac runs the same code with 4x MSAA.

   Fix: draw2DRectangle (0x4c5ce0) hands the driver its destination rectangle at 0x4c5db8
   (mov eax,[ebp+0x10] / mov [esp+4],eax).  Those 7 bytes become a call to a stub that hands over a copy
   starting at (-1,-1) when the rectangle starts at (0,0), so it reaches past the corner.  The far corner is
   kept, and so is any rectangle that starts elsewhere.  The function's entry, where FADE_TRACE hooks, is not
   touched.  Default on; FADE_EDGE_FIX=0 keeps the original.  ORIGINAL_MAC_EXPERIENCE=1 keeps the original
   (it kept the fix on from 2026-11-05 until the user turned it off there later that day). */
#define FE_SITE 0x004c5db8u

static const unsigned char fe_orig[7] = { 0x8b, 0x45, 0x10, 0x89, 0x44, 0x24, 0x04 };
static unsigned char *fe_stub;
static int32_t fe_rect[4];

__attribute__((cdecl, force_align_arg_pointer))
static const int32_t *gmv_fade_edge_rect(const int32_t *r)
{
    if (!r || r[0] != 0 || r[1] != 0)
        return r;
    fe_rect[0] = fe_rect[1] = -1;
    fe_rect[2] = r[2];
    fe_rect[3] = r[3];
    return fe_rect;     /* the driver reads it at once, before the next rectangle */
}

/* call fe_stub; nop; nop */
static void fe_site_bytes(unsigned char *b)
{
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)fe_stub - (FE_SITE + 5));
    b[0] = 0xe8;
    memcpy(b + 1, &rel, 4);
    b[5] = b[6] = 0x90;
}

/* 1 = the stub hands the driver the rectangle, 0 = the game's own bytes; -1 if the site is neither,
   -2 if the write failed or the stub could not be made. */
int fade_edge_set(int on)
{
    if (!fe_stub) {
        unsigned char *s = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!s)
            return -2;
        size_t o = 0;
        uint32_t fn = (uint32_t)(uintptr_t)&gmv_fade_edge_rect;
        s[o++] = 0x51;                                              /* push ecx  (both live: the driver's   */
        s[o++] = 0x52;                                              /* push edx   vtable and this)          */
        s[o++] = 0xff; s[o++] = 0x75; s[o++] = 0x10;                /* push dword [ebp+0x10]  ; the rect    */
        s[o++] = 0xb8; memcpy(s + o, &fn, 4); o += 4;               /* mov eax, gmv_fade_edge_rect          */
        s[o++] = 0xff; s[o++] = 0xd0;                               /* call eax                             */
        s[o++] = 0x83; s[o++] = 0xc4; s[o++] = 0x04;                /* add esp,4                            */
        s[o++] = 0x5a;                                              /* pop edx                              */
        s[o++] = 0x59;                                              /* pop ecx                              */
        s[o++] = 0x89; s[o++] = 0x44; s[o++] = 0x24; s[o++] = 0x08; /* mov [esp+8],eax  ; the caller's [esp+4] */
        s[o++] = 0xc3;                                              /* ret                                  */
        FlushInstructionCache(GetCurrentProcess(), s, o);
        fe_stub = s;
    }
    unsigned char ours[7];
    fe_site_bytes(ours);
    const void *p = (const void *)(uintptr_t)FE_SITE;
    if (memcmp(p, fe_orig, sizeof fe_orig) && memcmp(p, ours, sizeof ours))
        return -1;
    return patch_bytes(FE_SITE, on ? ours : fe_orig, sizeof ours) ? on : -2;
}

void apply_fade_edge_fix(void)
{
    const char *e = gmv_getenv("GMV_FADE_EDGE_FIX");
    if (e && *e == '0')
        return;
    int r = fade_edge_set(1);
    if (r == -1)
        gmv_log("[gamepatch] 0x%08x not the expected bytes; fade edge fix skipped\n", FE_SITE);
    else if (r == -2)
        gmv_log("[gamepatch] fade edge fix not applied (%lu)\n", GetLastError());
    else
        gmv_log("[gamepatch] full-screen fades cover the top row and left column too (FADE_EDGE_FIX=0 to disable)\n");
}
