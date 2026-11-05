/* sun_moon_hooks.c -- SUN_MOON: the game side of the sun and moon */
#include "compat.h"
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* ---- SUN_MOON: the iPhone version's sun and moon (an enhancement, off by default and under ORIGINAL_MAC_EXPERIENCE) ----

   The iPhone build (Gangstar2, the same engine) draws a sun and a moon in the sky, and the Mac
   build kept most of it.  sun.bdae moves the anchors *0x949a60 (sun) and **0x63e4cc (moon) across
   the sky.  The time-of-day update 0x142892 places the glow billboards *0x949a68 (sun) and
   *0x949a6c (moon) towards them through 0x142690.  The skybox render 0xbe468 still draws two disc
   billboards from CustomSkyBoxSceneNode [+0x118] (sun) and [+0x11c] (moon): each frame it puts a
   disc at the camera plus the disc's offset (float[3] at +0x1dc, written by 0x142690), shows it
   only while that offset is above -50, draws it with the sky and hides it again.  The skybox
   destructor 0xbf15c removes and releases both.  What the Mac build lost:
   1. The skybox constructor 0xbf552 leaves both disc pointers 0.  The iPhone one (0x2bf4d8)
      makes each disc when the tweak "draw skybox sun" (0x64110f here) or "draw skybox moon"
      (0x641110) is set: a 1024x1024 CBillboardSceneNode with material 8 of ./add-sun.tga or
      ./add-moon.tga, hidden, render order 100, added to the scene root.
   2. 0x142892 hides both glows every tick.  The iPhone then shows the sun glow while the time
      state [tod+4] is not 3 (night) and the moon glow while it is, on devices whose performance
      profile allows it (SGX, not MBX).
   The four textures are still in the Mac pack.

   The iPhone disc is a CBillboardSceneNode subclass with its base size at +0x1d4 and the offset
   at +0x1dc.  The Mac has no such class, and in its plain CBillboardSceneNode (0x469c10, 0x1dc
   bytes) +0x1d4 is the virtual base, where 0x142690 would read the disc's base size for setSize.
   So:
   - The call of 0x142892 at 0x1420d3 goes to gmv_sun_moon_tod.  Once per skybox, as soon as the
     skybox sits under a parent that has a scene manager, it makes the discs as the iPhone does,
     from 0x469c10 in the iPhone's 0x1f0 bytes so the offset at +0x1dc lies past the object, and
     adds them to that parent, the root the skybox itself is placed in.  After 0x142892 it shows
     the glow for the time of day.  It also keeps the game's clock (g_tod_ms) for BLOOM_WINDOWS,
     which takes this one call without SUN_MOON.
   - Its two calls of 0x142690 (0x143520 moon, 0x143552 sun) go to gmv_sun_moon_track, which makes
     the call without the disc, so the glow is placed as before, and then
     does the disc's part with the base size 1024: the offset, the position and setSize.
   GMV_SUN_MOON turns it on: 1 = ENHANCED, 2 = IPHONE.  With the options tab the hooks are in from the
   start and SUN AND MOON switches g_sm_on: off, the discs are kept below the horizon, where the skybox
   render hides them, and the glows are left to the game, which hides them every tick, so the sky is the
   Mac's.  The two differ only in the glow's draw: ENHANCED hides it behind the world and brightens it
   (the GL shim), IPHONE draws the material as it is, which has no depth test on either version
   (DefaultEffects.bdae holds the same bytes), so the glow shines through buildings. */
#define SM_SUN_GLOW   0x00949a68u
#define SM_MOON_GLOW  0x00949a6cu
#define SM_DRAW_SUN   0x0064110fu   /* tweak bytes "draw skybox sun" / "draw skybox moon" */
#define SM_DRAW_MOON  0x00641110u
#define SM_DISC_BYTES 0x1f0u
#define SM_DISC_OFS   0x1dc
#define SM_DISC_SIZE  1024.0f

const uint32_t sm_track_sites[] = { 0x00143520u, 0x00143552u };
static unsigned char *g_sm_failed;  /* skybox whose discs could not be made */
int g_sm_on;                 /* SUN_MOON not 0 (the time-of-day call is also taken for BLOOM_WINDOWS) */
int g_sm_hooked;             /* the time-of-day and track calls go to the hooks below */
int g_tod_ms = -1;                  /* the game's clock, ms into the day (int64 at tod+8); -1 = not read yet */
float g_sun_dir[3];                 /* towards the sun, from its anchor, for DYNAMIC_SHADOWS; 0,0,0 until it is known */
float g_moon_dir[3];                /* the same for the moon */
float g_cam_world[3];               /* the camera's absolute position, at the last tick, for the same */

