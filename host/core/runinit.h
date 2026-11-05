/* Stage 2d: run the game's C++ static initializers (the __mod_init_func table). */
#ifndef GMV_RUNINIT_H
#define GMV_RUNINIT_H

#include <stddef.h>
#include <stdint.h>

#include "image.h"
#include "macho.h"

/* Calls the first `limit` static initializers (0 = all) in order, one at a time, logging each
   before it runs.  A fault inside one is reported by index/address through the VEH and stops
   the process.  Returns 0 if all requested initializers returned normally. */
int run_initializers(const macho *m, const gmvp *t, uint32_t limit, char *err, size_t errsz);

/* Calls the game's main() (run_initializers must have run first).  Returns 0 if main returns. */
int run_main(const macho *m, const gmvp *t, uint32_t main_addr, char *err, size_t errsz);

#endif
