/* water_reflections.c -- WATER_REFLECTIONS */
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

/* ---- WATER_REFLECTIONS: an enhancement, off by default and under ORIGINAL_MAC_EXPERIENCE ----------------
   Screen-space reflections on the sea, applied where and when the ambient occlusion is (between the world
   and the 2D layer, inside ao_run's state save) and before it and the bloom.  Neither the Mac nor the iOS
   game reflects anything in its sea (a translucent sheet at height 0 that writes no depth).
   - Each chunk of sea (alpha-blended, no depth writes, client arrays, flat at sea level and at least
     SSR_MIN_SPAN across) sets SSR_BIT in the stencil where it is drawn, the game's stencil state saved
     around it; the first chunk of a frame clears the bit and keeps the view (its modelview).
   - The pass draws on those pixels alone: the point of the sea under each is where the eye's ray meets
     height 0 (the depth there is the sea bed's), and where something drawn is nearer than that point
     it is left alone.  The sea's normal is ruffled by four travelling waves (deep-water speeds), faded
     out where a wave is under a few pixels long so the distance does not shimmer.
   - The reflected ray is marched across the screen against the depth of the layer most draws had (the
     city's near pass), perspective-correct, with a binary search at the first crossing; what it hits
     is taken from the frame as drawn so far, fading to the sky towards the screen's edges.  A ray that
     hits nothing takes the screen where its direction meets the sky (the far pass and the skyline count
     as sky), and past the screen the sky's colour at eye level (FOG_MODE >= 1; without it the
     reflection fades out there).
   - It is mixed in by Fresnel's law for water (Schlick, 2% head-on), times REFLECTION_STRENGTH, and
     thinned with the sea's own fog (the hybrid's curve at FOG_MODE=2, the linear fog otherwise). */
float g_ssr_strength = 1.0f;                     /* REFLECTION_STRENGTH */
#define SSR_BIT      0x80                               /* the stencil bit the sea sets */
#define SSR_MIN_SPAN 5000.0f                            /* world units: the smallest sea chunks are over 100 m */

static struct {
    unsigned frame;                                     /* g_frame_no + 1 of the last frame the sea was marked in */
    GLfloat view[16];                                   /* its modelview: world to eye */
    unsigned chunks;                                    /* marked in that frame */
} sea_mk;

int ssr_frame(void)
{
    return sea_mk.frame == g_frame_no + 1;
}

/* Before a draw: 1 = it is a chunk of sea, marked in the stencil until sea_mark_end. */
int sea_mark_begin(GLsizei count, GLenum type, const void *idx)
{
    if (!g_ssr_want || !g_ssr || g_sky_draw || g_glt.last_fb || !g_ao_depth_on || g_ao_dmask || !g_blend_on ||
        g_blend_src != GL_SRC_ALPHA || g_blend_dst != GL_ONE_MINUS_SRC_ALPHA || type != GL_UNSIGNED_SHORT ||
        count < 3 || count > 20000)
        return 0;
    ao_proj_refresh();
    if (!g_ao_persp)
        return 0;
    GLint sz = 0, ty = 0, st = 0;
    const char *p = NULL;
    glGetIntegerv(GL_VERTEX_ARRAY_SIZE, &sz);
    glGetIntegerv(GL_VERTEX_ARRAY_TYPE, &ty);
    glGetIntegerv(GL_VERTEX_ARRAY_STRIDE, &st);
    glGetPointerv(GL_VERTEX_ARRAY_POINTER, (void **)&p);
    p = cl_ptr(0x8896, p, NULL);                        /* GL_VERTEX_ARRAY_BUFFER_BINDING */
    const uint16_t *ix = (const uint16_t *)cl_ptr(0x8895, idx, NULL);
    if (!ix || !p || sz != 3 || ty != GL_FLOAT)
        return 0;
    if (!st)
        st = 12;
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    for (int k = 0; k < count; k++) {
        const float *v = (const float *)(p + (size_t)ix[k] * st);
        if (fabsf(v[2]) > 50)
            return 0;                                   /* not flat at sea level */
        x0 = fminf(x0, v[0]), x1 = fmaxf(x1, v[0]), y0 = fminf(y0, v[1]), y1 = fmaxf(y1, v[1]);
    }
    if (x1 - x0 < SSR_MIN_SPAN && y1 - y0 < SSR_MIN_SPAN)
        return 0;
    glPushAttrib(GL_STENCIL_BUFFER_BIT | GL_SCISSOR_BIT);
    if (glIsEnabled(GL_STENCIL_TEST)) {
        static int logged;
        if (!logged++)
            gmv_log("[ssr] the game has its stencil test on while it draws the sea; the sea's mark takes it over for the draw\n");
    }
    if (!ssr_frame()) {
        sea_mk.frame = g_frame_no + 1;
        sea_mk.chunks = 0;
        glGetFloatv(GL_MODELVIEW_MATRIX, sea_mk.view);
        glDisable(GL_SCISSOR_TEST);
        glStencilMask(SSR_BIT);
        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);
    }
    sea_mk.chunks++;
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, SSR_BIT, SSR_BIT);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glStencilMask(SSR_BIT);
    return 1;
}

