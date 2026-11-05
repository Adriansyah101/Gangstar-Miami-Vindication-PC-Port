/* The game's imports: what each one is, and what its bind slots get pointed at. */
#ifndef GMV_IMPORTS_H
#define GMV_IMPORTS_H

#include <stddef.h>
#include <stdint.h>

#include "macho.h"

typedef struct {
    const char *name;               /* points into the Mach-O */
    int ordinal;                    /* dylib ordinal from the bind stream */
    int nslots, lazy_slots, nl_slots, data_slots;
    int weak_import;                /* every bind of it is weak: may resolve to 0 */
    int is_data;
    uint32_t target;                /* what slots get (before addend) */
    enum { IMP_HOST, IMP_MISSING_CODE, IMP_MISSING_DATA, IMP_WEAK_NULL } how;
} import;

typedef struct {
    import *imp;
    int n;
    uint32_t nslots;
    uint32_t count[4];              /* by .how */
} import_table;

/* Implemented host symbols; empty until later stages add shims. */
typedef struct {
    const char *name;
    void *addr;
} host_symbol;

int imports_collect(const macho *m, import_table *t, char *err, size_t errsz);
/* Resolves every import (host symbol, missing-code thunk, no-access data stand-in or 0)
   and writes all regular and lazy bind slots. */
int imports_bind(const macho *m, import_table *t, char *err, size_t errsz);
/* Name of the missing data import whose stand-in contains addr, or NULL. */
const char *imports_guard_name(uint32_t addr);
const char *imports_lib_name(const macho *m, int ordinal);

#endif
