/* ao.c -- AMBIENT_OCCLUSION */
#include <math.h>
#include <string.h>
#include "compat.h"
#include <GL/gl.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "render.h"

/* ---- AMBIENT_OCCLUSION: an enhancement, off by default and under ORIGINAL_MAC_EXPERIENCE ----------
   Screen-space ambient occlusion from the game's depth buffer, applied between its 3D world and its 2D
   layer so the HUD, map and text stay untouched.  The engine (glitch) is fixed-function: it loads its
   projections with glLoadMatrixf (the one glOrtho, at 0x19a6fd, is the EAGLView's), so the switch is
   read from the projection itself -- a perspective matrix has w depending on z (m[11] = +-1, m[15] =
   0), the 2D layer's has not.  On the screen (not in the game's own FBOs), once enough depth-tested
   perspective draws have gone by, the AO is applied right before the first draw under a non-perspective
   projection; or before a depth clear, or before the present, if the 2D layer never came.  At most once
   per presented frame, so the frames the game keeps on screen between draws (fades) are not darkened
   twice.  The pass: copy the depth to a texture, compute AO at half resolution from view-space positions
   (the inverse of the scene's projection, so the engine's handedness does not matter), blur it twice
   (keeping depth edges), and multiply it into the screen with depth-aware upsampling.  It fades out with
   the game's fog and where its radius shrinks below a pixel.  AO_STRENGTH scales the darkening, AO_RADIUS
   is the reach in world units.  All GL state it touches is pushed and restored. */
int g_ao_want;                                   /* AMBIENT_OCCLUSION=1 */
int g_ao = 1;                                    /* 0 once the setup failed */
float g_ao_strength = 1.2f, g_ao_radius = 60;    /* world units are about 1 cm (near plane 50, fog from 45000) */
GLenum g_ao_mode = GL_MODELVIEW;                 /* glMatrixMode */
int g_ao_proj_dirty = 1, g_ao_persp;
GLfloat g_ao_proj[16];                           /* current projection */
GLfloat g_ao_dr[2] = {0, 1};                     /* glDepthRange */
int g_ao_depth_on, g_ao_fog_on;                  /* GL_DEPTH_TEST, GL_FOG as the game set them */
int g_blend_on;                                  /* GL_BLEND and glBlendFunc as the game set them */
GLenum g_blend_src = GL_ONE, g_blend_dst = GL_ZERO;
int g_ao_dmask = 1;                              /* glDepthMask */
GLfloat g_ao_fogp[4] = {GL_EXP, 0, 1, 1};        /* mode, start, end, density */
/* The engine draws parts of a frame under different projections and depth ranges (setDepthRange), so
   one depth buffer can hold several layers.  Each depth-writing perspective draw is counted under its
   projection + depth range; the AO uses the one most draws had and leaves the other layers alone. */
g_ao_layer_t g_ao_layer[AO_LAYERS];
int g_ao_nlayers, g_ao_cur = -1;                 /* layers this frame; the current draw's (-1 = look up) */
unsigned g_ao_3d;                                /* depth-tested perspective draws since the frame began */
int g_ao_done;                                   /* applied this frame */
unsigned g_ao_why[3];                            /* applied at: the 2D switch, a depth clear, the present */
#if GMV_DEV
unsigned g_ao_runs;
#endif

aof_t aof;

ao_t ao;

const char ao_vs[] =
    "#version 120\n"
    "void main() { gl_Position = vec4(gl_Vertex.xy, 0.0, 1.0); }\n";