void sea_mark_end(int on)
{
    if (on)
        glPopAttrib();
}

static const char ssr_fs[] =
    "#version 120\n"
    "uniform sampler2D scn, dep;\n"
    "uniform vec2 full, dr;\n"
    "uniform vec4 vp, plane, fogp, skyc;\n"             /* plane: up in eye space, up . a point of the sea;
                                                           fogp: start, 1 / (end - start), mode, a; skyc: colour, known */
    "uniform mat4 proj, invp, vinv;\n"
    "uniform mat3 rot;\n"                               /* world to eye, for directions */
    "uniform float t, str, wmin, reach;\n"
    "float dat(vec2 px) { return texture2D(dep, px / full).r; }\n"
    "vec3 unproj(vec2 px, float z) {\n"
    "    vec4 p = invp * vec4((px - vp.xy) / vp.zw * 2.0 - 1.0, z, 1.0);\n"
    "    return p.xyz / p.w;\n"
    "}\n"
    "float wof(vec3 p) { return proj[0][3] * p.x + proj[1][3] * p.y + proj[2][3] * p.z + proj[3][3]; }\n"
    "vec2 screen(vec4 h) { return (h.xy / h.w * 0.5 + 0.5) * vp.zw + vp.xy; }\n"
    /* what is drawn at px, as clip w; sky and the far pass, beyond the layer, are nothing to hit */
    "float scene_w(vec2 px) {\n"
    "    float d = dat(px);\n"
    "    if (d < dr.x || d >= dr.y - 1e-5) return 1e30;\n"
    "    return wof(unproj(px, (d - dr.x) / (dr.y - dr.x) * 2.0 - 1.0));\n"
    "}\n"
    /* the slope of one wave, len world units long; gone where it is under 6 pixels, whole from 12 */
    "vec2 wave(vec2 xy, vec2 dir, float len, float slope, float fp) {\n"
    "    float k = 6.2831853 / len;\n"
    "    float ph = k * dot(dir, xy) - sqrt(981.0 * k) * t;\n"
    "    return dir * (slope * cos(ph) * clamp(len / fp / 6.0 - 1.0, 0.0, 1.0));\n"
    "}\n"
    "void main() {\n"
    "    vec2 px = gl_FragCoord.xy;\n"
    "    vec3 v = normalize(unproj(px, -1.0));\n"
    "    float dn = dot(plane.xyz, v);\n"
    "    if (dn > -1e-6) discard;\n"
    "    vec3 p0 = v * (plane.w / dn);\n"
    "    float w0 = wof(p0);\n"
    "    if (scene_w(px) < w0 * 0.99 - 30.0) discard;\n"
    "    float clear = 1.0;\n"
    "    if (fogp.z == 2.0) {\n"
    "        float s = clamp((length(p0) - fogp.x) * fogp.y, 0.0, 1.0), s8 = s * s;\n"
    "        s8 *= s8; s8 *= s8;\n"
    "        clear = (fogp.w > 1e-3 ? (exp(-fogp.w * s * s) - exp(-fogp.w)) / (1.0 - exp(-fogp.w)) : 1.0 - s * s) * (1.0 - s8);\n"
    "    } else if (fogp.z == 1.0)\n"
    "        clear = 1.0 - clamp((w0 - fogp.x) * fogp.y, 0.0, 1.0);\n"
    "    if (clear <= 0.0) discard;\n"
    "    vec2 xy = (vinv * vec4(p0, 1.0)).xy;\n"
    "    float fp = w0 / (0.5 * abs(proj[0][0]) * vp.z);\n"
    "    vec2 g = wave(xy, vec2(0.80, 0.60), 900.0, 0.016, fp) + wave(xy, vec2(-0.45, 0.89), 530.0, 0.012, fp)\n"
    "           + wave(xy, vec2(0.97, -0.24), 310.0, 0.009, fp) + wave(xy, vec2(-0.71, -0.70), 190.0, 0.007, fp);\n"
    "    vec3 n = normalize(rot * vec3(-g, 1.0));\n"
    "    vec3 r = reflect(v, n);\n"
    "    float ru = dot(r, plane.xyz);\n"
    "    if (ru < 0.02) r = normalize(r + plane.xyz * (0.02 - ru));\n"
    "    float F = 0.02 + 0.98 * pow(1.0 - clamp(dot(-v, n), 0.0, 1.0), 5.0);\n"
    /* the sky the ray would see: on screen where its direction is, else the colour at eye level */
    "    vec3 sky = skyc.rgb;\n"
    "    float skyw = skyc.a;\n"
    "    vec4 hi = proj * vec4(r, 0.0);\n"
    "    if (hi.w > 1e-6) {\n"
    "        vec2 si = screen(hi), sc = clamp(si, vec2(0.5), full - 0.5);\n"
    "        if (dat(sc) >= dr.y - 1e-5) {\n"
    "            float on = 1.0 - clamp(length(si - sc) / (0.25 * full.y), 0.0, 1.0);\n"
    "            sky = mix(sky, texture2D(scn, sc / full).rgb, skyw > 0.0 ? on : 1.0);\n"
    "            skyw = max(skyw, on);\n"
    "        }\n"
    "    }\n"
    /* the march: from the sea's point to `reach` away or the near plane, cut at the screen's edges */
    "    vec3 p1 = p0 + r * reach;\n"
    "    float w1 = wof(p1);\n"
    "    if (w1 < wmin) p1 = p0 + (p1 - p0) * max((w0 - wmin) / (w0 - w1), 0.0);\n"
    "    vec4 h0 = proj * vec4(p0, 1.0), h1 = proj * vec4(p1, 1.0);\n"
    "    float k0 = 1.0 / h0.w, k1 = 1.0 / h1.w;\n"
    "    vec2 s0 = screen(h0), ds = screen(h1) - s0;\n"
    "    float amax = 1.0;\n"
    "    if (ds.x > 1e-3) amax = min(amax, (full.x - 0.5 - s0.x) / ds.x);\n"
    "    else if (ds.x < -1e-3) amax = min(amax, (0.5 - s0.x) / ds.x);\n"
    "    if (ds.y > 1e-3) amax = min(amax, (full.y - 0.5 - s0.y) / ds.y);\n"
    "    else if (ds.y < -1e-3) amax = min(amax, (0.5 - s0.y) / ds.y);\n"
    "    amax = max(amax, 0.0);\n"
    "    float noise = fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715))));\n"
    "    float ap = 0.0, rwp = w0, hit = -1.0;\n"
    "    for (int i = 1; i <= 48; i++) {\n"
    "        float a = amax * pow((float(i) - noise) / 48.0, 1.6);\n"
    "        float rw = 1.0 / mix(k0, k1, a), sw = scene_w(s0 + ds * a);\n"
    "        if (rw > sw && rw - sw < max(0.03 * sw + 50.0, 2.0 * abs(rw - rwp))) {\n"
    "            float lo = ap;\n"
    "            hit = a;\n"
    "            for (int j = 0; j < 5; j++) {\n"
    "                float m = 0.5 * (lo + hit);\n"
    "                if (1.0 / mix(k0, k1, m) > scene_w(s0 + ds * m)) hit = m; else lo = m;\n"
    "            }\n"
    "            break;\n"
    "        }\n"
    "        ap = a;\n"
    "        rwp = rw;\n"
    "    }\n"
    "    vec3 c = sky * skyw;\n"
    "    float wt = skyw;\n"
    "    if (hit >= 0.0) {\n"
    "        vec2 s = s0 + ds * hit;\n"
    "        float e = clamp(min(min(s.x, full.x - s.x), min(s.y, full.y - s.y)) / (0.06 * full.y), 0.0, 1.0);\n"
    "        c = c * (1.0 - e) + texture2D(scn, s / full).rgb * e;\n"
    "        wt = wt * (1.0 - e) + e;\n"
    "    }\n"
    "    gl_FragColor = vec4(c / max(wt, 1e-4), min(F * str, 0.9) * clear * wt);\n"
    "}\n";

