/* shimsfnd.c -- Foundation/Cocoa object stubs, stage 3d+.

   These are Objective-C methods on Cocoa classes the game messages (NSBundle, NSString, ...).
   They are plain host functions used as IMPs: gmv_objc_lookup, after failing to find a game
   method and the NSObject-level defaults, consults the two tables here by (class-name, selector)
   and tail-jumps to the match.  Extend a table with one row per method as the --run-main probe
   loop names the next wall (see docs/NOTES.md, "Diagnostics").

   Objects we hand back are allocated by gmv_objc_new_instance(name): isa = a fake-class
   descriptor tagged with the class name, so a later message to them routes back here. */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"
#ifndef _WIN32
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_mouse.h>
#endif

#include "log.h"
#include "layout.h"
#include "shims.h"

struct fnd_method {
    const char *cls;
    const char *sel;
    void *imp;
};

/* ---- NSString (minimal) --------------------------------------------------------------- */

/* Our NSString objects are gmv_objc_new_instance("NSString") blocks (isa + 12 bytes) with an
   owned copy of the backing C string at offset 4.  Compile-time CFConstantString literals keep
   their C text at offset 8 instead; nsstring_cstr handles both. */
static void *make_nsstring(const char *cstr)
{
    void *o = gmv_objc_new_instance("NSString");
    if (o)
        ((char **)o)[1] = strdup(cstr ? cstr : "");
    return o;
}

void *gmv_make_nsstring(const char *s) { return make_nsstring(s); }

/* Our NSArray instances (gmv_objc_new_instance("NSArray") = isa + 12 bytes) keep the element count
   at +4 and a malloc'd buffer of that many id pointers at +8.  Built by gmv_make_nsarray1 (one
   element) and +[NSArray arrayWithObjects:] (a nil-terminated list). */
static void *make_nsarray_n(void *const *elems, uint32_t n)
{
    void *a = gmv_objc_new_instance("NSArray");
    if (!a)
        return NULL;
    void **buf = NULL;
    if (n) {
        buf = malloc((size_t)n * sizeof(void *));
        if (buf)
            memcpy(buf, elems, (size_t)n * sizeof(void *));
        else
            n = 0;
    }
    ((uint32_t *)a)[1] = n;                      /* +4 count */
    ((void **)a)[2] = buf;                       /* +8 elements */
    return a;
}

void *gmv_make_nsarray1(void *elem) { return make_nsarray_n(&elem, 1); }

static void *fnd_NSArray_count(void *self, const char *sel, ...)
{
    (void)sel;
    return self ? (void *)(uintptr_t)((uint32_t *)self)[1] : NULL;
}
static void *fnd_NSArray_objectAtIndex(void *self, const char *sel, uint32_t idx)
{
    (void)sel;
    if (!self)
        return NULL;
    uint32_t n = ((uint32_t *)self)[1];
    void **buf = ((void **)self)[2];
    return (buf && idx < n) ? buf[idx] : NULL;
}
static void *fnd_NSArray_firstObject(void *self, const char *sel, ...)
{
    (void)sel;
    if (!self)
        return NULL;
    uint32_t n = ((uint32_t *)self)[1];
    void **buf = ((void **)self)[2];
    return (buf && n) ? buf[0] : NULL;
}
static void *fnd_NSArray_lastObject(void *self, const char *sel, ...)
{
    (void)sel;
    if (!self)
        return NULL;
    uint32_t n = ((uint32_t *)self)[1];
    void **buf = ((void **)self)[2];
    return (buf && n) ? buf[n - 1] : NULL;
}

/* +[NSArray arrayWithObjects:(id)first, ...] -- a nil-terminated variadic list (i386 cdecl, args
   pushed right-to-left so a va_list after `first` reads them in order).  Capped so a caller that
   forgets the nil terminator can't run off the stack. */
static void *fnd_NSArray_arrayWithObjects(void *cls, const char *sel, void *first, ...)
{
    (void)cls;
    (void)sel;
    void *tmp[64];
    uint32_t n = 0;
    if (first) {
        tmp[n++] = first;
        va_list ap;
        va_start(ap, first);
        while (n < (uint32_t)(sizeof(tmp) / sizeof(tmp[0]))) {
            void *o = va_arg(ap, void *);
            if (!o)
                break;
            tmp[n++] = o;
        }
        va_end(ap);
    }
    return make_nsarray_n(tmp, n);
}

static const char *nsstring_cstr(const void *o)
{
    if (!o)
        return NULL;
    if (gmv_objc_is_cfstring(o))
        return *(const char *const *)((const char *)o + 8);
    const char *cn = gmv_objc_instance_class_name(o);
    if (cn && !strcmp(cn, "NSString"))
        return *(const char *const *)((const char *)o + 4);
    return NULL;
}

/* Public accessor: the backing C string of an NSString/CFString object, or NULL. */
const char *gmv_nsstring_cstr(const void *o) { return nsstring_cstr(o); }

static void *fnd_NSString_UTF8String(void *self, const char *sel, ...)
{
    (void)sel;
    return (void *)nsstring_cstr(self);     /* returns a const char * */
}

static void *fnd_NSString_appendPathComponent(void *self, const char *sel, void *comp)
{
    (void)sel;
    const char *a = nsstring_cstr(self), *b = nsstring_cstr(comp);
    char buf[1024];
    snprintf(buf, sizeof buf, "%s/%s", a ? a : "", b ? b : "");
    return make_nsstring(buf);
}
/* +[NSThread detachNewThreadSelector:toTarget:withObject:] -- spawn a real thread that performs
   [target selector:object].  The trampoline calls game code, so it is force-aligned. */
struct ns_thread { void *target; const char *sel; void *obj; };
__attribute__((force_align_arg_pointer))
static DWORD WINAPI ns_thread_tramp(void *p)
{
    struct ns_thread s = *(struct ns_thread *)p;
    free(p);
    void *(*msg)(void *, const char *, ...) = (void *(*)(void *, const char *, ...))(void *)gmv_objc_msgSend;
    msg(s.target, s.sel, s.obj);
    return 0;
}
static void *fnd_NSThread_detach(void *cls, const char *sel, const char *selector, void *target, void *obj)
{
    (void)cls;
    (void)sel;
    struct ns_thread *s = malloc(sizeof *s);
    if (s) {
        s->target = target;
        s->sel = selector;
        s->obj = obj;
        HANDLE h = CreateThread(NULL, 0, ns_thread_tramp, s, 0, NULL);
        if (h)
            CloseHandle(h);
        else
            free(s);
    }
    return NULL;
}

/* Networking (GameLoft Live / IGP, on a worker thread): make URLs/requests into inert fake
   objects and connections into nil, so the feature silently does nothing instead of crashing. */
static void *fnd_NSURL_urlWithString(void *cls, const char *sel, void *str)
{
    (void)cls;
    (void)sel;
    void *u = gmv_objc_new_instance("NSURL");
    if (u)
        ((void **)u)[1] = str;                  /* keep the string for -absoluteString */
    return u;
}
static void *fnd_NSURL_absoluteString(void *self, const char *sel, ...)
{
    (void)sel;
    return self ? ((void **)self)[1] : NULL;
}
static void *fnd_NSURLRequest_with(void *cls, const char *sel, ...)
{
    (void)cls;
    (void)sel;
    return gmv_objc_new_instance("NSURLRequest");
}

/* URL percent-escaping: return the text unchanged (good enough for the web/IGP code path). */
static void *fnd_NSString_percentEscapes(void *self, const char *sel, uint32_t enc)
{
    (void)sel;
    (void)enc;
    const char *s = nsstring_cstr(self);
    return make_nsstring(s ? s : "");
}
static void *fnd_NSString_replace(void *self, const char *sel, void *target, void *repl)
{
    (void)sel;
    const char *s = nsstring_cstr(self), *t = nsstring_cstr(target), *r = nsstring_cstr(repl);
    if (!s)
        return make_nsstring("");
    if (!t || !*t)
        return make_nsstring(s);
    if (!r)
        r = "";
    size_t tl = strlen(t), rl = strlen(r), n = 0;
    char out[4096];
    for (const char *p = s; *p && n + 1 < sizeof out;) {
        if (strncmp(p, t, tl) == 0) {
            for (size_t i = 0; i < rl && n + 1 < sizeof out; i++)
                out[n++] = r[i];
            p += tl;
        } else {
            out[n++] = *p++;
        }
    }
    out[n] = 0;
    return make_nsstring(out);
}
static void *fnd_NSString_writeToFile(void *self, const char *sel, void *path, int atomically,
                                      uint32_t enc, void **err)
{
    (void)sel;
    (void)atomically;
    (void)enc;
    if (err)
        *err = NULL;
    const char *s = nsstring_cstr(self), *p = nsstring_cstr(path);
    if (p && s) {
        FILE *f = fopen(p, "wb");
        if (f) {
            fwrite(s, 1, strlen(s), f);
            fclose(f);
            return (void *)1;                   /* YES */
        }
    }
    return (void *)0;
}
static void *fnd_NSString_appendString(void *self, const char *sel, void *other)
{
    (void)sel;
    const char *a = nsstring_cstr(self), *b = nsstring_cstr(other);
    char buf[2048];
    snprintf(buf, sizeof buf, "%s%s", a ? a : "", b ? b : "");
    return make_nsstring(buf);
}

