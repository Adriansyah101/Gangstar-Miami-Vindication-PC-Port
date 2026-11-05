#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

#include "layout.h"
#include "log.h"
#include "platform.h"

static FILE *log_file;
static int log_off;                 /* log_discard: no file this run */
static char *held;                  /* logged before log_open */
static size_t nheld, capheld;
/* The folders the log does not name, most specific first: the game's, the exe's, the user's.  Each has two slots:
   UTF-8 and, on Windows, the ANSI code page; on Linux, the folder with its links resolved. */
enum { HIDE_APP, HIDE_EXE = 2, HIDE_PROFILE = 4, NHIDE = 6 };
static struct { char path[GMV_PATH_MAX]; size_t n; const char *tag; } hide[NHIDE];

static void hide_finish(int slot, const char *tag)
{
    char *p = hide[slot].path;
    size_t n = strlen(p);
    while (n > 3 && (p[n - 1] == '\\' || p[n - 1] == '/'))
        p[--n] = 0;
    if (n <= 3 || n < strlen(tag))  /* "C:\" alone would hide every path; the tag must fit in its place */
        n = 0;
    hide[slot].tag = tag;
    hide[slot].n = n;
}

static void hide_set(int slot, const char *utf8, const char *tag)
{
    snprintf(hide[slot].path, sizeof hide[slot].path, "%s", utf8);
    hide_finish(slot, tag);
#ifdef _WIN32
    wchar_t w[MAX_PATH];
    hide[slot + 1].path[0] = 0;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, MAX_PATH))
        WideCharToMultiByte(CP_ACP, 0, w, -1, hide[slot + 1].path, sizeof hide[slot + 1].path, NULL, NULL);
    hide_finish(slot + 1, tag);
#else
    /* The second slot: the folder with its links resolved, which is how some code keeps it (the game's folder
       can be a link). */
    char real[PATH_MAX];
    hide[slot + 1].path[0] = 0;
    if (realpath(utf8, real) && strcmp(real, hide[slot].path) && strlen(real) < sizeof hide[slot + 1].path)
        strcpy(hide[slot + 1].path, real);
    hide_finish(slot + 1, tag);
#endif
}

void log_hide_path(const char *path, const char *tag)
{
    hide_set(!strcmp(tag, "%APP%") ? HIDE_APP : HIDE_EXE, path, tag);
}

static void profile_init(void)
{
    static int done;
    if (done)
        return;
    done = 1;
#ifdef _WIN32
    wchar_t w[MAX_PATH];
    char utf8[GMV_PATH_MAX];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", w, MAX_PATH);
    if (n && n < MAX_PATH && WideCharToMultiByte(CP_UTF8, 0, w, -1, utf8, sizeof utf8, NULL, NULL))
        hide_set(HIDE_PROFILE, utf8, "%USERPROFILE%");
#else
    const char *home = getenv("HOME");
    if (home)
        hide_set(HIDE_PROFILE, home, "$HOME");
#endif
}

static int same_char(char a, char b)
{
    if (a == '/')
        a = '\\';
    if (b == '/')
        b = '\\';
    return a == b || ((a | 0x20) == (b | 0x20) && (a | 0x20) >= 'a' && (a | 0x20) <= 'z');
}

/* s with every copy of a hidden folder (either slash, any case) written as its tag. */
static size_t scrub1(char *s, size_t n, const char *path, size_t len, const char *tag)
{
    if (!len || n < len)
        return n;
    size_t o = 0, nt = strlen(tag);
    for (size_t i = 0; i < n;) {
        size_t k = 0;
        while (k < len && i + k < n && same_char(s[i + k], path[k]))
            k++;
        if (k == len) {
            memmove(s + o, tag, nt);                /* the tag is never longer than what it replaces */
            o += nt;
            i += len;
        } else
            s[o++] = s[i++];
    }
    return o;
}

static size_t scrub(char *s, size_t n)
{
    for (int i = 0; i < NHIDE; i++)
        n = scrub1(s, n, hide[i].path, hide[i].n, hide[i].tag ? hide[i].tag : "");
    return n;
}

#ifdef _WIN32
#define TAIL_MAX MAX_PATH
#else
#define TAIL_MAX GMV_PATH_MAX
#endif

