/* diag_spawn.c -- diagnostic: SPAWN_SURVEY */
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

/* ---- SPAWN_SURVEY (diagnostic): the city's spawn points and what lives in the world ----
   Each spawn point (level data, SpawnPoint.cpp) counts down a timer (+0x24, set at its setup 0x084390
   from a random pick in its kind's range) and then asks 0x080272, from 0x082af8 only, whether it may
   spawn.  Its kind (+0x28, the data's +0x4c) picks a static record at setup, kept by its index
   (+0x2c) in the table 0x6888c0: {category, default cap, delays in ms, distances}, at 0x6426e0..
   0x642820 (category 3 for kind 3, 5 for 5, 17 for 6 and 17, 16 for 16, 25 for 25, 8 for the rest).
   The check: the world's count of that category (world +0x414 -> int[]) under the point's own cap
   (+0x78 in game mode 3, else +0x74; the record's default if negative), itself capped by the
   performance profile for kinds 3 (+0x1c), 17 (+0x24) and 8 (+0x20); then categories 3, 8, 16 and 17
   together under 48, except category 16, which has only its own cap.  The survey logs each spawn
   point once and, every 15 s, the count of each category and the objects in the world's three
   lists by vtable (the game has no RTTI; its __ZTV symbols name them), type byte (+0x24) and the
   category that type maps to (the byte table at *0x63e174, as the check reads it). */
#define SV_TYPE_CAT  0x0063e174u   /* -> category by type byte */
#define SV_MODE      0x0068a104u
#define SV_LIST_FN   0x00020fd2u   /* first object of list k (0, 1, 2) */
#define SV_PROFILE   0x00949a00u

sv_t sv;

uint32_t sv_rd(uint32_t a)
{
    return (a > 0x1000 && !IsBadReadPtr((void *)(uintptr_t)a, 4)) ? *(const uint32_t *)(uintptr_t)a : 0;
}

static void sv_dump(uint32_t world)
{
    const int *cnt = (const int *)(uintptr_t)sv_rd(world + 0x414);
    char line[2048];
    int o = 0;
    for (int c = 0; c < 32 && cnt && !IsBadReadPtr(cnt, 32 * 4); c++)
        if (cnt[c] || sv.asks[c])
            o += snprintf(line + o, sizeof line - o, " %d:%d (asked %u, allowed %u, made %u)", c, cnt[c], sv.asks[c],
                          sv.yes[c], sv.made[c]);
    line[o] = 0;
    gmv_log("[spawnsurvey] world counts by category (category:count):%s\n", line);
    memset(sv.asks, 0, sizeof sv.asks);
    memset(sv.yes, 0, sizeof sv.yes);
    memset(sv.made, 0, sizeof sv.made);
    const uint8_t *tc = (const uint8_t *)(uintptr_t)sv_rd(SV_TYPE_CAT);
    if (tc && IsBadReadPtr(tc, 256))
        tc = NULL;
    for (int k = 0; k < 3; k++) {
        struct { uint32_t vt; int type, n; } t[64];
        int nt = 0, total = 0;
        uint32_t ob = ((uint32_t (*)(uint32_t, int))(uintptr_t)SV_LIST_FN)(world, k);
        for (int i = 0; ob && i < 4000 && !IsBadReadPtr((void *)(uintptr_t)ob, 0x28); i++, ob = sv_rd(ob + 0xc)) {
            uint32_t vt = sv_rd(ob);
            int type = *(const uint8_t *)(uintptr_t)(ob + 0x24);
            int j = 0;
            while (j < nt && !(t[j].vt == vt && t[j].type == type))
                j++;
            if (j == nt && nt < 64)
                t[nt].vt = vt, t[nt].type = type, t[nt].n = 0, nt++;
            if (j < nt)
                t[j].n++;
            total++;
        }
        char big[4096];
        o = 0;
        for (int j = 0; j < nt; j++)
            o += snprintf(big + o, sizeof big - o, " %08x/%d/%d x%d", t[j].vt, t[j].type, tc ? tc[t[j].type] : -1, t[j].n);
        big[o] = 0;
        gmv_log("[spawnsurvey]   list %d: %d objects (vtable/type/category xcount):%s\n", k, total, big);
    }
}

void sv_note(uint32_t sp, int r)
{
    uint32_t world = sv_rd(SV_WORLD);
    uint32_t rec = sv_rd(SV_TABLE + 4 * *(const uint8_t *)(uintptr_t)(sp + 0x2c));
    int cat = (int)sv_rd(rec);
    if (cat >= 0 && cat < 32)
        sv.asks[cat]++, sv.yes[cat] += r != 0;
    int i = 0;
    while (i < sv.n && sv.sp[i] != sp)
        i++;
    if (i == sv.n && sv.n < 256) {
        sv.sp[sv.n++] = sp;
        const int *cnt = (const int *)(uintptr_t)sv_rd(world + 0x414);
        gmv_log("[spawnsurvey] spawn point %d @%08x (vtable %08x): kind %d, category %d, band %d, default cap %d, its own "
                "caps %d/%d (mode %d), delays %d..%d / %d..%d ms, timer %d, %d slots, now %d of that category -> %s\n",
                i, sp, sv_rd(sp), (int)sv_rd(sp + 0x28), cat, (int)sv_rd(sp + 0x48), (int)sv_rd(rec + 4), (int)sv_rd(sp + 0x74),
                (int)sv_rd(sp + 0x78), (int)sv_rd(SV_MODE), (int)sv_rd(rec + 8), (int)sv_rd(rec + 12),
                (int)sv_rd(rec + 16), (int)sv_rd(rec + 20), *(const int16_t *)(uintptr_t)(sp + 0x24),
                (int)((sv_rd(sp + 0x50) - sv_rd(sp + 0x4c)) / 4), cnt && cat >= 0 && cat < 32 ? cnt[cat] : -1,
                r ? "allowed" : "refused");
    }
    DWORD now = GetTickCount();
    if ((int)(now - sv.next) >= 0 && world) {
        if (sv.next)
            sv_dump(world);
        else {
            const int *pf = (const int *)(uintptr_t)SV_PROFILE;
            gmv_log("[spawnsurvey] performance profile +0x14..+0x24: %d %d %d %d %d\n", pf[5], pf[6], pf[7], pf[8], pf[9]);
        }
        sv.next = now + 15000;
    }
}