static void *fnd_NSString_substringToIndex(void *self, const char *sel, uint32_t idx)
{
    (void)sel;
    const char *s = nsstring_cstr(self);
    if (!s)
        return make_nsstring("");
    uint32_t len = (uint32_t)strlen(s);
    if (idx > len)
        idx = len;
    char tmp[1024];
    if (idx >= sizeof tmp)
        idx = sizeof tmp - 1;
    memcpy(tmp, s, idx);
    tmp[idx] = 0;
    return make_nsstring(tmp);
}
static void *fnd_NSString_hasPrefix(void *self, const char *sel, void *prefix)
{
    (void)sel;
    const char *s = nsstring_cstr(self), *p = nsstring_cstr(prefix);
    if (!s || !p)
        return (void *)0;
    return (void *)(uintptr_t)(strncmp(s, p, strlen(p)) == 0);
}

static void *fnd_NSString_stringWithUTF8String(void *cls, const char *sel, const char *s, ...)
{
    (void)cls;
    (void)sel;
    return make_nsstring(s);
}
/* +[NSString stringWithCString:encoding:] -- take the C string, ignore the encoding. */
static void *fnd_NSString_stringWithCString(void *cls, const char *sel, const char *s, uint32_t enc)
{
    (void)cls;
    (void)sel;
    (void)enc;
    return make_nsstring(s);
}
/* NSDate / NSDateFormatter: minimal -- a date is a fake object, formatting gives a fixed string. */
static void *g_date;
static void *fnd_NSDate_date(void *cls, const char *sel, ...)
{
    (void)cls;
    (void)sel;
    if (!g_date)
        g_date = gmv_objc_new_instance("NSDate");
    return g_date;
}
static void *fnd_NSDateFormatter_stringFromDate(void *self, const char *sel, void *date)
{
    (void)self;
    (void)sel;
    (void)date;
    return make_nsstring("2026-01-01 00:00:00");
}

/* -init... on a freshly +alloc'd NSString: store the backing copy at offset 4, return self. */
static void *fnd_NSString_initWithString(void *self, const char *sel, void *other)
{
    (void)sel;
    const char *s = nsstring_cstr(other);
    ((char **)self)[1] = strdup(s ? s : "");
    return self;
}
static void *fnd_NSString_initWithUTF8String(void *self, const char *sel, const char *s)
{
    (void)sel;
    ((char **)self)[1] = strdup(s ? s : "");
    return self;
}

static uint32_t fnd_NSString_length(void *self, const char *sel, ...)
{
    (void)sel;
    const char *s = nsstring_cstr(self);
    return s ? (uint32_t)strlen(s) : 0;
}

/* isEqual: / isEqualToString: -- compare the backing text (BOOL in eax). */
static void *fnd_NSString_isEqual(void *self, const char *sel, void *other)
{
    (void)sel;
    const char *a = nsstring_cstr(self), *b = nsstring_cstr(other);
    if (!a || !b)
        return (void *)(uintptr_t)(a == b);
    return (void *)(uintptr_t)(strcmp(a, b) == 0);
}

static void fmt_put(char *buf, size_t cap, size_t *n, const char *s)
{
    while (*s && *n + 1 < cap)
        buf[(*n)++] = *s++;
}

/* printf-style NSString formatting shared by +stringWithFormat: and -initWithFormat:.  Standard
   conversions go to snprintf one spec at a time; %@ becomes the argument's NSString text (or its
   class name).  Length modifiers are dropped (on i386 long == int). */
static void nsformat(char *out, size_t cap, const char *f, va_list ap)
{
    size_t n = 0;
    if (!f) {
        out[0] = 0;
        return;
    }
    for (const char *p = f; *p && n + 1 < cap;) {
        if (*p != '%') {
            out[n++] = *p++;
            continue;
        }
        const char *start = p++;
        if (*p == '%') {
            out[n++] = '%';
            p++;
            continue;
        }
        char spec[32];
        int si = 0;
        spec[si++] = '%';
        while (*p && strchr("-+ #0123456789.", *p) && si < 28)
            spec[si++] = *p++;
        while (*p && strchr("lhqLzjt", *p))         /* drop length modifiers */
            p++;
        char conv = *p ? *p++ : 0;
        char tmp[512];
        if (conv == '@') {
            void *o = va_arg(ap, void *);
            const char *cs = nsstring_cstr(o);
            if (!cs)
                cs = o ? gmv_objc_instance_class_name(o) : "(null)";
            fmt_put(out, cap, &n, cs ? cs : "<obj>");
        } else if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' ||
                   conv == 'X' || conv == 'o' || conv == 'c') {
            spec[si++] = conv;
            spec[si] = 0;
            snprintf(tmp, sizeof tmp, spec, va_arg(ap, int));
            fmt_put(out, cap, &n, tmp);
        } else if (conv == 'f' || conv == 'F' || conv == 'g' || conv == 'G' ||
                   conv == 'e' || conv == 'E') {
            spec[si++] = conv;
            spec[si] = 0;
            snprintf(tmp, sizeof tmp, spec, va_arg(ap, double));
            fmt_put(out, cap, &n, tmp);
        } else if (conv == 's') {
            spec[si++] = 's';
            spec[si] = 0;
            snprintf(tmp, sizeof tmp, spec, va_arg(ap, char *));
            fmt_put(out, cap, &n, tmp);
        } else if (conv == 'p') {
            spec[si++] = 'p';
            spec[si] = 0;
            snprintf(tmp, sizeof tmp, spec, va_arg(ap, void *));
            fmt_put(out, cap, &n, tmp);
        } else {                                    /* unknown: emit the raw spec */
            while (start < p && n + 1 < cap)
                out[n++] = *start++;
        }
    }
    out[n] = 0;
}

/* Public formatter (NSLog in shimsappkit.c uses it to print the game's %d/%@ args). */
void gmv_nsformat(char *out, size_t cap, const char *f, va_list ap)
{
    nsformat(out, cap, f, ap);
}

static void *fnd_NSString_stringWithFormat(void *cls, const char *sel, void *format, ...)
{
    (void)cls;
    (void)sel;
    char out[2048];
    va_list ap;
    va_start(ap, format);
    nsformat(out, sizeof out, nsstring_cstr(format), ap);
    va_end(ap);
    return make_nsstring(out);
}

/* -[NSString initWithFormat:] on a freshly +alloc'd NSString: format, store, return self. */
static void *fnd_NSString_initWithFormat(void *self, const char *sel, void *format, ...)
{
    (void)sel;
    char out[2048];
    va_list ap;
    va_start(ap, format);
    nsformat(out, sizeof out, nsstring_cstr(format), ap);
    va_end(ap);
    ((char **)self)[1] = strdup(out);
    return self;
}

/* ---- NSBundle ------------------------------------------------------------------------- */

static void *g_main_bundle;
static char g_bundle_path[520];             /* the real .app directory (set from main.c) */

void gmv_fnd_set_bundle_path(const char *path)
{
    if (path) {
        strncpy(g_bundle_path, path, sizeof g_bundle_path - 1);
        g_bundle_path[sizeof g_bundle_path - 1] = 0;
    }
}

int gmv_in_bundle(const char *path)
{
    size_t n = strlen(g_bundle_path);
    return n && !strncmp(path, g_bundle_path, n) && (path[n] == '/' || path[n] == '\\' || !path[n]);
}

static void *fnd_NSBundle_mainBundle(void *self, const char *sel, ...)
{
    (void)self;
    (void)sel;
    if (!g_main_bundle)
        g_main_bundle = gmv_objc_new_instance("NSBundle");
    return g_main_bundle;
}

static void *fnd_NSBundle_bundlePath(void *self, const char *sel, ...)
{
    (void)self;
    (void)sel;
    return make_nsstring(g_bundle_path);
}

static void *g_info_dict;
static void *fnd_NSBundle_infoDictionary(void *self, const char *sel, ...)
{
    (void)self;
    (void)sel;
    if (!g_info_dict)
        g_info_dict = gmv_objc_new_instance("NSDictionary");
    return g_info_dict;
}

static void *fnd_NSBundle_resourcePath(void *self, const char *sel, ...)
{
    (void)self;
    (void)sel;
    char path[1024];
    snprintf(path, sizeof path, "%s/Contents/Resources", g_bundle_path);
    return make_nsstring(path);
}

/* -[NSBundle pathForResource:ofType:] -> <.app>/Contents/Resources/<name>.<ext>, or nil if the
   file isn't there (so the game can fall back / skip, e.g. a missing intro video). */
static void *fnd_NSBundle_pathForResource(void *self, const char *sel, void *name, void *ext)
{
    (void)self;
    (void)sel;
    const char *n = nsstring_cstr(name);
    const char *e = nsstring_cstr(ext);
    if (!n)
        return NULL;
    char path[1024];
    snprintf(path, sizeof path, "%s/Contents/Resources/%s%s%s",
             g_bundle_path, n, (e && *e) ? "." : "", e ? e : "");
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
        return NULL;
    return make_nsstring(path);
}

/* ---- NSWindow (no real window yet: the designated initializer just returns self) ------ */

