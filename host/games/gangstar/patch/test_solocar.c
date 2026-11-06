/* test_solocar.c -- test aid: hear only the player's car */
#include "compat.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* ---- test aid: hear ONLY the player's car (GMV_SOLO_CAR=1) ------------------------------------

   For judging the engine pitch by ear (and in gmv_audio_dump.wav) without traffic, peds, music or
   the radio in the mix.  Two parts:

   1. Which voices are the player's car.  The car-sound module (0xa2fc2..0xa3f61, driven by
      Car/Moto::UpdateCarSounds, vtbl+0xd8) talks to the SoundManager only through the 5-entry
      function table at 0x689dd0, installed by 0x10aba8 from the pointer slots at 0x63e478 (read by
      nothing else).  UpdateCarSounds only runs for the vehicle whose driver (+0x280) is the player
      (0x113a46 sets +0x1e4=1 only then), so every (sample, handle) through that table is the
      player's car.  We wrap isPlaying (called on every engine layer every frame) and play, and
      resolve each pair the way the SoundManager does: inst = SM(0x688780)->[0xc][(sample-SM[0x250])
      *16 + handle*4]; AudioSource = 0x5b0440(device 0x64d5e0, &inst->[0x24]) under the device lock
      (0x5be000/0x5bdfd0 on device+0xf8, as 0x5b1600 does); voice = AudioSource->[0x134].  Each hit
      stamps that voice in a small whitelist (game thread only).
   2. Muting everything else.  The device mixer 0x5bfc70 (root vtbl[0x4c] -> 0x5bfe60) sums EVERY
      voice of every class through one call, voice->vtbl[0x60](voice, accum, frames) at 0x5bfddb.
      We re-route that call through a trampoline that asks gmv_solo_mixbuf which buffer to mix
      into: the real accumulator for a whitelisted voice, a discarded scratch buffer otherwise.  A
      muted voice still renders, so its play position, ramps and end-of-sample state advance
      exactly as before; only its contribution to the output is dropped.  The audio thread only
      compares pointers, never dereferences game objects.

   Test-only (changes what you hear); off unless GMV_SOLO_CAR is set. */
#define SOLO_MIX_SITE    0x005bfdceu  /* mov edx,[0x94b8dc]; mov [esp+4],edx; mov [esp],eax; call [ecx+0x60] */
#define SOLO_MIX_CONT    0x005bfddeu  /* first instruction after the voice-render call                    */
#define SOLO_ACCUM_PTR   0x0094b8dcu  /* mixer's int32 stereo accumulation buffer pointer                */
#define CARSND_API_SLOTS 0x0063e478u  /* isPlaying, stop, setVolume, play, setPitch (read only by 0x10aba8) */
#define CARSND_API_TABLE 0x00689dd0u  /* live table: isPlaying, play, stop, setPitch, setVolume           */
#define SM_ISPLAYING     0x0006149au
#define SM_PLAY          0x0006137cu
#define SM_SETPITCH      0x00061036u
#define SM_SINGLETON     0x00688780u
#define AUDIO_DEVICE     0x0064d5e0u
#define DEV_FIND_SOURCE  0x005b0440u  /* AudioSource *(device, emitter handle *)                          */
#define DEV_LOCK         0x005be000u
#define DEV_UNLOCK       0x005bdfd0u

#define SOLO_MAX      32
#define SOLO_KEEP_MS  1000            /* a voice stays audible this long after its last car-sound hit */
#define SOLO_FRAMES   4096

static void *volatile g_solo_voice[SOLO_MAX];
static volatile DWORD g_solo_stamp[SOLO_MAX];
static int32_t        g_solo_scratch[SOLO_FRAMES * 2];

typedef uint32_t (*solo_api2_t)(uint32_t, uint32_t);
typedef uint32_t (*solo_api4_t)(uint32_t, uint32_t, uint32_t, uint32_t);
static solo_api2_t g_solo_orig_isplaying;
static solo_api4_t g_solo_orig_play;

