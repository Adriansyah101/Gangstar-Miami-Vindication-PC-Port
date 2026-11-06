/* fog.c -- FOG_MODE 1: fog on the whole world, and the sea carried on past the map's edge (sea_beyond) */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "compat.h"
#include <GL/gl.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "render.h"

/* ---- FOG_MODE 1/2: fog on the whole world (distances from DRAW_DISTANCE, the patches gmv_set_fog) ---------
   The game fogs per material (bits 12-13 of the render-state word, read at 0x4fc871; 0 = glDisable(GL_FOG))
   and most of the city's materials -- buildings, roads, the sea -- have it off, on the Mac too.  With fog
   distances given, a perspective, depth-tested draw to the screen that the game left unfogged is drawn
   with the game's linear fog anyway.  Left alone: the sky dome and the sun and moon discs (g_sky_draw,
   set by the patches around the skybox render), the glows, the 2D layer and the game's own framebuffers.
   Additive draws are fogged towards black and multiplying ones towards white, so they fade out instead
   of turning into fog-coloured shapes.
   The fog's colour is the sky's.  The game's fog colour is a near-white that drifts away from the sky at
   dusk and night, so the fogged city stood out against the sky as a pale band.  The sky gradient is a
   wall kept in front of the camera (its largest x) whose vertex colours change with height, symmetric
   about eye level; sky_read takes its colours and position as it is drawn, every frame, and the fog
   colour becomes the sky's at eye level, also for the materials the game fogs itself (ao_Fogfv).
   The distant skyline (farcity, the alpha-blended skybox mesh: a half cylinder around the camera, z up,
   its foot where the world is seen to end once the patches has raised the skybox with the camera, a
   skirt below it down to z -0.26 and the buildings up to z 0.25) has no real
   distance.  It lies past the end of the drawn city, so at its foot it takes the fog of the city's far
   end (g_fog_world_far, from the patches), and the city's fogged edge runs into it without a seam; above
   that the fog thins to FOG_FARCITY of it by FOG_FARCITY_RISE, so the taller buildings stand out of the
   haze, and its colour turns to the sky's right behind each point.  A small shader draws it as the
   game's material does (unit 0 texture times vertex colour; the alpha test stays fixed-function) with
   that fog.  Without shaders it gets FOG_FARCITY of the fog colour throughout, through a constant fog
   coordinate. */
extern float g_fog_world_far;                           /* world units */
#define FOG_FARCITY      0.6f
#define FOG_FARCITY_RISE 0.08f                          /* in the mesh's z: about 8 degrees above the horizon */
GLfloat g_fog_col[4];                            /* GL_FOG_COLOR in force */
GLfloat g_fog_game[4];                           /* GL_FOG_COLOR as the game last set it */
int g_fog_game_set;
int g_fog_mode;                                  /* FOG_MODE: 0 the game's own, 1 linear, 2 the hybrid */
float g_fog_height = 6000;                       /* FOG_HEIGHT, world units; 0 = as thick at every height */

sky_t sky;

/* An array or index pointer as client memory: as given when no buffer object is bound for it (`binding`
   names its binding, e.g. GL_COLOR_ARRAY_BUFFER_BINDING), else where it lies in the data of a game
   buffer made static (GMV_VBO_STATIC keeps that data); NULL when not readable. */
const char *cl_ptr(GLenum binding, const void *p, GLint *buf)
{
    GLint b = 0;
    glGetIntegerv(binding, &b);
    if (buf)
        *buf = b;
    if (!b)
        return p;
    return gmv_vbostatic_client((unsigned)b, (uintptr_t)p);
}

/* GL_ARRAY_BUFFER for the host's own client arrays (0) and back; the array pointers keep the buffer
   bound when they were given. */
void cl_array_buffer(GLuint b)
{
    static void(APIENTRY * bind)(GLenum, GLuint);
    if (!bind) bind = gl_real("glBindBuffer");
    if (bind)
        bind(0x8892, b);
}

