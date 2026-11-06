/* gl_bind.c -- CGL, and binding the game's GL imports to opengl32 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"
#include <SDL3/SDL_video.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "glshim.h"

/* ---- CGL (low-level OpenGL framework C ABI; cdecl, we no-op) -------------------------- */

#define FN(mach_name, host_fn) {mach_name, (void *)(host_fn)}

static int cgl_set_parameter(void *ctx, int param, const void *val)
{
    (void)ctx; (void)param; (void)val;
    return 0;                                   /* kCGLNoError */
}
static int cgl_get_parameter(void *ctx, int param, int *val)
{
    (void)ctx; (void)param;
    if (val)
        *val = 0;
    return 0;
}
static int cgl_enable(void *ctx, int feature)  { (void)ctx; (void)feature; return 0; }
static int cgl_disable(void *ctx, int feature) { (void)ctx; (void)feature; return 0; }
/* -[EAGLView reshape] brackets its GL calls with CGLLockContext/CGLUnlockContext (thread safety
   for the single GL context).  We render single-threaded on the context's thread, so these are
   no-ops returning kCGLNoError. */
static int cgl_lock(void *ctx)   { (void)ctx; return 0; }
static int cgl_unlock(void *ctx) { (void)ctx; return 0; }

/* ---- binding the game's GL imports to opengl32 (cdecl -> stdcall) --------------------- */

extern void gmv_gl_forward(void);               /* shimsobjc.S */
extern void gmv_gl_lazy(void);                  /* shimsobjc.S */

/* One per GL import: the resolved stdcall fn (lazily filled) and the GL name (no underscore). */
struct glcell {
    void *fn;
    const char *name;
    int tried;
};

static uint8_t *g_gl_thunks;
static size_t g_gl_off;

/* The one place the host looks up a GL function, by its GL name ("glClear"), on both systems: SDL asks
   the driver (wglGetProcAddress, glXGetProcAddress or eglGetProcAddress) and then the GL library
   itself.  Needs the GL context current.  Some Windows drivers answer 1, 2, 3 or -1 for a name they
   do not hand out, the GL 1.1 functions among them, and SDL passes that on as a function: those count
   as no answer, and Windows then takes the function from opengl32.dll, where GL 1.1 lives. */
void *gl_real(const char *name)
{
    void *p = (void *)(uintptr_t)SDL_GL_GetProcAddress(name);
    uintptr_t v = (uintptr_t)p;
    if (v <= 3 || v == (uintptr_t)-1)
        p = NULL;
#ifdef _WIN32
    if (!p) {
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        if (gl)
            p = (void *)(uintptr_t)GetProcAddress(gl, name);
    }
#endif
    return p;
}

/* Called by gmv_gl_lazy on first use: resolve the GL function now that a context exists. */
void *gmv_gl_lookup(struct glcell *c)
{
    if (!c->fn && !c->tried) {
        c->tried = 1;
        c->fn = gl_real(c->name);
        if (!c->fn)
            gmv_trace("[gl] unresolved %s -> no-op\n", c->name);
    }
    return c->fn;
}

/* A lazily-bound GL thunk: `mov eax, <cell> ; jmp gmv_gl_lazy`. */
void *gmv_gl_resolve(const char *name)
{
    if (!name || strncmp(name, "_gl", 3) != 0)
        return NULL;
    static int trace_checked, msaa_wrap;
    if (!trace_checked) {
        trace_checked = 1;
#if GMV_DEV
        const char *e = gmv_dbgenv("GMV_GL_TRACE");
        g_gl_trace = e && *e && *e != '0';
#endif
        const char *nf = gmv_getenv("GMV_NO_FLICKER");   /* default ON; set GMV_NO_FLICKER=0 to disable */
        g_no_flicker = nf ? (*nf && *nf != '0') : 1;
        const char *tf = gmv_getenv("GMV_TEXFIX");       /* default ON; GMV_TEXFIX=0 to compare */
        g_texfix = tf ? (*tf && *tf != '0') : 1;
        g_tab_live = gmv_config_tab();                  /* also under ORIGINAL_MAC_EXPERIENCE, which it switches */
        msaa_wrap = 1;                                  /* the screen FBO is made at any MSAA (msaa_init) */
        gmv_game_gl_setup();
    }
    {
        void *t = gmv_game_gl_intercept(name);
        if (t)
            return t;
    }
    if (msaa_wrap) {
        if (!strcmp(name, "_glGetIntegerv"))
            return (void *)ms_GetIntegerv;
        if (!strcmp(name, "_glReadPixels"))
            return (void *)ms_ReadPixels;
        if (!strcmp(name, "_glCopyTexSubImage2D"))
            return (void *)ms_CopyTexSubImage2D;
        if (!strcmp(name, "_glDrawBuffers"))
            return (void *)ms_DrawBuffers;
    }
    /* glViewport + glBindFramebuffer are wrapped UNCONDITIONALLY: the window-follow (resolution)
       logic in tr_Viewport must see every main-framebuffer viewport, and it relies on last_fb (set
       by tr_BindFramebuffer) to tell the screen from an FBO -- independent of trace/anti-flicker. */
    if (!strcmp(name, "_glViewport") || !strcmp(name, "_glBindFramebuffer") ||
        !strcmp(name, "_glBindFramebufferEXT"))
        return gl_trace_intercept(name);
#if GMV_DEV
    if (gmv_prof_on()) {
        void *t = gl_prof_intercept(name);
        if (t)
            return t;
    }
#endif
    /* The tr_* wrappers both trace AND track g_drew_since_swap (for the anti-flicker present-skip)
       and the texture-unit state texfix needs, so install them when GMV_GL_TRACE, GMV_NO_FLICKER
       or GMV_TEXFIX is set, for AMBIENT_OCCLUSION and BLOOM (draws, depth clears, depth test and fog) and
       for the whole-world fog (and the hybrid fog's texture environments), and for the options menu to
       switch those on. */
    if (g_gl_trace || g_no_flicker || g_texfix || gmv_game_gl_wants_draws()) {
        void *t = gl_trace_intercept(name);
        if (t)
            return t;
    }
    if (!g_gl_thunks) {
        g_gl_thunks = VirtualAlloc(NULL, 0x20000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!g_gl_thunks)
            return NULL;
    }
    if (g_gl_off + 16 > 0x20000)
        return NULL;
    struct glcell *cell = calloc(1, sizeof *cell);
    if (!cell)
        return NULL;
    cell->name = _strdup(name + 1);             /* GL name without the leading underscore */

    uint8_t *p = g_gl_thunks + g_gl_off;
    g_gl_off += 16;
    p[0] = 0xB8;                                 /* mov eax, imm32 (the cell) */
    memcpy(p + 1, &cell, 4);
    p[5] = 0xE9;                                 /* jmp rel32 -> gmv_gl_lazy */
    uint32_t rel = (uint32_t)((uintptr_t)gmv_gl_lazy - (uintptr_t)(p + 10));
    memcpy(p + 6, &rel, 4);
    return p;
}

const host_symbol gmv_host_symbols_gl[] = {
    FN("_CGLSetParameter", cgl_set_parameter),
    FN("_CGLGetParameter", cgl_get_parameter),
    FN("_CGLEnable", cgl_enable),
    FN("_CGLDisable", cgl_disable),
    FN("_CGLLockContext", cgl_lock),
    FN("_CGLUnlockContext", cgl_unlock),

    {NULL, NULL},
};
