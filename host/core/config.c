/* config.c -- GangstarMV.ini, the host's settings file, in savegame\ next to the exe (older builds kept it
   beside the exe; gmv_config_load moves it into savegame\ the first time).

   Every setting is one of the GMV_* switches the host used to read only from the environment.
   gmv_getenv("GMV_X") answers, in order:
     1. in a dev build, the environment variable GMV_X, if it is set (exactly as getenv would, so
        one-off command-line tests keep working and win over the file);
     2. key X from GangstarMV.ini, if it has a non-empty value;
     3. NULL -- the call site's built-in default.
   Keys are case-insensitive and may be written with or without the GMV_ prefix; [sections] only
   group them.  A missing file is created from the game's table with its defaults (for Gangstar the
   enhancements on, the user's choice of 2026-11-05); a key the file leaves blank or lacks gets the
   call site's built-in value, which for the enhancements is the Mac's.  A line for a key the game
   has taken out (its table's retired list) is skipped without the unknown-setting line.

   ORIGINAL_MAC_EXPERIENCE=1 puts a fixed answer for the keys in mac_preset ahead of all three.

   Loaded once from gmv_main before anything else runs and read-only afterwards, so the game's
   threads can call gmv_getenv freely.  gmv_config_save (the GAMEPATCH options tab) rewrites one
   key's line in the file without changing what gmv_getenv answers; gmv_config_file_value reads
   what the file holds now. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "layout.h"
#include "log.h"
#include "platform.h"
#include "shims.h"

#define MAX_SETTINGS 256
static char *g_vals[MAX_SETTINGS];     /* non-empty values from the file, or NULL */
static char *g_saved[MAX_SETTINGS];    /* values gmv_config_save wrote since then (NULL = blank) */
static unsigned char g_is_saved[MAX_SETTINGS];
static char g_path[GMV_PATH_MAX];   /* the file, for gmv_config_save */

static int g_mac;                   /* ORIGINAL_MAC_EXPERIENCE in effect */

static const gmv_setting *find_setting(const char *key)
{
    if (!_strnicmp(key, "GMV_", 4))
        key += 4;
    for (size_t i = 0; i < gmv_game_settings.n; i++)
        if (!_stricmp(key, gmv_game_settings.list[i].name))
            return &gmv_game_settings.list[i];
    return NULL;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        *--e = 0;
    return s;
}

/* The help texts write paths the Windows way; elsewhere a backslash in them is the system's slash. */
static void help_seps(char *s)
{
#ifdef _WIN32
    (void)s;
#else
    for (; *s; s++)
        if (*s == '\\')
            *s = '/';
#endif
}

static void put_help(FILE *f, const char *text)
{
    fputs("; ", f);
#ifdef _WIN32
    fputs(text, f);
#else
    for (; *text; text++)
        fputc(*text == '\\' ? '/' : *text, f);
#endif
    fputs("\r\n", f);
}

static void write_template(const char *path)
{
    FILE *f = gmv_fopen(path, "wb");
    if (!f) {
        gmv_log("[config] could not create %s (%lu); using built-in defaults\n", path, gmv_last_error());
        return;
    }
    fputs(gmv_game_settings.header, f);
    for (size_t s = 0; s < gmv_game_settings.nsections; s++) {
        fprintf(f, "\r\n[%s]\r\n", gmv_game_settings.section_help[s][0]);
        put_help(f, gmv_game_settings.section_help[s][1]);
        for (size_t i = 0; i < gmv_game_settings.n; i++)
            if (!strcmp(gmv_game_settings.list[i].section, gmv_game_settings.section_help[s][0])) {
                fputs("\r\n", f);
                put_help(f, gmv_game_settings.list[i].help);
                fprintf(f, "%s=%s\r\n", gmv_game_settings.list[i].name, gmv_game_settings.list[i].def);
            }
    }
    fclose(f);
    gmv_log("[config] created %s with the default settings\n", path);
}