ssr_t ssr;

static int ssr_init(void)
{
    if (!ao_procs("water reflections"))
        return 0;
    if (!(ssr.p = ao_program("water reflections", ao_vs, ssr_fs))) {
        gmv_log("[ssr] water reflections off\n");
        return 0;
    }
    return 1;
}

/* Size the textures to the screen and check the framebuffers; called inside ao_run's state save. */
int ssr_alloc(int w, int h)
{
    if (!ssr.fbo_scene) {
        glGenTextures(1, &ssr.t_scene);
        glGenTextures(1, &ssr.t_depth);
        msf.GenFramebuffers(1, &ssr.fbo_scene);
        msf.GenFramebuffers(1, &ssr.fbo_depth);
    }
    ssr.w = w;
    ssr.h = h;
    ao_tex(ssr.t_scene, GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE);
    bl_linear();
    msf.BindFramebuffer(MS_FRAMEBUFFER, ssr.fbo_scene);
    aof.FramebufferTexture2D(MS_FRAMEBUFFER, 0x8CE0, GL_TEXTURE_2D, ssr.t_scene, 0);
    GLenum st = msf.CheckFramebufferStatus(MS_FRAMEBUFFER);
    ao_tex(ssr.t_depth, 0x88F0, w, h, 0x84F9, 0x84FA);         /* DEPTH24_STENCIL8, DEPTH_STENCIL, 24_8 */
    glTexParameteri(GL_TEXTURE_2D, 0x884C, GL_NONE);            /* GL_TEXTURE_COMPARE_MODE */
    msf.BindFramebuffer(MS_FRAMEBUFFER, ssr.fbo_depth);
    aof.FramebufferTexture2D(MS_FRAMEBUFFER, 0x821A, GL_TEXTURE_2D, ssr.t_depth, 0);   /* DEPTH_STENCIL_ATTACHMENT */
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    GLenum sd = msf.CheckFramebufferStatus(MS_FRAMEBUFFER);
    if (st == 0x8CD5)
        st = sd;
    if (st != 0x8CD5) {
        gmv_log("[ssr] framebuffer incomplete (0x%x) at %dx%d; water and vehicle reflections off\n", st, w, h);
        return 0;
    }
    return 1;
}

