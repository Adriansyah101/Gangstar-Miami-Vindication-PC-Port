/* veh.c -- faults on Windows, by a vectored exception handler.  What they mean is fault.c's. */
#ifdef _WIN32
#include <string.h>
#include <windows.h>

#include "game.h"
#include "imports.h"
#include "layout.h"
#include "log.h"
#include "veh.h"

#if GMV_DEV
static volatile uint32_t g_watch_addr;  /* 4-byte field watched by DR0 (gmv_watch_arm) */
#endif

static LONG CALLBACK handler(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *cx = ep->ContextRecord;
    /* Hardware data watchpoint (DR0..DR2, armed by gmv_watch_arm): a #DB single-step fires AFTER
       the instruction that wrote a watched byte, so cx->Eip points just past the writer.  Log the
       writer eip + register snapshot + a game-range stack scan to identify who sets the layer
       content flags [0x949a58] / draw-enable CVars [0x68541a]/[0x685438]. */
#if GMV_DEV
    if (er->ExceptionCode == EXCEPTION_SINGLE_STEP) {
        static volatile LONG whits, dumps;
        LONG h = InterlockedIncrement(&whits);
        int val = *(const int *)(uintptr_t)g_watch_addr;   /* watched field value AFTER the write */
        /* Compact one-liner for every write (cap 100) so value transitions are visible in order. */
        if (h <= 100)
            gmv_log("[watch] #%ld [0x%08x]<-%d eip=0x%08x (%s)\n",
                    h, (unsigned)g_watch_addr, val, (unsigned)cx->Eip, fault_where((uint32_t)cx->Eip));
        /* Full register + game-range stack dump for the first several writes (cap 10) so we can
           identify every writer of the watched field and the call chain that set it. */
        if (InterlockedIncrement(&dumps) <= 10) {
            gmv_log("        eax=%08lx ecx=%08lx edx=%08lx ebx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx\n",
                    cx->Eax, cx->Ecx, cx->Edx, cx->Ebx, cx->Esi, cx->Edi, cx->Ebp, cx->Esp);
            const uint32_t *sp = (const uint32_t *)(uintptr_t)cx->Esp;
            if (!IsBadReadPtr((void *)sp, 64 * sizeof(uint32_t)))
                for (int k = 0; k < 64; k++)
                    if (sp[k] >= 0x1000 && sp[k] < GMV_TEXT_HI)
                        gmv_log("        [esp+%02x] -> game 0x%08x\n", k * 4, sp[k]);
        }
        cx->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
#endif
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    uint32_t eip = cx->Eip, fault = (uint32_t)er->ExceptionInformation[1], to;
    int fetch = (uint32_t)(uintptr_t)er->ExceptionAddress == eip && fault == eip;

    if (fetch && fault_redirect(eip, &to)) {
        cx->Eip = to;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (gmv_dump_is_writer())
        return EXCEPTION_CONTINUE_SEARCH;       /* dbghelp's own, while the crash dump is written */
    /* Re-entrancy guard: our crash dump below probes possibly-wild pointers (IsBadReadPtr).  Such a
       probe raises its own AV, which this vectored handler would otherwise hijack and mis-report as
       a second game crash, or as a missing import when the pointer lands on one's guard page.  On
       re-entry, bow out so the inner frame SEH (e.g. IsBadReadPtr's own __except) resolves it --
       IsBadReadPtr then simply returns "bad" and the dump skips that ptr.
       Another thread that faults meanwhile waits: the first fault ends the process. */
    static volatile LONG in_handler;
    LONG me = (LONG)GetCurrentThreadId();
    if (in_handler == me)
        return EXCEPTION_CONTINUE_SEARCH;
    const char *missing = imports_guard_name(fault);
    if (missing) {
        gmv_log("\n[MISSING DATA] game touched unimplemented data import %s at eip 0x%08x\n", missing, eip);
        gmv_dump_note(ep);
        gmv_fatal(4, "The game used data this port does not have yet: %s.", missing);
    }
    if (InterlockedCompareExchange(&in_handler, me, 0) != 0)
        Sleep(INFINITE);
    int access = er->ExceptionInformation[0] == 8 ? GMV_ACCESS_EXEC : er->ExceptionInformation[0] ? GMV_ACCESS_WRITE
                                                                                                 : GMV_ACCESS_READ;
    gmv_regs r = {eip, cx->Eax, cx->Ecx, cx->Edx, cx->Ebx, cx->Esi, cx->Edi, cx->Ebp, cx->Esp};
    fault_report(&r, access, fault);
    if (fault_stop_note()) {                    /* stop cleanly with the diagnostic instead of a silent crash */
        gmv_dump_note(ep);
        gmv_fatal(6, "The game crashed: a bad memory %s at 0x%08x, in %s.",
                  access == GMV_ACCESS_EXEC ? "jump" : fault_access_name(access), eip, fault_where(eip));
    }
    in_handler = 0;
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Whatever nothing else handled, access violations before the game's code runs included: an illegal
   instruction, a division by zero, a stack overflow.  Only exceptions that would end the process
   get here, so the handlers inside the GL driver and system DLLs keep theirs. */
static LONG CALLBACK last_chance(EXCEPTION_POINTERS *ep)
{
    uint32_t eip = (uint32_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    gmv_log("\n[fault] unhandled exception 0x%08lx at eip 0x%08x in %s\n",
            ep->ExceptionRecord->ExceptionCode, eip, fault_where(eip));
    gmv_dump_note(ep);
    gmv_fatal(9, "The game crashed: exception 0x%08lx at 0x%08x, in %s.",
              ep->ExceptionRecord->ExceptionCode, eip, fault_where(eip));
}

#if GMV_DEV
/* Arm hardware data write-watchpoints on the CALLING thread (the game's main/render thread):
   DR0 = 0x949a58 (4 bytes: the menu "layer has content" flags [0x949a58..0x949a5b]);
   DR1 = 0x68541a, DR2 = 0x685438 (1 byte each: the main-layer draw-enable CVars).
   DR7: L0|L1|L2 enabled; RW=01 (write) each; LEN0=11 (4B), LEN1=LEN2=00 (1B).  Debug registers
   are per-thread; SetThreadContext on the current thread applies them (standard hw-bp technique). */
void gmv_watch_arm(uint32_t addr)
{
    CONTEXT c;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    HANDLE th = GetCurrentThread();
    if (!GetThreadContext(th, &c)) {
        gmv_log("[watch] GetThreadContext failed err=%lu\n", GetLastError());
        return;
    }
    g_watch_addr = addr;
    c.Dr0 = addr;        /* 4-byte write watchpoint on the caller-chosen field */
    c.Dr1 = 0;
    c.Dr2 = 0;
    c.Dr3 = 0;
    c.Dr6 = 0;
    c.Dr7 = 0x000D0001u; /* L0 only; RW0=01 (write), LEN0=11 (4 bytes) */
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!SetThreadContext(th, &c)) {
        gmv_log("[watch] SetThreadContext failed err=%lu\n", GetLastError());
        return;
    }
    gmv_log("[watch] armed DR0=0x%08x(4B,write) DR7=0x%08x on tid=%lu\n",
            (unsigned)addr, (unsigned)0x000D0001u, GetCurrentThreadId());
}
#endif

int veh_install(uint32_t code_lo, uint32_t code_hi)
{
    fault_set_low_range(code_lo, code_hi);
    SetUnhandledExceptionFilter(last_chance);
    return AddVectoredExceptionHandler(1, handler) ? 0 : -1;
}
#endif
