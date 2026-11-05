/* fault_linux.c -- faults on Linux, by signal handlers: veh.c's counterpart.  What they mean is fault.c's.

   A jump into the low range is an ordinary SIGSEGV whose address is the instruction's own: the handler moves eip into
   the copy and returns, as the Windows handler does.  Anything else stops the process with the same report. */
#ifndef _WIN32
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/ucontext.h>

#include "game.h"
#include "imports.h"
#include "layout.h"
#include "log.h"
#include "platform.h"
#include "veh.h"

/* The page-fault error code's bits (x86). */
#define PF_WRITE 0x02
#define PF_INSTR 0x10

/* Room for the handler when the fault is a stack overflow: on the main thread, which runs the game. */
static char alt_stack[64 * 1024];

static void regs_of(const ucontext_t *uc, gmv_regs *r)
{
    const greg_t *g = uc->uc_mcontext.gregs;
    r->eip = (uint32_t)g[REG_EIP];
    r->eax = (uint32_t)g[REG_EAX];
    r->ecx = (uint32_t)g[REG_ECX];
    r->edx = (uint32_t)g[REG_EDX];
    r->ebx = (uint32_t)g[REG_EBX];
    r->esi = (uint32_t)g[REG_ESI];
    r->edi = (uint32_t)g[REG_EDI];
    r->ebp = (uint32_t)g[REG_EBP];
    r->esp = (uint32_t)g[REG_ESP];
}

static void on_signal(int sig, siginfo_t *si, void *context)
{
    static volatile uint32_t in_handler;
    ucontext_t *uc = context;
    gmv_regs r;
    regs_of(uc, &r);
    uint32_t addr = (uint32_t)(uintptr_t)si->si_addr, me, to;

    if (sig != SIGSEGV && sig != SIGBUS) {
        gmv_log("\n[fault] unhandled exception (signal %d) at eip 0x%08x in %s\n", sig, r.eip, fault_where(r.eip));
        gmv_dump_note_signal(sig, si, uc);
        gmv_fatal(9, "The game crashed: signal %d at 0x%08x, in %s.", sig, r.eip, fault_where(r.eip));
    }
    if (addr == r.eip && fault_redirect(r.eip, &to)) {
        uc->uc_mcontext.gregs[REG_EIP] = (greg_t)to;
        return;
    }
    /* A fault while reporting one: let the system end the process. */
    me = gmv_thread_id();
    if (in_handler == me) {
        signal(sig, SIG_DFL);
        return;
    }
    const char *missing = imports_guard_name(addr);
    if (missing) {
        gmv_log("\n[MISSING DATA] game touched unimplemented data import %s at eip 0x%08x\n", missing, r.eip);
        gmv_dump_note_signal(sig, si, uc);
        gmv_fatal(4, "The game used data this port does not have yet: %s.", missing);
    }
    /* Another thread that faults meanwhile waits: the first fault ends the process. */
    if (!__sync_bool_compare_and_swap(&in_handler, 0, me))
        gmv_sleep_forever();
    unsigned long err = (unsigned long)uc->uc_mcontext.gregs[REG_ERR];
    int access = err & PF_INSTR ? GMV_ACCESS_EXEC : err & PF_WRITE ? GMV_ACCESS_WRITE : GMV_ACCESS_READ;
    fault_report(&r, access, addr);
    fault_stop_note();
    gmv_dump_note_signal(sig, si, uc);
    gmv_fatal(6, "The game crashed: a bad memory %s at 0x%08x, in %s.",
              access == GMV_ACCESS_EXEC ? "jump" : fault_access_name(access), r.eip, fault_where(r.eip));
}

/* Hardware watchpoints are a Windows dev build's diagnostic. */
void gmv_watch_arm(uint32_t addr)
{
    gmv_log("[watch] 0x%08x: not available on this system\n", addr);
}

int veh_install(uint32_t code_lo, uint32_t code_hi)
{
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
    stack_t st;
    memset(&st, 0, sizeof st);
    st.ss_sp = alt_stack;
    st.ss_size = sizeof alt_stack;
    if (sigaltstack(&st, NULL))
        return -1;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    /* NODEFER: a fault inside the handler reaches the guard above instead of killing the process unreported. */
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++)
        if (sigaction(sigs[i], &sa, NULL))
            return -1;
    fault_set_low_range(code_lo, code_hi);
    return 0;
}
#endif
