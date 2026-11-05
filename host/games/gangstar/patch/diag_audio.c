/* diag_audio.c -- diagnostics: engine voices and AudioSource */
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

/* ---- diagnostic: engine-voice RATE + VOLUME over time (GMV_CARSND_TRACE=1) ----

   The engine "rev" in this build is built from the 3 engine voices (idle/onidle/offidle, 32kHz ->
   idle base rate 11888 = 0.726x), created on enter-vehicle.  Two candidate mechanisms:
     - a VOLUME crossfade between the three loops (voice +0x54), and/or
     - a per-voice RATE sweep (voice +0x38, the 2.14-fixed playback step; 0x4000 = 1.0x).
   The OLD trace filtered to rate==11888 and so could only ever see the crossfade -- if the game
   actually sweeps the rate for RPM, that was invisible.  This version ADOPTS each voice at its
   idle-rate birth (rate==11888) and then keeps following it even as its rate moves, logging rate,
   volume and doppler (+0xdc) on every meaningful change with a serial number.  So one clean
   standstill->floor->hold->release pass shows definitively whether the pitch sweeps (rate climbs)
   or the blend crossfades (volumes swap), and whether the motion is smooth or jumpy ("aneh").
   The user's reference is the macOS build (video "Gangstar Miami Vindication on mac #2"), the SAME
   build we port, so any mismatch here is a real port bug.  Pure diagnostic; 0x5c2dc0 render is
   called under the mixer lock so reading the voice is safe. */
#define VC_FUNC 0x005c2dc0u     /* PCM-voice render (class 0x6846c8, vtbl[0x60])                  */
#define VC_CONT 0x005c2dc6u     /* resume after the 6 displaced prologue bytes                   */

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_engvol_log(void *voice)
{
    if (!voice || IsBadReadPtr(voice, 0xe0))
        return;
    const int32_t *v = (const int32_t *)voice;
    int32_t f38 = v[0x38 / 4];            /* base rate (2.14 fixed; 0x4000 = 1.0x, 11888 = 0.726x) */
    int32_t f54 = v[0x54 / 4];            /* volume      (0x4000 = unity)                          */
    int32_t f64 = v[0x64 / 4];            /* PITCH factor (0x4000 = 1.0x, clamp 0x8000 = 2.0x)     */
    int32_t f6c = v[0x6c / 4];            /* effective step = (f38*f64>>14)*fdc>>14 (what pitches)  */
    int32_t fdc = v[0xdc / 4];            /* doppler     (0x4000 = 1.0x)                           */

    /* Adopt each engine voice at its idle-rate birth, then keep tracking it.  The real engine
       pitch is NOT the base rate f38 (which stays 11888) -- it is the per-voice PITCH factor f64,
       which the mixer multiplies into the sample step f6c (0x5c2e61..0x5c2e75).  The old trace read
       only f38/f54/fdc and so was blind to f64; this version logs f64 and the resulting step f6c so
       a standstill->floor->release pass shows whether the pitch actually sweeps in our port. */
    static void    *seen[48];
    static int32_t  lrate[48], lvol[48], lpit[48], lstep[48];
    static unsigned nseen, lines, seq;

    int idx = -1;
    for (unsigned i = 0; i < nseen; i++)
        if (seen[i] == voice) { idx = (int)i; break; }

    int first = 0;
    if (idx < 0) {
        if (f38 != 11888)                 /* only adopt NEW voices that begin at engine idle rate */
            return;
        if (nseen >= 48)
            return;
        idx = (int)nseen++;
        seen[idx] = voice;
        lrate[idx] = f38; lvol[idx] = f54; lpit[idx] = f64; lstep[idx] = f6c;
        first = 1;
    }

    int32_t dr = f38 - lrate[idx]; if (dr < 0) dr = -dr;
    int32_t dv = f54 - lvol[idx];  if (dv < 0) dv = -dv;
    int32_t dp = f64 - lpit[idx];  if (dp < 0) dp = -dp;
    int32_t ds = f6c - lstep[idx]; if (ds < 0) ds = -ds;

    if (first || dr > 64 || dv > 128 || dp > 64 || ds > 64) {
        lrate[idx] = f38; lvol[idx] = f54; lpit[idx] = f64; lstep[idx] = f6c;
        if (lines++ < 4000)
            gmv_log("[engrev] #%u v%d base=%d pitch=%.3fx step=%d(%.3fx) vol=%.3f dop=%.3fx spd=%.0f%s\n",
                    seq++, idx, (int)f38, (double)f64 / 16384.0,
                    (int)f6c, (double)f6c / 16384.0,
                    (double)f54 / 16384.0, (double)fdc / 16384.0,
                    (double)g_carsnd_last_speed,
                    first ? " [new]" : "");
    }
}