/* Half resolution: out = (AO, eye depth).  Positions come from the full-resolution depth. */
static const char ao_fs_ao[] =
    "#version 120\n"
    "uniform sampler2D dep;\n"
    "uniform vec2 full;\n"
    "uniform vec4 vp;\n"
    "uniform mat4 invp;\n"
    "uniform float pxs, rad, str;\n"
    "uniform vec4 fog;\n"
    "uniform vec2 dr;\n"
    "float dat(vec2 px) { return texture2D(dep, px / full).r; }\n"
    "bool out_of_layer(float d) { return d < dr.x || d >= dr.y - 1e-5; }\n"
    "vec3 vpos(vec2 px, float d) {\n"
    "    vec4 p = invp * vec4((px - vp.xy) / vp.zw * 2.0 - 1.0, (d - dr.x) / (dr.y - dr.x) * 2.0 - 1.0, 1.0);\n"
    "    return p.xyz / p.w;\n"
    "}\n"
    "vec3 pos(vec2 px) { return vpos(px, dat(px)); }\n"
    "void main() {\n"
    "    vec2 px = min(floor(gl_FragCoord.xy) * 2.0 + 0.5, full - 0.5);\n"
    "    float d = dat(px);\n"
    "    if (out_of_layer(d)) { gl_FragColor = vec4(1.0, 1e9, 0.0, 1.0); return; }\n"
    "    vec3 c = vpos(px, d);\n"
    "    float D = abs(c.z);\n"
    "    vec3 r = pos(px + vec2(1.0, 0.0)), l = pos(px - vec2(1.0, 0.0));\n"
    "    vec3 u = pos(px + vec2(0.0, 1.0)), b = pos(px - vec2(0.0, 1.0));\n"
    "    vec3 dx = abs(r.z - c.z) < abs(c.z - l.z) ? r - c : c - l;\n"
    "    vec3 dy = abs(u.z - c.z) < abs(c.z - b.z) ? u - c : c - b;\n"
    "    vec3 n = normalize(cross(dx, dy));\n"
    "    if (dot(n, c) > 0.0) n = -n;\n"
    "    float rp = rad * pxs / D;\n"
    "    float vis = smoothstep(1.0, 3.0, rp);\n"
    "    if (fog.x == 1.0) vis *= clamp((fog.z - D) / max(fog.z - fog.y, 1e-4), 0.0, 1.0);\n"
    "    else if (fog.x == 2.0) vis *= exp(-fog.w * D);\n"
    "    else if (fog.x == 3.0) vis *= exp(-(fog.w * D) * (fog.w * D));\n"
    "    if (vis <= 0.0) { gl_FragColor = vec4(1.0, D, 0.0, 1.0); return; }\n"
    "    rp = min(rp, 0.15 * full.y);\n"
    "    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));\n"
    "    float r2 = rad * rad, occ = 0.0;\n"
    "    for (int i = 0; i < 12; i++) {\n"
    "        float t = (float(i) + 0.5) / 12.0;\n"
    "        float a = float(i) * 2.3999632 + noise * 6.2831853;\n"
    "        vec2 q = clamp(px + vec2(cos(a), sin(a)) * (rp * t), vec2(0.5), full - 0.5);\n"
    "        float dq = dat(q);\n"
    "        if (out_of_layer(dq)) continue;\n"
    "        vec3 v = vpos(q, dq) - c;\n"
    "        float vv = dot(v, v);\n"
    "        occ += max(dot(v, n) * inversesqrt(vv + 1e-6 * r2) - 0.1, 0.0) * max(1.0 - vv / r2, 0.0);\n"
    "    }\n"
    "    float A = clamp(1.0 - 4.0 * str * occ / 12.0, 0.0, 1.0);\n"
    "    gl_FragColor = vec4(mix(1.0, A, vis), D, 0.0, 1.0);\n"
    "}\n";

/* Half resolution, one direction: a Gaussian that does not mix across depth edges. */
static const char ao_fs_blur[] =
    "#version 120\n"
    "uniform sampler2D src;\n"
    "uniform vec2 hres, dir;\n"
    "void main() {\n"
    "    vec2 uv = gl_FragCoord.xy / hres;\n"
    "    vec2 c = texture2D(src, uv).rg;\n"
    "    float sum = c.r, ws = 1.0;\n"
    "    for (int i = 1; i <= 4; i++) {\n"
    "        float g = exp(-float(i * i) / 10.0);\n"
    "        for (int s = -1; s <= 1; s += 2) {\n"
    "            vec2 t = texture2D(src, uv + dir * float(i * s) / hres).rg;\n"
    "            float w = g * max(0.0, 1.0 - abs(t.g - c.g) / (0.03 * c.g + 1e-4));\n"
    "            sum += t.r * w;\n"
    "            ws += w;\n"
    "        }\n"
    "    }\n"
    "    gl_FragColor = vec4(sum / ws, c.g, 0.0, 1.0);\n"
    "}\n";