/* -[NSWindow initWithContentRect:(NSRect)styleMask:backing:defer:]; NSRect is 4 floats passed
   by value on i386.  A later stage turns this into a real Win32 window + WGL context. */
static void *fnd_NSWindow_initWithContentRect(void *self, const char *sel,
                                              float x, float y, float w, float h,
                                              uint32_t style, int backing, int defer)
{
    (void)sel; (void)x; (void)y; (void)w; (void)h; (void)style; (void)backing; (void)defer;
    return self;
}

static void *g_content_view;
static void *fnd_NSWindow_contentView(void *self, const char *sel, ...)
{
    (void)self;
    (void)sel;
    if (!g_content_view)
        g_content_view = gmv_objc_new_instance("NSView");
    return g_content_view;
}

/* -[NSView initWithFrame:(NSRect)] -- 4 floats by value; returns self (the game's EAGLView). */
static void *fnd_NSView_initWithFrame(void *self, const char *sel,
                                      float x, float y, float w, float h)
{
    (void)sel; (void)x; (void)y; (void)w; (void)h;
    return self;
}

/* -[NSView window] : the NSWindow hosting the view.  Our single GameWindow (captured at creation)
   stands in; the game asks the view for its window to warp/centre the cursor and check key state
   (EAGLView -centerMouse / -setMouseAlwaysCenter).  Returning it lets those proceed. */
extern void *gmv_objc_last_gamewindow;          /* shimsobjc.c */
static void *fnd_NSView_window(void *self, const char *sel, ...)
{
    (void)self;
    (void)sel;
    return gmv_objc_last_gamewindow;
}

/* ---- NSNumber (minimal: a fake object storing one long at +4) ------------------------- */
static void *make_nsnumber(long v)
{
    void *n = gmv_objc_new_instance("NSNumber");
    if (n)
        ((long *)n)[1] = v;                     /* value at +4 */
    return n;
}
static void *fnd_NSNumber_value(void *self, const char *sel, ...)
{
    (void)sel;
    return self ? (void *)(uintptr_t)((long *)self)[1] : NULL;   /* int/integer/boolValue in eax */
}
static void *fnd_NSNumber_with(void *cls, const char *sel, long v, ...)
{
    (void)cls;
    (void)sel;
    return make_nsnumber(v);
}

/* ---- NSUserDefaults (no persistent store yet; seed the resolution the game reads on launch) --
   -[GameDelegate loadUserSettings] does screenWidth/Height = [[defaults objectForKey:@"ScreenWidth"/
   @"ScreenHeight"] integerValue].  With a nil result those come out 0, and a 0x0 game resolution
   makes the engine cull everything (black screen).  Return the GL drawable size for those keys so
   the game runs at 1024x768; other keys stay nil so the game uses its own defaults. */
static void *g_user_defaults;

static int ud_trace(void)
{
    static int t = -1;
    if (!GMV_DEV)
        return 0;
    if (t < 0) { const char *e = gmv_dbgenv("GMV_RES_TRACE"); t = (e && *e && *e != '0') ? 1 : 0; }
    return t;
}

/* ---- prefs backing NSUserDefaults (integers/bools; enough for the resolution options)
   The game stores ScreenWidth/ScreenHeight/FullScreen via setInteger:/setBool:forKey: and reads
   them at launch (loadUserSettings); the GL shim sizes the window from them (gmv_effective_screen)
   and applies a change at once (gmv_gl_apply_mode).  Those three live in GangstarMV.ini's
   [Display] section (SCREEN_WIDTH, SCREEN_HEIGHT, FULLSCREEN), written back the moment the game
   changes them; an environment variable GMV_<key> wins, as for every ini key.  Any other key is
   kept for the run only.  Older builds kept the three in gmv_prefs.txt (savegame\ or next to the
   exe); prefs_migrate moves them into the ini once and deletes the file. */
static const struct { const char *ud, *ini; } ini_prefs[] = {
    {"ScreenWidth", "SCREEN_WIDTH"}, {"ScreenHeight", "SCREEN_HEIGHT"}, {"FullScreen", "FULLSCREEN"},
};
struct gmv_pref { char key[48]; long val; int used; };
static struct gmv_pref g_prefs[64];
static int g_prefs_loaded;

static const char *ini_pref(const char *key)
{
    for (size_t i = 0; i < sizeof ini_prefs / sizeof *ini_prefs; i++)
        if (!strcmp(key, ini_prefs[i].ud)) return ini_prefs[i].ini;
    return NULL;
}
static int ini_pref_get(const char *name, long *out)
{
    char env[64];
    snprintf(env, sizeof env, "GMV_%s", name);
    const char *s = gmv_envkey(env);
    if (!s) s = gmv_config_file_value(name);
    char *end;
    long v = s ? strtol(s, &end, 10) : 0;
    if (!s || end == s) return 0;
    *out = v;
    return 1;
}
static void ini_pref_set(const char *name, long v)
{
    const char *f = gmv_config_file_value(name);
    char *end;
    if (f && strtol(f, &end, 10) == v && end != f && !*end)
        return;                                     /* the file already says so */
    char buf[24];
    snprintf(buf, sizeof buf, "%ld", v);
    gmv_config_save(name, buf);
}

/* gmv_prefs.txt from an older build -> GangstarMV.ini, once. */
static void prefs_migrate(void)
{
    char exe[MAX_PATH];
    DWORD k = GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe);
    if (!k || k >= sizeof exe) return;
    char *slash = strrchr(exe, GMV_DIRSEP);
    if (slash) slash[1] = 0; else exe[0] = 0;
    char path[2][MAX_PATH + 32];
    snprintf(path[0], sizeof path[0], "%ssavegame" GMV_SEP "gmv_prefs.txt", exe);
    snprintf(path[1], sizeof path[1], "%sgmv_prefs.txt", exe);
    for (int p = 0; p < 2; p++) {
        FILE *f = fopen(path[p], "r");
        if (!f) continue;
        int ok = 1;
        char line[128];
        while (fgets(line, sizeof line, f)) {
            char key[48]; long v;
            const char *name;
            if (sscanf(line, "%47s %ld", key, &v) == 2 && (name = ini_pref(key))) {
                char buf[24];
                snprintf(buf, sizeof buf, "%ld", v);
                const char *cur = gmv_config_file_value(name);
                if (!(cur && !strcmp(cur, buf)))
                    ok &= gmv_config_save(name, buf);
            }
        }
        fclose(f);
        if (ok && DeleteFileA(path[p]))
            gmv_log("[res] moved the resolution from gmv_prefs.txt into " GMV_GAME_NAME ".ini\n");
    }
}
static void prefs_load(void)
{
    if (g_prefs_loaded) return;
    g_prefs_loaded = 1;
    prefs_migrate();
}
static int prefs_get(const char *key, long *out)
{
    prefs_load();
    const char *name = ini_pref(key);
    if (name) return ini_pref_get(name, out);
    for (int i = 0; i < 64; i++)
        if (g_prefs[i].used && !strcmp(g_prefs[i].key, key)) { *out = g_prefs[i].val; return 1; }
    return 0;
}
static void prefs_set(const char *key, long v)
{
    prefs_load();
    const char *name = ini_pref(key);
    if (name) { ini_pref_set(name, v); return; }
    for (int i = 0; i < 64; i++)
        if (g_prefs[i].used && !strcmp(g_prefs[i].key, key)) { g_prefs[i].val = v; return; }
    for (int i = 0; i < 64; i++)
        if (!g_prefs[i].used) {
            snprintf(g_prefs[i].key, sizeof g_prefs[i].key, "%s", key);
            g_prefs[i].val = v; g_prefs[i].used = 1; return;
        }
}

/* Exposed to the GL shim. */
long gmv_prefs_get_long(const char *key, long def) { long v; return prefs_get(key, &v) ? v : def; }

/* The native primary-monitor size (the default resolution, and the full-screen window size). */
static void native_desktop(long *w, long *h)
{
    int dw = 0, dh = 0;
    gmv_video_desktop(&dw, &dh);
    long sw = dw, sh = dh;
    if (sw < 320) sw = 1024;
    if (sh < 240) sh = 768;
    if (w) *w = sw;
    if (h) *h = sh;
}

/* The game's chosen Screen Resolution option (the Video dropdown value).  This is the RENDER
   resolution the game targets in BOTH windowed and full-screen -- the game's own design, and the
   faithful behaviour: full-screen renders this and the borderless window fills the monitor.  For a
   crisp native full-screen, the user picks the native mode (e.g. 1920x1080) in the dropdown; it is
   the default on first run.  Deliberately NOT remapped to native in full-screen: the earlier remap
   made the dropdown show one value while the game targeted another, so it got "stuck" at the
   windowed resolution.  Clamped to sane bounds. */
static void gmv_stored_res(long *w, long *h)
{
    long dw, dh; native_desktop(&dw, &dh);
    long ww = gmv_prefs_get_long("ScreenWidth", dw), hh = gmv_prefs_get_long("ScreenHeight", dh);
    if (ww < 320) ww = 320;
    if (ww > 7680) ww = 7680;
    if (hh < 240) hh = 240;
    if (hh > 4320) hh = 4320;
    if (w) *w = ww;
    if (h) *h = hh;
}

