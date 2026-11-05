/* platform_win32.c -- platform.h on Windows. */
#ifdef _WIN32
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "layout.h"
#include "log.h"
#include "platform.h"
#include "shims.h"

extern char gmv_reserve_start[], gmv_reserve_end[];

static int to_wide(const char *s, wchar_t *w, int n)
{
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) != 0;
}

static int to_utf8(const wchar_t *w, char *s, size_t n)
{
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, s, (int)n, NULL, NULL) != 0;
}

/* -municode: the entry point gets the real command line, whatever the code page. */
int wmain(int argc, wchar_t **argv)
{
    static char *args[64];
    static char text[64][GMV_PATH_MAX];
    if (argc > 64)
        argc = 64;
    for (int i = 0; i < argc; i++) {
        to_utf8(argv[i], text[i], GMV_PATH_MAX);
        args[i] = text[i];
    }
    return gmv_main(argc, args);
}

/* ---- files ---- */

void gmv_exe_dir(char *out, size_t n)
{
    wchar_t w[MAX_PATH];
    GetModuleFileNameW(NULL, w, MAX_PATH);
    *wcsrchr(w, L'\\') = 0;
    to_utf8(w, out, n);
}

FILE *gmv_fopen(const char *path, const char *mode)
{
    wchar_t w[MAX_PATH], m[16];
    return to_wide(path, w, MAX_PATH) && to_wide(mode, m, 16) ? _wfopen(w, m) : NULL;
}

int gmv_mkdir(const char *path)
{
    wchar_t w[MAX_PATH];
    return to_wide(path, w, MAX_PATH) && (CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
}

int gmv_remove(const char *path)
{
    wchar_t w[MAX_PATH];
    return to_wide(path, w, MAX_PATH) && DeleteFileW(w);
}

int gmv_rename(const char *from, const char *to)
{
    wchar_t a[MAX_PATH], b[MAX_PATH];
    return to_wide(from, a, MAX_PATH) && to_wide(to, b, MAX_PATH) && MoveFileW(a, b);
}

int gmv_replace(const char *from, const char *to)
{
    wchar_t a[MAX_PATH], b[MAX_PATH];
    return to_wide(from, a, MAX_PATH) && to_wide(to, b, MAX_PATH) && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
}

int gmv_is_file(const char *path)
{
    wchar_t w[MAX_PATH];
    if (!to_wide(path, w, MAX_PATH))
        return 0;
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int gmv_full_path(const char *path, char *out, size_t n)
{
    wchar_t w[MAX_PATH], full[MAX_PATH];
    return to_wide(path, w, MAX_PATH) && GetFullPathNameW(w, MAX_PATH, full, NULL) && to_utf8(full, out, n);
}

unsigned long gmv_last_error(void)
{
    return GetLastError();
}

/* ---- memory ---- */

int gmv_protect(uint32_t addr, uint32_t size, int prot)
{
    static const DWORD win[] = {PAGE_NOACCESS, PAGE_READONLY, PAGE_READWRITE, PAGE_EXECUTE_READ};
    DWORD old;
    return VirtualProtect((void *)(uintptr_t)addr, size, win[prot], &old) != 0;
}

int gmv_readable(const void *p, size_t n)
{
    return !IsBadReadPtr(p, n);
}

void gmv_secure_zero(void *p, size_t n)
{
    SecureZeroMemory(p, n);
}

int gmv_module_of(uint32_t addr, char *name, size_t n, uint32_t *base)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((const void *)(uintptr_t)addr, &mbi, sizeof mbi) ||
        !GetModuleFileNameA((HMODULE)mbi.AllocationBase, name, (DWORD)n))
        return 0;
    *base = (uint32_t)(uintptr_t)mbi.AllocationBase;
    return 1;
}

/* ---- threads and the process ---- */

uint32_t gmv_thread_id(void)
{
    return GetCurrentThreadId();
}

static DWORD WINAPI thread_start(void *fn)
{
    ((void (*)(void))fn)();
    return 0;
}

