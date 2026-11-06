/* vehicle_reflections.c -- VEHICLE_REFLECTIONS */
#include <math.h>
#include <string.h>
#include "compat.h"
#include <GL/gl.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "render.h"

/* ---- VEHICLE_REFLECTIONS: an enhancement, on by default, off under ORIGINAL_MAC_EXPERIENCE ---------------
   Screen-space reflections on the shiny cars, in the same place as the sea's and just after them.  The
   game's own shine on a car is a fixed 256² picture of a sky added onto the paint, looked up by which way
   the surface faces (unit 1 COMBINE ADD, its coordinates the normals themselves), so a car never mirrors
   the street; the cars without that pass (vans and the like) are matte, and stay so.
   - A body is a draw to the screen with that pass (and the env picture wider than 64: the characters'
     is 64²), opaque, writing depth, with normals.  Right after it, it is drawn again by car_mark into
     a screen-sized target of its own (normal in eye space, 10 bits a channel, and a depth of its own),
     with the game's matrices, viewport, depth range and culling, so it lands on the same pixels.
   - The pass reads that target where a body was drawn and something drawn later is not in front (the
     frame's depth is compared), marches the reflected ray across the screen as the sea's pass does
     (from just off the surface, so the car does not find itself where it starts), and where the ray
     hits something mixes it in by Fresnel's law (Schlick, 8% head-on: a real clear coat's 4% left the
     middle of a bonnet or trunk looking bare at the user's look; more than this reads as plastic), times
     VEHICLE_REFLECTION_STRENGTH, fading towards the screen's edges and with the fog.  A body within 6 m
     of where the ray started is the same car and is marched through: seen from behind, a trunk's rays
     meet the rear window, whose dark glass (it cannot show what the glass itself reflects) left the trunk
     looking bare.
   - Where the ray hits nothing (since 2026-11-06), the screen itself serves as a sphere map, looked up by
     the reflected direction: uv = 0.5 + 0.5 * its x and y in eye space, softened by a 5-tap blur, and mixed
     in by the same Fresnel.  Before, the game's sky picture stayed alone there, and the roofs and trunks,
     whose rays rise off the top of the screen, looked bare (the user's complaint of 2026-10-28, an SSR
     limit, not a bug).  A trick some car games of the time used with the frame before; here the frame
     itself is at hand, as the pass runs after the scene.  Not a mirror: a trunk can show what is in front
     of the camera, but the colours are the scene's and move with it.  Compared on a Laufer at the user's
     request with the sphere map alone and the march alone; the user chose this mix.
   - Paint and glass are one draw with no mask (the paint texture's alpha is 255 throughout), so they
     reflect alike. */
float g_vr_strength = 2.0f;                      /* VEHICLE_REFLECTION_STRENGTH (blank = 2, the user's choice) */
#if GMV_DEV
int g_vr_debug;                                  /* VEHICLE_REFLECTION_DEBUG (diagnostic) */
#endif

static struct {
    unsigned frame;                                     /* g_frame_no + 1 of the last frame a body was marked in */
    unsigned bodies;                                    /* marked in that frame */
    int ready;                                          /* 1 = the programs are built, -1 = they failed */
    GLuint p_mark, p_pass, fbo, t_n, t_d;               /* the marking draw's target: normals and their depth */
    int w, h;
} vr;

int vr_frame(void)
{
    return vr.frame == g_frame_no + 1;
}

/* The body again, with its normal in eye space (the game's vertices are already in world space, so the
   modelview is the view). */
static const char vr_vs_mark[] =
    "#version 120\n"
    "varying vec3 n, e;\n"
    "void main() {\n"
    "    n = gl_NormalMatrix * gl_Normal;\n"
    "    e = (gl_ModelViewMatrix * gl_Vertex).xyz;\n"
    "    gl_Position = ftransform();\n"
    "}\n";

/* A guard against a model whose normals point into the car (it would fall to the rim fade; none has been
   seen: the debug view showed no such pixel).  Back faces are culled, so the triangle drawn faces the eye:
   a normal against the triangle's own facing (from the screen's derivatives of its position) is turned
   round, and the alpha says so (2/3). */