static void sky_colour_at(GLfloat z, GLfloat *out)
{
    int i = 1;
    while (i < sky.n - 1 && z > sky.z[i])
        i++;
    GLfloat t = (z - sky.z[i - 1]) / (sky.z[i] - sky.z[i - 1]);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    for (int k = 0; k < 3; k++)
        out[k] = sky.c[i - 1][k] + (sky.c[i][k] - sky.c[i - 1][k]) * t;
}

/* Below eye level the gradient repeats the sky upside down.  It shows only where nothing of the world is
   drawn, past the end of the sea, and there it takes the fog's colour, as fully fogged sea would have:
   the gradient is drawn from a copy of its colours with those below eye level set to it. */
static struct {
    const void *ptr;                                    /* the game's colour array, restored after the draw */
    GLint stride;
    GLint buf, abo;                                     /* the buffer object it is in (0 = client memory); GL_ARRAY_BUFFER */
    unsigned char col[1024 * 64];
} sky_floor;

static int sky_floor_begin(GLsizei count, const uint16_t *ix, const char *p, GLint st, const char *cp, GLint cst,
                           GLint cst_given, const void *cp_given, GLint cbuf, GLint abo)
{
    unsigned mx = 0;
    for (int k = 0; k < count; k++)
        if (ix[k] > mx) mx = ix[k];
    if ((size_t)(mx + 1) * cst > sizeof sky_floor.col)
        return 0;
    unsigned char hz[4] = {(unsigned char)(sky.hz[0] * 255 + 0.5f), (unsigned char)(sky.hz[1] * 255 + 0.5f),
                           (unsigned char)(sky.hz[2] * 255 + 0.5f), 255};
    for (int k = 0; k < count; k++) {
        const float *v = (const float *)(p + (size_t)ix[k] * st);
        unsigned char *c = sky_floor.col + (size_t)ix[k] * cst;
        memcpy(c, cp + (size_t)ix[k] * cst, 4);
        if (v[2] < sky.cam[2] - 1e-4f)
            memcpy(c, hz, 3);
    }
    sky_floor.ptr = cp_given;
    sky_floor.stride = cst_given;
    sky_floor.buf = cbuf;
    sky_floor.abo = abo;
    if (abo)
        cl_array_buffer(0);
    glColorPointer(4, GL_UNSIGNED_BYTE, cst, sky_floor.col);
    if (abo)
        cl_array_buffer((GLuint)abo);
    return 1;
}

void sky_floor_end(void)
{
    if (sky_floor.buf != sky_floor.abo)
        cl_array_buffer((GLuint)sky_floor.buf);
    glColorPointer(4, GL_UNSIGNED_BYTE, sky_floor.stride, sky_floor.ptr);
    if (sky_floor.buf != sky_floor.abo)
        cl_array_buffer((GLuint)sky_floor.abo);
}

/* Before the sky gradient's draw (the skybox's unblended indexed draw, client arrays); 1 = its colours
   are replaced for the draw (sky_floor_end after it). */
