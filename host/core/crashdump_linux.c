/* crashdump_linux.c -- cache/GangstarMV.dmp on Linux: crashdump.c's counterpart, written as an ELF core file, which
   gdb opens with the program (gdb GangstarMV GangstarMV.dmp).

   It holds what the Windows dump holds: every thread's registers and stack, and the files mapped into the process by
   name only, with no folder.  The other threads are stopped by a signal while the file is written and let go after
   it, as MiniDumpWriteDump suspends and resumes them; a thread that blocks the signal (Mesa's) is left out.  All of it
   runs in the faulting thread's signal handler, so it keeps to system calls and static buffers: no malloc, no stdio.
   Memory goes to the file through write(), which answers EFAULT for a page it cannot read instead of faulting.
   Before the file is kept, every copy of the folders the log hides, of the environment's values and of the machine's
   name is blanked (log_scrub_bytes).  LOG=0 means no dump either. */
#ifndef _WIN32
#define _GNU_SOURCE
#include <elf.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/procfs.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "layout.h"
#include "log.h"
#include "platform.h"
#include "veh.h"

enum {
    MAX_THREADS = 64,
    MAX_MAPS = 4096,
    STACK_CAP = 1 << 20,        /* of each thread's stack, from its esp up */
    WAIT_MS = 500,              /* for the other threads to stop */
    PAGE = 4096,
};

static char dump_path[GMV_PATH_MAX];    /* empty: no dump this run */

/* The fault the dump is about (gmv_dump_note_signal); without one the writing thread's own registers stand in. */
static volatile int noted;
static int note_sig;
static pid_t note_tid;
static siginfo_t note_si;
static greg_t note_regs[NGREG];

static struct thread {
    pid_t tid;
    volatile int got;
    greg_t regs[NGREG];
} thr[MAX_THREADS];
static volatile int nthr, let_go;

static struct map {
    uint32_t start, end, off;
    int readable;
    const char *name;           /* the file's name without its folder, or NULL */
    size_t name_len;
} maps[MAX_MAPS];
static int nmaps;

static char maps_text[512 * 1024];
static unsigned char notes[256 * 1024];
static size_t notes_len;
static Elf32_Phdr ph[1 + MAX_THREADS];

static pid_t my_tid(void)
{
    return (pid_t)syscall(SYS_gettid);
}

void gmv_dump_arm(const char *path)
{
    snprintf(dump_path, sizeof dump_path, "%s", path);
}

void gmv_dump_note_signal(int sig, const void *si, const void *uc)
{
    if (!__sync_bool_compare_and_swap(&noted, 0, 1))
        return;
    note_sig = sig;
    note_tid = my_tid();
    memcpy(&note_si, si, sizeof note_si);
    memcpy(note_regs, ((const ucontext_t *)uc)->uc_mcontext.gregs, sizeof note_regs);
}

int gmv_dump_is_writer(void)
{
    return 0;                   /* the faulting thread writes it, and write() does not fault */
}

/* ---- stopping the other threads ---- */

static void on_stop(int sig, siginfo_t *si, void *context)
{
    (void)sig;
    (void)si;
    pid_t me = my_tid();
    for (int i = 0; i < nthr; i++)
        if (thr[i].tid == me) {
            memcpy(thr[i].regs, ((ucontext_t *)context)->uc_mcontext.gregs, sizeof thr[i].regs);
            __sync_synchronize();
            thr[i].got = 1;
            break;
        }
    struct timespec ms = {0, 1000000};
    while (!let_go)
        nanosleep(&ms, NULL);
}