void apply_engvol_trace(void)
{
    const char *e = gmv_dbgenv("GMV_CARSND_TRACE");
    if (!(e && *e && *e != '0'))
        return;
    static const unsigned char expect[6] = { 0x55, 0x89, 0xe5, 0x83, 0xec, 0x38 };
    if (memcmp((const void *)(uintptr_t)VC_FUNC, expect, sizeof expect) != 0) {
        gmv_log("[gamepatch] fn 0x%08x not the expected bytes; engvol trace skipped\n", VC_FUNC);
        return;
    }
    unsigned char *tr = VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) {
        gmv_log("[gamepatch] VirtualAlloc for engvol trampoline failed (%lu); skipped\n", GetLastError());
        return;
    }
    /* Entered via jmp from the entry: [esp]=retaddr [esp+4]=voice (cdecl). */
    size_t o = 0;
    tr[o++] = 0x8b; tr[o++] = 0x44; tr[o++] = 0x24; tr[o++] = 0x04;  /* mov eax,[esp+4] ; voice     */
    tr[o++] = 0x50;                                                  /* push eax        ; arg       */
    tr[o++] = 0xb8;                                                  /* mov eax,&gmv_engvol_log     */
    { uint32_t fnp = (uint32_t)(uintptr_t)&gmv_engvol_log; memcpy(tr + o, &fnp, 4); o += 4; }
    tr[o++] = 0xff; tr[o++] = 0xd0;                                 /* call eax                     */
    tr[o++] = 0x83; tr[o++] = 0xc4; tr[o++] = 0x04;                 /* add esp,4      ; cdecl clean  */
    tr[o++] = 0x55;                                                 /* push ebp       ; displaced    */
    tr[o++] = 0x89; tr[o++] = 0xe5;                                 /* mov ebp,esp                  */
    tr[o++] = 0x83; tr[o++] = 0xec; tr[o++] = 0x38;                 /* sub esp,0x38                 */
    tr[o++] = 0xff; tr[o++] = 0x25; size_t contref = o; o += 4;     /* jmp [cont_ptr] -> VC_CONT    */
    size_t contslot = o;
    { uint32_t c = VC_CONT; memcpy(tr + o, &c, 4); o += 4; }
    { uint32_t a = (uint32_t)(uintptr_t)(tr + contslot); memcpy(tr + contref, &a, 4); }
    FlushInstructionCache(GetCurrentProcess(), tr, o);

    unsigned char site[6];
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)tr - (VC_FUNC + 5));
    site[0] = 0xe9;
    memcpy(site + 1, &rel, 4);
    site[5] = 0x90;
    if (!patch_bytes(VC_FUNC, site, sizeof site)) {
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); engvol trace skipped\n", VC_FUNC, GetLastError());
        return;
    }
    gmv_log("[gamepatch] engine-rev trace installed at 0x%08x (trampoline @ %p); "
            "[engrev] logs engine-voice rate/volume/doppler over time (pitch sweep vs crossfade)\n",
            VC_FUNC, (void *)tr);
}

/* ---- diagnostic: AudioSource per-frame update, keyed by SOURCE (GMV_ENGSRC_TRACE=1) ----

   The per-voice render trace ([engrev]) cannot isolate the player's engine: voice handles are
   pooled, so one pointer carries many unrelated one-shots over time.  The AudioSource, by contrast,
   is a stable high-level object that lives while you are in the car and owns the engine voices at
   +0x134/+0x138.  Its update is update(AudioSource* this, float dt) at 0x5b1980; it ramps a volume
   pair (+0x5c cur / +0x60 tgt) and a PITCH pair (+0x94 cur / +0x98 tgt) toward their targets and
   pushes them to the voices via setVolume (vtbl 0x2c) / setPitch (vtbl 0x30).  The player reports
   the rev is UNSTABLE -- on throttle release the pitch sometimes spikes too high -- which, given
   this game's dt sensitivity, points at the ramp overshooting on a dt spike.  So we log, per source
   and on meaningful change, the pitch cur/tgt, volume cur/tgt, the dt handed to the update and the
   player speed.  Discovery pass: dump raw fields (as float AND int) so the field format is certain,
   capped so it cannot flood (gmv_log is not re-entrant-cheap).  Pure diagnostic. */