typedef void (*sm_vfn_p)(void *, const void *);

void **sm_vt(void *obj)
{
    return *(void ***)obj;
}

/* The material maker 0x1aba04(&mat, path, 8) returns its refcounted material by value: the
   first argument is the hidden struct-return pointer, which the callee pops (ret $4, the
   macOS i386 rule; the game's own call at 0x1abc3d re-subtracts 4 after it).  A plain C call
   leaves the stack 4 bytes off, so the call is made here by hand. */
static int *sm_material(const char *tex)
{
    int *mat = NULL;
    __asm__ volatile(
        "subl $16, %%esp\n\t"
        "movl %0, (%%esp)\n\t"
        "movl %1, 4(%%esp)\n\t"
        "movl $8, 8(%%esp)\n\t"
        "movl $0x1aba04, %%eax\n\t"
        "call *%%eax\n\t"
        "addl $12, %%esp"           /* the callee popped the 4-byte result pointer */
        :
        : "r"(&mat), "r"(tex)
        : "eax", "ecx", "edx", "memory", "cc");
    return mat;
}

/* The iPhone's disc maker 0x2bf3ac, with the Mac's own calls: material 0x1aba04, billboard
   0x469c10, setMaterial 0x469bb0, as the glow maker 0x1abad0 uses them. */
static unsigned char *sm_make_disc(void *driver, void *parent, const char *tex)
{
    int *mat = sm_material(tex);
    if (!mat) {
        gmv_log("[sunmoon] no material for %s; that disc is left out\n", tex);
        return NULL;
    }
    unsigned char *n = ((unsigned char *(*)(uint32_t, const char *, int, int))(uintptr_t)0x904e0u)(
        SM_DISC_BYTES, "the patches", __LINE__, 1);
    float pos[3] = {0, 0, 0}, size[2] = {SM_DISC_SIZE, SM_DISC_SIZE};
    ((void (*)(void *, void *, void *, const float *, const float *, int, int))(uintptr_t)0x469c10u)(
        n, driver, NULL, pos, size, -1, -1);
    float *ofs = (float *)(n + SM_DISC_OFS);
    ofs[0] = ofs[1] = 0;
    ofs[2] = -1000;             /* below the horizon until gmv_sun_moon_track places it */
    ((void (*)(void *, int **))(uintptr_t)0x469bb0u)(n, &mat);
    if (--*mat == 0) {
        ((void (*)(int *))(uintptr_t)0x4da6d0u)(mat);
        ((void (*)(void *))(uintptr_t)0x90450u)(mat);
    }
    void *sn = n + 4;
    ((sm_vfn_i)sm_vt(sn)[0x48 / 4])(sn, 0);         /* setVisible: the skybox render shows it */
    ((sm_vfn_i)sm_vt(sn)[0xd4 / 4])(sn, 100);
    ((sm_vfn_p)sm_vt(parent)[0x5c / 4])(parent, sn); /* addChild */
    return n;
}

static void sm_glow_show(uint32_t pp, int on)
{
    unsigned char *g = *(unsigned char **)(uintptr_t)pp;
    if (g)
        ((sm_vfn_i)sm_vt(g + 4)[0x48 / 4])(g + 4, on);
}

__attribute__((cdecl, force_align_arg_pointer))
void gmv_sun_moon_tod(unsigned char *tod)
{
    unsigned char *sky = *(unsigned char **)(uintptr_t)SM_SKYBOX_PP;
    unsigned char *parent = sky ? *(unsigned char **)(sky + 0xd8) : NULL;
    unsigned char **sun = sky ? (unsigned char **)(sky + 0x118) : NULL, **moon = sky ? sun + 1 : NULL;
    if (g_sm_on && parent && sky != g_sm_failed && !*sun && !*moon && *(void **)(parent + 0xfc)) {
        void *driver = ((void *(*)(void))(uintptr_t)0x31d0u)();
        if (*(const unsigned char *)(uintptr_t)SM_DRAW_SUN)
            *sun = sm_make_disc(driver, parent, "./add-sun.tga");
        if (*(const unsigned char *)(uintptr_t)SM_DRAW_MOON)
            *moon = sm_make_disc(driver, parent, "./add-moon.tga");
        if (!*sun && !*moon)
            g_sm_failed = sky;
        gmv_trace("[sunmoon] skybox %p: sun disc %p, moon disc %p, under %p (its parent %p, scene manager %p)\n",
                (void *)sky, (void *)*sun, (void *)*moon, (void *)parent, *(void **)(parent + 0xd8),
                *(void **)(parent + 0xfc));
    }
    ((void (*)(void *))(uintptr_t)SM_TOD_FN)(tod);
    long long ms = *(long long *)(tod + 8) % 86400000;
    g_tod_ms = (int)(ms < 0 ? ms + 86400000 : ms);
    if (!g_sm_on)
        return;
    int night = *(int *)(tod + 4) == 3;
    sm_glow_show(SM_SUN_GLOW, !night);
    sm_glow_show(SM_MOON_GLOW, night);
}