/* Full resolution, multiplied into the screen: the four nearest AO texels, weighted by depth. */
static const char ao_fs_comp[] =
    "#version 120\n"
    "uniform sampler2D aot, dep;\n"
    "uniform vec2 full, hres;\n"
    "uniform vec4 vp;\n"
    "uniform mat4 invp;\n"
    "uniform vec2 dr;\n"
    "void main() {\n"
    "    vec2 px = gl_FragCoord.xy;\n"
    "    float d = texture2D(dep, px / full).r;\n"
    "    if (d < dr.x || d >= dr.y - 1e-5) { gl_FragColor = vec4(1.0); return; }\n"
    "    vec4 p = invp * vec4((px - vp.xy) / vp.zw * 2.0 - 1.0, (d - dr.x) / (dr.y - dr.x) * 2.0 - 1.0, 1.0);\n"
    "    float D = abs(p.z / p.w);\n"
    "    vec2 hp = (px - 0.5) * 0.5, b = floor(hp), f = hp - b;\n"
    "    float sum = 0.0, ws = 0.0;\n"
    "    for (int j = 0; j < 2; j++)\n"
    "        for (int i = 0; i < 2; i++) {\n"
    "            vec2 s = texture2D(aot, (b + vec2(float(i), float(j)) + 0.5) / hres).rg;\n"
    "            float w = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y);\n"
    "            w *= max(1e-3, 1.0 - abs(s.g - D) / (0.05 * D + 1e-4));\n"
    "            sum += s.r * w;\n"
    "            ws += w;\n"
    "        }\n"
    "    gl_FragColor = vec4(vec3(ws > 0.0 ? sum / ws : 1.0), 1.0);\n"
    "}\n";

/* vs NULL: a fragment shader under the fixed-function vertex stage (lighting, texture matrices). */
GLuint ao_program(const char *what, const char *vs, const char *fs)
{
    const char *src[2] = {vs, fs};
    GLuint p = aof.CreateProgram();
    for (int k = 0; k < 2; k++) {
        if (!src[k])
            continue;
        GLint ok = 0;
        GLuint s = aof.CreateShader(k ? 0x8B30 : 0x8B31);       /* GL_FRAGMENT_SHADER / GL_VERTEX_SHADER */
        aof.ShaderSource(s, 1, &src[k], NULL);
        aof.CompileShader(s);
        aof.GetShaderiv(s, 0x8B81, &ok);                        /* GL_COMPILE_STATUS */
        if (!ok) {
            char log[1024] = "";
            aof.GetShaderInfoLog(s, sizeof log, NULL, log);
            gmv_log("[ao] %s shader does not compile: %s\n", what, log);
            return 0;
        }
        aof.AttachShader(p, s);
    }
    GLint ok = 0;
    aof.LinkProgram(p);
    aof.GetProgramiv(p, 0x8B82, &ok);                           /* GL_LINK_STATUS */
    if (!ok) {
        char log[1024] = "";
        aof.GetProgramInfoLog(p, sizeof log, NULL, log);
        gmv_log("[ao] %s program does not link: %s\n", what, log);
        return 0;
    }
    return p;
}

void ao_tex(GLuint t, GLint ifmt, int w, int h, GLenum fmt, GLenum type)
{
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);  /* GL_CLAMP_TO_EDGE */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
}

/* Size the textures to the screen; called inside ao_run's state save. */
void ao_alloc(int w, int h)
{
    ao.w = w;
    ao.h = h;
    ao.hw = (w + 1) / 2;
    ao.hh = (h + 1) / 2;
    ao_tex(ao.t_depth, 0x88F0, w, h, 0x84F9, 0x84FA);          /* DEPTH24_STENCIL8, DEPTH_STENCIL, 24_8 */
    glTexParameteri(GL_TEXTURE_2D, 0x884C, GL_NONE);            /* GL_TEXTURE_COMPARE_MODE */
    for (int k = 0; k < 2; k++)
        ao_tex(ao.t[k], 0x8230, ao.hw, ao.hh, 0x8227, GL_FLOAT);   /* RG32F, RG */
}

/* The shader and framebuffer entry points (AMBIENT_OCCLUSION and the SUN_MOON glows); `off` names
   what goes without them in the log. */