#define ENGSRC_FUNC 0x005b1980u    /* AudioSource::update(this, float dt)                            */
#define ENGSRC_CONT 0x005b1986u    /* resume after the 6 displaced prologue bytes (55 89 e5 57 56 53)*/

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_engsrc_log(void *src, uint32_t dtbits)
{
    if (!src || IsBadReadPtr(src, 0x140))
        return;
    /* only start once the player has actually been in a moving car, to skip menu/startup churn */
    static int armed;
    if (!armed) {
        if (g_carsnd_last_speed > 1.0f) armed = 1;
        else return;
    }
    static unsigned lines;
    if (lines >= 1200)
        return;

    const int32_t *s = (const int32_t *)src;
    int32_t volc = s[0x5c / 4], volt = s[0x60 / 4];
    int32_t pitc = s[0x94 / 4], pitt = s[0x98 / 4];
    void *v0 = (void *)(uintptr_t)s[0x134 / 4];
    void *v1 = (void *)(uintptr_t)s[0x138 / 4];
    float dt = 0.0f; memcpy(&dt, &dtbits, 4);

    /* Track per source; log on first sight and on meaningful pitch/vol movement.  Interpret the
       pitch/vol pairs as floats (the setVolume/setPitch args are floats); also print the raw int so
       a wrong guess is still recoverable from the log. */
    float fpc, fpt, fvc, fvt;
    memcpy(&fpc, &pitc, 4); memcpy(&fpt, &pitt, 4);
    memcpy(&fvc, &volc, 4); memcpy(&fvt, &volt, 4);

    static void  *seen[64];
    static float  lpc[64], lvc[64];
    static unsigned nseen;
    int idx = -1;
    for (unsigned i = 0; i < nseen; i++)
        if (seen[i] == src) { idx = (int)i; break; }
    int first = 0;
    if (idx < 0) {
        if (nseen >= 64) return;
        idx = (int)nseen++; seen[idx] = src; lpc[idx] = fpc; lvc[idx] = fvc; first = 1;
    }
    float dp = fpc - lpc[idx]; if (dp < 0) dp = -dp;
    float dv = fvc - lvc[idx]; if (dv < 0) dv = -dv;
    if (first || dp > 0.01f || dv > 0.02f) {
        lpc[idx] = fpc; lvc[idx] = fvc;
        lines++;
        gmv_log("[engsrc] %p dt=%.4f pit=%.3f->%.3f(i %d/%d) vol=%.3f->%.3f v=%p,%p spd=%.0f%s\n",
                src, (double)dt, (double)fpc, (double)fpt, (int)pitc, (int)pitt,
                (double)fvc, (double)fvt, v0, v1, (double)g_carsnd_last_speed,
                first ? " [new]" : "");
    }
}

void apply_engsrc_trace(void)
{
    const char *e = gmv_dbgenv("GMV_ENGSRC_TRACE");
    if (!(e && *e && *e != '0'))
        return;
    static const unsigned char expect[6] = { 0x55, 0x89, 0xe5, 0x57, 0x56, 0x53 };
    if (memcmp((const void *)(uintptr_t)ENGSRC_FUNC, expect, sizeof expect) != 0) {
        gmv_log("[gamepatch] fn 0x%08x not the expected bytes; engsrc trace skipped\n", ENGSRC_FUNC);
        return;
    }
    unsigned char *tr = VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) {
        gmv_log("[gamepatch] VirtualAlloc for engsrc trampoline failed (%lu); skipped\n", GetLastError());
        return;
    }
    /* Entered via jmp at function entry: [esp]=ret [esp+4]=this(AudioSource) [esp+8]=dt (cdecl). */
    size_t o = 0;
    tr[o++] = 0xff; tr[o++] = 0x74; tr[o++] = 0x24; tr[o++] = 0x08;  /* push dword [esp+8]  ; dtbits   */
    tr[o++] = 0xff; tr[o++] = 0x74; tr[o++] = 0x24; tr[o++] = 0x08;  /* push dword [esp+8]  ; this     */
    tr[o++] = 0xb8;                                                  /* mov eax,&gmv_engsrc_log        */
    { uint32_t fnp = (uint32_t)(uintptr_t)&gmv_engsrc_log; memcpy(tr + o, &fnp, 4); o += 4; }
    tr[o++] = 0xff; tr[o++] = 0xd0;                                 /* call eax                        */
    tr[o++] = 0x83; tr[o++] = 0xc4; tr[o++] = 0x08;                 /* add esp,8  ; cdecl clean 2 args */
    tr[o++] = 0x55;                                                 /* push ebp    ; displaced 6 bytes */
    tr[o++] = 0x89; tr[o++] = 0xe5;                                 /* mov ebp,esp                     */
    tr[o++] = 0x57;                                                 /* push edi                        */
    tr[o++] = 0x56;                                                 /* push esi                        */
    tr[o++] = 0x53;                                                 /* push ebx                        */
    tr[o++] = 0xff; tr[o++] = 0x25; size_t contref = o; o += 4;     /* jmp [cont_ptr] -> ENGSRC_CONT   */
    size_t contslot = o;
    { uint32_t c = ENGSRC_CONT; memcpy(tr + o, &c, 4); o += 4; }
    { uint32_t a = (uint32_t)(uintptr_t)(tr + contslot); memcpy(tr + contref, &a, 4); }
    FlushInstructionCache(GetCurrentProcess(), tr, o);

    unsigned char site[6];
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)tr - (ENGSRC_FUNC + 5));
    site[0] = 0xe9;
    memcpy(site + 1, &rel, 4);
    site[5] = 0x90;
    if (!patch_bytes(ENGSRC_FUNC, site, sizeof site)) {
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); engsrc trace skipped\n", ENGSRC_FUNC, GetLastError());
        return;
    }
    gmv_log("[gamepatch] AudioSource-update trace installed at 0x%08x (trampoline @ %p); "
            "[engsrc] logs per-source pitch/vol/dt vs speed while driving\n", ENGSRC_FUNC, (void *)tr);
}

