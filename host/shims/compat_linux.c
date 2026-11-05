/* compat_linux.c -- the parts of compat.h too long to sit in the header: VirtualQuery and VirtualProtect over
   /proc/self/maps and mprotect.  Windows has the real ones, so this file is empty there. */
#ifndef _WIN32
#include <fcntl.h>
#include "compat.h"

static DWORD page_protect(const char *perm)
{
    int r = perm[0] == 'r', w = perm[1] == 'w', x = perm[2] == 'x';
    if (x)
        return w ? PAGE_EXECUTE_READWRITE : r ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    return w ? PAGE_READWRITE : r ? PAGE_READONLY : PAGE_NOACCESS;
}

static int prot_of(DWORD p)
{
    switch (p & 0xff) {
    case PAGE_NOACCESS: return PROT_NONE;
    case PAGE_READONLY: return PROT_READ;
    case PAGE_READWRITE:
    case PAGE_WRITECOPY: return PROT_READ | PROT_WRITE;
    case PAGE_EXECUTE: return PROT_EXEC;
    case PAGE_EXECUTE_READ: return PROT_READ | PROT_EXEC;
    default: return PROT_READ | PROT_WRITE | PROT_EXEC;
    }
}

/* One line of maps: "lo-hi perm ...".  0 if it is not one. */
static int maps_line(const char *s, uintptr_t *lo, uintptr_t *hi, DWORD *prot)
{
    char *e;
    unsigned long a = strtoul(s, &e, 16);
    if (*e != '-')
        return 0;
    unsigned long b = strtoul(e + 1, &e, 16);
    if (*e != ' ' || !e[1] || !e[2] || !e[3])
        return 0;
    *lo = a, *hi = b, *prot = page_protect(e + 1);
    return 1;
}

/* Read with no allocation: hooks may ask this while the game's heap is in any state. */
size_t VirtualQuery(const void *p, MEMORY_BASIC_INFORMATION *m, size_t n)
{
    if (n < sizeof *m)
        return 0;
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)4095;
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    char buf[8192];                     /* a line is at most a path (4096) and its prefix */
    size_t have = 0;
    uintptr_t lo = 0, hi = 0, flo = 0, fhi = 0;
    DWORD prot = 0, fprot = 0;
    int found = 0, done = 0;
    while (!done) {
        ssize_t got = read(fd, buf + have, sizeof buf - 1 - have);
        if (got <= 0)
            break;
        have += (size_t)got;
        buf[have] = 0;
        char *s = buf, *nl;
        while (!done && (nl = strchr(s, '\n'))) {
            *nl = 0;
            if (maps_line(s, &lo, &hi, &prot)) {
                if (found) {
                    if (lo == fhi && prot == fprot)
                        fhi = hi;               /* the run goes on */
                    else
                        done = 1;
                } else if (a < lo) {
                    fhi = lo;                   /* in the gap before this mapping */
                    done = 2;
                } else if (a < hi) {
                    flo = lo, fhi = hi, fprot = prot;
                    found = 1;
                }
            }
            s = nl + 1;
        }
        have = (size_t)(buf + have - s);
        memmove(buf, s, have);
    }
    close(fd);
    memset(m, 0, sizeof *m);
    if (!found && done != 2)
        fhi = 0;                                /* past the last mapping: free to the top */
    if (found) {
        m->BaseAddress = m->AllocationBase = (void *)flo;
        m->RegionSize = (size_t)(fhi - flo);
        m->State = MEM_COMMIT;
        m->Protect = m->AllocationProtect = fprot;
    } else {
        m->BaseAddress = (void *)a;             /* as Windows: from p's page to the next mapping */
        m->RegionSize = (size_t)(fhi - a);
        m->State = MEM_FREE;
        m->Protect = PAGE_NOACCESS;
    }
    return sizeof *m;
}

BOOL VirtualProtect(void *p, size_t n, DWORD prot, DWORD *old)
{
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery(p, &m, sizeof m) || m.State != MEM_COMMIT)
        return FALSE;
    uintptr_t lo = (uintptr_t)p & ~(uintptr_t)4095, hi = ((uintptr_t)p + n + 4095) & ~(uintptr_t)4095;
    if (mprotect((void *)lo, hi - lo, prot_of(prot)))
        return FALSE;
    if (old)
        *old = m.Protect;
    return TRUE;
}
#endif