int sky_read(GLsizei count, GLenum type, const void *idx)
{
    if (!g_fog_all || !g_sky_draw || g_blend_on || type != GL_UNSIGNED_SHORT || count > 256)
        return 0;
    GLint abo = 0, cbuf = 0, sz = 0, ty = 0, st = 0, csz = 0, cty = 0, cst = 0;
    const char *p = NULL, *cp = NULL;
    const void *cp_given = NULL;
    glGetIntegerv(0x8894, &abo);                        /* GL_ARRAY_BUFFER_BINDING */
    glGetIntegerv(GL_VERTEX_ARRAY_SIZE, &sz);
    glGetIntegerv(GL_VERTEX_ARRAY_TYPE, &ty);
    glGetIntegerv(GL_VERTEX_ARRAY_STRIDE, &st);
    glGetPointerv(GL_VERTEX_ARRAY_POINTER, (void **)&p);
    glGetIntegerv(GL_COLOR_ARRAY_SIZE, &csz);
    glGetIntegerv(GL_COLOR_ARRAY_TYPE, &cty);
    glGetIntegerv(GL_COLOR_ARRAY_STRIDE, &cst);
    glGetPointerv(GL_COLOR_ARRAY_POINTER, (void **)&cp_given);
    p = cl_ptr(0x8896, p, NULL);                        /* GL_VERTEX_ARRAY_BUFFER_BINDING */
    cp = cl_ptr(0x8898, cp_given, &cbuf);               /* GL_COLOR_ARRAY_BUFFER_BINDING */
    const uint16_t *ix = (const uint16_t *)cl_ptr(0x8895, idx, NULL);
    if (!ix || !p || !cp || sz < 3 || ty != GL_FLOAT || csz != 4 || cty != GL_UNSIGNED_BYTE ||
        !glIsEnabled(GL_COLOR_ARRAY))
        return 0;
    GLint cst_given = cst;
    if (!st) st = sz * 4;
    if (!cst) cst = 4;
    GLfloat wall = -1e30f, z[SKY_KEYS], c[SKY_KEYS][3];
    for (int k = 0; k < count; k++) {
        const float *v = (const float *)(p + (size_t)ix[k] * st);
        if (v[0] > wall) wall = v[0];
    }
    int n = 0;
    for (int k = 0; k < count; k++) {
        const float *v = (const float *)(p + (size_t)ix[k] * st);
        const unsigned char *col = (const unsigned char *)(cp + (size_t)ix[k] * cst);
        if (fabsf(v[0] - wall) > 1e-4f)
            continue;
        int at = 0;
        while (at < n && z[at] < v[2] - 1e-4f)
            at++;
        if (at < n && fabsf(z[at] - v[2]) <= 1e-4f)
            continue;                                   /* a height already read */
        if (n == SKY_KEYS)
            return 0;                                   /* not the gradient expected */
        memmove(z + at + 1, z + at, (size_t)(n - at) * sizeof *z);
        memmove(c + at + 1, c + at, (size_t)(n - at) * sizeof *c);
        z[at] = v[2];
        for (int j = 0; j < 3; j++)
            c[at][j] = col[j] / 255.0f;
        n++;
    }
    if (n < 2)
        return 0;
    GLfloat m[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    GLfloat a = m[0], b = m[4], cc = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    GLfloat det = a * (e * i - f * h) - b * (d * i - f * g) + cc * (d * h - e * g);
    if (!(fabsf(det) > 1e-20f))
        return 0;
    GLfloat r[9] = {(e * i - f * h) / det, (cc * h - b * i) / det, (b * f - cc * e) / det,     /* by row */
                    (f * g - d * i) / det, (a * i - cc * g) / det, (cc * d - a * f) / det,
                    (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det};
    for (int row = 0; row < 3; row++) {
        sky.cam[row] = -(r[row * 3] * m[12] + r[row * 3 + 1] * m[13] + r[row * 3 + 2] * m[14]);
        for (int col = 0; col < 3; col++)
            sky.inv[col * 3 + row] = r[row * 3 + col];
    }
    memcpy(sky.z, z, sizeof z);
    memcpy(sky.c, c, sizeof c);
    sky.n = n;
    sky.wall = wall;
    sky_colour_at(sky.cam[2], sky.hz);
    sky.hz[3] = 1;
    static int logged;
    if (!logged++)
        gmv_trace("[fog] the fog takes the sky's colour at eye level (%d colours up the sky; now %.2f %.2f %.2f)\n",
                n, sky.hz[0], sky.hz[1], sky.hz[2]);
    memcpy(g_fog_col, sky.hz, sizeof g_fog_col);
    glFogfv(GL_FOG_COLOR, sky.hz);
    return sky_floor_begin(count, ix, p, st, cp, cst, cst_given, cp_given, cbuf, abo);
}

static const char farcity_vs[] =
    "#version 120\n"
    "varying float z;\n"
    "varying vec3 e;\n"
    "void main() {\n"
    "    gl_Position = ftransform();\n"
    "    gl_TexCoord[0] = gl_TextureMatrix[0] * gl_MultiTexCoord0;\n"
    "    gl_FrontColor = gl_Color;\n"
    "    z = gl_Vertex.z;\n"
    "    e = (gl_ModelViewMatrix * gl_Vertex).xyz;\n"
    "}\n";

/* The sky behind a point: its eye-space direction taken into the sky's space, to the wall. */
static const char farcity_fs[] =
    "#version 120\n"
    "uniform sampler2D tex;\n"
    "uniform float base, top, rise, wall;\n"
    "uniform int kn;\n"
    "uniform float kz[12];\n"
    "uniform vec3 kc[12];\n"
    "uniform mat3 inv;\n"
    "uniform vec3 cam;\n"
    "varying float z;\n"
    "varying vec3 e;\n"
    "vec3 sky() {\n"
    "    vec3 d = inv * e;\n"
    "    if (kn < 2 || abs(d.x) < 1e-6) return gl_Fog.color.rgb;\n"
    "    float t = (wall - cam.x) / d.x;\n"
    "    if (t <= 0.0) return gl_Fog.color.rgb;\n"
    "    float h = cam.z + t * d.z;\n"
    "    if (h < cam.z) return gl_Fog.color.rgb;\n"     /* below eye level the sky is the fog's colour */
    "    vec3 c = kc[0];\n"
    "    for (int i = 1; i < 12; i++)\n"
    "        if (i < kn && h > kz[i - 1])\n"
    "            c = mix(kc[i - 1], kc[i], clamp((h - kz[i - 1]) / (kz[i] - kz[i - 1]), 0.0, 1.0));\n"
    "    return c;\n"
    "}\n"
    "void main() {\n"
    "    vec4 c = texture2D(tex, gl_TexCoord[0].st) * gl_Color;\n"
    "    float s = smoothstep(0.0, rise, z);\n"
    "    c.rgb = mix(c.rgb, mix(gl_Fog.color.rgb, sky(), s), mix(base, top, s));\n"
    "    gl_FragColor = c;\n"
    "}\n";

static struct {
    int ready;                                          /* 0 = not tried, 1 = shader, -1 = fog coordinate */
    GLuint p;
    GLint u_tex, u_base, u_top, u_rise, u_wall, u_kn, u_kz, u_kc, u_inv, u_cam, prog;
} fcy;

static int farcity_init(void)
{
    if (!ao_procs("the skyline's rising haze") || !(fcy.p = ao_program("skyline", farcity_vs, farcity_fs)))
        return 0;
    fcy.u_tex = aof.GetUniformLocation(fcy.p, "tex");
    fcy.u_base = aof.GetUniformLocation(fcy.p, "base");
    fcy.u_top = aof.GetUniformLocation(fcy.p, "top");
    fcy.u_rise = aof.GetUniformLocation(fcy.p, "rise");
    fcy.u_wall = aof.GetUniformLocation(fcy.p, "wall");
    fcy.u_kn = aof.GetUniformLocation(fcy.p, "kn");
    fcy.u_kz = aof.GetUniformLocation(fcy.p, "kz");
    fcy.u_kc = aof.GetUniformLocation(fcy.p, "kc");
    fcy.u_inv = aof.GetUniformLocation(fcy.p, "inv");
    fcy.u_cam = aof.GetUniformLocation(fcy.p, "cam");
    return 1;
}

int fog_force_begin(void)
{
    if (!g_fog_all || g_ao_fog_on || g_sky_glow_draw || g_glt.last_fb || !g_ao_depth_on || g_ao_fogp[0] != GL_LINEAR)
        return 0;
    ao_proj_refresh();
    if (!g_ao_persp)
        return 0;
    int kind = 1;   /* 1 plain, 2 towards black, 3 towards white, 4 farcity by fog coordinate, 5 by shader */
    if (g_blend_on) {
        if (g_blend_dst == GL_ONE)
            kind = 2;
        else if (g_blend_src == GL_DST_COLOR || g_blend_dst == GL_SRC_COLOR)
            kind = 3;
    }
    if (g_sky_draw) {
        if (!g_blend_on || g_blend_dst != GL_ONE_MINUS_SRC_ALPHA)
            return 0;                                   /* the sky dome, the discs */
        kind = 4;
    }
    if (kind == 2 || kind == 3) {
        static const GLfloat black[4] = {0, 0, 0, 0}, white[4] = {1, 1, 1, 1};
        glFogfv(GL_FOG_COLOR, kind == 2 ? black : white);
    } else if (kind == 4) {
        GLfloat fs = g_ao_fogp[1], fe = g_ao_fogp[2];
        GLfloat base = g_fog_mode == 2 ? fh_amount(sqrtf(g_fog_clip * g_fog_clip + g_cam_z * g_cam_z), g_cam_z, 0)
                     : fe > fs ? (g_fog_world_far - fs) / (fe - fs) : 1;
        base = base > 1 ? 1 : base < 0 ? 0 : base;
        if (!fcy.ready)
            fcy.ready = farcity_init() ? 1 : -1;
        if (fcy.ready > 0) {
            glGetIntegerv(0x8B8D, &fcy.prog);           /* GL_CURRENT_PROGRAM */
            aof.UseProgram(fcy.p);
            aof.Uniform1i(fcy.u_tex, 0);
            aof.Uniform1f(fcy.u_base, base);
            aof.Uniform1f(fcy.u_top, base * FOG_FARCITY);
            aof.Uniform1f(fcy.u_rise, FOG_FARCITY_RISE);
            aof.Uniform1i(fcy.u_kn, sky.n);
            if (sky.n) {
                aof.Uniform1f(fcy.u_wall, sky.wall);
                aof.Uniform1fv(fcy.u_kz, sky.n, sky.z);
                aof.Uniform3fv(fcy.u_kc, sky.n, &sky.c[0][0]);
                aof.UniformMatrix3fv(fcy.u_inv, 1, GL_FALSE, sky.inv);
                aof.Uniform3fv(fcy.u_cam, 1, sky.cam);
            }
            static int logged;
            if (!logged++)
                gmv_trace("[fog] the skyline: %.0f%% fog at its foot (the city ends at %.0f m), %.0f%% higher up\n",
                        base * 100, g_fog_world_far / 100, base * FOG_FARCITY * 100);
            return 5;
        }
        static void(APIENTRY * fc)(GLfloat);
        if (!fc)
            fc = gl_real("glFogCoordf");
        glFogi(0x8450 /* GL_FOG_COORD_SRC */, 0x8451 /* GL_FOG_COORD */);
        if (fc)
            fc(fs + (fe - fs) * base * FOG_FARCITY);
    }
    glEnable(GL_FOG);
    return kind;
}

void fog_force_end(int kind)
{
    if (!kind)
        return;
    if (kind == 5) {
        aof.UseProgram((GLuint)fcy.prog);
        return;
    }
    glDisable(GL_FOG);
    if (kind == 2 || kind == 3)
        glFogfv(GL_FOG_COLOR, g_fog_col);
    else if (kind == 4)
        glFogi(0x8450, 0x8452 /* GL_FRAGMENT_DEPTH */);
}

/* Past g_fog_clip (the patches: the fog's end, if the drawn city reaches beyond it) the world would only
   be fog-coloured shapes, which covered the skyline's foot standing there: world draws to the screen
   are clipped at that depth from the eye, as the fog counts it, by a user clip plane given in eye space
   (the game uses its own clip planes only for its framebuffers).  It also spares the GPU the city that
   could not be seen. */
extern float g_fog_clip;

int fog_clip_begin(void)
{
    if (!g_fog_all || !(g_fog_clip > 0) || g_fog_clip >= g_fog_world_far || g_sky_draw || g_sky_glow_draw ||
        g_glt.last_fb || !g_ao_depth_on)
        return 0;
    ao_proj_refresh();
    if (!g_ao_persp)
        return 0;
    static GLdouble eq[4];
    GLdouble want[4] = {0, 0, -g_ao_proj[11], g_fog_clip};     /* -z + d >= 0 looking down +z, z + d >= 0 down -z */
    if (memcmp(eq, want, sizeof eq)) {
        GLint mode = GL_MODELVIEW;
        glGetIntegerv(GL_MATRIX_MODE, &mode);
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();
        glClipPlane(FOG_CLIP_PLANE, want);
        glPopMatrix();
        glMatrixMode(mode);
        memcpy(eq, want, sizeof eq);
        static int logged;
        if (!logged++)
            gmv_trace("[fog] the world ends at %.0f m, where the fog is complete\n", g_fog_clip / 100);
    }
    glEnable(FOG_CLIP_PLANE);
    return 1;
}

/* The city's sea (alpha-blended, no depth writes, unit 0 texture times vertex colour, client arrays
   interleaved by 24 bytes) fills the box SEA_X0..SEA_X1 x SEA_Y0..SEA_Y1, deep blue at its edge, and
   stops there.  Near the map's edges and from the air that edge is in view well inside the fog, a
   straight line with nothing past it.  With fog on the whole world the sea goes on past the box, around
   the camera to beyond the pass's far plane, in the edge's colour and with the texture laid as the
   sea's own (fitted to the chunk's texture coordinates, so it scrolls with them).  It is drawn right
   after the first chunk of sea on the box's edge in each pass of a frame (the city's near pass and its
   low-detail far pass, each with its own depth range; drawn in the far pass alone, it was all past the
   fog's clip whenever the near pass reached beyond it), in that chunk's state and fog, in squares of
   SEA_CELL so the fog is right across each. */
#define SEA_X0   -120719.0f
#define SEA_X1    119609.0f
#define SEA_Y0   -110358.0f
#define SEA_Y1    146129.0f
#define SEA_CELL  25000.0f
#define SEA_MAX   1024                                  /* squares a frame */

void sea_beyond(GLsizei count, GLenum type, const void *idx)
{
    static unsigned done[2] = {~0u, ~0u};               /* by pass: the near one, the low-detail far one */
    static unsigned char edge[4] = {30, 58, 111, 255};
    static GLfloat pos[SEA_MAX * 6][3], uv[SEA_MAX * 6][2];
    static unsigned char col[SEA_MAX * 6][4];
    int pass = g_ao_dr[0] >= 0.25f;
    if (done[pass] == g_frame_no || g_sky_draw || !g_blend_on || g_blend_src != GL_SRC_ALPHA ||
        g_blend_dst != GL_ONE_MINUS_SRC_ALPHA || type != GL_UNSIGNED_SHORT || count < 3 || count > 20000)
        return;
    GLboolean dmask = 1;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask);
    if (dmask)
        return;
    GLint cat = 0, sz = 0, ty = 0, st = 0, csz = 0, cty = 0, cst = 0, tsz = 0, tty = 0, tst = 0;
    const char *p = NULL, *cp = NULL, *tp = NULL;
    glGetIntegerv(0x84E1, &cat);                        /* GL_CLIENT_ACTIVE_TEXTURE */
    if (cat != 0x84C0 || !glIsEnabled(GL_COLOR_ARRAY) || !glIsEnabled(GL_TEXTURE_COORD_ARRAY))
        return;
    glGetIntegerv(GL_VERTEX_ARRAY_SIZE, &sz);
    glGetIntegerv(GL_VERTEX_ARRAY_TYPE, &ty);
    glGetIntegerv(GL_VERTEX_ARRAY_STRIDE, &st);
    glGetPointerv(GL_VERTEX_ARRAY_POINTER, (void **)&p);
    glGetIntegerv(GL_COLOR_ARRAY_SIZE, &csz);
    glGetIntegerv(GL_COLOR_ARRAY_TYPE, &cty);
    glGetIntegerv(GL_COLOR_ARRAY_STRIDE, &cst);
    glGetPointerv(GL_COLOR_ARRAY_POINTER, (void **)&cp);
    glGetIntegerv(GL_TEXTURE_COORD_ARRAY_SIZE, &tsz);
    glGetIntegerv(GL_TEXTURE_COORD_ARRAY_TYPE, &tty);
    glGetIntegerv(GL_TEXTURE_COORD_ARRAY_STRIDE, &tst);
    glGetPointerv(GL_TEXTURE_COORD_ARRAY_POINTER, (void **)&tp);
    p = cl_ptr(0x8896, p, NULL);                        /* GL_VERTEX_ARRAY_BUFFER_BINDING */
    cp = cl_ptr(0x8898, cp, NULL);                      /* GL_COLOR_ARRAY_BUFFER_BINDING */
    tp = cl_ptr(0x889A, tp, NULL);                      /* GL_TEXTURE_COORD_ARRAY_BUFFER_BINDING (unit 0) */
    const uint16_t *ix = (const uint16_t *)cl_ptr(0x8895, idx, NULL);
    if (!ix || !p || !cp || !tp || sz != 3 || ty != GL_FLOAT || st != 24 || csz != 4 || cty != GL_UNSIGNED_BYTE ||
        cst != 24 || tsz != 2 || tty != GL_FLOAT || tst != 24)
        return;
    /* Flat at sea level, on the box's edge, and three corners to fit the texture to. */
    int lo = -1, hi = -1, on_edge = -1;
    for (int k = 0; k < count; k++) {
        const float *v = (const float *)(p + (size_t)ix[k] * 24);
        if (fabsf(v[2]) > 1)
            return;
        if (lo < 0 || v[0] < ((const float *)(p + (size_t)lo * 24))[0]) lo = ix[k];
        if (hi < 0 || v[0] > ((const float *)(p + (size_t)hi * 24))[0]) hi = ix[k];
        if (on_edge < 0 && (fabsf(v[0] - SEA_X0) < 2 || fabsf(v[0] - SEA_X1) < 2 || fabsf(v[1] - SEA_Y0) < 2 ||
                            fabsf(v[1] - SEA_Y1) < 2))
            on_edge = ix[k];
    }
    if (on_edge < 0)
        return;
    const float *a = (const float *)(p + (size_t)lo * 24), *b = (const float *)(p + (size_t)hi * 24);
    double ex = b[0] - a[0], ey = b[1] - a[1], best = 0;
    int third = -1;
    for (int k = 0; k < count; k++) {
        const float *v = (const float *)(p + (size_t)ix[k] * 24);
        double area = fabs(ex * (v[1] - a[1]) - ey * (v[0] - a[0]));
        if (area > best) best = area, third = ix[k];
    }
    if (third < 0 || best < 1e6)
        return;
    const float *c = (const float *)(p + (size_t)third * 24);
    const float *ta = (const float *)(tp + (size_t)lo * 24), *tb = (const float *)(tp + (size_t)hi * 24),
                *tc = (const float *)(tp + (size_t)third * 24);
    double dx1 = b[0] - a[0], dy1 = b[1] - a[1], dx2 = c[0] - a[0], dy2 = c[1] - a[1];
    double det = dx1 * dy2 - dy1 * dx2, fit[2][3];
    for (int j = 0; j < 2; j++) {
        double d1 = tb[j] - ta[j], d2 = tc[j] - ta[j];
        fit[j][0] = (d1 * dy2 - dy1 * d2) / det;
        fit[j][1] = (dx1 * d2 - d1 * dx2) / det;
        fit[j][2] = ta[j] - fit[j][0] * a[0] - fit[j][1] * a[1];
    }
    for (int k = 0; k < count; k += count / 8 + 1) {   /* the texture must lie flat across the sea */
        const float *v = (const float *)(p + (size_t)ix[k] * 24), *t = (const float *)(tp + (size_t)ix[k] * 24);
        for (int j = 0; j < 2; j++)
            if (fabs(fit[j][0] * v[0] + fit[j][1] * v[1] + fit[j][2] - t[j]) > 0.01)
                return;
    }
    memcpy(edge, cp + (size_t)on_edge * 24, 4);
    done[pass] = g_frame_no;
    /* The camera, and how far this pass sees (corners of the screen included). */
    GLfloat m[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    float camx = -(m[0] * m[12] + m[1] * m[13] + m[2] * m[14]), camy = -(m[4] * m[12] + m[5] * m[13] + m[6] * m[14]);
    float far1 = fabsf(g_ao_proj[14] / (g_ao_proj[10] + 1)), far2 = fabsf(g_ao_proj[14] / (g_ao_proj[10] - 1));
    float r = 1.5f * (far1 > far2 ? far1 : far2);
    if (!(r > 0) || r > 2e6f)
        return;
    float x0 = camx - r, x1 = camx + r, y0 = camy - r, y1 = camy + r;
    float rect[4][4] = {{x0, SEA_X0, y0, y1}, {SEA_X1, x1, y0, y1},
                        {x0 > SEA_X0 ? x0 : SEA_X0, x1 < SEA_X1 ? x1 : SEA_X1, y0, SEA_Y0},
                        {x0 > SEA_X0 ? x0 : SEA_X0, x1 < SEA_X1 ? x1 : SEA_X1, SEA_Y1, y1}};
    int n = 0;
    for (int q = 0; q < 4; q++) {
        float *R = rect[q];
        if (R[1] <= R[0] || R[3] <= R[2])
            continue;
        int nx = (int)ceilf((R[1] - R[0]) / SEA_CELL), ny = (int)ceilf((R[3] - R[2]) / SEA_CELL);
        for (int i = 0; i < nx; i++)
            for (int j = 0; j < ny && n < SEA_MAX; j++) {
                float qx0 = R[0] + (R[1] - R[0]) * i / nx, qx1 = R[0] + (R[1] - R[0]) * (i + 1) / nx;
                float qy0 = R[2] + (R[3] - R[2]) * j / ny, qy1 = R[2] + (R[3] - R[2]) * (j + 1) / ny;
                const float corner[6][2] = {{qx0, qy0}, {qx1, qy0}, {qx1, qy1}, {qx0, qy0}, {qx1, qy1}, {qx0, qy1}};
                for (int k = 0; k < 6; k++) {
                    GLfloat *P = pos[n * 6 + k];
                    P[0] = corner[k][0], P[1] = corner[k][1], P[2] = 0;
                    for (int t = 0; t < 2; t++)
                        uv[n * 6 + k][t] = (GLfloat)(fit[t][0] * P[0] + fit[t][1] * P[1] + fit[t][2]);
                    memcpy(col[n * 6 + k], edge, 4);
                }
                n++;
            }
    }
    if (!n)
        return;
    static void(APIENTRY * draw)(GLenum, GLint, GLsizei);
    static void(APIENTRY * client_tex)(GLenum);
    if (!draw) draw = gl_real("glDrawArrays");
    if (!client_tex) client_tex = gl_real("glClientActiveTexture");
    if (!draw || !client_tex)
        return;
    GLint abo = 0;
    glGetIntegerv(0x8894, &abo);                        /* GL_ARRAY_BUFFER_BINDING */
    glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
    if (abo)
        cl_array_buffer(0);                             /* the squares are client arrays */
    for (int u = 3; u >= 0; u--) {
        client_tex(0x84C0 + u);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(0x845E);                       /* GL_SECONDARY_COLOR_ARRAY */
    glDisableClientState(0x8457);                       /* GL_FOG_COORD_ARRAY */
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, pos);
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, col);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    draw(GL_TRIANGLES, 0, n * 6);
    glPopClientAttrib();
    if (abo)                                            /* the pop restores it too; the game's own bind cache relies on it */
        cl_array_buffer((GLuint)abo);
    client_tex(0x84C0);
    static int logged;
    if (!logged++)
        gmv_trace("[fog] the sea goes on past the map's edge (%d squares now, colour %u %u %u)\n", n, edge[0], edge[1],
                edge[2]);
}