/* The chosen Screen Resolution, which the game draws at in both modes (the GL shim scales it to the
   monitor in full screen). */
void gmv_chosen_resolution(long *w, long *h)
{
    gmv_stored_res(w, h);
}

/* The size the host WINDOW should use this run.  Full-screen: the native monitor size (borderless
   covers the screen; the game's drawing is scaled to fill via gmv_gl_follow_viewport).  Windowed:
   the chosen resolution.  the GL shim sizes/positions the window from this. */
void gmv_effective_screen(long *w, long *h, int *fullscreen)
{
    long fs = gmv_prefs_get_long("FullScreen", 1);
    if (fs) {
        native_desktop(w, h);
        if (fullscreen) *fullscreen = 1;
        return;
    }
    gmv_stored_res(w, h);
    if (fullscreen) *fullscreen = 0;
}

static void *fnd_NSUserDefaults_objectForKey(void *self, const char *sel, void *key)
{
    (void)self;
    (void)sel;
    const char *k = nsstring_cstr(key);
    if (ud_trace())
        gmv_log("[res] objectForKey: '%s'\n", k ? k : "?");
    if (k && (!strcmp(k, "ScreenWidth") || !strcmp(k, "ScreenHeight"))) {
        long sw, sh; gmv_stored_res(&sw, &sh);
        return make_nsnumber(!strcmp(k, "ScreenWidth") ? sw : sh);
    }
    if (k && !strcmp(k, "FullScreen"))
        return make_nsnumber(gmv_prefs_get_long("FullScreen", 1) ? 1 : 0);
    long v;
    if (k && prefs_get(k, &v))                      /* any other persisted int setting */
        return make_nsnumber(v);
    return NULL;
}

static void *fnd_NSUserDefaults_integerForKey(void *self, const char *sel, void *key)
{
    (void)self; (void)sel;
    const char *k = nsstring_cstr(key);
    if (k && (!strcmp(k, "ScreenWidth") || !strcmp(k, "ScreenHeight"))) {
        long sw, sh; gmv_stored_res(&sw, &sh);
        return (void *)(uintptr_t)(!strcmp(k, "ScreenWidth") ? sw : sh);
    }
    long v;
    if (k && prefs_get(k, &v)) return (void *)(uintptr_t)v;
    return (void *)0;
}
static void *fnd_NSUserDefaults_boolForKey(void *self, const char *sel, void *key)
{
    (void)self; (void)sel;
    const char *k = nsstring_cstr(key);
    if (k && !strcmp(k, "FullScreen"))
        return (void *)(uintptr_t)(gmv_prefs_get_long("FullScreen", 1) ? 1 : 0);
    long v;
    if (k && prefs_get(k, &v)) return (void *)(uintptr_t)(v ? 1 : 0);
    return (void *)0;
}

/* The game writes the in-game resolution/fullscreen option here; we persist it (applied on the next
   launch, matching how the game reads these only at startup).  GMV_RES_TRACE=1 logs each write. */
static void *fnd_NSUserDefaults_setObject(void *self, const char *sel, void *val, void *key)
{
    (void)self; (void)sel;
    if (ud_trace()) {
        const char *k = nsstring_cstr(key), *vs = nsstring_cstr(val);
        gmv_log("[res] setObject:forKey: key='%s' val='%s'\n", k ? k : "?", vs ? vs : "(non-string)");
    }
    return (void *)0;                               /* strings not persisted yet (not needed for res) */
}
static void *fnd_NSUserDefaults_setInteger(void *self, const char *sel, long v, void *key)
{
    (void)self; (void)sel;
    const char *k = nsstring_cstr(key);
    if (ud_trace()) gmv_log("[res] setInteger:%ld forKey:'%s'\n", v, k ? k : "?");
    if (k) prefs_set(k, v);
    if (k && (!strcmp(k, "ScreenWidth") || !strcmp(k, "ScreenHeight") || !strcmp(k, "FullScreen")))
        gmv_gl_apply_mode();                        /* apply the new Video option to the window now */
    return (void *)0;
}
static void *fnd_NSUserDefaults_setBool(void *self, const char *sel, long v, void *key)
{
    (void)self; (void)sel;
    const char *k = nsstring_cstr(key);
    if (ud_trace()) gmv_log("[res] setBool:%ld forKey:'%s'\n", v, k ? k : "?");
    if (k) prefs_set(k, v ? 1 : 0);
    if (k && (!strcmp(k, "FullScreen") || !strcmp(k, "ScreenWidth") || !strcmp(k, "ScreenHeight")))
        gmv_gl_apply_mode();                        /* fullscreen toggle takes effect immediately */
    return (void *)0;
}

static void *fnd_NSUserDefaults_standard(void *cls, const char *sel, ...)
{
    (void)cls;
    (void)sel;
    if (!g_user_defaults)
        g_user_defaults = gmv_objc_new_instance("NSUserDefaults");
    return g_user_defaults;
}

/* ---- NSNotificationCenter (we don't deliver notifications; observers are no-ops) ------ */

static void *g_notif_center;
static void *fnd_NSNotificationCenter_default(void *cls, const char *sel, ...)
{
    (void)cls;
    (void)sel;
    if (!g_notif_center)
        g_notif_center = gmv_objc_new_instance("NSNotificationCenter");
    return g_notif_center;
}

static void *g_workspace;
static void *fnd_NSWorkspace_shared(void *cls, const char *sel, ...)
{
    (void)cls;
    (void)sel;
    if (!g_workspace)
        g_workspace = gmv_objc_new_instance("NSWorkspace");
    return g_workspace;
}
static void *fnd_NSWorkspace_notifCenter(void *self, const char *sel, ...)
{
    (void)self;
    (void)sel;
    return fnd_NSNotificationCenter_default(NULL, NULL);
}

/* The fake QTMovie from -[QTMovie initWithFile:error:] (set below); the logo-end simulation
   carries it in the notification. */
static void *g_qtmovie;

/* Observer registry so we can actually deliver notifications (e.g. to advance past the logo). */
#define MAX_OBS 64
static struct { void *observer; const char *sel; const char *name; } g_obs[MAX_OBS];
static int g_nobs;

/* -[NSNotificationCenter addObserver:selector:name:object:] */
static void *fnd_nc_addObserver(void *self, const char *cmd, void *observer,
                                const char *selector, void *name, void *object)
{
    (void)self;
    (void)cmd;
    (void)object;
    const char *nm = name ? nsstring_cstr(name) : NULL;
    if (g_nobs < MAX_OBS) {
        g_obs[g_nobs].observer = observer;
        g_obs[g_nobs].sel = selector;       /* a SEL = selector-name string */
        g_obs[g_nobs].name = nm;
        g_nobs++;
    }
    gmv_trace("[notif] observe %s -> sel %s\n", nm ? nm : "(any)", selector ? selector : "?");
    return NULL;
}

/* Deliver a notification to every matching observer.  Calls game code -> must realign. */
__attribute__((force_align_arg_pointer))
void gmv_notif_post(const char *name, void *notification)
{
    void *(*msg)(void *, const char *, ...) = (void *(*)(void *, const char *, ...))(void *)gmv_objc_msgSend;
    for (int i = 0; i < g_nobs; i++)
        if (g_obs[i].name && name && !strcmp(g_obs[i].name, name))
            msg(g_obs[i].observer, g_obs[i].sel, notification);
}

/* -[NSNotificationCenter postNotificationName:object:] */
static void *fnd_nc_post(void *self, const char *cmd, void *name, void *object)
{
    (void)self;
    (void)cmd;
    const char *nm = name ? nsstring_cstr(name) : NULL;
    if (nm)
        gmv_notif_post(nm, object);
    return NULL;
}

/* Build a fake NSNotification carrying the movie and post the QuickTime "did end" notification,
   so the game runs its real logo-finished handler and advances. */
void gmv_fire_movie_end(void)
{
    if (!g_qtmovie)
        return;
    void *note = gmv_objc_new_instance("NSNotification");
    if (note)
        ((void **)note)[1] = g_qtmovie;     /* -[NSNotification object] reads offset 4 */
    gmv_trace("[notif] firing QTMovieDidEndNotification (simulated logo end)\n");
    gmv_notif_post("QTMovieDidEndNotification", note);
    g_qtmovie = NULL;                        /* once */
}

/* -[QTMovie initWithFile:error:] -> a fake (non-nil) movie.  We don't decode video, but letting
   the game take its normal logo path (play the movie, observe its end) and then firing the
   movie-did-end notification ourselves advances the game's state machine past the logo. */
static char g_qtmovie_path[1024];           /* the file the game asked QTMovie to play (intro.mov) */

const char *gmv_intro_movie_path(void) { return g_qtmovie_path; }

static void *fnd_QTMovie_initWithFile(void *self, const char *sel, void *file, void **errorOut)
{
    (void)sel;
    if (errorOut)
        *errorOut = NULL;
    const char *fp = file ? nsstring_cstr(file) : NULL;
    if (fp) {
        strncpy(g_qtmovie_path, fp, sizeof g_qtmovie_path - 1);
        g_qtmovie_path[sizeof g_qtmovie_path - 1] = 0;
    }
    gmv_trace("[QTMovie] initWithFile: %s\n", fp ? fp : "(null)");
    g_qtmovie = self;
    return self;
}