static const char vr_fs_mark[] =
    "#version 120\n"
    "varying vec3 n, e;\n"
    "void main() {\n"
    "    vec3 m = normalize(n), g = cross(dFdx(e), dFdy(e));\n"
    "    if (dot(g, e) > 0.0) g = -g;\n"
    "    float a = 1.0;\n"
    "    if (dot(m, g) < 0.0) { m = -m; a = 0.667; }\n"
    "    gl_FragColor = vec4(m * 0.5 + 0.5, a);\n"
    "}\n";

static const char vr_fs[] =
    "#version 120\n"
    "uniform sampler2D scn, dep, nrm, cdep;\n"
    "uniform vec2 full, dr;\n"
    "uniform vec4 vp, fogp;\n"                          /* fogp: start, 1 / (end - start), mode, a */
    "uniform mat4 proj, invp;\n"
    "uniform float str, wmin, reach, dbg;\n"
    "float dat(vec2 px) { return texture2D(dep, px / full).r; }\n"
    /* where the ray finds nothing: the screen as a sphere map, by the reflected direction; b = blur in screens */
    "vec3 sphere_scr(vec3 r, float b) {\n"
    "    vec2 u = 0.5 + 0.5 * normalize(r).xy;\n"
    "    vec2 o = vec2(b * vp.w / vp.z, b);\n"
    "    vec3 c = texture2D(scn, (vp.xy + u * vp.zw) / full).rgb * 0.4;\n"
    "    c += texture2D(scn, (vp.xy + clamp(u + vec2(o.x, 0.0), 0.0, 1.0) * vp.zw) / full).rgb * 0.15;\n"
    "    c += texture2D(scn, (vp.xy + clamp(u - vec2(o.x, 0.0), 0.0, 1.0) * vp.zw) / full).rgb * 0.15;\n"
    "    c += texture2D(scn, (vp.xy + clamp(u + vec2(0.0, o.y), 0.0, 1.0) * vp.zw) / full).rgb * 0.15;\n"
    "    c += texture2D(scn, (vp.xy + clamp(u - vec2(0.0, o.y), 0.0, 1.0) * vp.zw) / full).rgb * 0.15;\n"
    "    return c;\n"
    "}\n"
    "vec3 unproj(vec2 px, float z) {\n"
    "    vec4 p = invp * vec4((px - vp.xy) / vp.zw * 2.0 - 1.0, z, 1.0);\n"
    "    return p.xyz / p.w;\n"
    "}\n"
    "float wof(vec3 p) { return proj[0][3] * p.x + proj[1][3] * p.y + proj[2][3] * p.z + proj[3][3]; }\n"
    "vec2 screen(vec4 h) { return (h.xy / h.w * 0.5 + 0.5) * vp.zw + vp.xy; }\n"
    "float scene_w(vec2 px) {\n"
    "    float d = dat(px);\n"
    "    if (d < dr.x || d >= dr.y - 1e-5) return 1e30;\n"
    "    return wof(unproj(px, (d - dr.x) / (dr.y - dr.x) * 2.0 - 1.0));\n"
    "}\n"
    /* VEHICLE_REFLECTION_DEBUG: a body's pixel tinted by what became of it, in place of being left out */
    "void drop(vec3 c) {\n"
    "    if (dbg < 0.5) discard;\n"
    "    gl_FragColor = vec4(c, 0.6);\n"
    "}\n"
    /* the frame shows, at px, a body within 6 m of pc: the car the ray started from */
    "bool self_at(vec2 px, float sw, vec3 pc) {\n"
    "    if (texture2D(nrm, px / full).a < 0.5) return false;\n"
    "    vec3 q = unproj(px, (texture2D(cdep, px / full).r - dr.x) / (dr.y - dr.x) * 2.0 - 1.0);\n"
    "    return abs(wof(q) - sw) < 0.005 * sw + 5.0 && distance(q, pc) < 600.0;\n"
    "}\n"
    "void main() {\n"
    "    vec2 px = gl_FragCoord.xy;\n"
    "    vec4 nd = texture2D(nrm, px / full);\n"
    "    if (nd.a < 0.5) discard;\n"
    "    float cd = texture2D(cdep, px / full).r;\n"
    "    if (cd < dr.x || cd >= dr.y - 1e-5) discard;\n"
    "    vec3 pc = unproj(px, (cd - dr.x) / (dr.y - dr.x) * 2.0 - 1.0);\n"
    "    float wc = wof(pc);\n"
    "    if (scene_w(px) < wc * 0.995 - 5.0) {\n"          /* something drawn after the body is in front */
    "        drop(vec3(1.0, 0.0, 0.0));\n"
    "        return;\n"
    "    }\n"
    "    float clear = 1.0;\n"
    "    if (fogp.z == 2.0) {\n"
    "        float s = clamp((length(pc) - fogp.x) * fogp.y, 0.0, 1.0), s8 = s * s;\n"
    "        s8 *= s8; s8 *= s8;\n"
    "        clear = (fogp.w > 1e-3 ? (exp(-fogp.w * s * s) - exp(-fogp.w)) / (1.0 - exp(-fogp.w)) : 1.0 - s * s) * (1.0 - s8);\n"
    "    } else if (fogp.z == 1.0)\n"
    "        clear = 1.0 - clamp((wc - fogp.x) * fogp.y, 0.0, 1.0);\n"
    "    if (clear <= 0.0) {\n"
    "        drop(vec3(0.5));\n"
    "        return;\n"
    "    }\n"
    "    vec3 v = normalize(pc);\n"
    "    vec3 n = normalize(nd.xyz * 2.0 - 1.0);\n"
    "    float vn = dot(v, n);\n"
    "    float rim = smoothstep(0.03, 0.25, -vn);\n"      /* fades out at the outline, where the normals graze */
    "    if (rim <= 0.0) {\n"
    "        drop(vec3(0.0, 0.0, 1.0));\n"
    "        return;\n"
    "    }\n"
    "    if (vn > -0.05) n = normalize(n - v * (vn + 0.05));\n"   /* smoothed normals turned away at the outline */
    "    vec3 r = reflect(v, n);\n"
    "    float F = (0.08 + 0.92 * pow(1.0 - clamp(dot(-v, n), 0.0, 1.0), 5.0)) * rim;\n"
    /* the march, as the sea's, from just off the surface */
    "    vec3 p0 = pc + n * (0.004 * wc + 2.0);\n"
    "    float w0 = wof(p0);\n"
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
    "    bool past = false, through = false;\n"           /* the last step, any step, was behind the car itself */
    "    for (int i = 1; i <= 48; i++) {\n"
    "        float a = amax * pow((float(i) - noise) / 48.0, 1.6);\n"
    "        float rw = 1.0 / mix(k0, k1, a), sw = scene_w(s0 + ds * a);\n"
    "        if (rw > sw && rw - sw < max(0.03 * sw + 50.0, 2.0 * abs(rw - rwp))) {\n"
    /* a car does not mirror itself (the trunk would show the dark rear window): the ray goes on past it */
    "            if (self_at(s0 + ds * a, sw, pc)) {\n"
    "                ap = a;\n"
    "                rwp = rw;\n"
    "                past = through = true;\n"
    "                continue;\n"
    "            }\n"
    "            float lo = ap;\n"
    "            hit = a;\n"
    "            if (!past) for (int j = 0; j < 5; j++) {\n"
    "                float m = 0.5 * (lo + hit);\n"
    "                if (1.0 / mix(k0, k1, m) > scene_w(s0 + ds * m)) hit = m; else lo = m;\n"
    "            }\n"
    "            break;\n"
    "        }\n"
    "        ap = a;\n"
    "        rwp = rw;\n"
    "        past = false;\n"
    "    }\n"
    "    if (dbg > 0.5) {\n"                              /* magenta: the normal was turned round */
    "        drop(nd.a < 0.84 ? vec3(1.0, 0.0, 1.0) : hit < 0.0 ? vec3(1.0, 1.0, 0.0) : through ? vec3(0.0, 1.0, 1.0) : vec3(0.0, 1.0, 0.0));\n"
    "        return;\n"
    "    }\n"
    "    if (hit < 0.0) {\n"                              /* nothing hit: the screen as a sphere map */
    "        gl_FragColor = vec4(sphere_scr(r, 0.015), min(F * str, 0.85) * clear);\n"
    "        return;\n"
    "    }\n"
    "    vec2 s = s0 + ds * hit;\n"
    "    float e = clamp(min(min(s.x, full.x - s.x), min(s.y, full.y - s.y)) / (0.06 * full.y), 0.0, 1.0);\n"
    "    gl_FragColor = vec4(texture2D(scn, s / full).rgb, min(F * str, 0.85) * clear * e);\n"
    "}\n";

