/* diag_mission.c -- diagnostic: MISSION_SURVEY */
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

/* ---- MISSION_SURVEY (diagnostic): the mission records and what the save keeps of them ----
   The mission manager is *(*0x689e98 + 4) (0xf2ed0 + 4, as the menus get it).  Its records are an
   array at +0x50 of +0x54 pointers, by mission id (0x8ce6c); the one under way is +4, its state +0x1c
   (4 = the OBJECTIVE page's RESTART case, 2 set as one starts at 0x8dc96).  A record's +4, +0x1c and
   +0x18 (i, j, k) name a bit in the 7 words at +0x8c + 4i: bit 5(j-1) + k-1, set at 0x8da02 (the end
   of 0x8d9xx, which also clears +0x80) and read in 5-bit groups (0x8ce8a).  The save keeps these
   words as its MMSK blocks (written at 0x9867d, read back at 0x97750, from +0x90).  The OBJECTIVE page
   (0x179baa) shows a record's +0 (int), +0xc (text), +0x14, +0x24 and +0x28 (string ids, 0x1054 =
   none), the map +0x10.  Nothing is patched: once a second gmv_mission_poll reads the manager, logs
   every record when the list appears or changes, and then each change of the state, the flags (with
   the records whose bit it was) and the race's state. */
#define MS_APP       0x006853a0u
#define MS_FIELDS    16
#if GMV_DEV
static int ms_on;
#else
#define ms_on 0                 /* the replay shares the readers below; the survey itself is left out */
#endif

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

static void ms_log_record(int id, uint32_t rec, uint32_t mgr)
{
    uint32_t f[MS_FIELDS];
    for (int i = 0; i < MS_FIELDS; i++)
        f[i] = ms_rd(rec + 4 * i);
    int ci = (int)f[1], cj = (int)f[7], ck = (int)f[6];
    int done = -1;
    if (ci >= 1 && ci <= 7 && cj >= 1 && cj <= 6 && ck >= 1 && ck <= 5)
        done = (int)(ms_rd(mgr + 0x8c + 4 * ci) >> (5 * (cj - 1) + ck - 1) & 1);
    char t[4][64], c[64], raw[MS_FIELDS * 9 + 1];
    int o = 0;
    for (int i = 0; i < MS_FIELDS; i++)
        o += snprintf(raw + o, sizeof raw - o, " %x", f[i]);
    gmv_log("[mission] %3d @%08x: i %d j %d k %d -> %s; +0 %d, +0x10 %s, +0x14 %s, +0x24 %s, +0x28 %s, +0xc %s; raw%s\n",
            id, rec, ci, cj, ck, done < 0 ? "no flag" : done ? "flag set" : "flag clear", (int)f[0],
            ms_text(f[4], t[0], 64), ms_text(f[5], t[1], 64), ms_text(f[9], t[2], 64), ms_text(f[10], t[3], 64),
            f[3] ? ms_str_at(f[3], c, sizeof c) : "-", raw);
}

int ms_cmd_type(uint32_t c)
{
    uint32_t vt = ms_rd(c), fn = vt ? ms_rd(vt + 0x1c) : 0;
    return fn >= 0x2000 && fn < 0x600000 ? ((int (*)(uint32_t))(uintptr_t)fn)(c) : -1;
}

static void ms_scripts(DWORD now)
{
    static uint32_t lsc[MS_SC_MAX], lpc[MS_SC_MAX], lst[MS_SC_MAX];
    static int ln = -1;
    uint32_t list = ms_rd(MS_SCRIPTS + 0x2dc);
    int n = (int)ms_rd(MS_SCRIPTS + 0x2ec);
    if (!list || n <= 0 || n > MS_SC_MAX)
        return;
    int full = n != ln;
    if (full)
        gmv_log("[mission] t=%lu %d scripts at %08x\n", (unsigned long)now, n, list);
    for (int s = 0; s < n; s++) {
        uint32_t sc = ms_rd(list + 4 * s), cmds = ms_rd(sc + 0x10);
        int cnt = (int)ms_rd(sc + 4), pc = (int)ms_rd(sc + 8), st = (int)ms_rd(sc + 0x18);
        if (!full && sc == lsc[s] && (uint32_t)pc == lpc[s] && (uint32_t)st == lst[s])
            continue;
        if (cnt < 0 || cnt > 100000)
            cnt = 0;
        int id = -1, id67 = -1, at = -1;
        char types[MS_CMD_MAX * 6 + 16], name[64] = "-";
        int o = 0;
        for (int i = 0; i < cnt && i < MS_CMD_MAX; i++) {
            uint32_t c = ms_rd(cmds + 4 * i);
            int t = c ? ms_cmd_type(c) : -1;
            if (t == 0x63 && id < 0)
                id = (int)ms_rd(c + 0x10);
            if (t == 0x67 && id67 < 0)
                id67 = (int)ms_rd(c + 0x10);
            if ((t == 0x8f || t == 0x81) && at < 0 && i >= pc)
                at = i;
            if (t == 0x8f && name[0] == '-')
                ms_str_at(ms_rd(c + 0x14), name, sizeof name);
            if (full && o + 8 < (int)sizeof types)
                o += snprintf(types + o, sizeof types - o, i == pc ? " [%x]" : " %x", t);
        }
        if (id < 0)
            id = id67;
        if (full)
            gmv_log("[mission] script %3d @%08x: state %d, at %d of %d, mission %d, 0x8f name %s, raw %x %x %x %x %x %x %x %x;%s%s\n",
                    s, sc, st, pc, cnt, id, name, ms_rd(sc), ms_rd(sc + 0xc), ms_rd(sc + 0x14), ms_rd(sc + 0x1c),
                    ms_rd(sc + 0x20), ms_rd(sc + 0x24), ms_rd(sc + 0x28), ms_rd(sc + 0x2c), types,
                    cnt > MS_CMD_MAX ? " ..." : "");
        else {
            uint32_t c = pc >= 0 && pc < cnt ? ms_rd(cmds + 4 * pc) : 0;
            gmv_log("[mission] t=%lu script %d (mission %d): state %d -> %d, at %d -> %d (type %x)\n", (unsigned long)now, s,
                    id, (int)lst[s], st, (int)lpc[s], pc, c ? ms_cmd_type(c) : -1);
        }
        lsc[s] = sc, lpc[s] = (uint32_t)pc, lst[s] = (uint32_t)st;
    }
    ln = n;
}