static void list_threads(pid_t me)
{
    struct dirent64_ {
        uint64_t ino;
        int64_t off;
        unsigned short reclen;
        unsigned char type;
        char name[];
    };
    static char buf[8192];
    nthr = 1;
    thr[0].tid = me;
    int fd = open("/proc/self/task", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return;
    long n;
    while ((n = syscall(SYS_getdents64, fd, buf, sizeof buf)) > 0)
        for (long o = 0; o < n;) {
            const struct dirent64_ *d = (const void *)(buf + o);
            o += d->reclen;
            pid_t t = 0;
            for (const char *c = d->name; *c >= '0' && *c <= '9'; c++)
                t = t * 10 + (*c - '0');
            if (t > 0 && t != me && nthr < MAX_THREADS)
                thr[nthr++].tid = t;
        }
    close(fd);
}

/* Each other thread records its registers in its on_stop and waits there until let_go. */
static void stop_threads(void)
{
    int sig = SIGRTMAX - 2;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_stop;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigfillset(&sa.sa_mask);
    if (sigaction(sig, &sa, NULL))
        return;
    pid_t pid = getpid();
    for (int i = 1; i < nthr; i++)
        if (syscall(SYS_tgkill, pid, thr[i].tid, sig))
            thr[i].tid = 0;     /* gone meanwhile */
    struct timespec ms = {0, 1000000};
    for (int t = 0; t < WAIT_MS; t++) {
        int waiting = 0;
        for (int i = 1; i < nthr; i++)
            waiting += thr[i].tid && !thr[i].got;
        if (!waiting)
            break;
        nanosleep(&ms, NULL);
    }
}

/* ---- the process's memory map ---- */

static uint32_t hex(const char **p)
{
    uint32_t v = 0;
    for (;; (*p)++) {
        char c = **p;
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0)
            return v;
        v = v << 4 | (uint32_t)d;
    }
}

static void read_maps(void)
{
    nmaps = 0;
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    size_t len = 0;
    ssize_t n;
    while (len < sizeof maps_text - 1 && (n = read(fd, maps_text + len, sizeof maps_text - 1 - len)) > 0)
        len += (size_t)n;
    close(fd);
    maps_text[len] = 0;
    for (char *line = maps_text; *line && nmaps < MAX_MAPS;) {
        char *eol = strchr(line, '\n');
        if (!eol)
            break;                          /* a line cut off by the buffer's end */
        *eol = 0;
        const char *p = line;
        struct map *m = &maps[nmaps];
        m->start = hex(&p);
        p++;                                /* '-' */
        m->end = hex(&p);
        p++;
        m->readable = *p == 'r';
        p += 5;                             /* "rwxp " */
        m->off = hex(&p);
        for (int field = 0; field < 2 && *p; field++) {     /* the device and the inode */
            while (*p == ' ')
                p++;
            while (*p && *p != ' ')
                p++;
        }
        while (*p == ' ')
            p++;
        m->name = NULL;
        m->name_len = 0;
        if (*p == '/') {                    /* a file: its name only */
            const char *b = strrchr(p, '/') + 1;
            m->name = b;
            m->name_len = strlen(b);
        }
        nmaps++;
        line = eol + 1;
    }
}

/* ---- the file ---- */

static void note(uint32_t type, const void *desc, size_t len)
{
    size_t need = 12 + 8 + ((len + 3) & ~(size_t)3);
    if (notes_len + need > sizeof notes)
        return;
    uint32_t h[3] = {5, (uint32_t)len, type};
    memcpy(notes + notes_len, h, 12);
    memcpy(notes + notes_len + 12, "CORE\0\0\0", 8);
    memcpy(notes + notes_len + 20, desc, len);
    memset(notes + notes_len + 20 + len, 0, need - 20 - len);
    notes_len += need;
}

static void prstatus(pid_t tid, int sig, const greg_t *g)
{
    struct elf_prstatus ps;
    memset(&ps, 0, sizeof ps);
    ps.pr_info.si_signo = sig;
    ps.pr_cursig = (short)sig;
    ps.pr_pid = tid;
    ps.pr_ppid = getppid();
    elf_greg_t *r = ps.pr_reg;     /* user_regs_struct's order */
    r[0] = g[REG_EBX];
    r[1] = g[REG_ECX];
    r[2] = g[REG_EDX];
    r[3] = g[REG_ESI];
    r[4] = g[REG_EDI];
    r[5] = g[REG_EBP];
    r[6] = g[REG_EAX];
    r[7] = g[REG_DS];
    r[8] = g[REG_ES];
    r[9] = g[REG_FS];
    r[10] = g[REG_GS];
    r[11] = -1;                     /* orig_eax: not in a system call */
    r[12] = g[REG_EIP];
    r[13] = g[REG_CS];
    r[14] = g[REG_EFL];
    r[15] = g[REG_ESP];
    r[16] = g[REG_SS];
    note(NT_PRSTATUS, &ps, sizeof ps);
}

