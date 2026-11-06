/* aniso.c -- ANISOTROPIC_FILTERING */
#include <stdlib.h>
#include <string.h>
#include "compat.h"
#include <GL/gl.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "render.h"

/* ---- ANISOTROPIC_FILTERING: an enhancement, off by default and under ORIGINAL_MAC_EXPERIENCE ----
   The engine's texture-state code (0x501010) already sets GL_TEXTURE_MAX_ANISOTROPY_EXT on a texture
   whose anisotropy is dirty: min(the texture's own level, the GPU maximum it read with 0x84FF at
   0x505d2b).  So by default every texture gets exactly the level the game asks for, as on the Mac.
   ANISOTROPIC_FILTERING=N (2-16) raises that to at least N, capped at the GPU maximum, on every level
   the game sets and on every texture it gives a mipmapped minifying filter.  Only the GPU's sampling
   changes; nothing the game reads back does.  The first distinct levels the game asks for are logged
   as [aniso] lines.
   With GAMEPATCH_TAB the wrappers are in from the start whatever the level, and they note each
   texture's target, its own level and whether its filter is mipmapped, so a new level from the
   options menu's TEXTURE FILTERING (gmv_set_aniso) can be put on every live texture after the next
   present: the larger of the texture's own level and the new one, as if it had been there from the
   start. */
float g_aniso;                                   /* GMV_ANISOTROPIC_FILTERING; <= 1 = off */
static float g_aniso_max = -1, g_aniso_lvl = -1;        /* the GPU's maximum; the level in use */
static float g_aniso_want = -1;                         /* gmv_set_aniso's level, for aniso_sync */
typedef struct {
    unsigned char target, mip, set;                     /* target 1 = 2D, 2 = cube map; set: own level given */
    float own;
} af_tex;
static af_tex *g_af;                                    /* by texture name */
static GLuint g_af_n;

static float aniso_level(void)
{
    if (g_aniso_lvl < 0) {
        if (g_aniso_max < 0) {
            const char *ext = (const char *)glGetString(GL_EXTENSIONS);
            GLfloat mx = 1;
            if (ext && strstr(ext, "GL_EXT_texture_filter_anisotropic"))
                glGetFloatv(0x84FF, &mx);               /* GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT */
            g_aniso_max = mx;
        }
        g_aniso_lvl = g_aniso < g_aniso_max ? g_aniso : g_aniso_max;
        gmv_log("[aniso] ANISOTROPIC_FILTERING=%g, GPU maximum %g: textures get at least %gx\n", g_aniso, g_aniso_max,
                g_aniso_lvl);
    }
    return g_aniso_lvl;
}

/* The note of the texture bound to target, or NULL. */
static af_tex *af_entry(GLenum target)
{
    GLint name = 0;
    unsigned char t = target == GL_TEXTURE_2D ? 1 : target == 0x8513 ? 2 : 0;    /* GL_TEXTURE_CUBE_MAP */
    if (!g_tab_live || !t)
        return NULL;
    glGetIntegerv(t == 1 ? 0x8069 : 0x8514, &name);     /* GL_TEXTURE_BINDING_2D / _CUBE_MAP */
    if (name <= 0 || name >= (1 << 20))
        return NULL;
    if ((GLuint)name >= g_af_n) {
        GLuint n = g_af_n ? g_af_n : 1024;
        while (n <= (GLuint)name)
            n *= 2;
        af_tex *a = realloc(g_af, n * sizeof *a);
        if (!a)
            return NULL;
        memset(a + g_af_n, 0, (n - g_af_n) * sizeof *a);
        g_af = a;
        g_af_n = n;
    }
    g_af[name].target = t;
    return &g_af[name];
}

int gmv_set_aniso(float level)
{
    if (!g_tab_live)
        return 0;
    g_aniso_want = level > 16 ? 16 : level < 0 ? 0 : level;
    return 1;
}

