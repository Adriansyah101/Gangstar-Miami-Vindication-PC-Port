/* main.c -- the host for a macOS i386 game; layout.h names the game.

   Stage 1: load and verify.  The game image is mapped at its own addresses, the low range
   is copied and patched, every import is bound (to shims, or to stand-ins that report the
   name if reached), and everything is cross-checked.  Game code is never executed.

   usage: <game>.exe [--app <path to <game>.app>] */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"
#include "imports.h"
#include "layout.h"
#include "log.h"
#include "macho.h"
#include "platform.h"
#include "runinit.h"
#include "shims.h"
#include "veh.h"

static int failures;

static int check(int ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int check(int ok, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (GMV_DEV || !ok)
        gmv_log("[%s] %s\n", ok ? " ok " : "FAIL", buf);
    if (!ok)
        failures++;
    return ok;
}

static uint8_t *read_file(const char *path, uint32_t *size)
{
    FILE *f = gmv_fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t *buf = NULL;
    long sz = 0;
    if (!fseek(f, 0, SEEK_END) && (sz = ftell(f)) > 0 && sz < 0x10000000 && !fseek(f, 0, SEEK_SET) &&
        (buf = malloc((size_t)sz)) && fread(buf, 1, (size_t)sz, f) == (size_t)sz) {
        *size = (uint32_t)sz;
    } else {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

/* A <game>.app with the game in it: an empty one (the release's placeholder) does not count. */
static int is_app(const char *p)
{
    char exe[GMV_PATH_MAX + 64];
    snprintf(exe, sizeof exe, "%s" GMV_SEP "Contents" GMV_SEP "MacOS" GMV_SEP GMV_GAME_NAME, p);
    return gmv_is_file(exe);
}

static int find_app(int argc, char **argv, const char *exe_dir, char *app, size_t n)
{
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--app")) {
            snprintf(app, n, "%s", argv[i + 1]);
            return is_app(app);
        }
    static const char *const rel[] = {
        GMV_GAME_NAME ".app",
        ".." GMV_SEP GMV_GAME_NAME ".app",                                          /* project root: gmv-win32\GangstarMV.app */
#if GMV_DEV
        GMV_GAME_DEV_APP,
#endif
    };
    for (size_t i = 0; i < sizeof rel / sizeof *rel; i++) {
        char tmp[GMV_PATH_MAX + 128];
        snprintf(tmp, sizeof tmp, "%s" GMV_SEP "%s", exe_dir, rel[i]);
        if (gmv_full_path(tmp, app, n) && is_app(app))
            return 1;
    }
    return 0;
}

/* Say why it stops before the game's window. */
static void tell_user(const char *msg)
{
    gmv_alert(msg, 0);
}

/* ---- weak-bind consistency: with no slide, slots must already hold the game's own copy -- */

typedef struct {
    const macho *m;
    const gmvp *t;
    uint32_t total, ok, external, strong, bad;
} weak_ctx;

static int weak_cb(void *ctx, uint32_t slot, const char *name, int ordinal, int32_t addend, int flags)
{
    weak_ctx *w = ctx;
    (void)ordinal;
    if (flags & BINDF_STRONG_DEF) {
        w->strong++;
        return 0;
    }
    w->total++;
    uint32_t def = macho_lookup(w->m, name);
    if (!def) {                         /* a weak reference into a dylib: bound normally */
        w->external++;
        return 0;
    }
    uint32_t want = def + (uint32_t)addend, have = *(const uint32_t *)(uintptr_t)image_code_addr(w->t, slot);
    if (have == want || have == image_code_addr(w->t, want))
        w->ok++;
    else if (w->bad++ < 5)
        gmv_log("       weak slot 0x%06x %s: holds 0x%08x, definition is 0x%08x\n", slot, name, have, want);
    return 0;
}

/* ---- main ----------------------------------------------------------------------------- */

/* _main is stripped.  crt1's start builds argc/argv/envp/apple and then does
       call _main ; mov [esp], eax ; call _exit
   so the first call followed by that sequence names main. */
static uint32_t find_main(const macho *m)
{
    static const uint8_t prologue[] = {0x6A, 0x00, 0x89, 0xE5, 0x83, 0xE4, 0xF0};   /* push 0; mov ebp,esp; and esp,-16 */
    const uint8_t *e = macho_bytes(m, m->entry, 64);
    if (!e || memcmp(e, prologue, sizeof prologue))
        return 0;
    for (int i = 0; i + 13 <= 64; i++)
        if (e[i] == 0xE8 && !memcmp(e + i + 5, "\x89\x04\x24\xE8", 4))
            return m->entry + i + 5 + *(const int32_t *)(e + i + 1);
    return 0;
}

/* Which import a __symbol_stub entry (jmp [slot]) is bound to, after binding. */
static const char *stub_import(const import_table *it, uint32_t stub)
{
    if (stub < GMV_RESERVE_START || stub + 6 > GMV_MACHO_END)
        return "?";
    const uint8_t *p = (const uint8_t *)(uintptr_t)stub;
    if (p[0] != 0xFF || p[1] != 0x25)
        return "?";
    uint32_t slot = *(const uint32_t *)(p + 2);
    if (slot < GMV_RESERVE_START || slot + 4 > GMV_MACHO_END)
        return "?";
    uint32_t v = *(const uint32_t *)(uintptr_t)slot;
    for (int i = 0; i < it->n; i++)
        if (it->imp[i].how != IMP_WEAK_NULL && it->imp[i].target == v)
            return it->imp[i].name;
    return "?";
}

/* ---- protections ---------------------------------------------------------------------- */

static int protect(uint32_t lo, uint32_t hi, int prot, const char *what)
{
    lo &= ~0xFFFu;
    hi = (hi + 0xFFF) & ~0xFFFu;
    if (hi <= lo)
        return 1;
    return check(gmv_protect(lo, hi - lo, prot), "protect %-34s 0x%06x..0x%06x", what, lo, hi);
}

static int apply_protections(const macho *m, const gmvp *t)
{
    int ok = protect(GMV_LOWCOPY_BASE, GMV_LOWCOPY_BASE + (t->low_end - t->low_start),
                     GMV_PROT_RX, "low copy (code) r-x");
    for (int i = 0; i < m->nsegs; i++) {
        const macho_seg *s = &m->segs[i];
        if (!strcmp(s->name, "__PAGEZERO"))
            continue;
        uint32_t lo = s->vmaddr < GMV_RESERVE_START ? GMV_RESERVE_START : s->vmaddr;
        int prot = s->initprot == 5 ? GMV_PROT_RX : s->initprot == 3 ? GMV_PROT_RW :
                   s->initprot == 1 ? GMV_PROT_R : GMV_PROT_NONE;
        char what[64];
        snprintf(what, sizeof what, "%s (initprot %u)", s->name, s->initprot);
        ok &= protect(lo, s->vmaddr + s->vmsize, prot, what);
    }
    return ok;
}

void gmv_snd_init(void);
void gmv_joy_start(void);
void gmv_video_start(void);

int gmv_main(int argc, char **argv)
{
    char exe_dir[GMV_PATH_MAX], path[GMV_PATH_MAX + 64], app[GMV_PATH_MAX];
    char err[256] = "";
    gmv_exe_dir(exe_dir, sizeof exe_dir);
    log_hide_path(exe_dir, "%EXE%");
    snprintf(path, sizeof path, "%s" GMV_SEP "cache", exe_dir);
    gmv_mkdir(path);                        /* the game's scratch folder too (shimsappkit.c) */
    snprintf(path, sizeof path, "%s" GMV_SEP "cache" GMV_SEP GMV_GAME_NAME ".log", exe_dir);
    gmv_config_load(exe_dir);
    gmv_platform_early();
    const char *lg = gmv_getenv("GMV_LOG");
    int log_on = !(lg && *lg == '0');
    if (!log_on) {
        log_discard();
        gmv_remove(path);                   /* no stale log from an earlier run either */
    } else
        log_open(path);
    snprintf(path, sizeof path, "%s" GMV_SEP "cache" GMV_SEP GMV_GAME_NAME ".dmp", exe_dir);
    gmv_remove(path);                       /* an earlier crash's: the new log would not match it */
    if (log_on)
        gmv_dump_arm(path);
    gmv_shims_init();
    gmv_log(GMV_GAME_NAME " " GMV_PLATFORM_NAME " port %s\n\n", GMV_VERSION);
    gmv_platform_log_start();

    /* ---- 1. address space ---- */
    gmv_platform_address_space(check);
    if (failures) {
        gmv_log("\nthe address-space layout is wrong; nothing else can be trusted\n");
        tell_user(GMV_PLATFORM_OS " did not load " GMV_GAME_NAME GMV_EXE_SUFFIX " the way it needs (see cache" GMV_SEP
                  GMV_GAME_NAME ".log; LOG=1 in savegame" GMV_SEP GMV_GAME_NAME ".ini writes it).");
        return 1;
    }

    /* ---- 2. game files ---- */
    if (!check(find_app(argc, argv, exe_dir, app, sizeof app), "found " GMV_GAME_NAME ".app")) {
        gmv_log("       put " GMV_GAME_NAME ".app next to the exe, or pass --app <path>\n");
        tell_user("The game was not found.\n\nPut your own copy of the Mac game's " GMV_GAME_NAME ".app folder next to "
                  GMV_GAME_NAME GMV_EXE_SUFFIX " (in place of the empty " GMV_GAME_NAME ".app folder that comes with it), so that "
                  GMV_GAME_NAME ".app" GMV_SEP "Contents" GMV_SEP "MacOS" GMV_SEP GMV_GAME_NAME " is there, then start "
                  GMV_GAME_NAME GMV_EXE_SUFFIX " again.");
        return 1;
    }
    log_hide_path(app, "%APP%");
    gmv_fnd_set_bundle_path(app);           /* so NSBundle paths point at the real .app */
    uint32_t msize = 0, psize = 0;
    snprintf(path, sizeof path, "%s" GMV_SEP "Contents" GMV_SEP "MacOS" GMV_SEP GMV_GAME_NAME, app);
    uint8_t *mdata = read_file(path, &msize);
    if (!check(mdata != NULL, "read Contents" GMV_SEP "MacOS" GMV_SEP GMV_GAME_NAME " (%u bytes)", msize))
        return 1;
    macho m;
    if (!check(macho_parse(&m, mdata, msize, err, sizeof err) == 0, "Mach-O header and load commands %s", err))
        return 1;
    gmv_trace("       i386 executable, entry 0x%06x, %d segments, %d sections, %u symbols, %d dylibs\n",
            m.entry, m.nsegs, m.nsects, m.nsyms, m.ndylibs);
    for (int i = 0; i < m.nsegs; i++)
        gmv_trace("       %-12s 0x%08x..0x%08x  file 0x%07x+0x%07x  prot %u\n", m.segs[i].name, m.segs[i].vmaddr,
                m.segs[i].vmaddr + m.segs[i].vmsize, m.segs[i].fileoff, m.segs[i].filesize, m.segs[i].initprot);
    snprintf(path, sizeof path, "%s" GMV_SEP "gmv_patch.bin", exe_dir);
    uint8_t *pdata = read_file(path, &psize);
    if (!check(pdata != NULL, "read gmv_patch.bin next to the exe (%u bytes)", psize)) {
        tell_user("gmv_patch.bin is missing: keep it in the same folder as " GMV_GAME_NAME GMV_EXE_SUFFIX ".");
        return 1;
    }
    gmvp t;
    if (!check(gmvp_parse(&t, pdata, psize, &m, err, sizeof err) == 0, "patch table matches this binary %s", err))
        return 1;
    gmv_trace("       low range 0x%06x..0x%06x runs from 0x%06x, %u patches\n",
            t.low_start, t.low_end, GMV_LOWCOPY_BASE, t.npatch);

    /* ---- 3. exception handler ---- */
    const macho_sect *text = macho_sect_named(&m, "__TEXT", "__text");
    check(veh_install(text->addr, t.low_end) == 0, "exception handler installed");
    check(veh_selftest() == 0, "exception handler redirects a faulting jump (host-only self-test)");

    /* ---- 4. map and patch ---- */
    image_stats st;
    if (!check(image_copy(&m, &t, &st, err, sizeof err) == 0, "segments copied %s", err))
        return 1;
    gmv_trace("       %u bytes at their own addresses, %u bytes into the low copy\n", st.mapped_bytes, st.copied_low_bytes);
    uint32_t diff = image_compare(&m, &t);
    check(diff == 0, "mapped image is byte-identical to the file (%u bytes differ)", diff);
    if (!check(image_patch(&m, &t, &st, err, sizeof err) == 0, "low-range patches verified and applied %s", err))
        return 1;
    gmv_trace("       REL32_OUT %u, PIC_POP %u, REL32_IN %u, PTR_IN %u\n", st.applied[GMVP_REL32_OUT],
            st.applied[GMVP_PIC_POP], st.applied[GMVP_REL32_IN], st.applied[GMVP_PTR_IN]);

    weak_ctx w = {&m, &t, 0, 0, 0, 0, 0};
    int wr = macho_binds(&m, BIND_WEAK, weak_cb, &w, err, sizeof err);
    check(wr == 0 && w.bad == 0 && w.ok + w.external == w.total,
          "weak binds already point at the game's own definitions (%u ok, %u bad, %u external, %u strong defs) %s",
          w.ok, w.bad, w.external, w.strong, err);

    /* ---- 5. imports ---- */
    import_table it;
    if (!check(imports_collect(&m, &it, err, sizeof err) == 0, "bind streams decoded %s", err))
        return 1;
    gmv_trace("       %d imported symbols, %u bind slots\n", it.n, it.nslots);
    if (!check(imports_bind(&m, &it, err, sizeof err) == 0, "every bind slot written %s", err))
        return 1;
    gmv_trace("       implemented %u, missing code %u (thunk reports the name), missing data %u (no-access stand-in), weak->NULL %u\n",
            it.count[IMP_HOST], it.count[IMP_MISSING_CODE], it.count[IMP_MISSING_DATA], it.count[IMP_WEAK_NULL]);
#if GMV_DEV
    gmv_log("       by library:\n");
    for (int lib = 1; lib <= m.ndylibs; lib++) {
        int n = 0, impl = 0, data = 0;
        for (int i = 0; i < it.n; i++)
            if (it.imp[i].ordinal == lib) {
                n++;
                impl += it.imp[i].how == IMP_HOST;
                data += it.imp[i].is_data;
            }
        if (n)
            gmv_log("         %-28s %4d (%d data, %d implemented)\n", imports_lib_name(&m, lib), n, data, impl);
    }
    gmv_logfile("\nimport list (status, slots lazy/nl/data, library, name):\n");
    static const char *const how[] = {"host", "MISSING", "MISSING-DATA", "weak-null"};
    for (int i = 0; i < it.n; i++) {
        const import *im = &it.imp[i];
        gmv_logfile("  %-12s %3d/%3d/%4d  %-24s %s\n", how[im->how], im->lazy_slots, im->nl_slots,
                    im->data_slots, imports_lib_name(&m, im->ordinal), im->name);
    }
    gmv_logfile("\n");
#endif

    /* ---- 6. what stage 2 starts from ---- */
    uint32_t main_addr = find_main(&m), exit_stub = 0;
    check(m.entry - text->addr < text->size, "entry point 0x%06x is in __text (runs at 0x%06x)",
          m.entry, image_code_addr(&t, m.entry));
    if (main_addr) {
        const uint8_t *e = macho_bytes(&m, m.entry, 64);
        for (int i = 0; i + 13 <= 64; i++)
            if (e[i] == 0xE8 && !memcmp(e + i + 5, "\x89\x04\x24\xE8", 4)) {
                exit_stub = m.entry + i + 13 + *(const int32_t *)(e + i + 9);
                break;
            }
    }
    const char *exit_name = stub_import(&it, exit_stub);
    check(main_addr - text->addr < text->size && !strcmp(exit_name, "_exit"),
          "main found through crt1 start: 0x%06x (runs at 0x%06x), then stub 0x%06x -> %s",
          main_addr, image_code_addr(&t, main_addr), exit_stub, exit_name);
    const macho_sect *init = macho_sect_named(&m, "__DATA", "__mod_init_func");
    uint32_t ninit = init ? init->size / 4 : 0, bad_init = 0;
    for (uint32_t i = 0; i < ninit; i++) {
        uint32_t f = ((const uint32_t *)(uintptr_t)init->addr)[i];
        uint32_t orig = f - GMV_LOWCOPY_BASE < GMV_LOWCOPY_CAP ? f - GMV_LOW_DELTA : f;
        bad_init += !(orig - text->addr < text->size) || image_code_addr(&t, orig) != f;
    }
    check(bad_init == 0, "%u static initializers all point at runnable game code", ninit);

    /* ---- 7. final protections ---- */
    apply_protections(&m, &t);

    if (GMV_DEV)
        gmv_log("\n");
    if (failures) {
        gmv_log("STAGE 1 FAILED: %d check(s) failed (see above)\n", failures);
        tell_user(GMV_GAME_NAME " could not load the game: its files do not match what this port expects "
                  "(see cache" GMV_SEP GMV_GAME_NAME ".log; LOG=1 in savegame" GMV_SEP GMV_GAME_NAME ".ini writes it).");
    } else {
#if GMV_DEV
        gmv_log("STAGE 1 PASSED: the game image is loaded at its own addresses, %u patches applied,\n"
                "%u bind slots written (%u of %d imports implemented so far).\n",
                t.npatch, it.nslots, it.count[IMP_HOST], it.n);
#else
        gmv_log("the game is loaded (%u patches, %u of %d imports)\n", t.npatch, it.count[IMP_HOST], it.n);
#endif
    }

    /* ---- 8. run game code.  Default (no flags, e.g. a double-click) = run the game.  Pass
       --verify-only to stop after the load+verify above (the old default), --run-init [N] to run
       only the first N static initializers. ---- */
    int want_init = 0, want_main = 0, verify_only = 0;
    uint32_t init_limit = 0;                /* 0 = all */
    for (int i = 1; GMV_DEV && i < argc; i++) {
        if (!strcmp(argv[i], "--run-init")) {
            want_init = 1;
            if (i + 1 < argc) {
                char *end = NULL;
                unsigned long v = strtoul(argv[i + 1], &end, 10);
                if (end && *end == 0)
                    init_limit = (uint32_t)v;
            }
        } else if (!strcmp(argv[i], "--run-main")) {
            want_main = 1;
        } else if (!strcmp(argv[i], "--verify-only") || !strcmp(argv[i], "--no-run")) {
            verify_only = 1;
        }
    }
    if (!want_init && !want_main && !verify_only)
        want_main = 1;                      /* no mode given (double-click) -> launch the game */
    if (!failures && (want_init || want_main)) {
        char err2[256] = "";
        gmv_video_start();
        gmv_snd_init();
        gmv_joy_start();
        /* main() needs every static initializer to have run first, so --run-main forces all. */
        uint32_t lim = want_main ? 0 : init_limit;
        if (run_initializers(&m, &t, lim, err2, sizeof err2) != 0) {
            gmv_log("stage 2d: run_initializers failed: %s\n", err2);
            gmv_fatal(1, "The game could not start: %s.", err2);
        } else if (want_main && run_main(&m, &t, main_addr, err2, sizeof err2) != 0) {
            gmv_log("stage 3: run_main failed: %s\n", err2);
            gmv_fatal(1, "The game could not start: %s.", err2);
        }
    }

    gmv_trace("log: cache" GMV_SEP GMV_GAME_NAME ".log\n");
    log_close();
    return failures ? 1 : 0;
}