void gmv_config_load(const char *exe_dir)
{
    char path[GMV_PATH_MAX], old[GMV_PATH_MAX];
    snprintf(path, sizeof path, "%s" GMV_SEP "savegame", exe_dir);
    gmv_mkdir(path);
    snprintf(path, sizeof path, "%s" GMV_SEP "savegame" GMV_SEP GMV_GAME_NAME ".ini", exe_dir);
    snprintf(old, sizeof old, "%s" GMV_SEP GMV_GAME_NAME ".ini", exe_dir);
    if (gmv_rename(old, path))              /* fails harmlessly when there is no old file or the new one exists */
        gmv_log("[config] moved %s to %s\n", old, path);
    snprintf(g_path, sizeof g_path, "%s", path);
    FILE *f = gmv_fopen(path, "rb");
    if (!f) {
        write_template(path);
        f = gmv_fopen(path, "rb");
        if (!f)
            return;
    }
    char line[1024];
    int lineno = 0, nset = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        if (++lineno == 1 && !memcmp(p, "\xEF\xBB\xBF", 3))
            p += 3;                                 /* UTF-8 BOM from Notepad */
        p[strcspn(p, "\n")] = 0;
        p = trim(p);
        if (!*p || *p == ';' || *p == '#' || *p == '[')
            continue;
        char *eq = strchr(p, '=');
        if (!eq) {
            gmv_log("[config] " GMV_GAME_NAME ".ini line %d: no '=' in \"%s\" (ignored)\n", lineno, p);
            continue;
        }
        *eq = 0;
        char *key = trim(p), *val = trim(eq + 1);
        size_t n = strlen(val);
        if (n >= 2 && val[0] == '"' && val[n - 1] == '"') {     /* "C:\path with spaces\x.wav" */
            val[n - 1] = 0;
            val++;
        }
        const gmv_setting *s = find_setting(key);
        if (!s) {
            const char *bare = _strnicmp(key, "GMV_", 4) ? key : key + 4;
            int retired = 0;        /* a key taken out of the game: an older file still has its line */
            for (size_t r = 0; r < gmv_game_settings.nretired && !retired; r++)
                retired = !_stricmp(bare, gmv_game_settings.retired[r]);
            if (!retired)
                gmv_log("[config] " GMV_GAME_NAME ".ini line %d: unknown setting \"%s\" (ignored)\n", lineno, key);
            continue;
        }
        size_t i = (size_t)(s - gmv_game_settings.list);
        free(g_vals[i]);
        g_vals[i] = *val ? _strdup(val) : NULL;
    }
    fclose(f);
    for (size_t i = 0; i < gmv_game_settings.n; i++) {
#if GMV_DEV
        char env[64];
        snprintf(env, sizeof env, "GMV_%s", gmv_game_settings.list[i].name);
        const char *e = getenv(env);
        if (e)
            gmv_log("[config] %s=%s from the environment (overrides " GMV_GAME_NAME ".ini)\n", env, e);
        else
#endif
        if (g_vals[i] && strcmp(g_vals[i], gmv_game_settings.list[i].def))
            gmv_log("[config] %s=%s\n", gmv_game_settings.list[i].name, g_vals[i]);
        nset += g_vals[i] != NULL;
    }
    gmv_log("[config] read %s (%d settings given; non-default ones listed above)\n", path, nset);

    const char *m = gmv_getenv("GMV_ORIGINAL_MAC_EXPERIENCE");
    if (!(m && *m && *m != '0'))
        return;
    for (size_t p = 0; p < gmv_game_settings.npreset; p++) {
        char env[64];
        snprintf(env, sizeof env, "GMV_%s", gmv_game_settings.preset[p].name);
        const char *v = gmv_getenv(env);
        const char *want = gmv_game_settings.preset[p].val;
        if (v && *v && (!want || strcmp(v, want)))
            gmv_log("[config] %s=%s overridden by ORIGINAL_MAC_EXPERIENCE (now %s)\n", gmv_game_settings.preset[p].name, v,
                    want ? want : "default");
    }
    g_mac = 1;
    gmv_log("%s", gmv_game_settings.preset_log);
}

void gmv_config_set_mac(int on)
{
    g_mac = on != 0;
}

int gmv_config_mac(void)            /* ORIGINAL_MAC_EXPERIENCE in effect now (it can change in game) */
{
    return g_mac;
}

int gmv_config_tab(void)            /* GAMEPATCH_TAB: on unless it says 0, so an older ini without the key gets it too */
{
    const char *t = gmv_getenv("GMV_GAMEPATCH_TAB");
    return !(t && *t == '0');
}

const char *gmv_config_file_value(const char *name)
{
    const gmv_setting *s = find_setting(name);
    if (!s)
        return NULL;
    size_t i = (size_t)(s - gmv_game_settings.list);
    return g_is_saved[i] ? g_saved[i] : g_vals[i];
}

/* Set key name to val in the file: every line that sets it is rewritten as KEY=val (the key as
   written, the comments and the rest kept).  A key the file lacks is added with its comment at the
   end of its [section], or the section is added at the end of the file.  Through a temporary
   file, so a failed write leaves the old one.  1 on success. */