int gmv_thread_run(void (*fn)(void), int wait)
{
    HANDLE t = CreateThread(NULL, 0, thread_start, (void *)fn, 0, NULL);
    if (!t)
        return 0;
    if (wait)
        WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return 1;
}

void gmv_sleep_forever(void)
{
    for (;;)
        Sleep(INFINITE);
}

void gmv_exit(int code)
{
    ExitProcess((UINT)code);
}

/* ---- the user ---- */

void gmv_alert(const char *text, int on_top)
{
    static wchar_t w[2048];
    if (!to_wide(text, w, 2048))
        w[0] = 0;
    MessageBoxW(NULL, w, L"" GMV_GAME_NAME, MB_OK | MB_ICONERROR | (on_top ? MB_TOPMOST | MB_SETFOREGROUND : 0));
}

void gmv_release_cursor(void)
{
    ClipCursor(NULL);
}

/* ---- starting ---- */

static int dpi_aware;

void gmv_platform_early(void)
{
    /* Before any window: Windows would otherwise stretch this one when the display is scaled above 100%. */
    const char *da = gmv_getenv("GMV_DPI_AWARE");
    dpi_aware = !(da && *da == '0') && SetProcessDPIAware();
}

void gmv_platform_log_start(void)
{
    if (dpi_aware) {
        HDC dc = GetDC(NULL);
        gmv_log("display scale %d%%\n\n", GetDeviceCaps(dc, LOGPIXELSX) * 100 / 96);
        ReleaseDC(NULL, dc);
    }
}

/* Is every page of [lo, hi) committed and owned by the exe image?  The range starts as one region,
   but a protection change on any page inside it (a copy-on-write touch, or another process such as
   security software adjusting a page) splits it into several regions that are all still the exe's.
   Requiring a single region made startup fail intermittently for no layout reason, so walk them
   all; the first region that is not the exe's is logged so a real failure stays diagnosable. */
static int range_owned_by_exe(uintptr_t lo, uintptr_t hi)
{
    for (uintptr_t a = lo; a < hi;) {
        MEMORY_BASIC_INFORMATION mbi = {0};
        if (!VirtualQuery((void *)a, &mbi, sizeof mbi)) {
            gmv_log("       VirtualQuery(0x%08x) failed (%lu)\n", (unsigned)a, GetLastError());
            return 0;
        }
        if (mbi.AllocationBase != (void *)GMV_EXE_BASE || mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE) {
            gmv_log("       region 0x%08x..0x%08x alloc=0x%08x state=0x%lx type=0x%lx prot=0x%lx\n",
                    (unsigned)(uintptr_t)mbi.BaseAddress,
                    (unsigned)((uintptr_t)mbi.BaseAddress + mbi.RegionSize),
                    (unsigned)(uintptr_t)mbi.AllocationBase, mbi.State, mbi.Type, mbi.Protect);
            return 0;
        }
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return 1;
}

void gmv_platform_address_space(gmv_check_fn check)
{
    check(GetModuleHandleW(NULL) == (HMODULE)GMV_EXE_BASE, "exe mapped at 0x%08x (want 0x%08x)",
          (unsigned)(uintptr_t)GetModuleHandleW(NULL), GMV_EXE_BASE);
    check((uintptr_t)gmv_reserve_start == GMV_RESERVE_START && (uintptr_t)gmv_reserve_end == GMV_RESERVE_END &&
          range_owned_by_exe(GMV_RESERVE_START, GMV_RESERVE_END),
          "game range 0x%08x..0x%08x reserved by the exe image", GMV_RESERVE_START, GMV_RESERVE_END);
    DWORD dep = 0;
    BOOL perm = FALSE;
    GetProcessDEPPolicy(GetCurrentProcess(), &dep, &perm);
    if (!(dep & PROCESS_DEP_ENABLE)) {
        SetProcessDEPPolicy(PROCESS_DEP_ENABLE);
        GetProcessDEPPolicy(GetCurrentProcess(), &dep, &perm);
    }
    check(dep & PROCESS_DEP_ENABLE, "DEP enabled (stray jumps into the PE header page must fault)");
}
#endif