/* Blanks each copy of s in d, keeping the length so a crash dump stays valid.  tail: and what follows it in that
   string, the rest of a path. */
static void blank_narrow(unsigned char *d, size_t n, const char *s, size_t len, int tail)
{
    for (size_t o = 0; o + len <= n; o++) {
        size_t k = 0;
        while (k < len && same_char((char)d[o + k], s[k]))
            k++;
        if (k < len)
            continue;
        while (tail && o + k < n && k < TAIL_MAX && d[o + k] >= 0x20)
            k++;
        memset(d + o, '_', k);
        o += k - 1;
    }
}

#ifdef _WIN32

static void blank_wide(unsigned char *d, size_t n, const wchar_t *s, size_t len, int tail)
{
    for (size_t o = 0; o + len * 2 <= n; o++) {
        size_t k = 0;
        while (k < len && (s[k] < 0x80 ? !d[o + k * 2 + 1] && same_char((char)d[o + k * 2], (char)s[k])
                                       : d[o + k * 2] == (s[k] & 0xff) && d[o + k * 2 + 1] == s[k] >> 8))
            k++;
        if (k < len)
            continue;
        while (tail && o + k * 2 + 1 < n && k < MAX_PATH && (d[o + k * 2] >= 0x20 || d[o + k * 2 + 1]))
            k++;
        for (size_t j = 0; j < k; j++)
            d[o + j * 2] = '_', d[o + j * 2 + 1] = 0;
        o += k * 2 - 1;
    }
}

/* For the crash dump: each folder as the log knows it and in UTF-16, and every value of the environment, which the
   C library keeps copies of (the user's name, and whatever else someone keeps there). */
void log_scrub_bytes(unsigned char *d, size_t n)
{
    profile_init();
    for (int i = 0; i < NHIDE; i++) {
        size_t len = hide[i].n;
        if (!len)
            continue;
        blank_narrow(d, n, hide[i].path, len, 1);
        wchar_t w[MAX_PATH];
        int nw = i % 2 ? 0 : MultiByteToWideChar(CP_UTF8, 0, hide[i].path, (int)len, w, MAX_PATH);
        if (nw)
            blank_wide(d, n, w, (size_t)nw, 1);
    }
    wchar_t *env = GetEnvironmentStringsW();
    static char a[8192];
    for (wchar_t *e = env; e && *e; e += wcslen(e) + 1) {
        wchar_t *v = wcschr(e + 1, L'=');       /* +1: some names start with one, "=C:=C:\..." */
        size_t nv = v ? wcslen(++v) : 0;
        if (nv < 3)
            continue;
        int path = wcschr(v, L'\\') != NULL;    /* HOMEPATH also stands inside a path that starts \Device\... */
        blank_wide(d, n, v, nv, path);
        for (int cp = 0; cp < 2; cp++) {
            int m = WideCharToMultiByte(cp ? CP_ACP : CP_UTF8, 0, v, (int)nv, a, sizeof a, NULL, NULL);
            if (m)
                blank_narrow(d, n, a, (size_t)m, path);
        }
    }
    if (env)
        FreeEnvironmentStringsW(env);
}
#else
extern char **environ;

/* The same on Linux, where the strings are narrow: each folder, every value of the environment (the main thread's
   stack starts with a copy of it), and the machine's name.  The longest go first: once a short one (the user's name)
   is blanked, a longer one that holds it (the game's folder, OLDPWD, PATH) would no longer match. */
void log_scrub_bytes(unsigned char *d, size_t n)
{
    profile_init();
    for (size_t prev = (size_t)-1;;) {
        size_t len = 0;
        for (int i = 0; i < NHIDE; i++)
            if (hide[i].n < prev && hide[i].n > len)
                len = hide[i].n;
        for (char **e = environ; e && *e; e++) {
            const char *v = strchr(*e, '=');
            size_t nv = v ? strlen(v + 1) : 0;
            if (nv < prev && nv > len)
                len = nv;
        }
        if (len < 3)
            break;
        for (int i = 0; i < NHIDE; i++)
            if (hide[i].n == len)
                blank_narrow(d, n, hide[i].path, len, 1);
        for (char **e = environ; e && *e; e++) {
            const char *v = strchr(*e, '=');
            if (v && strlen(++v) == len)
                blank_narrow(d, n, v, len, strchr(v, '/') != NULL);
        }
        prev = len;
    }
    char host[256];
    if (!gethostname(host, sizeof host - 1)) {
        host[sizeof host - 1] = 0;
        if (strlen(host) >= 3)
            blank_narrow(d, n, host, strlen(host), 0);
    }
}
#endif