/* 0x142690 without its disc part, then that part with the base size 1024. */
__attribute__((cdecl, force_align_arg_pointer))
void gmv_sun_moon_track(unsigned char *smgr, unsigned char *disc, unsigned char *anchor, void *glow)
{
    ((void (*)(void *, void *, void *, void *))(uintptr_t)SM_TRACK_FN)(smgr, NULL, anchor, glow);
    unsigned char *dummy = anchor ? *(unsigned char **)(anchor + 0xd8) : NULL;
    unsigned char *cam = *(unsigned char **)(smgr + 0xe4);
    if (dummy && glow && glow == *(void **)(uintptr_t)SM_SUN_GLOW)
        memcpy(g_sun_dir, dummy + 0x40, sizeof g_sun_dir);      /* absolute position; the shadows' pass normalises it */
    if (dummy && glow && glow == *(void **)(uintptr_t)SM_MOON_GLOW)
        memcpy(g_moon_dir, dummy + 0x40, sizeof g_moon_dir);
    if (cam)
        memcpy(g_cam_world, cam + 0x40, sizeof g_cam_world);
    if (disc && !g_sm_on) {
        ((float *)(disc + SM_DISC_OFS))[2] = -1000;            /* below the horizon: hidden */
        return;
    }
    if (!disc || !dummy || !cam)
        return;
    float d[3], c[3];
    memcpy(d, dummy + 0x40, sizeof d);                          /* absolute position */
    memcpy(c, cam + 0x40, sizeof c);
    float scale = ((const float *(*)(void *))sm_vt(dummy)[0x90 / 4])(dummy)[0];
    float l = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    if (l != 0.0f) {
        l = 1.0f / sqrtf(l);
        d[0] *= l, d[1] *= l, d[2] *= l;
    }
    d[0] *= 1000.0f, d[1] *= 1000.0f, d[2] *= 1000.0f;
    memcpy(disc + SM_DISC_OFS, d, sizeof d);
    float p[3] = {d[0] + c[0], d[1] + c[1], d[2] + c[2]};
    ((sm_vfn_p)sm_vt(disc + 4)[0xa4 / 4])(disc + 4, p);        /* setPosition */
    float size[2] = {SM_DISC_SIZE * scale, SM_DISC_SIZE * scale};
    ((sm_vfn_p)sm_vt(disc)[8 / 4])(disc, size);                 /* setSize */
}

extern int g_sky_glow_draw;

static int sm_is_glow(const unsigned char *n)
{
    return n == *(unsigned char **)(uintptr_t)SM_SUN_GLOW || n == *(unsigned char **)(uintptr_t)SM_MOON_GLOW;
}

/* render(node, draw): with draw 0 it only builds the quad (0x46afdb), so the flag must be passed on.
   SUN_MOON off: no flag, so a glow the game draws before it hides them stays the Mac's draw. */
__attribute__((cdecl, force_align_arg_pointer))
void gmv_sun_moon_render(unsigned char *n, int draw)
{
    g_sky_glow_draw = g_sm_on && sm_is_glow(n);
    ((void (*)(void *, int))(uintptr_t)SM_BB_RENDER)(n, draw);
    g_sky_glow_draw = 0;
}

__attribute__((cdecl, force_align_arg_pointer))
void gmv_sun_moon_render_sn(unsigned char *sn, int draw)
{
    g_sky_glow_draw = g_sm_on && sm_is_glow(sn - 4);
    ((void (*)(void *, int))(uintptr_t)SM_BB_RENDER_THUNK)(sn, draw);
    g_sky_glow_draw = 0;
}

/* The Mac's skybox render draws sky-mesh [+0x54c], farcity-mesh [+0x550] (the distant skyline) and
   only then the discs, which have no depth test, so the moon showed in front of the skyline.  The
   iPhone's (0x2bfef4) draws the discs before the skyline.  The farcity draw at 0xbe9f1 becomes a call
   of gmv_sun_moon_layers, which draws them in the iPhone's order, and then jumps past the game's disc
   draws to 0xbea57. */