static int vr_init(void)
{
    if (!ao_procs("vehicle reflections"))
        return 0;
    vr.p_mark = ao_program("vehicle reflections' marking", vr_vs_mark, vr_fs_mark);
    vr.p_pass = vr.p_mark ? ao_program("vehicle reflections", ao_vs, vr_fs) : 0;
    if (!vr.p_pass) {
        gmv_log("[vr] vehicle reflections off\n");
        return 0;
    }
    glGenTextures(1, &vr.t_n);
    glGenTextures(1, &vr.t_d);
    msf.GenFramebuffers(1, &vr.fbo);
    return 1;
}

/* Size the marking target to the screen and check it; leaves it bound. */
static int vr_alloc(int w, int h)
{
    vr.w = w;
    vr.h = h;
    ao_tex(vr.t_n, 0x8059, w, h, GL_RGBA, GL_UNSIGNED_BYTE);   /* RGB10_A2 */
    ao_tex(vr.t_d, 0x81A6, w, h, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT);   /* DEPTH_COMPONENT24 */
    glTexParameteri(GL_TEXTURE_2D, 0x884C, GL_NONE);            /* GL_TEXTURE_COMPARE_MODE */
    msf.BindFramebuffer(MS_FRAMEBUFFER, vr.fbo);
    aof.FramebufferTexture2D(MS_FRAMEBUFFER, 0x8CE0, GL_TEXTURE_2D, vr.t_n, 0);
    aof.FramebufferTexture2D(MS_FRAMEBUFFER, 0x8D00, GL_TEXTURE_2D, vr.t_d, 0);   /* GL_DEPTH_ATTACHMENT */
    GLenum st = msf.CheckFramebufferStatus(MS_FRAMEBUFFER);
    if (st != 0x8CD5) {
        gmv_log("[vr] framebuffer incomplete (0x%x) at %dx%d; vehicle reflections off\n", st, w, h);
        return 0;
    }
    return 1;
}