static void out(int console, const char *fmt, va_list ap)
{
    char buf[2048], *p = buf;
    va_list aq;
    va_copy(aq, ap);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n < 0) {
        va_end(aq);
        return;
    }
    if ((size_t)n >= sizeof buf && (p = malloc((size_t)n + 1)))
        vsnprintf(p, (size_t)n + 1, fmt, aq);
    else if (!p)
        p = buf, n = sizeof buf - 1;
    va_end(aq);
    profile_init();
    size_t m = scrub(p, (size_t)n);
#if GMV_DEV
    if (console) {
        fwrite(p, 1, m, stdout);
        fflush(stdout);
    }
#endif
    if (log_file) {
        fwrite(p, 1, m, log_file);
        if (console)
            fflush(log_file);
    } else if (!log_off) {
        if (nheld + m > capheld) {
            size_t cap = (nheld + m) * 2 + 4096;
            char *h = realloc(held, cap);
            if (h)
                held = h, capheld = cap;
        }
        if (nheld + m <= capheld)
            memcpy(held + nheld, p, m), nheld += m;
    }
    if (p != buf)
        free(p);
    /* scrub left the rest of a hidden folder behind the shorter text, where a crash dump's stack would keep it. */
    gmv_secure_zero(buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf);
}

void log_open(const char *path)
{
    log_file = gmv_fopen(path, "w");
    if (log_file && nheld)
        fwrite(held, 1, nheld, log_file);
    free(held);
    held = NULL;
    nheld = capheld = 0;
}

void log_discard(void)
{
    log_off = 1;
    free(held);
    held = NULL;
    nheld = capheld = 0;
}

void gmv_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    out(1, fmt, ap);
    va_end(ap);
}

void gmv_logfile(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    out(0, fmt, ap);
    va_end(ap);
}

void log_close(void)
{
    if (log_file)
        fclose(log_file);
    log_file = NULL;
}

/* gmv_fatal's box runs on a thread of its own.  The stopped thread may own the game's window, and a
   box on it would dispatch that window's messages into the game's WndProc while its modal loop runs.
   Another thread that stops meanwhile waits here until the player has answered. */
void gmv_audio_stop(void);

static char fatal_text[1536];
static volatile uint32_t fatal_owner, fatal_box_thread;

static void fatal_box(void)
{
    fatal_box_thread = gmv_thread_id();
    gmv_alert(fatal_text, 1);
}

void gmv_fatal(int code, const char *fmt, ...)
{
    uint32_t me = gmv_thread_id();
    if (!__sync_bool_compare_and_swap(&fatal_owner, 0, me)) {
        if (fatal_owner == me || fatal_box_thread == me)
            gmv_exit(code);             /* the stop itself failed: no second box */
        gmv_sleep_forever();
    }
    char why[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    int dumped = code != 1 && code != 10 && gmv_dump_write();   /* those two: the game never got to run */
    snprintf(fatal_text, sizeof fatal_text, GMV_GAME_NAME " has to stop.\n\n%s\n\n%s", why,
             log_off ? "To see the details, set LOG=1 in savegame" GMV_SEP GMV_GAME_NAME ".ini and try again."
             : dumped ? "The details are in cache" GMV_SEP GMV_GAME_NAME ".log and cache" GMV_SEP GMV_GAME_NAME ".dmp. "
                        "Please send those two files with your bug report."
                      : "The details are in cache" GMV_SEP GMV_GAME_NAME ".log. Please send that file with your bug report.");
    log_close();
    gmv_release_cursor();               /* the game keeps the mouse inside its window */
    gmv_thread_run(gmv_audio_stop, 0);
    if (!gmv_thread_run(fatal_box, 1))
        fatal_box();
    gmv_exit(code);
}