/* The frame as drawn so far (resolved if multisampled) and its depth into ssr.t_scene / ssr.t_depth;
   0 if their framebuffers cannot be had. */
int ssr_grab(int w, int h)
{
    if (ssr.bad || ((w != ssr.w || h != ssr.h) && !ssr_alloc(w, h))) {
        ssr.bad = 1;
        return 0;
    }
    msf.BindFramebuffer(MS_READ_FB, g_ms_fbo);
    msf.BindFramebuffer(MS_DRAW_FB, ssr.fbo_scene);
    msf.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    msf.BindFramebuffer(MS_DRAW_FB, ssr.fbo_depth);
    msf.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    return 1;
}

/* The fog of a layer (its GL fog mode, start, end) as the shaders' fogp: start, 1 / (end - start),
   mode (0 none, 1 linear, 2 the hybrid), a.  The hybrid's density is fixed for points at height 0 seen
   from the eye's height. */
void ssr_fog(const GLfloat *fogs, GLfloat *fogp)
{
    fogp[0] = fogp[1] = fogp[2] = fogp[3] = 0;
    if (fogs[0] == 0x2601 && fogs[2] > fogs[1]) {               /* GL_LINEAR */
        float a0 = g_fog_height > 0 ? fmaxf(g_cam_z / g_fog_height, 0) : 0;
        float rho = a0 < 1e-3f ? expf(-0.5f * a0) : (1 - expf(-a0)) / a0;
        fogp[0] = fogs[1];
        fogp[1] = 1.0f / (fogs[2] - fogs[1]);
        fogp[2] = g_fog_mode == 2 && g_fog_all ? 2 : 1;
        fogp[3] = FOG_HYBRID_K * rho;
    }
}

/* Inside ao_run's state save (depth test, blend, stencil test, scissor and the rest off; unit 0 active),
   on the frame's sea marks; li is the layer most draws had. */