/* Before each glDrawElements to the screen (the bodies come that way), in the game's state: this draw has
   the sky shine pass, so it is a shiny car's body (1) or, with the small env picture, a character (2). */
int shine_body(GLenum mode, GLsizei count)
{
    if (g_glt.last_fb || g_sky_draw || mode < GL_TRIANGLES || count < 3 || g_blend_on || !g_ao_dmask || !g_ao_depth_on)
        return 0;
    unsigned on = g_tf_tex2d & (g_texfix ? g_tf_tcarr : 0xF) & 0xF;
    if ((on & 3) != 3 || g_te[1].mode != 0x8570 || g_te[1].crgb != GL_ADD || !glIsEnabled(GL_NORMAL_ARRAY))
        return 0;
    void(APIENTRY * act)(GLenum) = tf_active_real();
    if (!act)
        return 0;
    GLint tw1 = 0;
    act(0x84C1);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw1);
    act(0x84C0 + g_tf_active);
    ao_proj_refresh();
    return !g_ao_persp ? 0 : tw1 <= 64 ? 2 : 1;
}

/* A body is drawn again into the marking target. */
void car_mark(GLenum mode, GLsizei count, GLenum type, const void *idx)
{
    static void(APIENTRY * de)(GLenum, GLsizei, GLenum, const void *);
    int w = g_ms_w, h = g_ms_h;
    if (!g_ms_fbo)
        gmv_gl_client_size(&w, &h);
    if (w <= 0 || h <= 0)
        return;
    if (!vr.ready)
        vr.ready = vr_init() ? 1 : -1;
    if (!de)
        de = gl_real("glDrawElements");
    if (vr.ready < 0 || !de) {
        g_vr = 0;
        return;
    }
    GLint dfb = 0, rfb = 0, prog = 0;
    glGetIntegerv(MS_DRAW_FB_BINDING, &dfb);
    glGetIntegerv(MS_READ_FB_BINDING, &rfb);
    glGetIntegerv(0x8B8D, &prog);                       /* GL_CURRENT_PROGRAM */
    if (w != vr.w || h != vr.h) {
        glPushAttrib(GL_TEXTURE_BIT);
        int ok = vr_alloc(w, h);
        glPopAttrib();
        if (!ok) {
            g_vr = 0;
            msf.BindFramebuffer(MS_READ_FB, (GLuint)rfb);
            msf.BindFramebuffer(MS_DRAW_FB, (GLuint)dfb);
            return;
        }
    }
    glPushAttrib(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_ENABLE_BIT);
    msf.BindFramebuffer(MS_FRAMEBUFFER, vr.fbo);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    if (!vr_frame()) {
        vr.frame = g_frame_no + 1;
        vr.bodies = 0;
        GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);    /* the game's scissor stays for the draw */
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 0);
        glClearDepth(1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (sc)
            glEnable(GL_SCISSOR_TEST);
    }
    vr.bodies++;
    glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_STENCIL_TEST);
    glDepthFunc(GL_LEQUAL);
    aof.UseProgram(vr.p_mark);
    de(mode, count, type, idx);
    aof.UseProgram((GLuint)prog);
    glPopAttrib();
    msf.BindFramebuffer(MS_READ_FB, (GLuint)rfb);
    msf.BindFramebuffer(MS_DRAW_FB, (GLuint)dfb);
}