#define SM_FARCITY_SITE 0x000be9f1u
#define SM_DISCS_END    0x000bea57u

static const unsigned char sm_farcity_code[] = {   /* movl 0x550(%esi),%eax; testl; je; ... */
    0x8b, 0x86, 0x50, 0x05, 0x00, 0x00, 0x85, 0xc0, 0x74, 0x10, 0x8b, 0x08, 0x89, 0x04, 0x24,
    0xc7, 0x44, 0x24, 0x04, 0x01, 0x00, 0x00, 0x00, 0xff, 0x51, 0x1c,
};

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_sun_moon_layers(unsigned char *sky)
{
    for (int i = 0; i < 2; i++) {
        unsigned char *disc = *(unsigned char **)(sky + 0x118 + 4 * i);
        if (disc && (disc[0x10c] & 1))
            ((sm_vfn_i)sm_vt(disc)[0x20 / 4])(disc, 1);             /* render */
    }
    void *city = *(void **)(sky + 0x550);
    if (city)
        ((sm_vfn_i)sm_vt(city)[0x1c / 4])(city, 1);
}

void sm_discs_behind_farcity(void)
{
    unsigned char code[11] = {0x56, 0xe8, 0, 0, 0, 0, 0x83, 0xc4, 0x04, 0xeb, 0};  /* push %esi; call; add $4,%esp; jmp */
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)gmv_sun_moon_layers - (SM_FARCITY_SITE + 6));
    memcpy(code + 2, &rel, 4);
    code[10] = (unsigned char)(SM_DISCS_END - (SM_FARCITY_SITE + sizeof code));
    if (memcmp((const void *)(uintptr_t)SM_FARCITY_SITE, sm_farcity_code, sizeof sm_farcity_code) != 0)
        gmv_log("[gamepatch] skybox layer draws not the expected bytes; the discs stay in front of the skyline\n");
    else if (patch_bytes(SM_FARCITY_SITE, code, sizeof code))
        gmv_log("[gamepatch] sun and moon discs drawn behind the distant skyline\n");
}

/* 1 if the call at site goes to fn. */
int sm_is_call(uint32_t site, uint32_t fn)
{
    const unsigned char *p = (const unsigned char *)(uintptr_t)site;
    int32_t rel;
    memcpy(&rel, p + 1, 4);
    return p[0] == 0xe8 && site + 5 + (uint32_t)rel == fn;
}

int sm_redirect(uint32_t site, void *hook)
{
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)hook - (site + 5));
    if (patch_bytes(site + 1, (const unsigned char *)&rel, 4))
        return 1;
    gmv_log("[gamepatch] write at 0x%08x failed (%lu); sun and moon partly applied\n", site + 1, GetLastError());
    return 0;
}

/* GAMEPATCH_TAB: the options tab switches settings in game, ORIGINAL_MAC_EXPERIENCE among them, so
   their hooks go in at the start, doing nothing while their settings are the Mac's. */
int tab_live(void)
{
    return gmv_config_tab();
}

/* With the options tab the draw distance, the densities and SPAWN_FADE get their hooks for what it can
   ask for.  Under ORIGINAL_MAC_EXPERIENCE they are out, with the Mac's collision tables and spawn pool:
   its in-game switch puts them in or takes them out (mac_hooks_in, mac_hooks_out). */
int tables_live(void)
{
    return tab_live() && !gmv_config_mac();
}

/* BLOOM_WINDOWS (the GL shim) lights the windows by the game's clock, read in gmv_sun_moon_tod. */
int bloom_windows_on(void)
{
    const char *b = gmv_getenv("GMV_BLOOM"), *w = gmv_getenv("GMV_BLOOM_WINDOWS");   /* BLOOM=2; blank = 1 */
    return (b && atoi(b) >= 2 && !(w && *w && atof(w) <= 0)) || tab_live();
}

/* SUN_MOON: 0 = the Mac's sky, 1 = ENHANCED, 2 = IPHONE. */
static int sm_mode(void)
{
    return !g_sm_on ? 0 : g_sky_glow_iphone ? 2 : 1;
}

void sm_mode_set(int mode)
{
    g_sm_on = mode != 0;
    g_sky_glow_iphone = mode == 2;
}

/* The options menu's SUN AND MOON: the mode, or -1 when the hooks are not in. */
int sun_moon_set(int mode)
{
    if (!g_sm_hooked)
        return -1;
    if (sm_mode() != mode)
        gmv_log("[gamepatch] sun and moon %s\n", mode == 2 ? "as on the iPhone" : mode ? "enhanced" : "off");
    sm_mode_set(mode);
    return mode;
}