static void prpsinfo(void)
{
    struct elf_prpsinfo pi;
    memset(&pi, 0, sizeof pi);
    pi.pr_sname = 'R';
    pi.pr_pid = getpid();
    pi.pr_ppid = getppid();
    memcpy(pi.pr_fname, GMV_GAME_NAME, sizeof GMV_GAME_NAME < sizeof pi.pr_fname ? sizeof GMV_GAME_NAME : sizeof pi.pr_fname - 1);
    memcpy(pi.pr_psargs, GMV_GAME_NAME, sizeof GMV_GAME_NAME < sizeof pi.pr_psargs ? sizeof GMV_GAME_NAME : sizeof pi.pr_psargs - 1);
    note(NT_PRPSINFO, &pi, sizeof pi);
}

/* NT_FILE: the mapped files, each by its name without the folder. */
static void file_note(void)
{
    static unsigned char d[128 * 1024];
    uint32_t count = 0;
    for (int i = 0; i < nmaps; i++)
        count += maps[i].name != NULL;
    size_t at = 8 + 12 * (size_t)count;
    if (at > sizeof d)
        return;
    uint32_t head[2] = {count, PAGE};
    memcpy(d, head, 8);
    size_t w = 8;
    for (int i = 0; i < nmaps; i++) {
        if (!maps[i].name)
            continue;
        uint32_t e[3] = {maps[i].start, maps[i].end, maps[i].off / PAGE};
        memcpy(d + w, e, 12);
        w += 12;
        if (at + maps[i].name_len + 1 > sizeof d)
            return;
        memcpy(d + at, maps[i].name, maps[i].name_len + 1);
        at += maps[i].name_len + 1;
    }
    note(NT_FILE, d, at);
}

/* The readable stretch of a thread's stack from esp up, or 0 if esp is in no readable mapping. */
static int stack_of(uint32_t esp, uint32_t *lo, uint32_t *hi)
{
    for (int i = 0; i < nmaps; i++)
        if (maps[i].readable && esp >= maps[i].start && esp < maps[i].end) {
            *lo = esp & ~(uint32_t)(PAGE - 1);
            *hi = maps[i].end - *lo > STACK_CAP ? *lo + STACK_CAP : maps[i].end;
            return 1;
        }
    return 0;
}

static int write_all(int fd, const void *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0)
            return 0;
        p = (const char *)p + w;
        n -= (size_t)w;
    }
    return 1;
}

/* Memory into the file a page at a time; a page that cannot be read becomes zeros. */
static int write_memory(int fd, uint32_t lo, uint32_t hi)
{
    static const unsigned char zero[PAGE];
    for (uint32_t a = lo; a < hi; a += PAGE) {
        ssize_t w = write(fd, (const void *)(uintptr_t)a, PAGE);
        if (w < 0)
            w = 0;
        if (w < PAGE && !write_all(fd, zero, (size_t)(PAGE - w)))
            return 0;
    }
    return 1;
}