/* Game thread: resolve a car-sound (sample, handle) to its AudioSource and render voice. */
static void *solo_resolve(int sample, int handle, char **src_out)
{
    *src_out = NULL;
    if (handle < 0 || handle > 3)
        return NULL;
    const char *sm = *(const char *const *)(uintptr_t)SM_SINGLETON;
    if (!sm || !(sm[0x220] & 2))
        return NULL;
    int idx = sample - *(const int *)(sm + 0x250);
    if (idx < 0 || idx >= *(const int *)(sm + 0x24c))
        return NULL;
    char *inst = *(char **)(*(char *const *)(sm + 0xc) + idx * 0x10 + handle * 4);
    char *dev  = *(char *const *)(uintptr_t)AUDIO_DEVICE;
    if (!inst || !dev)
        return NULL;

    typedef void (*lock_t)(void *);
    typedef char *(*find_t)(void *, void *);
    ((lock_t)(uintptr_t)DEV_LOCK)(dev + 0xf8);
    char *src   = ((find_t)(uintptr_t)DEV_FIND_SOURCE)(dev, inst + 0x24);
    void *voice = src ? *(void **)(src + 0x134) : NULL;
    ((lock_t)(uintptr_t)DEV_UNLOCK)(dev + 0xf8);
    *src_out = src;
    return voice;
}

/* Game thread: stamp the voice behind a car-sound (sample, handle) in the whitelist. */
static void solo_note(int sample, int handle)
{
    char *src;
    void *voice = solo_resolve(sample, handle, &src);
    if (!voice)
        return;

    DWORD now = GetTickCount();
    int slot = -1, oldest = 0;
    for (int i = 0; i < SOLO_MAX; i++) {
        if (g_solo_voice[i] == voice) { slot = i; break; }
        if (now - g_solo_stamp[i] > now - g_solo_stamp[oldest]) oldest = i;
    }
    if (slot < 0) {
        static unsigned logged;
        slot = oldest;
        g_solo_voice[slot] = voice;
        if (logged++ < 32)
            gmv_log("[solo] player-car voice %p (vtbl %p) <- sample %d handle %d src %p\n",
                    voice, *(void **)voice, sample, handle, (void *)src);
    }
    g_solo_stamp[slot] = now;
}

__attribute__((cdecl, force_align_arg_pointer))
static uint32_t gmv_solo_isplaying(uint32_t sample, uint32_t handle)
{
    uint32_t r = g_solo_orig_isplaying(sample, handle);
    if (r & 0xff)                                        /* bool in al */
        solo_note((int)sample, (int)handle);
    return r;
}

__attribute__((cdecl, force_align_arg_pointer))
static uint32_t gmv_solo_play(uint32_t sample, uint32_t loop, uint32_t vol, uint32_t pitch)
{
    uint32_t h = g_solo_orig_play(sample, loop, vol, pitch);
    solo_note((int)sample, (int)h);
    return h;
}

/* Diagnostic (GMV_SOLO_CAR + GMV_CARSND_TRACE): every engine pitch the car-sound module asks for
   (setPitch = rpm / layer reference rpm, 0xa3016/0xa3308/0xa35b2/0xa387e), next to what the chain
   below holds at that moment -- AudioSource +0xa0 (setPitch target, 0x5b2200) / +0x98 (pushed to the
   voice by update 0x5b1980) and render voice +0x60 target / +0x64 current / +0x68 per-buffer step
   (0x5c0120, consumed by the render 0x5c2dc0).  The voice side reflects the previous frame's push.
   Logged per sample on a >0.5% change in the request, so a flat line means the game held the rpm. */
typedef uint32_t (*solo_api3_t)(uint32_t, uint32_t, uint32_t);
static solo_api3_t g_solo_orig_setpitch;
static int g_carpitch_trace;