/* ---- diagnostic: AudioSource DESTRUCTOR count (GMV_ENGSRC_TRACE=1) ----------------------------

   The engsrc trace shows many engine sources accumulating.  To tell a real leak (created, never
   freed) from healthy churn (created and freed), hook the two AudioSource destructors and count how
   many sources are actually destroyed, to compare against the [engsrc] "[new]" creations in the same
   run.  Deleting dtor 0x5b1900 (-> operator delete 0x90450) and non-deleting dtor 0x5b1940; both have
   prologue 55 89 e5 53 83 ec 14 (7 bytes).  Pure diagnostic. */
#define ENGDTOR_DEL   0x005b1900u
#define ENGDTOR_NODEL 0x005b1940u

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_engdtor_log(void *src, uint32_t which)
{
    static unsigned ndel, nnodel, lines;
    if (which) nnodel++; else ndel++;
    unsigned tot = ndel + nnodel;
    if (lines++ < 200 || (tot % 100) == 0)       /* detail early; running tally every 100 thereafter */
        gmv_log("[engdtor] destroy %s src=%p  (deleting=%u non-deleting=%u total=%u)\n",
                which ? "nodel" : "del", src, ndel, nnodel, tot);
}

static void install_dtor_hook(uint32_t addr, uint32_t which)
{
    static const unsigned char expect[7] = { 0x55, 0x89, 0xe5, 0x53, 0x83, 0xec, 0x14 };
    if (memcmp((const void *)(uintptr_t)addr, expect, sizeof expect) != 0) {
        gmv_log("[gamepatch] dtor 0x%08x not the expected bytes; engdtor hook skipped\n", addr);
        return;
    }
    unsigned char *tr = VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return;
    size_t o = 0;
    tr[o++] = 0x68; { memcpy(tr + o, &which, 4); o += 4; }          /* push imm32 which   (arg1)       */
    tr[o++] = 0xff; tr[o++] = 0x74; tr[o++] = 0x24; tr[o++] = 0x08; /* push dword [esp+8] this (arg0)   */
    tr[o++] = 0xb8; { uint32_t fnp = (uint32_t)(uintptr_t)&gmv_engdtor_log; memcpy(tr + o, &fnp, 4); o += 4; }
    tr[o++] = 0xff; tr[o++] = 0xd0;                                 /* call eax                        */
    tr[o++] = 0x83; tr[o++] = 0xc4; tr[o++] = 0x08;                 /* add esp,8                       */
    tr[o++] = 0x55;                                                 /* push ebp    ; displaced 7 bytes */
    tr[o++] = 0x89; tr[o++] = 0xe5;                                 /* mov ebp,esp                     */
    tr[o++] = 0x53;                                                 /* push ebx                        */
    tr[o++] = 0x83; tr[o++] = 0xec; tr[o++] = 0x14;                 /* sub esp,0x14                    */
    tr[o++] = 0xff; tr[o++] = 0x25; size_t contref = o; o += 4;     /* jmp [cont] -> addr+7            */
    size_t contslot = o;
    { uint32_t c = addr + 7; memcpy(tr + o, &c, 4); o += 4; }
    { uint32_t a = (uint32_t)(uintptr_t)(tr + contslot); memcpy(tr + contref, &a, 4); }
    FlushInstructionCache(GetCurrentProcess(), tr, o);

    unsigned char site[7];
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)tr - (addr + 5));
    site[0] = 0xe9; memcpy(site + 1, &rel, 4); site[5] = 0x90; site[6] = 0x90;
    if (!patch_bytes(addr, site, sizeof site)) {
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); engdtor hook skipped\n", addr, GetLastError());
        return;
    }
    gmv_log("[gamepatch] AudioSource dtor hook at 0x%08x (which=%u, trampoline @ %p)\n", addr, which, (void *)tr);
}

void apply_engdtor_trace(void)
{
    const char *e = gmv_dbgenv("GMV_ENGSRC_TRACE");
    if (!(e && *e && *e != '0'))
        return;
    install_dtor_hook(ENGDTOR_DEL, 0);
    install_dtor_hook(ENGDTOR_NODEL, 1);
}