static const char *write_file(int *threads)
{
    const greg_t *regs[MAX_THREADS];
    int sig[MAX_THREADS];
    *threads = 0;
    for (int i = 0; i < nthr; i++) {
        regs[i] = noted && thr[i].tid == note_tid ? note_regs : i == 0 || thr[i].got ? thr[i].regs : NULL;
        sig[i] = noted && thr[i].tid == note_tid ? note_sig : 0;
    }

    /* The notes: the first thread's with the process's, then the others' registers. */
    notes_len = 0;
    int nph = 1;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < nthr; i++) {
            int first = noted ? thr[i].tid == note_tid : i == 0;
            if (!regs[i] || first != !pass)
                continue;
            prstatus(thr[i].tid, sig[i], regs[i]);
            ++*threads;
            if (first) {
                prpsinfo();
                if (noted)
                    note(NT_SIGINFO, &note_si, sizeof note_si);
                file_note();
            }
        }

    /* The stacks, each its own segment, page-aligned in the file. */
    uint32_t off = (uint32_t)(sizeof(Elf32_Ehdr) + sizeof ph[0] * (1 + (size_t)nthr) + notes_len);
    off = (off + PAGE - 1) & ~(uint32_t)(PAGE - 1);
    for (int i = 0; i < nthr; i++) {
        uint32_t lo, hi;
        if (!regs[i] || !stack_of((uint32_t)regs[i][REG_ESP], &lo, &hi))
            continue;
        Elf32_Phdr *p = &ph[nph++];
        memset(p, 0, sizeof *p);
        p->p_type = PT_LOAD;
        p->p_offset = off;
        p->p_vaddr = lo;
        p->p_filesz = p->p_memsz = hi - lo;
        p->p_flags = PF_R | PF_W;
        p->p_align = PAGE;
        off += hi - lo;
    }
    memset(&ph[0], 0, sizeof ph[0]);
    ph[0].p_type = PT_NOTE;
    ph[0].p_offset = (uint32_t)(sizeof(Elf32_Ehdr) + sizeof ph[0] * (size_t)nph);
    ph[0].p_filesz = (uint32_t)notes_len;

    Elf32_Ehdr eh;
    memset(&eh, 0, sizeof eh);
    memcpy(eh.e_ident, ELFMAG, SELFMAG);
    eh.e_ident[EI_CLASS] = ELFCLASS32;
    eh.e_ident[EI_DATA] = ELFDATA2LSB;
    eh.e_ident[EI_VERSION] = EV_CURRENT;
    eh.e_ident[EI_OSABI] = ELFOSABI_NONE;
    eh.e_type = ET_CORE;
    eh.e_machine = EM_386;
    eh.e_version = EV_CURRENT;
    eh.e_phoff = sizeof eh;
    eh.e_ehsize = sizeof eh;
    eh.e_phentsize = sizeof ph[0];
    eh.e_phnum = (Elf32_Half)nph;

    int fd = open(dump_path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return "cannot create the file";
    const char *err = NULL;
    if (!write_all(fd, &eh, sizeof eh) || !write_all(fd, ph, sizeof ph[0] * (size_t)nph) || !write_all(fd, notes, notes_len))
        err = "cannot write the file";
    for (int k = 1; k < nph && !err; k++)
        if (lseek(fd, (off_t)ph[k].p_offset, SEEK_SET) < 0
            || !write_memory(fd, ph[k].p_vaddr, ph[k].p_vaddr + ph[k].p_filesz))
            err = "cannot write the file";
    if (!err) {     /* a file that still names the folders is not kept */
        off_t size = lseek(fd, 0, SEEK_END);
        unsigned char *d = size > 0 ? mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
        if (d == MAP_FAILED)
            err = "cannot clean the file";
        else {
            log_scrub_bytes(d, (size_t)size);
            if (munmap(d, (size_t)size))
                err = "cannot clean the file";
        }
    }
    if (close(fd))
        err = err ? err : "cannot write the file";
    return err;
}

int gmv_dump_write(void)
{
    if (!dump_path[0])
        return 0;
    pid_t me = my_tid();
    list_threads(me);
    if (!noted || note_tid != me) {     /* the writing thread's own registers */
        ucontext_t uc;
        if (getcontext(&uc) == 0)
            memcpy(thr[0].regs, uc.uc_mcontext.gregs, sizeof thr[0].regs);
    }
    let_go = 0;
    stop_threads();
    read_maps();
    int threads;
    const char *err = write_file(&threads);
    let_go = 1;
    if (err) {
        unlink(dump_path);
        gmv_log("[crash] no dump: %s\n", err);
        return 0;
    }
    gmv_log("[crash] wrote cache" GMV_SEP GMV_GAME_NAME ".dmp (%d of %d threads)\n", threads, nthr);
    return 1;
}
#endif