__attribute__((cdecl, force_align_arg_pointer))
static uint32_t gmv_solo_setpitch(uint32_t sample, uint32_t handle, uint32_t pitch)
{
    uint32_t r = g_solo_orig_setpitch(sample, handle, pitch);
    if (!g_carpitch_trace)
        return r;
    static int   ls[32];
    static float lp[32];
    static unsigned n, lines;
    float p; memcpy(&p, &pitch, 4);
    int i = 0;
    while (i < (int)n && ls[i] != (int)sample) i++;
    if (i == (int)n) {
        if (n >= 32) return r;
        ls[n] = (int)sample; lp[n] = -1.0f; n++;
    }
    float d = p - lp[i]; if (d < 0) d = -d;
    if (d <= lp[i] * 0.005f || lines >= 6000)
        return r;
    lp[i] = p;
    if (lines++ == 0)
        gmv_log("[carpitch] voice ramp limit DAT_0064d5f8=%d\n", *(const int *)(uintptr_t)0x0064d5f8u);
    char *src;
    const int32_t *v = (const int32_t *)solo_resolve((int)sample, (int)handle, &src);
    float s98 = 0, sa0 = 0;
    if (src) { memcpy(&s98, src + 0x98, 4); memcpy(&sa0, src + 0xa0, 4); }
    gmv_log("[carpitch] t=%lu s%d h%d req=%.3f src(a0=%.3f 98=%.3f) voice(tgt=%.3f cur=%.3f step=%d st=%d len=%d)\n",
            GetTickCount(), (int)sample, (int)handle, (double)p, (double)sa0, (double)s98,
            v ? v[0x60 / 4] / 16384.0 : -1.0, v ? v[0x64 / 4] / 16384.0 : -1.0,
            v ? v[0x68 / 4] : 0, v ? v[0x7c / 4] : 0, v ? v[8 / 4] : 0);
    return r;
}

/* Audio thread (inside the mixer): which buffer this voice mixes into. */
__attribute__((cdecl, force_align_arg_pointer))
static void *gmv_solo_mixbuf(void *voice, void *accum, int frames)
{
    DWORD now = GetTickCount();
    for (int i = 0; i < SOLO_MAX; i++)
        if (g_solo_voice[i] == voice && now - g_solo_stamp[i] < SOLO_KEEP_MS)
            return accum;
    if (frames <= 0 || frames > SOLO_FRAMES)
        return accum;
    memset(g_solo_scratch, 0, (size_t)frames * 8);
    return g_solo_scratch;
}

static int solo_swap_ptr(uint32_t va, uint32_t expect, uint32_t repl)
{
    uint32_t cur;
    memcpy(&cur, (const void *)(uintptr_t)va, 4);
    if (cur != expect)
        return 0;
    return patch_bytes(va, (const unsigned char *)&repl, 4);
}

