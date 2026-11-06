/* veh.h -- first-chance fault handling for the game image: veh.c on Windows and fault_linux.c on Linux get the
   fault, fault.c says what it means. */
#ifndef GMV_VEH_H
#define GMV_VEH_H

#include <stdint.h>

#include "game.h"

/* Execution that faults inside [code_lo, code_hi) -- the low range's code, not the Mach-O
   header before it -- is moved into its copy; reads of a missing data import's stand-in
   are reported by name; other faults are logged and passed on. */
int veh_install(uint32_t code_lo, uint32_t code_hi);
/* Exercises the redirect path with host code only (game code is not touched). */
int veh_selftest(void);
uint32_t veh_redirect_count(void);

/* Stage 2: record which static initializer is running so a fault inside one is reported by
   index/address (and stops cleanly) rather than crashing anonymously. */
void veh_note_init(uint32_t index, uint32_t addr);
void veh_clear_init(void);
/* Stage 3: mark that main() is running, so a fault inside it is reported and stops cleanly. */
void veh_note_main(uint32_t addr);

/* Diagnostic: arm hw data write-watchpoints (DR0..DR2) on the calling thread to catch who writes
   the layer-content flags / draw-enable CVars.  The VEH handler logs the writer eip + stack. */
void gmv_watch_arm(uint32_t addr);

#ifndef _WIN32
/* crashdump_linux.c: the fault the crash dump is about (a siginfo_t and a ucontext_t), before gmv_fatal. */
void gmv_dump_note_signal(int sig, const void *si, const void *uc);
#endif

/* ---- fault.c, for the two handlers ---- */

enum { GMV_ACCESS_READ, GMV_ACCESS_WRITE, GMV_ACCESS_EXEC };
void fault_set_low_range(uint32_t lo, uint32_t hi);
/* A jump to eip that faulted: 1 if it belongs elsewhere, which is written to *to. */
int fault_redirect(uint32_t eip, uint32_t *to);
/* What an address is, and the word for an access. */
const char *fault_where(uint32_t a);
const char *fault_access_name(int access);
/* Everything the log says about a fault. */
void fault_report(const gmv_regs *r, int access, uint32_t addr);
/* 1 if game code was running, and the log says which. */
int fault_stop_note(void);

#endif
