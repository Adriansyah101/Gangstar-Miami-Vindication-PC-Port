/* render.h -- what Gangstar's render effects share with each other. */
#ifndef GANGSTAR_RENDER_H
#define GANGSTAR_RENDER_H

#include "glshim.h"

#if !GMV_DEV
#define g_fog_survey 0
#define g_bloom_survey 0
#define g_veh_survey 0
#define g_vr_debug 0
#endif
typedef struct {                                        /* SUN_MOON glow occlusion */
    int mode;                                           /* 1 = shader, 2 = depth test */
    GLboolean test;
    GLint func, prog, act, tex, tex2;
    GLfloat range[2];
} glow_state_t;
extern int g_ssr_want, g_ssr;
extern int g_vr_want, g_vr;
extern int g_ds_want, g_ds;
#define FOG_CLIP_PLANE 0x3005                           /* GL_CLIP_PLANE5 */
#if GMV_DEV
extern int g_fog_survey;
extern int g_bloom_survey;
extern int g_veh_survey;
#endif

/* present_hold.c */
uint32_t gmv_top_state_vtable(void);
int menu_flash_hold(unsigned frame, uint32_t vt, int drew);
#if GMV_DEV
void flicker_cap(unsigned frame, int presented, uint32_t vt);
#endif

/* aniso.c */
extern float g_aniso;
void aniso_sync(void);
void af_TexParameterf(GLenum target, GLenum pname, GLfloat v);
void af_TexParameteri(GLenum target, GLenum pname, GLint v);
void af_DeleteTextures(GLsizei n, const GLuint *names);
void pi_CompressedTexImage2D(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h,
                                    GLint border, GLsizei size, const void *data);
void pi_DeleteTextures(GLsizei n, const GLuint *names);

