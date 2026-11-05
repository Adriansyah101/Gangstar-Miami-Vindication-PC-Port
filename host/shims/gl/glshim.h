/* glshim.h -- the GL shim's parts share this, and a game's render code may use it. */
#ifndef GMV_GLSHIM_H
#define GMV_GLSHIM_H

#include "compat.h"
#include <stdint.h>
#include <GL/gl.h>

#include "game.h"

/* gl_window.c */
#if !defined(GL_BGR_EXT)
#define GL_BGR_EXT 0x80E0
#endif
#if GMV_DEV
extern int g_gl_trace;
#endif
#if !(GMV_DEV)
#define g_gl_trace 0
#endif
extern int g_no_flicker;
extern volatile int g_drew_since_swap;
extern struct SDL_Window *g_window;
extern void *g_glctx;
extern volatile LONG g_pace_gen;
extern int g_swap_default;
double pace_frame_ms(void);
int swap_interval(int v);
int swap_want(void);

/* gl_msaa.c */
extern int g_msaa;
extern int g_tab_live;
extern GLuint g_ms_fbo, g_ms_rb[2];
extern int g_ms_w, g_ms_h;
extern GLuint g_ms_glow;
extern int g_glow_bufs;
extern int g_glow_used;
typedef struct {
    void(APIENTRY *GenFramebuffers)(GLsizei, GLuint *);
    void(APIENTRY *BindFramebuffer)(GLenum, GLuint);
    void(APIENTRY *GenRenderbuffers)(GLsizei, GLuint *);
    void(APIENTRY *BindRenderbuffer)(GLenum, GLuint);
    void(APIENTRY *RenderbufferStorageMultisample)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
    void(APIENTRY *FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
    GLenum(APIENTRY *CheckFramebufferStatus)(GLenum);
    void(APIENTRY *BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
} msf_t;
extern msf_t msf;
#define MS_FRAMEBUFFER      0x8D40
#define MS_READ_FB          0x8CA8
#define MS_DRAW_FB          0x8CA9
#define MS_DRAW_FB_BINDING  0x8CA6                      /* = GL_FRAMEBUFFER_BINDING */
#define MS_READ_FB_BINDING  0x8CAA
#define MS_RENDERBUFFER     0x8D41
void *ms_proc(const char *core, const char *ext);
extern int g_fb_scale_off;
void msaa_init(void);
void msaa_resolve(GLint x, GLint y, GLint w, GLint h);
void msaa_present(void);
void msaa_sync_samples(void);
void msaa_sync_size(void);
void diag_read_begin(GLenum buf);
void diag_read_end(void);

/* gl_context.c */
void ensure_gl(void);
void gmv_gl_client_size(int *w, int *h);
void gmv_gl_render_size(int *w, int *h);
void gmv_gl_view_rect(int *x, int *y, int *w, int *h, int *gw, int *gh);
void gl_swap_window(void);
void gl_window_fit(int fs, int w, int h);

/* gl_diag.c */
#if GMV_DEV
void dump_framebuffer(const char *path);
/* Diagnostic for the death fade (GMV_FADE_TRACE; armed by the patches on the tick the player's
   health reaches 0): for 12 s log every tick's draws, the order of clears / framebuffer binds /
   fade calls / draws, and the present decision, and save the pre-swap back buffer of presented
   ticks, subsampled to at most 640 px wide: every 4th presented frame, and every one while a fade
   is being drawn.  File names carry the frame number, the GetTickCount stamp the [fade] / [fadecap]
   lines use, and the ms since health reached 0.  The GL state at the first draw after each fade
   call is logged too.  Shows whether the black reaches the back buffer, what wipes or hides it, and
   whether it is presented. */
#define CAP_MS 12000u
extern int g_cap_ticks;
extern unsigned g_cap_session, g_cap_saved, g_cap_presented;
extern DWORD g_cap_t0, g_cap_last_fade;
extern unsigned g_cap_clears, g_cap_draws;
extern char g_cap_seq[200];
extern size_t g_cap_seqlen;
extern unsigned g_cap_run;
extern uint32_t g_fade_pending;
extern int g_post_fade;
extern uint32_t g_quad_probe;
void post_fade_probe(const char *what, unsigned a, unsigned b);
void cap_seq(const char *s);
void fade_state_log(const char *what, GLenum mode, GLsizei count);
extern int g_corner_watch;
void fade_corners(const char *when);
void write_bmp24(const char *path, int w, int h, int row, const unsigned char *px);
void fade_capture_dump(unsigned frame, DWORD t);
#endif
#if !(GMV_DEV)
extern unsigned g_cap_clears, g_cap_draws, g_cap_run;
#define g_cap_ticks (-1)
#define g_fade_pending 0u
#define g_quad_probe 0u
#define cap_seq(s) ((void)0)
#define post_fade_probe(what, a, b) ((void)0)
#define fade_state_log(what, mode, count) ((void)0)
#endif

/* gl_present.c */
#if GMV_DEV
enum { PM_START, PM_POST, PM_SSR, PM_VR, PM_AO, PM_BLOOM, PM_PRES, PM_END, PM_N };
#define PM_RING 4
typedef struct {
    int on;                                              
    int started, slot;
    GLuint q[PM_RING][PM_N];
    unsigned set[PM_RING], sdraws[PM_RING], draws;
    long long cpu[PM_N];
    void(APIENTRY *GenQueries)(GLsizei, GLuint *);
    void(APIENTRY *QueryCounter)(GLuint, GLenum);
    void(APIENTRY *GetQueryObjectiv)(GLuint, GLenum, GLint *);
    void(APIENTRY *GetQueryObjectui64v)(GLuint, GLenum, unsigned long long *);
    double seg[PM_N], total, max_total, cpu_post, cpu_pres, freq;
    unsigned frames, late, cpu_frames, posts, draws_sum, draws_max;
    long long t_log;
} pm_t;
extern pm_t pm;
void pm_mark(int m);
void pm_begin(void);
#define PM_DRAW() do { if (pm.on) { if (!pm.started) pm_begin(); pm.draws++; } } while (0)
#define PM_CLEAR() do { if (pm.on && !pm.started) pm_begin(); } while (0)
#endif
#if !(GMV_DEV)
#define PM_DRAW() ((void)0)
#define PM_CLEAR() ((void)0)
#define pm_mark(m) ((void)0)
#endif

/* gl_trace.c */
struct gltrace {
    int clears, da_calls, de_calls;
    long da_verts, de_verts;
    float cc[4];                                 
    int vp[4];                                   
    unsigned last_fb;                            
    long max_buf;                                
    int tex_binds;
};
extern struct gltrace g_glt;
void tr_ClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void tr_Clear(GLbitfield m);
void tr_Viewport(GLint x, GLint y, GLsizei w, GLsizei h);

/* gl_draw.c */
extern int g_texfix;
extern unsigned g_tf_active, g_tf_client;
extern uint8_t g_tf_tex2d, g_tf_tcarr;
void(APIENTRY *tf_active_real(void))(GLenum);
typedef struct {
    GLint mode, crgb, calpha, src[6], op[6];             
    GLfloat scale[2];                                    
} g_te_t;
extern g_te_t g_te[4];
unsigned texfix_begin(GLenum mode);
void texfix_end(unsigned m);
void *gl_trace_intercept(const char *name);
#if GMV_DEV
void pf_CompressedTexImage2D(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h,
                                    GLint border, GLsizei size, const void *data);
void *gl_prof_intercept(const char *name);
#endif

/* gl_msaa_wraps.c */
void ms_GetIntegerv(GLenum pname, GLint *v);
void ms_ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *px);
void ms_CopyTexSubImage2D(GLenum target, GLint level, GLint xo, GLint yo, GLint x, GLint y,
                                 GLsizei w, GLsizei h);
void ms_DrawBuffers(GLsizei n, const GLenum *bufs);

/* gl_trace.c */
void gl_trace_frame_end(unsigned frame);

#endif