void ssr_pass(int w, int h, int li)
{
    static int ready;
    if (!ready)
        ready = ssr_init() ? 1 : -1;
    if (ready < 0) {
        g_ssr = 0;
        return;
    }
    const GLfloat *P = g_ao_layer[li].m, *dr = g_ao_layer[li].dr, *fogs = g_ao_layer[li].fog, *V = sea_mk.view;
    const GLint *vp = g_ao_layer[li].vp;
    GLfloat invp[16], vinv[16];
    float ux = V[8], uy = V[9], uz = V[10], ul = sqrtf(ux * ux + uy * uy + uz * uz);
    if (!ao_invert(P, invp) || !ao_invert(V, vinv) || vp[2] <= 0 || vp[3] <= 0 || !(dr[1] > dr[0]) || !(ul > 0))
        return;
    ux /= ul, uy /= ul, uz /= ul;
    float c = ux * V[12] + uy * V[13] + uz * V[14];             /* up . the eye-space point of the world's origin */
    if (!(c < 0))
        return;                                                 /* the eye is not above the sea */
    float nearp = fabsf(P[14] / (P[10] - 1)), farp = fabsf(P[14] / (P[10] + 1));
    GLfloat fogp[4];
    ssr_fog(fogs, fogp);
    int skyk = g_fog_all && sky.n > 0;
    if (!ssr_grab(w, h)) {
        g_ssr = 0;
        return;
    }

    GLuint p = ssr.p;
    aof.UseProgram(p);
    aof.Uniform1i(aof.GetUniformLocation(p, "scn"), 0);
    aof.Uniform1i(aof.GetUniformLocation(p, "dep"), 1);
    aof.Uniform2f(aof.GetUniformLocation(p, "full"), (GLfloat)w, (GLfloat)h);
    aof.Uniform2f(aof.GetUniformLocation(p, "dr"), dr[0], dr[1]);
    aof.Uniform4f(aof.GetUniformLocation(p, "vp"), (GLfloat)vp[0], (GLfloat)vp[1], (GLfloat)vp[2], (GLfloat)vp[3]);
    aof.Uniform4f(aof.GetUniformLocation(p, "plane"), ux, uy, uz, c);
    aof.Uniform4f(aof.GetUniformLocation(p, "fogp"), fogp[0], fogp[1], fogp[2], fogp[3]);
    aof.Uniform4f(aof.GetUniformLocation(p, "skyc"), skyk ? sky.hz[0] : 0, skyk ? sky.hz[1] : 0, skyk ? sky.hz[2] : 0,
                  skyk ? 1.0f : 0.0f);
    aof.UniformMatrix4fv(aof.GetUniformLocation(p, "proj"), 1, GL_FALSE, P);
    aof.UniformMatrix4fv(aof.GetUniformLocation(p, "invp"), 1, GL_FALSE, invp);
    aof.UniformMatrix4fv(aof.GetUniformLocation(p, "vinv"), 1, GL_FALSE, vinv);
    aof.UniformMatrix3fv(aof.GetUniformLocation(p, "rot"), 1, GL_FALSE,
                         (const GLfloat[]){V[0], V[1], V[2], V[4], V[5], V[6], V[8], V[9], V[10]});
    aof.Uniform1f(aof.GetUniformLocation(p, "t"), (GLfloat)(GetTickCount() % 600000u) / 1000.0f);
    aof.Uniform1f(aof.GetUniformLocation(p, "str"), g_ssr_strength);
    aof.Uniform1f(aof.GetUniformLocation(p, "wmin"), nearp * 1.5f);
    aof.Uniform1f(aof.GetUniformLocation(p, "reach"), farp * 2);
    msf.BindFramebuffer(MS_FRAMEBUFFER, g_ms_fbo);             /* the screen */
    glViewport(0, 0, w, h);
    aof.ActiveTexture(0x84C1);
    glBindTexture(GL_TEXTURE_2D, ssr.t_depth);
    aof.ActiveTexture(0x84C0);
    glBindTexture(GL_TEXTURE_2D, ssr.t_scene);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_EQUAL, SSR_BIT, SSR_BIT);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glStencilMask(0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    aof.BlendEquation(0x8006);                                  /* GL_FUNC_ADD */
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    ao_tri();
    glDisable(GL_STENCIL_TEST);                                 /* the marks are spent */
    glStencilMask(SSR_BIT);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glDisable(GL_BLEND);

    static unsigned runs;
    if (runs++ % 1200 == 0)
        gmv_trace("[ssr] pass %u at %dx%d: %u chunks of sea, strength %g, eye %.0f m above it, fog %s, sky colour %s\n",
                runs, w, h, sea_mk.chunks, g_ssr_strength, -c / 100,
                fogp[2] == 2 ? "hybrid" : fogp[2] == 1 ? "linear" : "none", skyk ? "known" : "unknown");
}