/* After a present: gmv_set_aniso's level on every noted texture. */
void aniso_sync(void)
{
    if (g_aniso_want < 0)
        return;
    g_aniso = g_aniso_want;
    g_aniso_want = -1;
    g_aniso_lvl = -1;
    GLfloat lvl = aniso_level();
    GLint b2 = 0, bc = 0;
    unsigned n = 0;
    glGetIntegerv(0x8069, &b2);
    glGetIntegerv(0x8514, &bc);
    for (GLuint i = 1; i < g_af_n; i++) {
        af_tex *t = &g_af[i];
        if (!t->target || !(t->mip || t->set) || !glIsTexture(i))
            continue;
        GLenum target = t->target == 1 ? GL_TEXTURE_2D : 0x8513;
        GLfloat v = t->set && t->own > 1 ? t->own : 1;
        glBindTexture(target, i);
        glTexParameterf(target, 0x84FE, v > lvl ? v : lvl);
        n++;
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)b2);
    glBindTexture(0x8513, (GLuint)bc);
    gmv_trace("[aniso] %u textures set to at least %gx\n", n, lvl);
}

static void aniso_note(GLfloat v)
{
    static GLfloat seen[8];
    static int nseen;
    for (int i = 0; i < nseen; i++)
        if (seen[i] == v)
            return;
    if (nseen < 8) {
        seen[nseen++] = v;
        gmv_trace("[aniso] game asked for anisotropy %g\n", v);
    }
}

static void aniso_raise(GLenum target)
{
    GLfloat lvl = aniso_level(), cur = 1;
    if (lvl <= 1)
        return;
    glGetTexParameterfv(target, 0x84FE, &cur);          /* GL_TEXTURE_MAX_ANISOTROPY_EXT */
    if (cur < lvl)
        glTexParameterf(target, 0x84FE, lvl);
}

static int aniso_mip_filter(GLint f) { return f >= 0x2700 && f <= 0x2703; }    /* *_MIPMAP_* */

void af_TexParameterf(GLenum target, GLenum pname, GLfloat v)
{
    static void(APIENTRY * real)(GLenum, GLenum, GLfloat);
    if (!real) real = gl_real("glTexParameterf");
    if (pname == 0x84FE) {
        aniso_note(v);
        af_tex *t = af_entry(target);
        if (t) {
            t->set = 1;
            t->own = v;
        }
        GLfloat lvl = aniso_level();
        if (v < lvl)
            v = lvl;
    }
    if (real) real(target, pname, v);
    if (pname == GL_TEXTURE_MIN_FILTER) {
        af_tex *t = af_entry(target);
        if (t)
            t->mip = (unsigned char)aniso_mip_filter((GLint)v);
        if (aniso_mip_filter((GLint)v))
            aniso_raise(target);
    }
}
void af_TexParameteri(GLenum target, GLenum pname, GLint v)
{
    static void(APIENTRY * real)(GLenum, GLenum, GLint);
    if (!real) real = gl_real("glTexParameteri");
    if (pname == 0x84FE) {
        af_TexParameterf(target, pname, (GLfloat)v);
        return;
    }
    if (real) real(target, pname, v);
    if (pname == GL_TEXTURE_MIN_FILTER) {
        af_tex *t = af_entry(target);
        if (t)
            t->mip = (unsigned char)aniso_mip_filter(v);
        if (aniso_mip_filter(v))
            aniso_raise(target);
    }
}
void af_DeleteTextures(GLsizei n, const GLuint *names)
{
    static void(APIENTRY * real)(GLsizei, const GLuint *);
    if (!real) real = gl_real("glDeleteTextures");
    for (GLsizei i = 0; names && i < n; i++)
        if (names[i] < g_af_n)
            memset(&g_af[names[i]], 0, sizeof *g_af);
    if (real) real(n, names);
}

/* PAD_ICONS (padicons.c): it sees the uploads of compressed textures and the deletes. */
void pi_CompressedTexImage2D(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h,
                                    GLint border, GLsizei size, const void *data)
{
#if GMV_DEV
    if (gmv_prof_on())
        pf_CompressedTexImage2D(target, level, ifmt, w, h, border, size, data);
    else
#endif
    {
        static void(APIENTRY * real)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
        if (!real) real = gl_real("glCompressedTexImage2D");
        if (real) real(target, level, ifmt, w, h, border, size, data);
    }
    if (target == GL_TEXTURE_2D)
        gmv_padicons_upload(level, ifmt, w, h, size, data);
}
void pi_DeleteTextures(GLsizei n, const GLuint *names)
{
    gmv_padicons_deleted(n, names);
    af_DeleteTextures(n, names);
}
