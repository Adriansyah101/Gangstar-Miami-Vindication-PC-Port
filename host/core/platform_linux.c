/* platform_linux.c -- platform.h on Linux.

   The exe is an ordinary non-PIE i386 program, linked well above the game's range, so nothing has to be done at link
   time as on Windows: the first thing gmv_main does is map the range.  Below 0x10000 nothing can be mapped under the
   default vm.mmap_min_addr, as on Windows, so the same layout and the same gmv_patch.bin serve both. */
#ifndef _WIN32
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#include "layout.h"
#include "log.h"
#include "platform.h"

int main(int argc, char **argv)
{
    return gmv_main(argc, argv);
}

/* ---- files ---- */

void gmv_exe_dir(char *out, size_t n)
{
    ssize_t got = readlink("/proc/self/exe", out, n - 1);
    if (got <= 0)
        got = 0;
    out[got] = 0;
    char *s = strrchr(out, '/');
    if (s)
        *s = 0;
    else
        snprintf(out, n, ".");
}

FILE *gmv_fopen(const char *path, const char *mode)
{
    return fopen(path, mode);
}

int gmv_mkdir(const char *path)
{
    return !mkdir(path, 0755) || errno == EEXIST;
}

int gmv_remove(const char *path)
{
    return !unlink(path);
}

int gmv_rename(const char *from, const char *to)
{
    struct stat st;
    if (!stat(to, &st)) {
        errno = EEXIST;
        return 0;
    }
    return !rename(from, to);
}

int gmv_replace(const char *from, const char *to)
{
    return !rename(from, to);
}

int gmv_is_file(const char *path)
{
    struct stat st;
    return !stat(path, &st) && !S_ISDIR(st.st_mode);
}

int gmv_full_path(const char *path, char *out, size_t n)
{
    char full[PATH_MAX];
    if (!realpath(path, full) || strlen(full) >= n)
        return 0;
    strcpy(out, full);
    return 1;
}

unsigned long gmv_last_error(void)
{
    return (unsigned long)errno;
}

/* ---- memory ---- */

int gmv_protect(uint32_t addr, uint32_t size, int prot)
{
    static const int unix_prot[] = {PROT_NONE, PROT_READ, PROT_READ | PROT_WRITE, PROT_READ | PROT_EXEC};
    return !mprotect((void *)(uintptr_t)addr, size, unix_prot[prot]);
}

/* The kernel does the reading and answers with an error instead of a fault. */
int gmv_readable(const void *p, size_t n)
{
    static __thread char buf[4096];
    const char *a = p;
    while (n) {
        size_t k = n < sizeof buf ? n : sizeof buf;
        struct iovec to = {buf, k}, from = {(void *)a, k};
        if (process_vm_readv(getpid(), &to, 1, &from, 1, 0) != (ssize_t)k)
            return 0;
        a += k;
        n -= k;
    }
    return 1;
}

void gmv_secure_zero(void *p, size_t n)
{
    explicit_bzero(p, n);
}

int gmv_module_of(uint32_t addr, char *name, size_t n, uint32_t *base)
{
    Dl_info info;
    if (!dladdr((const void *)(uintptr_t)addr, &info) || !info.dli_fname)
        return 0;
    snprintf(name, n, "%s", info.dli_fname);
    *base = (uint32_t)(uintptr_t)info.dli_fbase;
    return 1;
}

/* ---- threads and the process ---- */

uint32_t gmv_thread_id(void)
{
    return (uint32_t)syscall(SYS_gettid);
}

static void *thread_start(void *fn)
{
    ((void (*)(void))fn)();
    return NULL;
}

int gmv_thread_run(void (*fn)(void), int wait)
{
    pthread_t t;
    if (pthread_create(&t, NULL, thread_start, (void *)fn))
        return 0;
    if (wait)
        pthread_join(t, NULL);
    else
        pthread_detach(t);
    return 1;
}

void gmv_sleep_forever(void)
{
    for (;;)
        pause();
}

void gmv_exit(int code)
{
    _exit(code);
}

/* ---- the user ---- */

/* SDL when it is linked in: the loader on its own (host/linux/build_stage1.sh) has only the console. */
extern bool SDL_ShowSimpleMessageBox(unsigned flags, const char *title, const char *message, void *window)
    __attribute__((weak));

void gmv_alert(const char *text, int on_top)
{
    (void)on_top;
    fprintf(stderr, "%s\n", text);
    if (SDL_ShowSimpleMessageBox)
        SDL_ShowSimpleMessageBox(0x10u /* SDL_MESSAGEBOX_ERROR */, GMV_GAME_NAME, text, NULL);
}

void gmv_release_cursor(void)
{
    /* The mouse is SDL's there, and SDL's hold on it ends with the process. */
}

/* ---- starting ---- */

void gmv_platform_early(void)
{
}

void gmv_platform_log_start(void)
{
}

void gmv_platform_address_space(gmv_check_fn check)
{
    void *want = (void *)(uintptr_t)GMV_RESERVE_START;
    size_t size = GMV_RESERVE_END - GMV_RESERVE_START;
    void *got = mmap(want, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    int err = errno;
    if (!check(got == want, "game range 0x%08x..0x%08x reserved", GMV_RESERVE_START, GMV_RESERVE_END)) {
        gmv_log("       mmap gave 0x%08x, error %d. Something holds the range, or vm.mmap_min_addr is above 0x%x "
                "(see /proc/sys/vm/mmap_min_addr), or the system forbids writable code (SELinux's execmem)\n",
                (unsigned)(uintptr_t)got, err, GMV_RESERVE_START);
        if (got != MAP_FAILED)
            munmap(got, size);
    }
    /* A program marked without a non-executable stack gets READ_IMPLIES_EXEC, and then nothing readable faults. */
    check(!(personality(0xffffffffu) & READ_IMPLIES_EXEC), "readable memory is not executable (stray jumps must fault)");
}
#endif
