/* fix_mission_save.c -- MISSION_SAVE_FIX: the save keeps Reginald's missions (all 8 groups of mission flags) */
#include "compat.h"
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* ---- MISSION_SAVE_FIX: a fix of the original game, asked for by the user on 2026-11-06 (default on, and on
   under ORIGINAL_MAC_EXPERIENCE too, the user's choice: a save made under the preset must not lose Reginald's
   missions, or they would be gone from MISSION_REPLAY once the preset is switched off again) ----
   Why: the mission manager keeps one completion bit per mission in 8 words, one per group (mgr+0x90, groups
   1-8; 0x8d018).  Group 8 is Reginald's five missions in Little Haiti.  The save writer 0x98148 writes them as
   MMSK blocks in a loop that stops at the count of another list, the scripts' tutorial list (scripts+0x3a8,
   read from a file by 0x33400; 7 entries), so the 8th word is never written.  The loader (0x977ac) takes
   indices up to 7, so the file format has room for it: only the writer is short.  Whose fault: the original
   game's; the Mac build does the same.  What exposed it: the user asked to have Reginald's missions marked
   done in their save (2026-11-06); a save with the 8th word added loaded it, and the game's next save dropped
   it again.
   The fix: the loop's test `cmp [eax+0x3a8], edi; jg` (0x986d4) becomes `cmp edi, 8; jl`, so the writer
   writes all 8 words.  A save made with it still loads in the original game. */
#define MSF_SITE  0x000986d4u

/* 1 = the writer writes 8 words, 0 = the original; -1 if the site is neither, -2 if the write failed. */
int msf_set(int on)
{
    static const unsigned char orig[8] = { 0x39, 0xb8, 0xa8, 0x03, 0x00, 0x00, 0x7f, 0xd4 };   /* cmp [eax+0x3a8],edi; jg */
    static const unsigned char fixd[8] = { 0x83, 0xff, 0x08, 0x90, 0x90, 0x90, 0x7c, 0xd4 };   /* cmp edi,8; nop x3; jl */
    const void *p = (const void *)(uintptr_t)MSF_SITE;
    if (memcmp(p, orig, sizeof orig) && memcmp(p, fixd, sizeof fixd))
        return -1;
    return patch_bytes(MSF_SITE, on ? fixd : orig, sizeof orig) ? on : -2;
}

void apply_mission_save_fix(void)
{
    const char *e = gmv_getenv("GMV_MISSION_SAVE_FIX");
    if (e && *e == '0')
        return;
    int r = msf_set(1);
    if (r == -1)
        gmv_log("[gamepatch] mission flags' save loop 0x%08x not the expected bytes; MISSION_SAVE_FIX skipped\n", MSF_SITE);
    else if (r == -2)
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); MISSION_SAVE_FIX skipped\n", MSF_SITE, GetLastError());
    else
        gmv_log("[gamepatch] MISSION_SAVE_FIX on: the save keeps all 8 groups of mission flags (Reginald's included)\n");
}
