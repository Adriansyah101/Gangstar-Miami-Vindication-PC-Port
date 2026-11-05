#ifndef GMV_LOG_H
#define GMV_LOG_H

#include <stddef.h>

/* build.ps1 -Release defines GMV_RELEASE: the diagnostics and test aids are left out of the build. */
#ifdef GMV_RELEASE
#define GMV_DEV 0
#else
#define GMV_DEV 1
#endif

/* The port's version, the first line of its log. */
#define GMV_VERSION "V1.0.0"

/* Everything goes to cache\GangstarMV.log next to the exe, and in a dev build to the console too.  What
   is logged before log_open (or log_discard, LOG=0) is held and written when the file opens.  The user's
   profile folder is written as %USERPROFILE%, and the folders given to log_hide_path by their tags, so a
   log can be shared without a name or a folder of the player's. */
void log_open(const char *path);
void log_discard(void);
void log_hide_path(const char *path, const char *tag);
void gmv_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Detail that only belongs in the log file. */
void gmv_logfile(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* What the port does from moment to moment: a dev build logs it, a release build has neither the text
   nor the call. */
#if GMV_DEV
#define gmv_trace gmv_log
#else
#define gmv_trace(...) do { if (0) gmv_log(__VA_ARGS__); } while (0)
#endif
void log_close(void);
/* The game or the host cannot go on: close the log, tell the player in a message box and end the
   process with this exit code, from any thread.  The reason is one short sentence. */
void gmv_fatal(int code, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));
/* crashdump.c: the dump gmv_fatal leaves after a crash.  note = the exception a dump should open on. */
struct _EXCEPTION_POINTERS;
void gmv_dump_arm(const char *path);
void gmv_dump_note(struct _EXCEPTION_POINTERS *ep);
int gmv_dump_write(void);
int gmv_dump_is_writer(void);
/* Blanks every copy of the folders the log hides, keeping the length of d. */
void log_scrub_bytes(unsigned char *d, size_t n);

#endif