/* -[NSNotification object] / -name : the backing object/name we stashed at offsets 4 / 8. */
static void *fnd_notification_object(void *self, const char *sel, ...)
{
    (void)sel;
    return self ? ((void **)self)[1] : NULL;
}

/* ---- NSColor (opaque; only ever passed to no-op setters like setBackgroundColor:) ----- */
static void *g_color;
static void *fnd_NSColor_color(void *cls, const char *sel, ...)
{
    (void)cls;
    (void)sel;
    if (!g_color)
        g_color = gmv_objc_new_instance("NSColor");
    return g_color;
}

static void *fnd_NSLocale_preferredLanguages(void *cls, const char *sel, ...)
{
    (void)cls;
    (void)sel;
    return gmv_make_nsarray1(gmv_make_nsstring("en"));
}
static void *g_locale;
static void *fnd_NSLocale_current(void *cls, const char *sel, ...)
{
    (void)cls;
    (void)sel;
    if (!g_locale)
        g_locale = gmv_objc_new_instance("NSLocale");
    return g_locale;
}

static void *g_main_screen;
static void *fnd_NSScreen_mainScreen(void *cls, const char *sel, ...)
{
    (void)cls;
    (void)sel;
    if (!g_main_screen)
        g_main_screen = gmv_objc_new_instance("NSScreen");
    return g_main_screen;
}

/* ---- NSTimer (the game's frame driver; the run loop in shimsappkit fires it) ---------- */
static void *g_timer_target, *g_timer_obj;
static const char *g_timer_sel;

static void *fnd_NSTimer_scheduled(void *cls, const char *sel, double interval,
                                   void *target, const char *selector, void *userInfo, int repeats)
{
    (void)cls; (void)sel; (void)userInfo; (void)repeats;
    g_timer_target = target;
    g_timer_sel = selector;
    if (!g_timer_obj)
        g_timer_obj = gmv_objc_new_instance("NSTimer");
    gmv_trace("[NSTimer] scheduled every %.4fs -> [target %s]\n", interval, selector ? selector : "?");
    return g_timer_obj;
}

void *gmv_timer_target(void)       { return g_timer_target; }
const char *gmv_timer_selector(void) { return g_timer_sel; }
void *gmv_timer_object(void)       { return g_timer_obj; }

/* ---- -[NSObject performSelector:withObject:afterDelay:] : deferred, run from the run loop -- */
#define MAX_DEFER 32
static struct { void *target; const char *sel; void *arg; } g_defer[MAX_DEFER];
static int g_defer_head, g_defer_tail;

static void *fnd_performAfterDelay(void *self, const char *_cmd, const char *aSel,
                                   void *arg, double delay)
{
    (void)_cmd;
    (void)delay;
    if ((g_defer_tail + 1) % MAX_DEFER != g_defer_head) {
        g_defer[g_defer_tail].target = self;
        g_defer[g_defer_tail].sel = aSel;
        g_defer[g_defer_tail].arg = arg;
        g_defer_tail = (g_defer_tail + 1) % MAX_DEFER;
    }
    gmv_trace("[perform] deferred [target %s] after %.3fs\n", aSel ? aSel : "?", delay);
    return NULL;
}

/* FIFO drain for the run loop; returns 0 when empty. */
int gmv_defer_take(void **target, const char **sel, void **arg)
{
    if (g_defer_head == g_defer_tail)
        return 0;
    *target = g_defer[g_defer_head].target;
    *sel = g_defer[g_defer_head].sel;
    *arg = g_defer[g_defer_head].arg;
    g_defer_head = (g_defer_head + 1) % MAX_DEFER;
    return 1;
}

