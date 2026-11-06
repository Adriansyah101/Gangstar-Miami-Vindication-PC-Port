/* game_read.c -- reading the game's memory without faulting (ms_rd), its strings (ms_text) and its script commands */
#include "compat.h"
#include <stdint.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* The mission records, as MISSION_SURVEY found them (2026-10-28; the survey was taken out 2026-11-06):
   The mission manager is *(*0x689e98 + 4) (0xf2ed0 + 4, as the menus get it).  Its records are an
   array at +0x50 of +0x54 pointers, by mission id (0x8ce6c); the one under way is +4, its state +0x1c
   (4 = the OBJECTIVE page's RESTART case, 2 set as one starts at 0x8dc96).  A record's +4, +0x1c and
   +0x18 (i, j, k) name a bit in the 7 words at +0x8c + 4i: bit 5(j-1) + k-1, set at 0x8da02 (the end
   of 0x8d9xx, which also clears +0x80) and read in 5-bit groups (0x8ce8a).  The save keeps these
   words as its MMSK blocks (written at 0x9867d, read back at 0x97750, from +0x90).  The OBJECTIVE page
   (0x179baa) shows a record's +0 (int), +0xc (text), +0x14, +0x24 and +0x28 (string ids, 0x1054 =
   none), the map +0x10.  A script's commands are at +0x10 (+4 of them, the one at +8 next, state +0x18);
   a command's type is its vtable's +0x1c. */
#define MS_APP       0x006853a0u

uint32_t ms_rd(uint32_t a)
{
#ifndef _WIN32
    /* Called often: VirtualQuery here reads a file, gmv_readable is one system call. */
    return a >= 0x1000 && gmv_readable((const void *)(uintptr_t)a, 4) ? *(const uint32_t *)(uintptr_t)a : 0;
#else
    MEMORY_BASIC_INFORMATION mbi;
    if (a < 0x1000 || !VirtualQuery((const void *)(uintptr_t)a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                         PAGE_EXECUTE_WRITECOPY)) ||
        (const char *)mbi.BaseAddress + mbi.RegionSize - (const char *)(uintptr_t)a < 4)
        return 0;
    return *(const uint32_t *)(uintptr_t)a;
#endif
}

/* A string's text as the table already holds it (0x8f426 without its load of a missing group), quoted
   and cut short; "-" for none, "?" for what is not a loaded string. */
static const char *ms_str_at(uint32_t p, char *out, size_t n)
{
    size_t o = 0;
    out[o++] = '"';
    for (int i = 0; p && i < 48 && o + 3 < n; i++) {
        uint32_t w = ms_rd((p + i) & ~3u);
        unsigned char c = (unsigned char)(w >> 8 * ((p + i) & 3));
        if (!c)
            break;
        out[o++] = c >= 0x20 && c < 0x7f ? (char)c : '.';
    }
    out[o++] = '"';
    out[o] = 0;
    return out;
}

const char *ms_text(uint32_t id, char *out, size_t n)
{
    if (id == MS_NONE_ID)
        return "-";
    uint32_t table = ms_rd(ms_rd(MS_APP) + MS_STRINGS);
    uint32_t lang = ms_rd(table + 4);
    uint32_t grp = id >= 0x1000 && id < 0x10000 && lang < 16 ? ms_rd(table + 8 + lang * 0x34 + (id >> 12 & 15) * 4) : 0;
    uint32_t p = grp ? ms_rd(grp + (id & 0xfff) * 4) : 0;
    return p ? ms_str_at(p, out, n) : "?";
}

int ms_cmd_type(uint32_t c)
{
    uint32_t vt = ms_rd(c), fn = vt ? ms_rd(vt + 0x1c) : 0;
    return fn >= 0x2000 && fn < 0x600000 ? ((int (*)(uint32_t))(uintptr_t)fn)(c) : -1;
}