int gmv_config_save(const char *name, const char *val)
{
    const gmv_setting *s = find_setting(name);
    if (!s || !g_path[0])
        return 0;
    const char *sh = "";
    for (size_t j = 0; j < gmv_game_settings.nsections; j++)
        if (!strcmp(gmv_game_settings.section_help[j][0], s->section))
            sh = gmv_game_settings.section_help[j][1];
    size_t i = (size_t)(s - gmv_game_settings.list), n = 0;
    char *old = NULL;
    FILE *f = gmv_fopen(g_path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        old = malloc(len > 0 ? (size_t)len + 1 : 1);
        n = old && len > 0 ? fread(old, 1, (size_t)len, f) : 0;
        fclose(f);
    }
    size_t vl = strlen(val), al = strlen(sh) + strlen(s->help) + strlen(s->section) + strlen(s->name) + vl + 32;
    size_t cap = n + (n + 1) * (vl + 1) + al + 64;
    char *out = malloc(cap), *o = out, *add = malloc(al);
    if (!out || !add) {
        free(old);
        free(out);
        free(add);
        return 0;
    }
    int found = 0, in_sec = 0, have_sec = 0;
    size_t ins = (size_t)-1, sl = strlen(s->section);   /* ins: the header after the key's section */
    for (size_t p = 0; p < n;) {
        size_t e = p;
        while (e < n && old[e] != '\n')
            e++;
        size_t ln = e - p, k = p;
        if (p == 0 && ln >= 3 && !memcmp(old, "\xEF\xBB\xBF", 3))
            k = 3;
        while (k < e && (old[k] == ' ' || old[k] == '\t'))
            k++;
        if (k < e && old[k] == '[') {
            if (in_sec && ins == (size_t)-1)
                ins = (size_t)(o - out);
            in_sec = e - k >= sl + 2 && !_strnicmp(old + k + 1, s->section, sl) && old[k + 1 + sl] == ']';
            have_sec |= in_sec;
        }
        const char *eq = memchr(old + k, '=', e - k);
        char key[64] = "";
        if (eq && old[k] != ';' && old[k] != '#' && old[k] != '[' && (size_t)(eq - (old + k)) < sizeof key) {
            memcpy(key, old + k, (size_t)(eq - (old + k)));
            key[eq - (old + k)] = 0;
        }
        if (find_setting(trim(key)) == s) {
            int cr = ln && old[e - 1] == '\r';
            size_t head = (size_t)(eq - old) + 1 - p;          /* up to and with '=' */
            memcpy(o, old + p, head), o += head;
            memcpy(o, val, vl), o += vl;
            if (cr)
                *o++ = '\r';
            found = 1;
        } else
            memcpy(o, old + p, ln), o += ln;
        if (e < n)
            *o++ = '\n';
        p = e + 1;
    }
    if (!found) {
        int at_end = ins == (size_t)-1, an;
        if (at_end && o > out && o[-1] != '\n')
            *o++ = '\r', *o++ = '\n';
        if (!have_sec)
            an = snprintf(add, al, "\r\n[%s]\r\n; %s\r\n\r\n; %s\r\n", s->section, sh, s->help);
        else if (at_end)
            an = snprintf(add, al, "\r\n; %s\r\n", s->help);
        else        /* before the next header, after the blank line that already precedes it */
            an = snprintf(add, al, "; %s\r\n", s->help);
        help_seps(add);
        an += snprintf(add + an, al - (size_t)an, "%s=%s\r\n%s", s->name, val, have_sec && !at_end ? "\r\n" : "");
        char *at = at_end ? o : out + ins;
        memmove(at + an, at, (size_t)(o - at));
        memcpy(at, add, (size_t)an);
        o += an;
    }
    free(add);
    free(old);
    char tmp[GMV_PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_path);
    f = gmv_fopen(tmp, "wb");
    int ok = f && fwrite(out, 1, (size_t)(o - out), f) == (size_t)(o - out);
    if (f)
        ok = !fclose(f) && ok;
    free(out);
    if (ok && !gmv_replace(tmp, g_path))
        ok = 0;
    if (!ok) {
        unsigned long why = gmv_last_error();
        gmv_remove(tmp);
        gmv_log("[config] could not save %s=%s to " GMV_GAME_NAME ".ini (%lu)\n", s->name, val, why);
        return 0;
    }
    g_saved[i] = *val ? _strdup(val) : NULL;    /* the old one is left: a reader may still hold it */
    g_is_saved[i] = 1;
    gmv_log("[config] saved %s=%s to " GMV_GAME_NAME ".ini\n", s->name, val);
    return 1;
}

const char *gmv_getenv(const char *name)
{
    if (g_mac) {
        const char *key = _strnicmp(name, "GMV_", 4) ? name : name + 4;
        for (size_t p = 0; p < gmv_game_settings.npreset; p++)
            if (!_stricmp(key, gmv_game_settings.preset[p].name))
                return gmv_game_settings.preset[p].val;
    }
    const char *e = gmv_envkey(name);
    if (e)
        return e;
    const gmv_setting *s = find_setting(name);
    return s ? g_vals[s - gmv_game_settings.list] : NULL;
}

/* A key as it answers now: gmv_getenv, but what the options menu saved counts at once. */
const char *gmv_config_live(const char *name)
{
    if (g_mac)
        return gmv_getenv(name);
    const char *e = gmv_envkey(name);
    return e ? e : gmv_config_file_value(name);
}