/* ---- NSFileManager (real: backed by the Win32 filesystem) ----------------------------- */
static void *g_filemgr;
static void *fnd_fm_default(void *cls, const char *sel, ...)
{
    (void)cls;
    (void)sel;
    if (!g_filemgr)
        g_filemgr = gmv_objc_new_instance("NSFileManager");
    return g_filemgr;
}
static void *fnd_fm_fileExists(void *self, const char *sel, void *path)
{
    (void)self;
    (void)sel;
    const char *p = nsstring_cstr(path);
    return (void *)(uintptr_t)(p && GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES);
}
static void *fnd_fm_fileExistsIsDir(void *self, const char *sel, void *path, char *isDir)
{
    (void)self;
    (void)sel;
    const char *p = nsstring_cstr(path);
    DWORD a = p ? GetFileAttributesA(p) : INVALID_FILE_ATTRIBUTES;
    if (isDir)
        *isDir = (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
    return (void *)(uintptr_t)(a != INVALID_FILE_ATTRIBUTES);
}
static void *fnd_fm_createDir(void *self, const char *sel, void *path, int interm, void *attrs, void **err)
{
    (void)self;
    (void)sel;
    (void)interm;
    (void)attrs;
    const char *p = nsstring_cstr(path);
    if (err)
        *err = NULL;
    if (p)
        CreateDirectoryA(p, NULL);
    return (void *)1;                            /* YES */
}

/* Shared trivial IMPs (integer/pointer result in eax). */
static void *fnd_zero(void *self, const char *sel, ...) { (void)self; (void)sel; return (void *)0; }
static void *fnd_yes(void *self, const char *sel, ...)  { (void)self; (void)sel; return (void *)1; }
static void *fnd_ret_self(void *self, const char *sel, ...) { (void)sel; return self; }

/* +[NSCursor hide]/+[NSCursor unhide] -- the game's own cursor-visibility toggle (reached through
   -[EAGLView hideCursore:]).  It hides the cursor exactly when it enters relative mouse-look, so we
   record the intent; shimsinput.c follows it for hands-free auto mouse-look (GMV_AUTO_LOOK). */
static void *fnd_NSCursor_hide(void *self, const char *sel, ...)   { (void)self; (void)sel; gmv_input_set_cursor_hidden(1); return NULL; }
static void *fnd_NSCursor_unhide(void *self, const char *sel, ...) { (void)self; (void)sel; gmv_input_set_cursor_hidden(0); return NULL; }

/* -[NSApplication terminate:] -- the Cocoa "quit the app" message.  On macOS terminate: ends the
   process and NEVER returns.  The game reaches it only from its exit state: the per-frame dispatcher
   at 0x596680 calls the state object's vtbl slot 0x1c, which (in the quit state, via quit-helper
   0x19bae0) sends terminate:, and then IMMEDIATELY calls vtbl slot 0x20 for the next update step.
   We used to no-op terminate: "so play continues", but that return is exactly the bug: slot 0x20
   then runs on a half-torn-down quitting state and faults (write to a wrong sub-object at 0x11c6c5,
   seen as the teardown crash).  So we now do what the game expects -- quit cleanly and do not return.
   We exit the process directly (ExitProcess kills every thread at once, so no further audio render
   callback or frame update touches the dying state; the OS closes the audio device).  Set
   GMV_TERM_NOOP=1 to restore the old no-op (debug escape hatch, should a spurious terminate: appear).
   objc_msgSend tail-jumps here, so our return address is the game instruction right after its
   `call objc_msgSend`. */
static void *fnd_NSApp_terminate(void *self, const char *sel, ...)
{
    (void)self; (void)sel;
    uint32_t caller = (uint32_t)(uintptr_t)__builtin_return_address(0);
    const char *noop = gmv_dbgenv("GMV_TERM_NOOP");
    int do_noop = noop && *noop && *noop != '0';
    gmv_trace("[appkit] -[NSApplication terminate:] sent by game (caller 0x%08x); %s\n",
            caller, do_noop ? "NO-OP so play continues" : "quitting cleanly (ExitProcess 0)");
    uint32_t *fp = (uint32_t *)__builtin_frame_address(0);
    for (int d = 0; GMV_DEV && d < 16 && fp && !IsBadReadPtr(fp, 8); d++) {
        uint32_t ret = fp[1], prev = fp[0];
        if (ret >= GMV_LOW_START && ret < GMV_LOWCOPY_BASE)
            gmv_log("        terminate# %-2d ret 0x%08x\n", d, ret);
        if (prev <= (uint32_t)(uintptr_t)fp) break;
        fp = (uint32_t *)(uintptr_t)prev;
    }
    if (do_noop)
        return (void *)0;           /* old behaviour: returns -> game continues into teardown crash */
    log_close();                    /* flush GangstarMV.log before we leave */
    ExitProcess(0);
    return (void *)0;               /* unreachable */
}

/* ---- struct-returning methods (objc_msgSend_stret) ------------------------------------ */

/* QTMovieView delegate: stored in a global (one logo MovieView) -- can't stash it in a game
   object's ivars.  movieDidEnd: forwards through this to the game's GameDelegate. */
static void *g_view_delegate;
static void *fnd_setViewDelegate(void *self, const char *sel, void *d)
{
    (void)self;
    (void)sel;
    g_view_delegate = d;
    return NULL;
}
static void *fnd_viewDelegate(void *self, const char *sel, ...)
{
    (void)self;
    (void)sel;
    return g_view_delegate;
}

/* NSRect is 4 floats; macOS i386 returns it via a hidden sret pointer the callee pops (ret $4),
   so these use callee_pop_aggregate_return(1).  objc_msgSend_stret tail-jumps here with the
   stack {sret, self, sel}, which is exactly gcc's (hidden sret, self, sel) layout. */
typedef struct { float x, y, w, h; } NSRectF;

__attribute__((callee_pop_aggregate_return(1)))
static NSRectF fnd_rect_full(void *self, const char *sel)
{
    (void)self;
    (void)sel;
    NSRectF r = {0.0f, 0.0f, 1024.0f, 768.0f};
    return r;
}

static const struct fnd_method g_stret_methods[] = {
    {"NSScreen", "frame", (void *)fnd_rect_full},
    {"NSScreen", "visibleFrame", (void *)fnd_rect_full},
    {"NSWindow", "frame", (void *)fnd_rect_full},
    {"NSWindow", "contentRectForFrameRect:", (void *)fnd_rect_full},
    {"NSView", "frame", (void *)fnd_rect_full},
    {"NSView", "bounds", (void *)fnd_rect_full},
    {NULL, NULL, NULL},
};

void *gmv_fnd_stret_method(const char *cls, const char *sel)
{
    for (const struct fnd_method *m = g_stret_methods; m->cls; m++)
        if (!strcmp(m->cls, cls) && !strcmp(m->sel, sel))
            return m->imp;
    return NULL;
}

/* ---- method tables -------------------------------------------------------------------- */

static const struct fnd_method g_class_methods[] = {
    {"NSBundle", "mainBundle", (void *)fnd_NSBundle_mainBundle},
    {"NSString", "stringWithFormat:", (void *)fnd_NSString_stringWithFormat},
    {"NSString", "stringWithUTF8String:", (void *)fnd_NSString_stringWithUTF8String},
    {"NSString", "stringWithCString:encoding:", (void *)fnd_NSString_stringWithCString},
    {"NSDate", "date", (void *)fnd_NSDate_date},
    {"NSThread", "detachNewThreadSelector:toTarget:withObject:", (void *)fnd_NSThread_detach},
    {"NSURL", "URLWithString:", (void *)fnd_NSURL_urlWithString},
    {"NSURLRequest", "requestWithURL:", (void *)fnd_NSURLRequest_with},
    {"NSMutableURLRequest", "requestWithURL:", (void *)fnd_NSURLRequest_with},
    {"NSUserDefaults", "standardUserDefaults", (void *)fnd_NSUserDefaults_standard},
    {"NSNotificationCenter", "defaultCenter", (void *)fnd_NSNotificationCenter_default},
    {"NSNumber", "numberWithInt:", (void *)fnd_NSNumber_with},
    {"NSNumber", "numberWithInteger:", (void *)fnd_NSNumber_with},
    {"NSNumber", "numberWithBool:", (void *)fnd_NSNumber_with},
    {"NSArray", "arrayWithObjects:", (void *)fnd_NSArray_arrayWithObjects},
    {"NSColor", "blackColor", (void *)fnd_NSColor_color},
    {"NSColor", "whiteColor", (void *)fnd_NSColor_color},
    {"NSColor", "clearColor", (void *)fnd_NSColor_color},
    {"NSScreen", "mainScreen", (void *)fnd_NSScreen_mainScreen},
    {"NSLocale", "preferredLanguages", (void *)fnd_NSLocale_preferredLanguages},
    {"NSLocale", "currentLocale", (void *)fnd_NSLocale_current},
    {"NSTimer", "scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:", (void *)fnd_NSTimer_scheduled},
    {"NSWorkspace", "sharedWorkspace", (void *)fnd_NSWorkspace_shared},
    {"NSFileManager", "defaultManager", (void *)fnd_fm_default},
    {"NSCursor", "hide", (void *)fnd_NSCursor_hide},
    {"NSCursor", "unhide", (void *)fnd_NSCursor_unhide},
    {NULL, NULL, NULL},
};

/* ---- NSEvent (synthetic, for injected mouse input; see shimsinput.c) ------------------
   Our fake NSEvent stores the window-space location (two floats) at offsets 4 and 8.  macOS i386
   returns an 8-byte struct (NSPoint) in eax:edx, so -locationInWindow returns a 2-word struct
   holding the float bit patterns. */
typedef struct { uint32_t x, y; } ns_point2;
static ns_point2 fnd_NSEvent_location(void *self, const char *sel, ...)
{
    (void)sel;
    ns_point2 p = {0, 0};
    if (self) {
        p.x = ((const uint32_t *)self)[1];      /* float bits at +4 */
        p.y = ((const uint32_t *)self)[2];      /* float bits at +8 */
    }
    return p;
}
static void *fnd_NSEvent_zero(void *self, const char *sel, ...) { (void)self; (void)sel; return NULL; }

/* -[NSEvent deltaY] -- the scroll-wheel amount for -scrollWheel:.  Stored at +12 by
   gmv_make_nsscroll.  Returns CGFloat (= float on i386), i.e. in ST(0); objc_msgSend tail-jumps
   here so the game reads our ST(0) directly. */
static float fnd_NSEvent_deltaY(void *self, const char *sel, ...)
{
    (void)sel;
    return self ? ((const float *)self)[3] : 0.0f;   /* float bits at +12 */
}

/* -[NSWindow convertBaseToScreen:(NSPoint)] / convertScreenToBase: -- window<->screen point
   conversion.  NSPoint is two floats by value (after self,_cmd) and is returned in eax:edx.  Our
   single window fills the screen origin we report (0,0), so the transform is identity; the game
   uses it to position the warp target for -[EAGLView centerMouse], which we do not warp anyway. */
static ns_point2 fnd_convert_point(void *self, const char *sel, uint32_t x, uint32_t y)
{
    (void)self;
    (void)sel;
    ns_point2 p = {x, y};                       /* pass the point through (float bits preserved) */
    return p;
}

void *gmv_make_nsevent(float x, float y)
{
    void *e = gmv_objc_new_instance("NSEvent");
    if (e) {
        ((float *)e)[1] = x;                    /* +4 */
        ((float *)e)[2] = y;                    /* +8 */
    }
    return e;
}

/* As gmv_make_nsevent, plus a scroll amount at +12 read back by -[NSEvent deltaY]. */
void *gmv_make_nsscroll(float x, float y, float deltaY)
{
    void *e = gmv_make_nsevent(x, y);
    if (e)
        ((float *)e)[3] = deltaY;               /* +12 (instances are >= 16 bytes) */
    return e;
}

/* ---- NSAlert (modal dialog; the social/Twitter features raise one) --------------------------
   The game uses the synchronous path: [[NSAlert alloc] init], setMessageText:/setInformativeText:/
   setAlertStyle:, addButtonWithTitle: (1-3 times), then runModal.  We render it as a native Win32
   MessageBox owned by the game window and map the clicked button back to NSAlertFirstButtonReturn +
   index.  The per-alert state lives in a small struct hung off the instance's +4 slot. */
struct nsalert {
    char *msg, *info;
    char *buttons[4];
    int nbuttons;
    int style;
};

static struct nsalert *alert_of(void *self)
{
    if (!self)
        return NULL;
    struct nsalert *a = ((struct nsalert **)self)[1];
    if (!a) {
        a = calloc(1, sizeof *a);
        ((struct nsalert **)self)[1] = a;
    }
    return a;
}

static void *fnd_NSAlert_setMessageText(void *self, const char *sel, void *s)
{
    (void)sel;
    struct nsalert *a = alert_of(self);
    if (a) { free(a->msg); const char *c = nsstring_cstr(s); a->msg = c ? strdup(c) : NULL; }
    return NULL;
}
static void *fnd_NSAlert_setInformativeText(void *self, const char *sel, void *s)
{
    (void)sel;
    struct nsalert *a = alert_of(self);
    if (a) { free(a->info); const char *c = nsstring_cstr(s); a->info = c ? strdup(c) : NULL; }
    return NULL;
}
static void *fnd_NSAlert_setAlertStyle(void *self, const char *sel, int style)
{
    (void)sel;
    struct nsalert *a = alert_of(self);
    if (a)
        a->style = style;
    return NULL;
}
static void *g_nsbutton_dummy;
static void *fnd_NSAlert_addButton(void *self, const char *sel, void *title)
{
    (void)sel;
    struct nsalert *a = alert_of(self);
    if (a && a->nbuttons < (int)(sizeof a->buttons / sizeof a->buttons[0])) {
        const char *t = nsstring_cstr(title);
        a->buttons[a->nbuttons++] = strdup(t ? t : "");
    }
    if (!g_nsbutton_dummy)
        g_nsbutton_dummy = gmv_objc_new_instance("NSButton");
    return g_nsbutton_dummy;                    /* NSButton*; the game ignores the return */
}

/* Force the modal MessageBox to the foreground over the borderless full-screen game window.
   MessageBox draws on top but cannot grab the keyboard/foreground away from a WS_POPUP window
   that covers the whole screen, so the dialog ignores the mouse and keys until the user taps the
   Windows key (which breaks the game's foreground hold).  We do the same thing programmatically:
   clear the system foreground-lock timeout for a moment, then activate the dialog.  Driven by a
   NULL-window timer -- MessageBox's own modal message loop dispatches it. */
#ifdef _WIN32
static UINT_PTR g_alert_fg_timer;
static int      g_alert_fg_tries;

static void alert_force_fg(HWND dlg)
{
    if (!dlg)
        return;
    HWND  fgw   = GetForegroundWindow();
    DWORD fgTid = fgw ? GetWindowThreadProcessId(fgw, NULL) : 0;
    DWORD myTid = GetCurrentThreadId();
    DWORD lock  = 0;
    SystemParametersInfoA(SPI_GETFOREGROUNDLOCKTIMEOUT, 0, &lock, 0);
    SystemParametersInfoA(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, (PVOID)0, SPIF_SENDCHANGE);
    if (fgTid && fgTid != myTid) AttachThreadInput(myTid, fgTid, TRUE);
    BringWindowToTop(dlg);
    SetForegroundWindow(dlg);
    SetActiveWindow(dlg);
    SetFocus(dlg);
    if (fgTid && fgTid != myTid) AttachThreadInput(myTid, fgTid, FALSE);
    SystemParametersInfoA(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, (PVOID)(UINT_PTR)lock, SPIF_SENDCHANGE);
}

static void CALLBACK alert_fg_tick(HWND hwnd, UINT msg, UINT_PTR id, DWORD t)
{
    (void)hwnd; (void)msg; (void)t;
    HWND owner = (HWND)gmv_gl_window_hwnd();
    HWND dlg   = owner ? GetWindow(owner, GW_ENABLEDPOPUP) : NULL;  /* the MessageBox popup */
    if (dlg && dlg != owner) {
        alert_force_fg(dlg);
        KillTimer(NULL, id);
        g_alert_fg_timer = 0;
    } else if (++g_alert_fg_tries >= 20) {      /* ~0.6s and no dialog yet: give up quietly */
        KillTimer(NULL, id);
        g_alert_fg_timer = 0;
    }
}
#endif

static void *fnd_NSAlert_runModal(void *self, const char *sel, ...)
{
    (void)sel;
    struct nsalert *a = alert_of(self);
    char text[2048];
    text[0] = 0;
    if (a && a->msg)
        strncat(text, a->msg, sizeof text - strlen(text) - 1);
    if (a && a->info && *a->info) {
        strncat(text, "\n\n", sizeof text - strlen(text) - 1);
        strncat(text, a->info, sizeof text - strlen(text) - 1);
    }
    int nb = a ? a->nbuttons : 0;
    long ret;                                   /* NSAlertFirstButtonReturn = 1000, then +1 each */
#ifndef _WIN32
    /* The alert's own buttons, by their titles: the first is Return's, the last Escape's. */
    SDL_MessageBoxButtonData b[3];
    int shown = nb < 1 ? 1 : nb > 3 ? 3 : nb;
    for (int i = 0; i < shown; i++) {
        b[i].flags = (i == 0 ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0) |
                     (i == shown - 1 && shown > 1 ? SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT : 0);
        b[i].buttonID = 1000 + i;
        b[i].text = i < nb && a->buttons[i] && *a->buttons[i] ? a->buttons[i] : "OK";
    }
    SDL_MessageBoxData box = {(a && a->style >= 2) ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_INFORMATION, gmv_gl_window(),
                              GMV_GAME_TITLE, text[0] ? text : " ", shown, b, NULL};
    gmv_trace("[NSAlert] runModal: \"%s\" buttons=%d\n", (a && a->msg) ? a->msg : "", nb);
    if (gmv_gl_window())
        SDL_SetWindowRelativeMouseMode(gmv_gl_window(), false);     /* the input shim sets it again */
    int hit = -1;
    if (!SDL_ShowMessageBox(&box, &hit) || hit < 0)
        hit = 1000 + shown - 1;                 /* closed: the last button, as Escape */
    ret = hit;
#else
    UINT type = MB_SETFOREGROUND | MB_TOPMOST;
    type |= (a && a->style >= 2) ? MB_ICONERROR : MB_ICONINFORMATION;
    if (nb >= 3)      type |= MB_YESNOCANCEL;
    else if (nb == 2) type |= MB_OKCANCEL;
    else              type |= MB_OK;

    gmv_trace("[NSAlert] runModal: \"%s\" buttons=%d\n", (a && a->msg) ? a->msg : "", nb);
    /* Release the game's cursor confinement and arrange for the dialog to be pulled to the
       foreground while it is up, so it is clickable/typable without tapping the Windows key. */
    ClipCursor(NULL);
    ReleaseCapture();
    g_alert_fg_tries = 0;
    g_alert_fg_timer = SetTimer(NULL, 0, 30, alert_fg_tick);
    int r = MessageBoxA((HWND)gmv_gl_window_hwnd(), text[0] ? text : " ",
                        GMV_GAME_TITLE, type);
    if (g_alert_fg_timer) { KillTimer(NULL, g_alert_fg_timer); g_alert_fg_timer = 0; }

    if (nb >= 3)
        ret = (r == IDYES) ? 1000 : (r == IDNO) ? 1001 : 1002;
    else if (nb == 2)
        ret = (r == IDOK) ? 1000 : 1001;
    else
        ret = 1000;
#endif

    if (a) {                                    /* the game discards the alert after runModal */
        free(a->msg);
        free(a->info);
        for (int i = 0; i < a->nbuttons; i++)
            free(a->buttons[i]);
        free(a);
        ((void **)self)[1] = NULL;
    }
    return (void *)(intptr_t)ret;
}

static const struct fnd_method g_instance_methods[] = {
    {"NSEvent", "locationInWindow", (void *)fnd_NSEvent_location},
    {"NSEvent", "deltaY", (void *)fnd_NSEvent_deltaY},
    {"NSEvent", "type", (void *)fnd_NSEvent_zero},
    {"NSEvent", "buttonNumber", (void *)fnd_NSEvent_zero},
    {"NSEvent", "modifierFlags", (void *)fnd_NSEvent_zero},
    {"NSEvent", "window", (void *)fnd_NSEvent_zero},
    {"NSBundle", "bundlePath", (void *)fnd_NSBundle_bundlePath},
    {"NSBundle", "pathForResource:ofType:", (void *)fnd_NSBundle_pathForResource},
    {"NSBundle", "resourcePath", (void *)fnd_NSBundle_resourcePath},
    {"NSBundle", "infoDictionary", (void *)fnd_NSBundle_infoDictionary},
    {"NSBundle", "objectForInfoDictionaryKey:", (void *)fnd_zero},
    {"NSDictionary", "objectForKey:", (void *)fnd_zero},
    {"NSDictionary", "valueForKey:", (void *)fnd_zero},
    {"NSLocale", "objectForKey:", (void *)fnd_zero},
    {"NSDateFormatter", "setDateFormat:", (void *)fnd_zero},
    {"NSDateFormatter", "setLocale:", (void *)fnd_zero},
    {"NSDateFormatter", "stringFromDate:", (void *)fnd_NSDateFormatter_stringFromDate},
    {"NSAlert", "setMessageText:", (void *)fnd_NSAlert_setMessageText},
    {"NSAlert", "setInformativeText:", (void *)fnd_NSAlert_setInformativeText},
    {"NSAlert", "setAlertStyle:", (void *)fnd_NSAlert_setAlertStyle},
    {"NSAlert", "addButtonWithTitle:", (void *)fnd_NSAlert_addButton},
    {"NSAlert", "runModal", (void *)fnd_NSAlert_runModal},
    {"NSArray", "count", (void *)fnd_NSArray_count},
    {"NSArray", "objectAtIndex:", (void *)fnd_NSArray_objectAtIndex},
    {"NSArray", "lastObject", (void *)fnd_NSArray_lastObject},
    {"NSArray", "firstObject", (void *)fnd_NSArray_firstObject},
    {"NSFileManager", "fileExistsAtPath:", (void *)fnd_fm_fileExists},
    {"NSFileManager", "fileExistsAtPath:isDirectory:", (void *)fnd_fm_fileExistsIsDir},
    {"NSFileManager", "createDirectoryAtPath:withIntermediateDirectories:attributes:error:", (void *)fnd_fm_createDir},
    {"NSString", "UTF8String", (void *)fnd_NSString_UTF8String},
    {"NSString", "length", (void *)fnd_NSString_length},
    {"NSString", "initWithString:", (void *)fnd_NSString_initWithString},
    {"NSString", "initWithUTF8String:", (void *)fnd_NSString_initWithUTF8String},
    {"NSString", "isEqual:", (void *)fnd_NSString_isEqual},
    {"NSString", "isEqualToString:", (void *)fnd_NSString_isEqual},
    {"NSString", "substringToIndex:", (void *)fnd_NSString_substringToIndex},
    {"NSString", "hasPrefix:", (void *)fnd_NSString_hasPrefix},
    {"NSString", "stringByAppendingPathComponent:", (void *)fnd_NSString_appendPathComponent},
    {"NSString", "stringByAppendingString:", (void *)fnd_NSString_appendString},
    {"NSString", "writeToFile:atomically:encoding:error:", (void *)fnd_NSString_writeToFile},
    {"NSString", "initWithFormat:", (void *)fnd_NSString_initWithFormat},
    {"NSString", "stringByReplacingOccurrencesOfString:withString:", (void *)fnd_NSString_replace},
    {"NSString", "stringByAddingPercentEscapesUsingEncoding:", (void *)fnd_NSString_percentEscapes},
    {"NSURL", "absoluteString", (void *)fnd_NSURL_absoluteString},
    {"NSMutableURLRequest", "setHTTPMethod:", (void *)fnd_zero},
    {"NSMutableURLRequest", "setHTTPBody:", (void *)fnd_zero},
    {"NSMutableURLRequest", "setValue:forHTTPHeaderField:", (void *)fnd_zero},
    {"NSURLConnection", "initWithRequest:delegate:", (void *)fnd_zero},
    {"NSData", "initWithContentsOfURL:", (void *)fnd_zero},
    {"NSData", "dataWithContentsOfURL:", (void *)fnd_zero},
    {"NSData", "length", (void *)fnd_zero},
    {"NSData", "bytes", (void *)fnd_zero},
    {"NSWindow", "initWithContentRect:styleMask:backing:defer:", (void *)fnd_NSWindow_initWithContentRect},
    /* NSWindow configuration -- all no-ops for now (no real window); the superclass walk routes
       these from GameWindow.  setFrame:/setFrame:display: take an NSRect by value, harmlessly
       ignored by fnd_zero. */
    {"NSWindow", "setAcceptsMouseMovedEvents:", (void *)fnd_zero},
    {"NSWindow", "setAcceptsTouchEvents:", (void *)fnd_zero},
    {"NSWindow", "setBackgroundColor:", (void *)fnd_zero},
    {"NSWindow", "setHidesOnDeactivate:", (void *)fnd_zero},
    {"NSWindow", "setOpaque:", (void *)fnd_zero},
    {"NSWindow", "setLevel:", (void *)fnd_zero},
    {"NSWindow", "setStyleMask:", (void *)fnd_zero},
    {"NSWindow", "setPresentationOptions:", (void *)fnd_zero},
    {"NSWindow", "setPreservesContentDuringLiveResize:", (void *)fnd_zero},
    {"NSWindow", "setTitle:", (void *)fnd_zero},
    {"NSWindow", "setDelegate:", (void *)fnd_zero},
    {"NSWindow", "setContentView:", (void *)fnd_zero},
    {"NSWindow", "setFrame:display:", (void *)fnd_zero},
    {"NSWindow", "makeKeyAndOrderFront:", (void *)fnd_zero},
    {"NSWindow", "makeFirstResponder:", (void *)fnd_zero},
    {"NSWindow", "convertBaseToScreen:", (void *)fnd_convert_point},
    {"NSWindow", "convertScreenToBase:", (void *)fnd_convert_point},
    {"NSWindow", "center", (void *)fnd_zero},
    {"NSWindow", "display", (void *)fnd_zero},
    {"NSWindow", "miniaturize:", (void *)fnd_zero},
    {"NSWindow", "zoom:", (void *)fnd_zero},
    {"NSWindow", "orderFront:", (void *)fnd_zero},
    {"NSWindow", "contentView", (void *)fnd_NSWindow_contentView},
    {"NSOpenGLPixelFormat", "initWithAttributes:", (void *)fnd_ret_self},
    {"NSOpenGLContext", "initWithFormat:shareContext:", (void *)gmv_gl_context_init},
    {"NSOpenGLContext", "makeCurrentContext", (void *)gmv_gl_make_current},
    {"NSOpenGLContext", "flushBuffer", (void *)gmv_gl_flush_buffer},
    {"NSOpenGLContext", "setView:", (void *)fnd_zero},
    {"NSOpenGLContext", "clearDrawable", (void *)fnd_zero},
    {"NSOpenGLContext", "update", (void *)fnd_zero},
    {"NSOpenGLContext", "setValues:forParameter:", (void *)fnd_zero},
    {"NSOpenGLContext", "CGLContextObj", (void *)fnd_ret_self},
    {"NSView", "initWithFrame:", (void *)fnd_NSView_initWithFrame},
    {"NSView", "window", (void *)fnd_NSView_window},
    {"NSView", "setFrame:", (void *)fnd_zero},
    {"NSView", "setNeedsDisplay:", (void *)fnd_zero},
    {"NSView", "setAcceptsTouchEvents:", (void *)fnd_zero},
    {"NSView", "setPostsFrameChangedNotifications:", (void *)fnd_zero},
    {"NSView", "lockFocus", (void *)fnd_zero},
    {"NSView", "unlockFocus", (void *)fnd_zero},
    {"NSView", "addTrackingRect:owner:userData:assumeInside:", (void *)fnd_yes},
    {"NSView", "removeTrackingRect:", (void *)fnd_zero},
    {"NSView", "removeFromSuperview", (void *)fnd_zero},
    {"NSView", "addSubview:", (void *)fnd_zero},
    {"NSView", "setHidden:", (void *)fnd_zero},
    {"NSView", "setAutoresizingMask:", (void *)fnd_zero},
    {"NSApplication", "setPresentationOptions:", (void *)fnd_zero},
    {"NSApplication", "activateIgnoringOtherApps:", (void *)fnd_zero},
    {"NSApplication", "setDelegate:", (void *)fnd_zero},
    {"NSApplication", "finishLaunching", (void *)fnd_zero},
    {"NSApplication", "isActive", (void *)fnd_yes},
    {"NSApplication", "isHidden", (void *)fnd_zero},
    {"NSApplication", "keyWindow", (void *)fnd_zero},
    {"NSApplication", "terminate:", (void *)fnd_NSApp_terminate},
    {"NSNotificationCenter", "addObserver:selector:name:object:", (void *)fnd_nc_addObserver},
    {"NSNotificationCenter", "removeObserver:name:object:", (void *)fnd_zero},
    {"NSNotificationCenter", "removeObserver:", (void *)fnd_zero},
    {"NSNotification", "object", (void *)fnd_notification_object},
    {"NSNotification", "name", (void *)fnd_notification_object},
    {"NSWorkspace", "notificationCenter", (void *)fnd_NSWorkspace_notifCenter},
    {"NSNotificationCenter", "postNotificationName:object:", (void *)fnd_nc_post},
    {"NSObject", "performSelector:withObject:afterDelay:", (void *)fnd_performAfterDelay},
    {"QTMovie", "initWithFile:error:", (void *)fnd_QTMovie_initWithFile},
    {"QTMovie", "play", (void *)fnd_zero},
    {"QTMovie", "stop", (void *)fnd_zero},
    {"QTMovie", "gotoBeginning", (void *)fnd_zero},
    {"QTMovie", "autoplay", (void *)fnd_zero},
    {"QTMovie", "setAttribute:forKey:", (void *)fnd_zero},
    {"QTMovie", "setMuted:", (void *)fnd_zero},
    {"QTMovieView", "initWithFrame:", (void *)fnd_NSView_initWithFrame},
    {"QTMovieView", "setDelegate:", (void *)fnd_setViewDelegate},
    {"QTMovieView", "delegate", (void *)fnd_viewDelegate},
    {"QTMovieView", "setMovie:", (void *)fnd_zero},
    {"QTMovieView", "setControllerVisible:", (void *)fnd_zero},
    {"QTMovieView", "setPreservesAspectRatio:", (void *)fnd_zero},
    {"QTMovieView", "setEditable:", (void *)fnd_zero},
    {"QTMovieView", "movie", (void *)fnd_zero},
    {"NSUserDefaults", "objectForKey:", (void *)fnd_NSUserDefaults_objectForKey},
    {"NSNumber", "integerValue", (void *)fnd_NSNumber_value},
    {"NSNumber", "intValue", (void *)fnd_NSNumber_value},
    {"NSNumber", "unsignedIntegerValue", (void *)fnd_NSNumber_value},
    {"NSNumber", "boolValue", (void *)fnd_NSNumber_value},
    {"NSUserDefaults", "stringForKey:", (void *)fnd_zero},
    {"NSUserDefaults", "boolForKey:", (void *)fnd_NSUserDefaults_boolForKey},
    {"NSUserDefaults", "integerForKey:", (void *)fnd_NSUserDefaults_integerForKey},
    {"NSUserDefaults", "setObject:forKey:", (void *)fnd_NSUserDefaults_setObject},
    {"NSUserDefaults", "setBool:forKey:", (void *)fnd_NSUserDefaults_setBool},
    {"NSUserDefaults", "setInteger:forKey:", (void *)fnd_NSUserDefaults_setInteger},
    {"NSUserDefaults", "registerDefaults:", (void *)fnd_zero},
    {"NSUserDefaults", "synchronize", (void *)fnd_yes},
    {NULL, NULL, NULL},
};

void *gmv_fnd_class_method(const char *cls, const char *sel)
{
    for (const struct fnd_method *m = g_class_methods; m->cls; m++)
        if (!strcmp(m->cls, cls) && !strcmp(m->sel, sel))
            return m->imp;
    return NULL;
}

void *gmv_fnd_instance_method(const char *cls, const char *sel)
{
    for (const struct fnd_method *m = g_instance_methods; m->cls; m++)
        if (!strcmp(m->cls, cls) && !strcmp(m->sel, sel))
            return m->imp;
    return NULL;
}
