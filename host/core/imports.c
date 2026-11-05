#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imports.h"
#include "layout.h"
#include "log.h"
#include "platform.h"
#include "shims.h"

#define FAIL(...) do { snprintf(err, errsz, __VA_ARGS__); return -1; } while (0)

#define THUNK_BYTES 16
#define GUARD_BYTES 64

/* The implemented host symbols live in shims.c / shimsw.c; gmv_shim_lookup searches them. */

static const import_table *active;      /* for the fault handler and missing-import reports */

extern void gmv_missing_entry(void);

const char *imports_lib_name(const macho *m, int ordinal)
{
    if (ordinal > 0 && ordinal <= m->ndylibs) {
        const char *s = strrchr(m->dylibs[ordinal - 1], '/');
        return s ? s + 1 : m->dylibs[ordinal - 1];
    }
    return ordinal == 0 ? "self" : ordinal == -1 ? "main" : ordinal == -2 ? "flat" : "?";
}

/* ---- collecting ----------------------------------------------------------------------- */

typedef struct {
    const macho *m;
    import_table *t;
    int *hash;                          /* index + 1 */
    uint32_t mask;
    int weak_seen, strong_seen;
    char *err;
    size_t errsz;
    int failed;
} collect_ctx;

static uint32_t hash_str(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static int find_or_add(collect_ctx *c, const char *name, int ordinal)
{
    uint32_t h = hash_str(name) & c->mask;
    while (c->hash[h]) {
        import *im = &c->t->imp[c->hash[h] - 1];
        if (!strcmp(im->name, name))
            return c->hash[h] - 1;
        h = (h + 1) & c->mask;
    }
    import *im = &c->t->imp[c->t->n];
    memset(im, 0, sizeof *im);
    im->name = name;
    im->ordinal = ordinal;
    im->weak_import = 1;
    c->hash[h] = ++c->t->n;
    return c->t->n - 1;
}

static int collect_cb(void *ctx, uint32_t slot, const char *name, int ordinal, int32_t addend, int flags)
{
    collect_ctx *c = ctx;
    (void)addend;
    if (ordinal <= 0) {
        snprintf(c->err, c->errsz, "bind of %s to special ordinal %d is not supported", name, ordinal);
        c->failed = 1;
        return 1;
    }
    import *im = &c->t->imp[find_or_add(c, name, ordinal)];
    if (im->ordinal != ordinal) {
        snprintf(c->err, c->errsz, "%s is bound from two dylibs", name);
        c->failed = 1;
        return 1;
    }
    const macho_sect *s = macho_sect_at(c->m, slot);
    if (!s || slot % 4 || !strcmp(s->segname, "__TEXT")) {
        snprintf(c->err, c->errsz, "bind slot 0x%x for %s is not an aligned data word", slot, name);
        c->failed = 1;
        return 1;
    }
    if ((s->flags & 0xFF) == 7)
        im->lazy_slots++;
    else if ((s->flags & 0xFF) == 6)
        im->nl_slots++;
    else
        im->data_slots++;
    if (!(flags & BINDF_WEAK_IMPORT))
        im->weak_import = 0;
    im->nslots++;
    c->t->nslots++;
    return 0;
}

/* Imports reached only through non-lazy pointers can be either functions whose address is
   taken or data; these name rules decide (the classification is logged for review). */
static int data_by_name(const char *n)
{
    static const char *const exact[] = {
        "___stderrp", "___stdoutp", "___stack_chk_guard", "__DefaultRuneLocale", "_NSApp",
        "__ZSt7nothrow", "__ZNSs4_Rep20_S_empty_rep_storageE", "___CFConstantStringClassReference",
        "_NSDefaultRunLoopMode", "_NSLocaleCountryCode", NULL
    };
    for (int i = 0; exact[i]; i++)
        if (!strcmp(n, exact[i]))
            return 1;
    if (!strncmp(n, "__ZTV", 5) || !strncmp(n, "__ZTI", 5) || !strncmp(n, "__ZTS", 5) || !strncmp(n, "__ZTT", 5))
        return 1;
    if (n[0] == '_' && n[1] == 'k' && n[2] >= 'A' && n[2] <= 'Z')
        return 1;
    size_t len = strlen(n);
    static const char *const suffix[] = {"Notification", "Key", "PboardType", NULL};
    for (int i = 0; suffix[i]; i++) {
        size_t sl = strlen(suffix[i]);
        if (len > sl && !strcmp(n + len - sl, suffix[i]))
            return 1;
    }
    return 0;
}

int imports_collect(const macho *m, import_table *t, char *err, size_t errsz)
{
    memset(t, 0, sizeof *t);
    uint32_t cap = 4096;
    t->imp = calloc(cap, sizeof *t->imp);
    collect_ctx c = {m, t, calloc(cap * 2, sizeof(int)), cap * 2 - 1, 0, 0, err, errsz, 0};
    if (!t->imp || !c.hash)
        FAIL("out of memory");
    for (int which = BIND_REGULAR; which <= BIND_LAZY; which++) {
        if (macho_binds(m, which, collect_cb, &c, err, errsz)) {
            free(c.hash);
            return -1;
        }
        if (c.failed) {
            free(c.hash);
            return -1;
        }
    }
    free(c.hash);
    /* a lazy slot is only ever called through; anything else may also be a function pointer
       kept in a table, so the name decides */
    for (int i = 0; i < t->n; i++) {
        import *im = &t->imp[i];
        im->is_data = im->lazy_slots ? 0 : data_by_name(im->name);
    }
    return 0;
}

/* ---- binding -------------------------------------------------------------------------- */

static const import *find_import(const import_table *t, const char *name)
{
    for (int i = 0; i < t->n; i++)
        if (!strcmp(t->imp[i].name, name))
            return &t->imp[i];
    return NULL;
}

typedef struct {
    const import_table *t;
    uint32_t written;
    int failed;
    char *err;
    size_t errsz;
    int last;                           /* streams repeat a name for runs of slots */
} bind_ctx;

static int bind_cb(void *ctx, uint32_t slot, const char *name, int ordinal, int32_t addend, int flags)
{
    bind_ctx *b = ctx;
    (void)ordinal;
    (void)flags;
    const import *im = NULL;
    if (b->last >= 0 && !strcmp(b->t->imp[b->last].name, name))
        im = &b->t->imp[b->last];
    else if ((im = find_import(b->t, name)))
        b->last = (int)(im - b->t->imp);
    if (!im) {
        snprintf(b->err, b->errsz, "bind of %s was not collected", name);
        b->failed = 1;
        return 1;
    }
    *(uint32_t *)(uintptr_t)slot = im->how == IMP_WEAK_NULL ? 0 : im->target + (uint32_t)addend;
    b->written++;
    return 0;
}

int imports_bind(const macho *m, import_table *t, char *err, size_t errsz)
{
    uint8_t *thunks = (uint8_t *)GMV_THUNK_BASE;
    int ncode = 0, ndata = 0;
    for (int i = 0; i < t->n; i++) {
        import *im = &t->imp[i];
        void *shim = gmv_shim_lookup(im->name);
        if (shim) {
            im->how = IMP_HOST;
            im->target = (uint32_t)(uintptr_t)shim;
        } else if (im->weak_import) {
            im->how = IMP_WEAK_NULL;    /* dyld semantics for a missing weak import */
            im->target = 0;
        } else if (im->is_data) {
            if ((ndata + 1) * GUARD_BYTES > GMV_DATAGUARD_SIZE)
                FAIL("too many missing data imports");
            im->how = IMP_MISSING_DATA;
            im->target = GMV_DATAGUARD_BASE + ndata++ * GUARD_BYTES;
        } else {
            if ((ncode + 1) * THUNK_BYTES > GMV_THUNK_SIZE)
                FAIL("too many missing imports");
            /* push <import index>; jmp gmv_missing_entry */
            uint8_t *p = thunks + ncode++ * THUNK_BYTES;
            uint32_t idx = (uint32_t)i, rel = (uint32_t)(uintptr_t)gmv_missing_entry - (uint32_t)(uintptr_t)(p + 10);
            p[0] = 0x68;
            memcpy(p + 1, &idx, 4);
            p[5] = 0xE9;
            memcpy(p + 6, &rel, 4);
            memset(p + 10, 0xCC, THUNK_BYTES - 10);
            im->how = IMP_MISSING_CODE;
            im->target = (uint32_t)(uintptr_t)p;
        }
        t->count[im->how]++;
    }
    active = t;
    bind_ctx b = {t, 0, 0, err, errsz, -1};
    for (int which = BIND_REGULAR; which <= BIND_LAZY; which++) {
        if (macho_binds(m, which, bind_cb, &b, err, errsz) || b.failed)
            return -1;
    }
    if (b.written != t->nslots)
        FAIL("wrote %u bind slots, collected %u", b.written, t->nslots);
    if (!gmv_protect(GMV_THUNK_BASE, GMV_THUNK_SIZE, GMV_PROT_RX) ||
        !gmv_protect(GMV_DATAGUARD_BASE, GMV_DATAGUARD_SIZE, GMV_PROT_NONE))
        FAIL("protecting the thunk area failed (%lu)", gmv_last_error());
    return 0;
}

const char *imports_guard_name(uint32_t addr)
{
    if (!active || addr - GMV_DATAGUARD_BASE >= GMV_DATAGUARD_SIZE)
        return NULL;
    for (int i = 0; i < active->n; i++) {
        const import *im = &active->imp[i];
        if (im->how == IMP_MISSING_DATA && addr - im->target < GUARD_BYTES)
            return im->name;
    }
    return "?";
}

/* Reached from a missing-code thunk: the thunk pushed the import index on top of the game's
   return address.  Stops the process; nothing sensible can be returned to the game. */
#ifndef _WIN32
__attribute__((force_align_arg_pointer))     /* the thunk pushed one word: Linux C wants 16 */
#endif
void __cdecl gmv_missing_import(uint32_t index, uint32_t ret)
{
    const char *name = active && index < (uint32_t)active->n ? active->imp[index].name : "?";
    gmv_log("\n[MISSING] game called unimplemented import %s (return address 0x%08x)\n", name, ret);
    gmv_log("stopping: this import needs a shim before the game can get further\n");
    gmv_fatal(3, "The game called something this port does not have yet: %s.", name);
}