int ao_procs(const char *off)
{
    static int done;
    if (done)
        return done > 0;
    done = -1;
    static const struct { const char *n; void **p; } fns[] = {
        {"glCreateShader", (void **)&aof.CreateShader}, {"glShaderSource", (void **)&aof.ShaderSource},
        {"glCompileShader", (void **)&aof.CompileShader}, {"glGetShaderiv", (void **)&aof.GetShaderiv},
        {"glGetShaderInfoLog", (void **)&aof.GetShaderInfoLog}, {"glCreateProgram", (void **)&aof.CreateProgram},
        {"glAttachShader", (void **)&aof.AttachShader}, {"glLinkProgram", (void **)&aof.LinkProgram},
        {"glGetProgramiv", (void **)&aof.GetProgramiv}, {"glGetProgramInfoLog", (void **)&aof.GetProgramInfoLog},
        {"glUseProgram", (void **)&aof.UseProgram}, {"glGetUniformLocation", (void **)&aof.GetUniformLocation},
        {"glUniform1i", (void **)&aof.Uniform1i}, {"glUniform1f", (void **)&aof.Uniform1f},
        {"glUniform2f", (void **)&aof.Uniform2f}, {"glUniform4f", (void **)&aof.Uniform4f},
        {"glUniformMatrix4fv", (void **)&aof.UniformMatrix4fv}, {"glUniformMatrix3fv", (void **)&aof.UniformMatrix3fv},
        {"glUniform1fv", (void **)&aof.Uniform1fv}, {"glUniform3fv", (void **)&aof.Uniform3fv},
        {"glFramebufferTexture2D", (void **)&aof.FramebufferTexture2D},
        {"glActiveTexture", (void **)&aof.ActiveTexture}, {"glBlendEquation", (void **)&aof.BlendEquation},
    };
    for (size_t i = 0; i < sizeof fns / sizeof *fns; i++)
        if (!(*fns[i].p = gl_real(fns[i].n))) {
            gmv_log("[ao] the driver has no %s; %s off\n", fns[i].n, off);
            return 0;
        }
    if (!msf.BindFramebuffer) {                                 /* MSAA=0 skipped loading these */
        msf.GenFramebuffers = ms_proc("glGenFramebuffers", "glGenFramebuffersEXT");
        msf.BindFramebuffer = ms_proc("glBindFramebuffer", "glBindFramebufferEXT");
        msf.CheckFramebufferStatus = ms_proc("glCheckFramebufferStatus", "glCheckFramebufferStatusEXT");
        msf.BlitFramebuffer = ms_proc("glBlitFramebuffer", "glBlitFramebufferEXT");
        if (!msf.GenFramebuffers || !msf.BindFramebuffer || !msf.CheckFramebufferStatus || !msf.BlitFramebuffer) {
            gmv_log("[ao] the driver has no framebuffer objects; %s off\n", off);
            return 0;
        }
    }
    done = 1;
    return 1;
}

int ao_init(void)
{
    if (!ao_procs("ambient occlusion"))
        return 0;
    ao.p_ao = ao_program("AO", ao_vs, ao_fs_ao);
    ao.p_blur = ao.p_ao ? ao_program("blur", ao_vs, ao_fs_blur) : 0;
    ao.p_comp = ao.p_blur ? ao_program("composite", ao_vs, ao_fs_comp) : 0;
    if (!ao.p_comp) {
        gmv_log("[ao] ambient occlusion off\n");
        return 0;
    }
    glGenTextures(1, &ao.t_depth);
    glGenTextures(2, ao.t);
    msf.GenFramebuffers(1, &ao.fbo_depth);
    msf.GenFramebuffers(2, ao.fbo);
    return 1;
}

/* Attach the textures (after ao_alloc gave them storage) and check the framebuffers. */
int ao_attach(void)
{
    msf.BindFramebuffer(MS_FRAMEBUFFER, ao.fbo_depth);
    aof.FramebufferTexture2D(MS_FRAMEBUFFER, 0x821A, GL_TEXTURE_2D, ao.t_depth, 0);   /* DEPTH_STENCIL_ATTACHMENT */
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    GLenum st = msf.CheckFramebufferStatus(MS_FRAMEBUFFER);
    for (int k = 0; k < 2; k++) {
        msf.BindFramebuffer(MS_FRAMEBUFFER, ao.fbo[k]);
        aof.FramebufferTexture2D(MS_FRAMEBUFFER, 0x8CE0, GL_TEXTURE_2D, ao.t[k], 0);
        GLenum s = msf.CheckFramebufferStatus(MS_FRAMEBUFFER);
        if (s != 0x8CD5)
            st = s;
    }
    if (st != 0x8CD5) {
        gmv_log("[ao] framebuffer incomplete (0x%x); ambient occlusion off\n", st);
        return 0;
    }
    return 1;
}

int ao_invert(const GLfloat *m, GLfloat *out)
{
    double inv[16], det;
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0)
        return 0;
    for (int i = 0; i < 16; i++)
        out[i] = (GLfloat)(inv[i] / det);
    return 1;
}

#if GMV_DEV
/* Eye depth at NDC z (for the log). */
double ao_eye_depth(const GLfloat *inv, double z)
{
    double pz = inv[10] * z + inv[14], pw = inv[11] * z + inv[15];
    return pw != 0 ? fabs(pz / pw) : HUGE_VAL;
}
#endif

void ao_tri(void)
{
    glBegin(GL_TRIANGLES);
    glVertex2f(-1, -1);
    glVertex2f(3, -1);
    glVertex2f(-1, 3);
    glEnd();
}