/* ao.c */
extern int g_ao_want;
extern int g_ao;
extern float g_ao_strength, g_ao_radius;
extern GLenum g_ao_mode;
extern int g_ao_proj_dirty, g_ao_persp;
extern GLfloat g_ao_proj[16];
extern GLfloat g_ao_dr[2];
extern int g_ao_depth_on, g_ao_fog_on;
extern int g_blend_on;
extern GLenum g_blend_src, g_blend_dst;
extern int g_ao_dmask;
extern GLfloat g_ao_fogp[4];
#define AO_LAYERS 6
typedef struct {
    GLfloat m[16], dr[2];
    GLint vp[4];
    GLfloat fog[4];                                      
    unsigned n;
} g_ao_layer_t;
extern g_ao_layer_t g_ao_layer[AO_LAYERS];
extern int g_ao_nlayers, g_ao_cur;
extern unsigned g_ao_3d;
extern int g_ao_done;
extern unsigned g_ao_why[3];
#if GMV_DEV
extern unsigned g_ao_runs;
#endif
typedef struct {
    GLuint(APIENTRY *CreateShader)(GLenum);
    void(APIENTRY *ShaderSource)(GLuint, GLsizei, const char *const *, const GLint *);
    void(APIENTRY *CompileShader)(GLuint);
    void(APIENTRY *GetShaderiv)(GLuint, GLenum, GLint *);
    void(APIENTRY *GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, char *);
    GLuint(APIENTRY *CreateProgram)(void);
    void(APIENTRY *AttachShader)(GLuint, GLuint);
    void(APIENTRY *LinkProgram)(GLuint);
    void(APIENTRY *GetProgramiv)(GLuint, GLenum, GLint *);
    void(APIENTRY *GetProgramInfoLog)(GLuint, GLsizei, GLsizei *, char *);
    void(APIENTRY *UseProgram)(GLuint);
    GLint(APIENTRY *GetUniformLocation)(GLuint, const char *);
    void(APIENTRY *Uniform1i)(GLint, GLint);
    void(APIENTRY *Uniform1f)(GLint, GLfloat);
    void(APIENTRY *Uniform2f)(GLint, GLfloat, GLfloat);
    void(APIENTRY *Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
    void(APIENTRY *UniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat *);
    void(APIENTRY *UniformMatrix3fv)(GLint, GLsizei, GLboolean, const GLfloat *);
    void(APIENTRY *Uniform1fv)(GLint, GLsizei, const GLfloat *);
    void(APIENTRY *Uniform3fv)(GLint, GLsizei, const GLfloat *);
    void(APIENTRY *FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    void(APIENTRY *ActiveTexture)(GLenum);
    void(APIENTRY *BlendEquation)(GLenum);
} aof_t;
extern aof_t aof;
typedef struct {
    GLuint p_ao, p_blur, p_comp;                         
    GLuint fbo_depth, fbo[2], t_depth, t[2];
    int w, h, hw, hh;
} ao_t;
extern ao_t ao;
extern const char ao_vs[];
GLuint ao_program(const char *what, const char *vs, const char *fs);
void ao_tex(GLuint t, GLint ifmt, int w, int h, GLenum fmt, GLenum type);
void ao_alloc(int w, int h);
int ao_procs(const char *off);
int ao_init(void);
int ao_attach(void);
int ao_invert(const GLfloat *m, GLfloat *out);
#if GMV_DEV
double ao_eye_depth(const GLfloat *inv, double z);
#endif
void ao_tri(void);

/* bloom.c */
extern int g_bloom_want;
extern float g_bloom_intensity, g_bloom_threshold;
extern float g_bloom_lights, g_bloom_windows;
extern int g_bg_want;
float bloom_part(int level, const char *env);
extern int g_tod_ms;
#define BL_LEVELS 6
typedef struct {
    GLuint p_pre, p_down, p_up;
    GLuint fbo_scene, t_scene, fbo_depth, t_depth, fbo[BL_LEVELS], t[BL_LEVELS];
    GLuint fbo_glow, t_glow;                             
    int lw[BL_LEVELS], lh[BL_LEVELS];
    int w, h, n;                                         
} bl_t;
extern bl_t bl;
void bl_linear(void);
void ao_proj_refresh(void);
void ao_draw(void);
void ao_clear(GLbitfield m);
extern unsigned g_frame_no;
void ao_frame_end(void);
void ao_cap(GLenum cap, int on);

/* fog.c */
extern int g_fog_all;
extern int g_sky_draw;
#define SKY_KEYS         12
extern GLfloat g_fog_col[4];
extern GLfloat g_fog_game[4];
extern int g_fog_game_set;
extern int gmv_fog_mode(void);
extern int gmv_fog_mode_set(int mode);
extern float g_fog_clip, g_cam_z;
extern int g_fog_mode;
extern float g_fog_height;
typedef struct {
    int n;                                               
    GLfloat z[SKY_KEYS], c[SKY_KEYS][3];                 
    GLfloat wall;                                        
    GLfloat inv[9], cam[3];                              
    GLfloat hz[4];                                       
} sky_t;
extern sky_t sky;
const char *cl_ptr(GLenum binding, const void *p, GLint *buf);
void cl_array_buffer(GLuint b);
extern int g_sky_floor_on;
void sky_floor_end(void);
int sky_read(GLsizei count, GLenum type, const void *idx);
int fog_force_begin(void);
void fog_force_end(int kind);
int fog_clip_begin(void);

/* fog_hybrid.c */
#define FOG_HYBRID_K 3.0f
extern const char fh_fs_head[];
extern const unsigned fh_fs_head_size;
typedef struct {
    int ready;                                           
    GLuint p[3];
    GLint u_invp[3], u_vp[3], u_dr[3], u_fogp[3], u_hgt[3];
    GLfloat set[3][30];                                  
    GLfloat proj[16], invp[16];
    int inv_ok;
    unsigned frame, shaded, linear, logs;                
    DWORD next_log;
} fh_t;
extern fh_t fh;
float fh_amount(float d, float h0, float h1);
int fh_params(GLfloat u[30]);
int fog_hybrid_begin(GLenum mode, int ff, unsigned tf);
void fog_hybrid_end(int on);

/* bloom_glow.c */
typedef struct {
    int ready;                                           
    unsigned frame;                                      
    GLuint p;                                            
    GLint u_gain, u_fogon, u_survey, u_invp, u_vp, u_dr, u_fogp, u_hgt;
    unsigned lights, windows;                            
    int light;                                           
    DWORD next_log;
    void(APIENTRY *BlendFunci)(GLuint, GLenum, GLenum);
    void(APIENTRY *Enablei)(GLenum, GLuint);
    void(APIENTRY *ColorMaski)(GLuint, GLboolean, GLboolean, GLboolean, GLboolean);
    void(APIENTRY *DrawBuffers)(GLsizei, const GLenum *);
    void(APIENTRY *ClearBufferfv)(GLenum, GLint, const GLfloat *);
} bg_t;
extern bg_t bg;
float bg_gain(void);
int bg_draw_begin(GLenum mode);
void bg_draw_end(int kind, int elems, GLenum mode, GLint first, GLsizei count, GLenum type, const void *idx);
void bg_clear(GLbitfield m);
void bg_single(void);
void live_sync(void);

/* survey_fog.c */
void fog_survey(GLenum mode, GLsizei count, int ff, void *caller);
void sea_survey(GLsizei count, GLenum type, const void *idx, GLint first, int ff, void *caller);

/* survey_bloom.c */
void bs_tint(int cls, const GLfloat *rgb, int elems, GLenum mode, GLint first, GLsizei count, GLenum type,
                    const void *idx);
void bloom_survey(int elems, GLenum mode, GLint first, GLsizei count, GLenum type, const void *idx, int shaded,
                         void *caller);
#define LS_CHAIN 8
int game_calls(const uint32_t *sp, uint32_t *out, int max);
void light_survey(int elems, GLenum mode, GLint first, GLsizei count, GLenum type, const void *idx);

/* survey_vehicle.c */
void vehicle_survey(int elems, GLenum mode, GLint first, GLsizei count, GLenum type, const void *idx,
                           void *caller);
void sea_beyond(GLsizei count, GLenum type, const void *idx);

/* water_reflections.c */
extern float g_ssr_strength;
int ssr_frame(void);
int sea_mark_begin(GLsizei count, GLenum type, const void *idx);
void sea_mark_end(int on);
typedef struct {
    GLuint p;
    GLuint fbo_scene, t_scene, fbo_depth, t_depth;       
    int w, h, bad;
} ssr_t;
extern ssr_t ssr;
int ssr_alloc(int w, int h);
int ssr_grab(int w, int h);
void ssr_fog(const GLfloat *fogs, GLfloat *fogp);
void ssr_pass(int w, int h, int li);

/* vehicle_reflections.c */
extern float g_vr_strength;
#if GMV_DEV
extern int g_vr_debug;
#endif
int vr_frame(void);
int shine_body(GLenum mode, GLsizei count);
void car_mark(GLenum mode, GLsizei count, GLenum type, const void *idx);
void vr_pass(int w, int h, int li);

/* dynamic_shadows.c */
extern int g_tod_ms;
int ds_frame(void);
int ds_kind(GLenum mode, GLsizei count, int body);
#if GMV_DEV
void ds_eye_frame_end(void);
#endif
void ds_mark(int kind, GLenum mode, GLsizei count, GLenum type, const void *idx);
int ds_mask_begin(int body);
void ds_mask_end(int on);
void ds_pass(int w, int h, int li);

/* sun_moon_glow.c */
extern int g_sky_glow_draw;
void glow_begin(glow_state_t *s);
void glow_end(const glow_state_t *s);
void *gl_ao_intercept(const char *name);

#endif