/* Inside ao_run's state save, after the sea's pass, on the frame's marked bodies; li is the layer most
   draws had. */
void vr_pass(int w, int h, int li)
{
    const GLfloat *P = g_ao_layer[li].m, *dr = g_ao_layer[li].dr, *fogs = g_ao_layer[li].fog;
    const GLint *vp = g_ao_layer[li].vp;
    GLfloat invp[16], fogp[4];
    if (vr.w != w || vr.h != h || !ao_invert(P, invp) || vp[2] <= 0 || vp[3] <= 0 || !(dr[1] > dr[0]))
        return;
    if (!ssr_grab(w, h)) {
        g_vr = 0;
        return;
    }
    float nearp = fabsf(P[14] / (P[10] - 1)), farp = fabsf(P[14] / (P[10] + 1));
    ssr_fog(fogs, fogp);

    GLuint p = vr.p_pass;
    aof.UseProgram(p);
    aof.Uniform1i(aof.GetUniformLocation(p, "scn"), 0);
    aof.Uniform1i(aof.GetUniformLocation(p, "dep"), 1);
    aof.Uniform1i(aof.GetUniformLocation(p, "nrm"), 2);
    aof.Uniform1i(aof.GetUniformLocation(p, "cdep"), 3);
    aof.Uniform2f(aof.GetUniformLocation(p, "full"), (GLfloat)w, (GLfloat)h);
    aof.Uniform2f(aof.GetUniformLocation(p, "dr"), dr[0], dr[1]);
    aof.Uniform4f(aof.GetUniformLocation(p, "vp"), (GLfloat)vp[0], (GLfloat)vp[1], (GLfloat)vp[2], (GLfloat)vp[3]);
    aof.Uniform4f(aof.GetUniformLocation(p, "fogp"), fogp[0], fogp[1], fogp[2], fogp[3]);
    aof.UniformMatrix4fv(aof.GetUniformLocation(p, "proj"), 1, GL_FALSE, P);
    aof.UniformMatrix4fv(aof.GetUniformLocation(p, "invp"), 1, GL_FALSE, invp);
    aof.Uniform1f(aof.GetUniformLocation(p, "str"), g_vr_strength);
    aof.Uniform1f(aof.GetUniformLocation(p, "dbg"), (GLfloat)g_vr_debug);
    aof.Uniform1f(aof.GetUniformLocation(p, "wmin"), nearp * 1.5f);
    aof.Uniform1f(aof.GetUniformLocation(p, "reach"), farp * 2);
    msf.BindFramebuffer(MS_FRAMEBUFFER, g_ms_fbo);             /* the screen */
    glViewport(0, 0, w, h);
    aof.ActiveTexture(0x84C3);
    glBindTexture(GL_TEXTURE_2D, vr.t_d);
    aof.ActiveTexture(0x84C2);
    glBindTexture(GL_TEXTURE_2D, vr.t_n);
    aof.ActiveTexture(0x84C1);
    glBindTexture(GL_TEXTURE_2D, ssr.t_depth);
    aof.ActiveTexture(0x84C0);
    glBindTexture(GL_TEXTURE_2D, ssr.t_scene);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    aof.BlendEquation(0x8006);                                  /* GL_FUNC_ADD */
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    ao_tri();
    glDisable(GL_BLEND);

    static unsigned runs;
    if (runs++ % 1200 == 0)
        gmv_trace("[vr] pass %u at %dx%d: %u car bodies, strength %g, fog %s\n", runs, w, h, vr.bodies, g_vr_strength,
                fogp[2] == 2 ? "hybrid" : fogp[2] == 1 ? "linear" : "none");
}
