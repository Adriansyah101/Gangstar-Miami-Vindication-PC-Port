/* crashdump.c -- cache\GangstarMV.dmp, what a crash leaves for the bug report besides the log.

   gmv_fatal writes it before the error box, on a thread of its own: the stopped thread may have
   overflowed its stack, and MiniDumpWriteDump suspends every other thread while it reads them.  dbghelp
   comes from System32 and only when a crash happens, so a normal run never loads it.

   A dump is memory, which a player cannot read before sending it.  So it holds only the threads' registers
   and stacks and the list of modules, with no module's folder.  The memory the stacks point at would help
   more, but that is the heap, where a path can sit in pieces no scrub can find (tried 2026-11-04, playtest's
   dumpclean).  Before the file is kept, every copy of the folders the log hides and of the environment's
   values is blanked (log_scrub_bytes).  LOG=0 means no dump either.  Linux writes its own, an ELF core file
   (crashdump_linux.c). */

#ifdef _WIN32
#include <wchar.h>
#include <windows.h>
#include <dbghelp.h>

#include "layout.h"
#include "log.h"

typedef BOOL(WINAPI *minidump_fn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                  PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

enum { DUMP_WAIT_MS = 20000 };

static wchar_t dump_path[MAX_PATH];     /* empty: no dump this run */
static EXCEPTION_POINTERS *volatile crash_ep;
static volatile DWORD crash_tid, writer_tid;
static HANDLE dump_file = INVALID_HANDLE_VALUE;
static const char *volatile dump_err;   /* why there is no file; NULL = it is there */

void gmv_dump_arm(const char *path)
{
    MultiByteToWideChar(CP_UTF8, 0, path, -1, dump_path, MAX_PATH - 1);
}

/* The first fault is the one the dump is opened on. */
void gmv_dump_note(struct _EXCEPTION_POINTERS *ep)
{
    if (!InterlockedCompareExchangePointer((void *volatile *)&crash_ep, ep, NULL))
        crash_tid = GetCurrentThreadId();
}

int gmv_dump_is_writer(void)
{
    return writer_tid && writer_tid == GetCurrentThreadId();
}

static int scrub_file(HANDLE f)
{
    int ok = 0;
    DWORD n = GetFileSize(f, NULL);
    HANDLE map = n && n != INVALID_FILE_SIZE ? CreateFileMappingW(f, NULL, PAGE_READWRITE, 0, 0, NULL) : NULL;
    unsigned char *d = map ? MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, 0) : NULL;
    if (d) {
        log_scrub_bytes(d, n);
        ok = UnmapViewOfFile(d);
    }
    if (map)
        CloseHandle(map);
    return ok;
}

static DWORD WINAPI dump_thread(void *unused)
{
    (void)unused;
    writer_tid = GetCurrentThreadId();
    HMODULE lib = LoadLibraryExW(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    minidump_fn write = lib ? (minidump_fn)(void *)GetProcAddress(lib, "MiniDumpWriteDump") : NULL;
    if (!write) {
        dump_err = "no dbghelp";
        return 0;
    }
    dump_file = CreateFileW(dump_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (dump_file == INVALID_HANDLE_VALUE) {
        dump_err = "cannot create the file";
        return 0;
    }
    MINIDUMP_EXCEPTION_INFORMATION ex = {crash_tid, crash_ep, FALSE};
    MINIDUMP_TYPE type = MiniDumpNormal | MiniDumpFilterModulePaths;
    if (!write(GetCurrentProcess(), GetCurrentProcessId(), dump_file, type, crash_ep ? &ex : NULL, NULL, NULL))
        dump_err = "MiniDumpWriteDump failed";
    else if (!scrub_file(dump_file))
        dump_err = "cannot clean the file";     /* a file that still names the folders is not kept */
    return 0;
}

int gmv_dump_write(void)
{
    if (!dump_path[0])
        return 0;
    dump_err = NULL;
    HANDLE t = CreateThread(NULL, 0, dump_thread, NULL, 0, NULL);
    if (!t)
        dump_err = "no thread";
    else {
        if (WaitForSingleObject(t, DUMP_WAIT_MS) != WAIT_OBJECT_0) {
            TerminateThread(t, 0);
            dump_err = "timed out";
        }
        CloseHandle(t);
    }
    if (dump_file != INVALID_HANDLE_VALUE) {
        CloseHandle(dump_file);
        dump_file = INVALID_HANDLE_VALUE;
    }
    if (dump_err) {
        DeleteFileW(dump_path);
        gmv_log("[crash] no dump: %s\n", dump_err);
        return 0;
    }
    gmv_log("[crash] wrote cache\\" GMV_GAME_NAME ".dmp\n");
    return 1;
}
#endif
