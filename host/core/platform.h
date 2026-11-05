/* platform.h -- what the loader asks of the operating system.  platform_win32.c and platform_linux.c answer it; the
   rest of core\ names no system call of its own, apart from crashdump.c and veh.c, which are Windows' only
   (crashdump_linux.c and fault_linux.c are their counterparts), and loadprof.c, which has a part for each.  Paths
   are UTF-8 everywhere. */
#ifndef GMV_PLATFORM_H
#define GMV_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define GMV_PATH_MAX 1024

#ifdef _WIN32
#define GMV_SEP "\\"
#define GMV_PLATFORM_NAME "Win32"
#define GMV_PLATFORM_OS "Windows"
#define GMV_EXE_SUFFIX ".exe"
#else
#include <strings.h>
#define GMV_SEP "/"
#define GMV_PLATFORM_NAME "Linux"
#define GMV_PLATFORM_OS "Linux"
#define GMV_EXE_SUFFIX ""
#define __cdecl
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _strdup strdup
#endif

/* A C function's name as asm in a C file calls it: Windows' has an underscore in front, Linux's none. */
#define GMV_ASM_STR_(x) #x
#define GMV_ASM_STR(x) GMV_ASM_STR_(x)
#define GMV_ASM_SYM(name) GMV_ASM_STR(__USER_LABEL_PREFIX__) #name

/* The program, as main() gets it there: argv in UTF-8. */
int gmv_main(int argc, char **argv);

/* ---- files ---- */

void gmv_exe_dir(char *out, size_t n);          /* the folder the exe is in, with no separator at the end */
FILE *gmv_fopen(const char *path, const char *mode);
int gmv_mkdir(const char *path);                /* these give 1 on success; an existing folder is not an error */
int gmv_remove(const char *path);
int gmv_rename(const char *from, const char *to);    /* fails if `to` exists */
int gmv_replace(const char *from, const char *to);   /* replaces `to` */
int gmv_is_file(const char *path);
int gmv_full_path(const char *path, char *out, size_t n);
unsigned long gmv_last_error(void);

/* ---- memory ---- */

enum { GMV_PROT_NONE, GMV_PROT_R, GMV_PROT_RW, GMV_PROT_RX };
int gmv_protect(uint32_t addr, uint32_t size, int prot);    /* 1 on success */
int gmv_readable(const void *p, size_t n);      /* reading these bytes would not fault */
void gmv_secure_zero(void *p, size_t n);
/* The module holding addr: its file (name, may be empty) and base.  0 if none. */
int gmv_module_of(uint32_t addr, char *name, size_t n, uint32_t *base);

/* ---- threads and the process ---- */

uint32_t gmv_thread_id(void);
int gmv_thread_run(void (*fn)(void), int wait); /* 1 if it started; wait: until it ends */
void gmv_sleep_forever(void) __attribute__((noreturn));
void gmv_exit(int code) __attribute__((noreturn));

/* ---- the user ---- */

/* A box the player has to answer, with the game's name for a title.  on_top: over everything. */
void gmv_alert(const char *text, int on_top);
void gmv_release_cursor(void);                  /* the game may have kept the mouse inside its window */

/* ---- starting ---- */

/* After the settings are read and before any window. */
void gmv_platform_early(void);
/* After the log has its first line: what it should know about the system. */
void gmv_platform_log_start(void);
typedef int (*gmv_check_fn)(int ok, const char *fmt, ...);
/* The game's address range is ours and a jump into memory that is not code faults: each thing that must hold goes
   through check, with what it says in the log. */
void gmv_platform_address_space(gmv_check_fn check);

#endif
