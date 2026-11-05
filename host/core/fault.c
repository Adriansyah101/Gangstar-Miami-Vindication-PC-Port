/* fault.c -- what a fault in the game means, on any system.  veh.c (Windows) and fault_linux.c get the fault from
   the system and come here with its registers. */
#include <stdint.h>
#include <string.h>

#include "game.h"
#include "imports.h"
#include "layout.h"
#include "log.h"
#include "platform.h"
#include "veh.h"

/* Self-test window inside __PAGEZERO: never game code, never mappable. */
#define SELFTEST_ADDR 0x800

static uint32_t low_start, low_end;
static volatile uint32_t redirects, selftest_hits;
static volatile uint32_t cur_init = 0xFFFFFFFFu, cur_init_addr;  /* set while a static initializer runs */
static volatile int in_main;                                    /* set while main() runs */
static volatile uint32_t main_entry;

void veh_note_init(uint32_t index, uint32_t addr)
{
    cur_init = index;
    cur_init_addr = addr;
}

void veh_clear_init(void)
{
    cur_init = 0xFFFFFFFFu;
}

void veh_note_main(uint32_t addr)
{
    in_main = 1;
    main_entry = addr;
}

void fault_set_low_range(uint32_t lo, uint32_t hi)
{
    low_start = lo;
    low_end = hi;
}

static int __cdecl selftest_target(void)
{
    return 0x5A17;
}

const char *fault_where(uint32_t a)
{
    if (a - low_start < low_end - low_start)
        return "game low range (should have been redirected)";
    if (a - GMV_LOWCOPY_BASE < GMV_LOWCOPY_CAP)
        return "game code (low copy)";
    if (a >= GMV_RESERVE_START && a < GMV_MACHO_END)
        return "game image";
    if (a - GMV_THUNK_BASE < GMV_THUNK_SIZE)
        return "import thunk";
    if (a - GMV_DATAGUARD_BASE < GMV_DATAGUARD_SIZE)
        return "missing data import";
    return "host or system";
}

const char *fault_access_name(int access)
{
    return access == GMV_ACCESS_EXEC ? "execute" : access == GMV_ACCESS_WRITE ? "write" : "read";
}

int fault_redirect(uint32_t eip, uint32_t *to)
{
    if (eip - SELFTEST_ADDR < 16) {
        *to = (uint32_t)(uintptr_t)selftest_target;
        __sync_add_and_fetch(&selftest_hits, 1);
        return 1;
    }
    if (low_end && eip - low_start < low_end - low_start) {
        *to = eip + GMV_LOW_DELTA;
        __sync_add_and_fetch(&redirects, 1);
        return 1;
    }
    return 0;
}

void fault_report(const gmv_regs *r, int access, uint32_t addr)
{
    uint32_t eip = r->eip;
    gmv_log("\n[fault] access violation (%s 0x%08x) at eip 0x%08x in %s\n", fault_access_name(access), addr, eip,
            fault_where(eip));
    gmv_log("        eax=%08x ecx=%08x edx=%08x ebx=%08x\n", r->eax, r->ecx, r->edx, r->ebx);
    gmv_log("        esi=%08x edi=%08x ebp=%08x esp=%08x\n", r->esi, r->edi, r->ebp, r->esp);
    {
        char modname[GMV_PATH_MAX] = {0};
        uint32_t base;
        if (gmv_module_of(eip, modname, sizeof modname, &base))
            gmv_log("        eip module: %s (base 0x%08x)\n", modname, base);
    }
    {   /* mini-backtrace: stack words that land in the game image or host code */
        const uint32_t *sp = (const uint32_t *)(uintptr_t)r->esp;
        if (gmv_readable(sp, 64 * sizeof(uint32_t)))
            for (int k = 0; k < 64; k++)
                if (sp[k] >= 0x1000 && sp[k] < GMV_TEXT_HI)
                    gmv_log("        [esp+%02x] -> game 0x%08x\n", k * 4, sp[k]);
    }
    {   /* ebp-chain backtrace: some game frames are huge (0x300+ bytes of locals), so the raw esp
           scan above only reaches locals, not the real return addresses.  Walk saved frame pointers
           ([ebp]=caller ebp, [ebp+4]=return address) to recover the actual call chain -- which names
           the subsystem that faulted (and whether an input handler is on the stack). */
        const uint32_t *fp = (const uint32_t *)(uintptr_t)r->ebp;
        for (int depth = 0; depth < 24 && fp && gmv_readable(fp, 8); depth++) {
            uint32_t ret = fp[1], prev = fp[0];
            if (ret >= 0x1000)
                gmv_log("        #%-2d ret 0x%08x (%s)\n", depth, ret, fault_where(ret));
            if (prev <= (uint32_t)(uintptr_t)fp)   /* ebp must ascend; stop on a loop or garbage */
                break;
            fp = (const uint32_t *)(uintptr_t)prev;
        }
    }
#if GMV_DEV
    {   /* deeper scan: return addresses (a call just before) in the game, the host or any module,
           for faults inside code whose ebp is not a frame pointer (the GL driver) */
        const unsigned char *sp = (const unsigned char *)(uintptr_t)r->esp;
        int shown = 0;
        for (int k = 0; k < 4096 && shown < 32; k++) {
            const uint32_t *w = (const uint32_t *)(sp + k * 4);
            if (((uintptr_t)w & 0xfff) == 0 || k == 0)
                if (!gmv_readable(w, 4096 - ((uintptr_t)w & 0xfff)))
                    break;
            uint32_t ra = *w;
            if (ra < 0x1000 || !gmv_readable((const void *)(uintptr_t)(ra - 6), 6))
                continue;
            const unsigned char *c = (const unsigned char *)(uintptr_t)ra;
            int call = c[-5] == 0xe8 || (c[-6] == 0xff && (c[-5] & 0x38) == 0x10) ||
                       (c[-2] == 0xff && (c[-1] & 0x38) == 0x10) || (c[-3] == 0xff && (c[-2] & 0x38) == 0x10);
            if (!call)
                continue;
            char mod[GMV_PATH_MAX] = {0};
            uint32_t base;
            if (ra < GMV_TEXT_HI)
                gmv_log("        [esp+%03x] ret game 0x%06x\n", k * 4, ra);
            else if (gmv_module_of(ra, mod, sizeof mod, &base)) {
                const char *b = strrchr(mod, GMV_SEP[0]);
                gmv_log("        [esp+%03x] ret %s+0x%x\n", k * 4, b ? b + 1 : mod, (unsigned)(ra - base));
            } else
                gmv_log("        [esp+%03x] ret 0x%08x (no module)\n", k * 4, ra);
            shown++;
        }
    }
    gmv_game_crash_note(r);
#endif
}

int fault_stop_note(void)
{
    if (cur_init == 0xFFFFFFFFu && !in_main)
        return 0;
    if (in_main)
        gmv_log("        while running main (entry 0x%08x)\n", main_entry);
    else
        gmv_log("        while running static initializer [%u] @ 0x%08x\n", cur_init, cur_init_addr);
    return 1;
}

int veh_selftest(void)
{
    int (*fn)(void) = (int (*)(void))(uintptr_t)SELFTEST_ADDR;
    uint32_t before = selftest_hits;
    int r = fn();
    return r == 0x5A17 && selftest_hits == before + 1 ? 0 : -1;
}

uint32_t veh_redirect_count(void)
{
    return redirects;
}