void gmv_mission_poll(void)
{
    static DWORD next;
    static uint32_t lmgr, lrecs, lflags[8], lstate[6], lrace = ~0u;
    static int ln = -1;
    if (!ms_on)
        return;
    DWORD now = GetTickCount();
    if ((int)(now - next) < 0)
        return;
    next = now + 1000;
    uint32_t owner = ms_rd(MS_OWNER);
    uint32_t mgr = owner ? ms_rd(owner + 4) : 0;
    if (!mgr) {
        if (lmgr)
            gmv_log("[mission] t=%lu the mission manager is gone\n", (unsigned long)now);
        lmgr = 0;
        return;
    }
    uint32_t recs = ms_rd(mgr + 0x50);
    int n = (int)ms_rd(mgr + 0x54);
    if (n < 0 || n > 1000)
        n = 0;
    if (mgr != lmgr || recs != lrecs || n != ln) {
        char words[7 * 9 + 1];
        int o = 0;
        for (int i = 1; i <= 7; i++)
            o += snprintf(words + o, sizeof words - o, " %x", lflags[i] = ms_rd(mgr + 0x8c + 4 * i));
        gmv_log("[mission] t=%lu mission manager %08x: %d records at %08x; flag words (save MMSK 0-6):%s\n",
                (unsigned long)now, mgr, n, recs, words);
        for (int id = 0; id < n; id++) {
            uint32_t rec = ms_rd(recs + 4 * id);
            if (rec)
                ms_log_record(id, rec, mgr);
            else
                gmv_log("[mission] %3d: none\n", id);
        }
        lmgr = mgr, lrecs = recs, ln = n;
        memset(lstate, 0xff, sizeof lstate);
    }
    static const int soff[6] = {4, 0x1c, 0x7c, 0x80, 8, 0xc};
    uint32_t st[6];
    for (int i = 0; i < 6; i++)
        st[i] = ms_rd(mgr + soff[i]);
    uint32_t b85 = ms_rd(mgr + 0x84) >> 8 & 0xff;
    if (memcmp(st, lstate, sizeof st)) {
        int cur = (int)st[0];
        uint32_t rec = cur >= 0 && cur < n ? ms_rd(recs + 4 * cur) : 0;
        char t[64];
        gmv_log("[mission] t=%lu under way %d (%s), state %d, +0x7c %d, +0x80 %d, +0x85 %u, +8 %d, +0xc %d\n",
                (unsigned long)now, cur, rec ? ms_text(ms_rd(rec + 0x10), t, sizeof t) : "-", (int)st[1], (int)st[2],
                (int)st[3], b85, (int)st[4], (int)st[5]);
        memcpy(lstate, st, sizeof st);
    }
    for (int i = 1; i <= 7; i++) {
        uint32_t w = ms_rd(mgr + 0x8c + 4 * i);
        if (w == lflags[i])
            continue;
        uint32_t on = w & ~lflags[i], off = lflags[i] & ~w;
        gmv_log("[mission] t=%lu flag word %d (MMSK %d): %x -> %x (bits on %x, off %x)\n", (unsigned long)now, i, i - 1,
                lflags[i], w, on, off);
        for (int b = 0; b < 30; b++)
            if ((on | off) >> b & 1)
                for (int id = 0; id < n; id++) {
                    uint32_t rec = ms_rd(recs + 4 * id);
                    if (rec && (int)ms_rd(rec + 4) == i && (int)ms_rd(rec + 0x1c) == b / 5 + 1 &&
                        (int)ms_rd(rec + 0x18) == b % 5 + 1)
                        ms_log_record(id, rec, mgr);
                }
        lflags[i] = w;
    }
    uint32_t race = ms_rd(owner + 8);
    uint32_t rs = race ? ms_rd(race + 0x14) : ~1u;
    if (rs != lrace) {
        gmv_log("[mission] t=%lu race %08x state %d\n", (unsigned long)now, race, (int)rs);
        lrace = rs;
    }
    ms_scripts(now);
}

void apply_mission_survey(void)
{
#if GMV_DEV
    const char *e = gmv_dbgenv("GMV_MISSION_SURVEY");
    ms_on = e && atoi(e) > 0;
    if (ms_on)
        gmv_log("[mission] MISSION_SURVEY=1: the mission records and their save flags, then each change, once a second\n");
#endif
}