void apply_solo_car(void)
{
    const char *e = gmv_dbgenv("GMV_SOLO_CAR");
    if (!(e && *e && *e != '0'))
        return;
    static const unsigned char expect[16] = {
        0x8b, 0x15, 0xdc, 0xb8, 0x94, 0x00,     /* mov edx,[0x94b8dc] */
        0x89, 0x54, 0x24, 0x04,                 /* mov [esp+4],edx    */
        0x89, 0x04, 0x24,                       /* mov [esp],eax      */
        0xff, 0x51, 0x60                        /* call [ecx+0x60]    */
    };
    const uint32_t *slots = (const uint32_t *)(uintptr_t)CARSND_API_SLOTS;
    if (memcmp((const void *)(uintptr_t)SOLO_MIX_SITE, expect, sizeof expect) != 0 ||
        slots[0] != SM_ISPLAYING || slots[3] != SM_PLAY) {
        gmv_log("[gamepatch] mixer call / car-sound API slots not the expected bytes; solo-car skipped\n");
        return;
    }
    /* Wrap the API first: if that fails, leave the mixer alone rather than mute everything. */
    g_solo_orig_isplaying = (solo_api2_t)(uintptr_t)SM_ISPLAYING;
    g_solo_orig_play      = (solo_api4_t)(uintptr_t)SM_PLAY;
    uint32_t wis = (uint32_t)(uintptr_t)&gmv_solo_isplaying, wpl = (uint32_t)(uintptr_t)&gmv_solo_play;
    if (!solo_swap_ptr(CARSND_API_SLOTS + 0x0, SM_ISPLAYING, wis) ||
        !solo_swap_ptr(CARSND_API_SLOTS + 0xc, SM_PLAY, wpl)) {
        gmv_log("[gamepatch] car-sound API slot write failed (%lu); solo-car skipped\n", GetLastError());
        return;
    }
    /* normally installed later by 0x10aba8 (from the slots above); cover an already-live table too */
    solo_swap_ptr(CARSND_API_TABLE + 0x0, SM_ISPLAYING, wis);
    solo_swap_ptr(CARSND_API_TABLE + 0x4, SM_PLAY, wpl);

    const char *t = gmv_dbgenv("GMV_CARSND_TRACE");
    if (t && *t && *t != '0' && slots[4] == SM_SETPITCH) {
        uint32_t wsp = (uint32_t)(uintptr_t)&gmv_solo_setpitch;
        g_solo_orig_setpitch = (solo_api3_t)(uintptr_t)SM_SETPITCH;
        if (solo_swap_ptr(CARSND_API_SLOTS + 0x10, SM_SETPITCH, wsp)) {
            solo_swap_ptr(CARSND_API_TABLE + 0xc, SM_SETPITCH, wsp);
            g_carpitch_trace = 1;
            gmv_log("[gamepatch] car-pitch trace on: [carpitch] logs each engine setPitch vs AudioSource/voice "
                    "state (voice ramp limit DAT_0064d5f8=%d now)\n", *(const int *)(uintptr_t)0x0064d5f8u);
        }
    }

    unsigned char *tr = VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) {
        gmv_log("[gamepatch] VirtualAlloc for solo-car trampoline failed (%lu); skipped\n", GetLastError());
        return;
    }
    /* Entered via `call` from the site with eax=voice, ecx=vtbl, frames already in the caller's
       [esp+8] (stored at 0x5bfdca) -> [esp+0xc] here.  Ends in a tail jump to the render, which
       returns straight to SOLO_MIX_SITE+5. */
    size_t o = 0;
    tr[o++] = 0x8b; tr[o++] = 0x15;                                  /* mov edx,[accum]              */
    { uint32_t a = SOLO_ACCUM_PTR; memcpy(tr + o, &a, 4); o += 4; }
    tr[o++] = 0x51;                                                  /* push ecx        ; save vtbl  */
    tr[o++] = 0x50;                                                  /* push eax        ; save voice */
    tr[o++] = 0xff; tr[o++] = 0x74; tr[o++] = 0x24; tr[o++] = 0x14;  /* push dword [esp+0x14] frames */
    tr[o++] = 0x52;                                                  /* push edx        ; accum      */
    tr[o++] = 0x50;                                                  /* push eax        ; voice      */
    tr[o++] = 0xb8;                                                  /* mov eax,&gmv_solo_mixbuf     */
    { uint32_t fnp = (uint32_t)(uintptr_t)&gmv_solo_mixbuf; memcpy(tr + o, &fnp, 4); o += 4; }
    tr[o++] = 0xff; tr[o++] = 0xd0;                                  /* call eax                     */
    tr[o++] = 0x83; tr[o++] = 0xc4; tr[o++] = 0x0c;                  /* add esp,12                   */
    tr[o++] = 0x89; tr[o++] = 0xc2;                                  /* mov edx,eax     ; buffer     */
    tr[o++] = 0x58;                                                  /* pop eax         ; voice      */
    tr[o++] = 0x59;                                                  /* pop ecx         ; vtbl       */
    tr[o++] = 0x89; tr[o++] = 0x54; tr[o++] = 0x24; tr[o++] = 0x08;  /* mov [esp+8],edx              */
    tr[o++] = 0x89; tr[o++] = 0x44; tr[o++] = 0x24; tr[o++] = 0x04;  /* mov [esp+4],eax              */
    tr[o++] = 0xff; tr[o++] = 0x61; tr[o++] = 0x60;                  /* jmp [ecx+0x60]  ; render     */
    FlushInstructionCache(GetCurrentProcess(), tr, o);

    unsigned char site[16];
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)tr - (SOLO_MIX_SITE + 5));
    site[0] = 0xe8;                                                  /* call trampoline              */
    memcpy(site + 1, &rel, 4);
    site[5] = 0xeb;                                                  /* jmp short SOLO_MIX_CONT      */
    site[6] = (unsigned char)(SOLO_MIX_CONT - (SOLO_MIX_SITE + 7));
    memset(site + 7, 0x90, sizeof site - 7);
    if (!patch_bytes(SOLO_MIX_SITE, site, sizeof site)) {
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); solo-car skipped\n", SOLO_MIX_SITE, GetLastError());
        return;
    }
    gmv_log("[gamepatch] SOLO CAR (test aid): only the player's car sounds are mixed; traffic, peds, "
            "music, radio and other SFX are muted (mixer hook 0x%08x, trampoline %p)\n",
            SOLO_MIX_SITE, (void *)tr);
}
