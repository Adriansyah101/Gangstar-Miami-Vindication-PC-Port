/* sync_camera.c -- CAMERA_SYNC: the camera follow at high tick rates, and its trace */
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

/* ---- camera follow at high tick rates (GMV_CAMERA_SYNC, on by default) ------------------------

   The camera manager's per-frame update (0x44640, called once per drawn frame with the frame's dt
   in ms) asks the active camera where it wants to be, then hands the eye and the target to
   0x43da4 / 0x43c48.  Each one either snaps the scene camera there or, if the scene camera is
   more than sqrt(3000) ~ 55 units away (the float at 0x5f1308, compared at 0x43e20 / 0x43cc4),
   glides there with a 0.1 s follow (AnimatableType<vector3d>::update 0x455a0: dt / 0.1 of the way
   per frame).  That test works as a latch.  While snapped, the distance is one frame's movement
   (speed * dt), so at the Mac's 30 drawn frames/s a running character's turns and stride peaks
   trip it; once gliding, the distance is speed * 0.1 s at any frame rate and running keeps it
   above 55.  The glide is the running camera "shake": the camera trails ~75 units and the
   stride's uneven speed (19-43 units per frame) pulses the trail.  At 120 drawn frames/s a frame's
   movement is a quarter as long, the latch never trips, the camera stays snapped to the
   character and the shake is gone (CAM_TRACE: trail 70-82 at 60 ticks/s, 0 at 240).
   The glide itself is not frame-rate free either: dt / 0.1 per frame trails speed * (0.1 s - dt),
   75 units at 30 frames/s but 105 at 120.  The fix makes both behave as at 30 frames/s, from the
   hooked entries of 0x43da4 / 0x43c48 (mgr, &wanted, 0.1f), which see the wanted position:
   - Follow time 0.0667 s + dt (dt capped at 33.3 ms), written over the 0.1f argument, so the
     trail is speed * 0.0667 s as on the Mac.
   - The latch test on the gap a Mac frame would see: the trail before this frame plus the wanted
     position's move over one Mac frame, i.e. gap - this frame's step + that move.  A Mac frame is
     33.3 ms plus however late it was drawn: at 60 ticks/s the drawn frames come as 42/24 and 38/29
     ms pairs with stalls of 60+ (log_cam60b.log), and a late frame at a stride peak is what starts
     most running glides there.  So the window is 33.3 ms plus the lateness of the frames drawn
     within it -- beyond 2 ticks and beyond the recent average frame, rounded up to whole ms, so a
     stretch of slower drawing at 240 ticks/s (12-16 ms frames) is not counted as a run of late
     Mac frames -- and the position that long ago is
     interpolated between frames (a window of whole frames runs long and over-triggers).  With an
     exact 33.3 ms window only turns started a glide and straight runs stayed rigid
     (log_cam240d.log); camsim2.py checks the widened window against late frames.  The first
     version scaled one frame's step instead, and the uneven 4-25 unit steps tripped it at random
     while the original test dropped each glide within a frame or two (log_cam240c.log).
   - A glide here starts one short frame behind where a Mac glide starts a whole frame behind, so
     it is held until that gap first exceeds 55, as long as the Mac would settle above 55 (a steady
     glide's gap is speed * 0.1 s: a 33.3 ms move of more than 18.3 units).
   The decision reaches the game by re-aiming both compares at host floats set to 0 (glide) or
   FLT_MAX (snap); a zero distance still snaps, as in the original.  The state is the follow time
   the previous frame left (manager +0x58 eye, +0x8c target: nonzero gliding, 0 snapped); the gap
   is the shown position (+0x40 / +0x74) to the wanted one.  scratchpad camsim.py simulated it
   against the original at 30 frames/s: same glide start and end, same trail.  Installed only when
   the tick pacer runs faster than 60 ticks/s; after a change of the options menu's FRAME RATE it
   is in force only while that holds (cam_sync_set). */
#define CT_MGR_UPDATE 0x00044640u   /* 55 89 e5 53 57 56 */
#define CS_EYE_FN     0x00043da4u   /* 55 89 e5 53 57 56 : (mgr, &wanted eye, 0.1f)    */
#define CS_TGT_FN     0x00043c48u   /* 55 89 e5 53 57 56 : (mgr, &wanted target, 0.1f) */
#define CS_EYE_CMP    0x00043e20u   /* 0f 2e 8e <disp32> : ucomiss xmm1,[esi+0x5f1308]  (esi 0x43db2) */
#define CS_TGT_CMP    0x00043cc4u   /* 0f 2e 8e <disp32> : ucomiss xmm1,[esi+0x5f1308]  (esi 0x43c56) */
#define CS_EYE_PIC    0x00043db2u
#define CS_TGT_PIC    0x00043c56u
#define CS_DIST2      3000.0f      /* the float at 0x5f1308 */
#define CS_REF_DT     33.333f      /* a drawn frame at the Mac's 60 ticks/s */
#define CS_HOLD_MS    100.0f       /* the 0.1 s follow time: a steady glide trails speed * 0.1 s */
#define CS_HIST       64           /* > 100 ms of frames down to FRAME_MS 1 */

int g_cam_sync;
#if GMV_DEV
static int g_cam_trace;
#else
#define g_cam_trace 0
#endif
static float g_cs_gate[2] = { CS_DIST2, CS_DIST2 };   /* eye, target */
static float g_cs_now;                                /* game ms, summed from the manager's dt */
static int g_cs_dt;                                   /* this frame's dt (ms) */
static float g_cs_nominal;                            /* whole ms of an on-time drawn frame (2 ticks) */
static float g_cs_avg;                                /* running average frame (ms) */
static float g_cs_late;                               /* this frame's lateness (ms) */
typedef struct {
    float t[CS_HIST], x[CS_HIST], y[CS_HIST];        /* wanted positions, newest at (n-1) % CS_HIST */
    float late[CS_HIST];                              /* how late each was drawn (ms) */
    unsigned n;
    int reached;                                      /* the gap has reached 55 since the glide began */
} cs_channel;
static cs_channel g_cs[2];
static float g_cs_dbg[4];                             /* eye's last test, for CAM_TRACE: window, move, gap, Mac gap */
static float g_ct_tick;                               /* tick period (ms) for CAM_TRACE's lateness counts */

static float ct_dist2d(const float *a, const float *b)
{
    float dx = a[0] - b[0], dy = a[1] - b[1];
    return sqrtf(dx * dx + dy * dy);
}

/* Returns the follow time (float bits) the hook stub writes over the call's 0.1f argument. */
__attribute__((cdecl, force_align_arg_pointer))
static uint32_t gmv_cs_follow(int ch, const unsigned char *mgr, const float *want)
{
    const unsigned dur_off = ch ? 0x8c : 0x58, shown_off = ch ? 0x74 : 0x40;
    float dur, shown[3];
    if (!g_cam_sync) {                                /* FRAME_MS back at 60 from the options menu */
        static const float orig = 0.1f;
        uint32_t bits;
        g_cs_gate[ch] = CS_DIST2;
        memcpy(&bits, &orig, 4);
        return bits;
    }
    memcpy(&dur, mgr + dur_off, 4);
    memcpy(shown, mgr + shown_off, 12);
    cs_channel *c = &g_cs[ch];
    unsigned k = c->n % CS_HIST;
    c->t[k] = g_cs_now;
    c->x[k] = want[0];
    c->y[k] = want[1];
    c->late[k] = g_cs_late;
    c->n++;
    /* A Mac frame is 33.3 ms plus however late it was drawn, and a late frame during a fast stride
       is what starts most glides at 60 ticks/s (frames of 42/24 and 38/29 ms, stalls of 60+, in
       log_cam60b.log).  So the window is 33.3 ms plus the lateness of the frames drawn in it. */
    unsigned old = c->n > CS_HIST ? c->n - CS_HIST : 0, j;
    float win = CS_REF_DT;
    for (j = c->n; j-- > old && g_cs_now - c->t[j % CS_HIST] < CS_REF_DT;)
        win += c->late[j % CS_HIST];
    if (win > 3.0f * CS_REF_DT)
        win = 3.0f * CS_REF_DT;
    /* where the wanted position was that long ago: interpolated between the samples around it,
       else the oldest kept */
    j = c->n - 1;
    while (j > old && g_cs_now - c->t[j % CS_HIST] < win)
        j--;
    float px = c->x[j % CS_HIST], py = c->y[j % CS_HIST];
    if (j + 1 < c->n && g_cs_now - c->t[j % CS_HIST] > win) {
        unsigned j1 = (j + 1) % CS_HIST;
        float a = (g_cs_now - win - c->t[j % CS_HIST]) / (c->t[j1] - c->t[j % CS_HIST]);
        px += (c->x[j1] - px) * a;
        py += (c->y[j1] - py) * a;
    }
    float move = sqrtf((want[0] - px) * (want[0] - px) + (want[1] - py) * (want[1] - py));
    float step = 0.0f;
    if (c->n > 1) {
        unsigned p = (c->n - 2) % CS_HIST;
        step = sqrtf((want[0] - c->x[p]) * (want[0] - c->x[p]) + (want[1] - c->y[p]) * (want[1] - c->y[p]));
    }
    float deq = ct_dist2d(shown, want) - step + move;    /* the gap a Mac frame would test */
    if (deq < 0.0f)
        deq = 0.0f;
    int glide;
    if (dur != 0.0f) {
        if (deq * deq > CS_DIST2)
            c->reached = 1;
        float settle = move * CS_HOLD_MS / CS_REF_DT;
        glide = deq * deq > CS_DIST2 || (!c->reached && settle * settle > CS_DIST2);
    } else {
        glide = deq * deq > CS_DIST2;
        c->reached = 0;
    }
    g_cs_gate[ch] = glide ? 0.0f : FLT_MAX;
    if (!ch) {
        g_cs_dbg[0] = win;
        g_cs_dbg[1] = move;
        g_cs_dbg[2] = ct_dist2d(shown, want);
        g_cs_dbg[3] = deq;
    }
    float dt = g_cs_dt < 0 ? 0.0f : g_cs_dt > CS_REF_DT ? CS_REF_DT : (float)g_cs_dt;
    float follow = (CS_HOLD_MS - CS_REF_DT + dt) / 1000.0f;
    uint32_t bits;
    memcpy(&bits, &follow, 4);
    return bits;
}

/* ---- diagnostic: how the camera follows the running character (GMV_CAM_TRACE=1) --------------

   Logs, every 2 s: frames/s, the dt range, the trailing distance (wanted - shown eye, manager
   +0x34 - +0x40), and the per-frame speeds of the character (the on-foot camera's target,
   cam+0x14) and of the shown eye, plus the last 16 frames as dt/character step/eye step/trail.
   Also counts the eye's glide starts and ends (its follow time, manager +0x58, turning nonzero /
   zero) and the frames drawn 5, 10 and 20+ ms later than 2 ticks, and logs the first few starts
   and ends of each 2 s with the frames before them (and, with CAMERA_SYNC, the test that
   decided: window, move over it, gap, and the Mac frame's gap). */

typedef struct { unsigned n; float lo, hi; double sum; } ct_range;

static void ct_add(ct_range *r, float v)
{
    if (!r->n || v < r->lo) r->lo = v;
    if (!r->n || v > r->hi) r->hi = v;
    r->sum += v;
    r->n++;
}

static void cam_trace_frame(const unsigned char *mgr, int dt)
{
    static DWORD t0;
    static unsigned calls;
    static ct_range r_dt, r_trail, r_char, r_eye;
    static float prev_char[3], prev_eye[3];
    static int have_prev;
    static struct { int dt; float ch, eye, trail; } wave[16];
    static unsigned wn;
    static unsigned starts, ends, events, late5, late10, late20;
    static float prev_dur;
    const unsigned char *cam = *(const unsigned char *const *)(mgr + 8);
    if (!cam)
        return;
    float want[3], shown[3], chr[3], dur;
    memcpy(want, mgr + 0x34, 12);
    memcpy(shown, mgr + 0x40, 12);
    memcpy(chr, cam + 0x14, 12);
    memcpy(&dur, mgr + 0x58, 4);
    float trail = ct_dist2d(want, shown);
    calls++;
    ct_add(&r_dt, (float)dt);
    if (g_ct_tick > 0.0f) {
        float late = (float)dt - 2.0f * g_ct_tick;
        late5 += late >= 5.0f;
        late10 += late >= 10.0f;
        late20 += late >= 20.0f;
    }
    if (have_prev) {
        float ch = ct_dist2d(chr, prev_char), eye = ct_dist2d(shown, prev_eye);
        if (ch > 0.5f && dt > 0) {          /* moving: per-frame speeds in units per real second */
            ct_add(&r_trail, trail);
            ct_add(&r_char, ch * 1000.0f / dt);
            ct_add(&r_eye, eye * 1000.0f / dt);
        }
        wave[wn % 16].dt = dt;
        wave[wn % 16].ch = ch;
        wave[wn % 16].eye = eye;
        wave[wn % 16].trail = trail;
        wn++;
        int start = prev_dur == 0.0f && dur != 0.0f, end = prev_dur != 0.0f && dur == 0.0f;
        starts += start;
        ends += end;
        if ((start || end) && events++ < 8) {
            char line[4 * 24], *p = line;
            for (unsigned k = wn > 4 ? wn - 4 : 0; k < wn; k++)
                p += sprintf(p, " %d/%.1f", wave[k % 16].dt, wave[k % 16].ch);
            if (g_cam_sync)
                gmv_log("[camtrace] glide %s: frames (dt/character step):%s, trail %.1f; test: window %.1f ms, "
                        "move %.1f, gap %.1f, Mac gap %.1f\n", start ? "start" : "end", line, trail,
                        g_cs_dbg[0], g_cs_dbg[1], g_cs_dbg[2], g_cs_dbg[3]);
            else
                gmv_log("[camtrace] glide %s: frames (dt/character step):%s, trail %.1f\n",
                        start ? "start" : "end", line, trail);
        }
    }
    prev_dur = dur;
    memcpy(prev_char, chr, 12);
    memcpy(prev_eye, shown, 12);
    have_prev = 1;
    DWORD now = GetTickCount();
    if (!t0)
        t0 = now;
    if (now - t0 < 2000)
        return;
    double secs = (now - t0) / 1000.0;
    t0 = now;
    gmv_log("[camtrace] %.1f frames/s, dt %.0f..%.0f ms (avg %.1f); moving frames %u: trail %.1f..%.1f, "
            "character speed %.0f..%.0f, eye speed %.0f..%.0f per s\n",
            calls / secs, r_dt.lo, r_dt.hi, r_dt.n ? r_dt.sum / r_dt.n : 0.0, r_trail.n,
            r_trail.lo, r_trail.hi, r_char.lo, r_char.hi, r_eye.lo, r_eye.hi);
    gmv_log("[camtrace] glide starts %u, ends %u; frames late by 5+/10+/20+ ms: %u/%u/%u\n",
            starts, ends, late5, late10, late20);
    starts = ends = events = late5 = late10 = late20 = 0;
    if (r_trail.n) {
        char line[16 * 32], *p = line;
        for (unsigned k = wn > 16 ? wn - 16 : 0; k < wn; k++)
            p += sprintf(p, " %d/%.1f/%.1f/%.1f", wave[k % 16].dt, wave[k % 16].ch, wave[k % 16].eye,
                         wave[k % 16].trail);
        gmv_log("[camtrace] last frames (dt/character step/eye step/trail):%s\n", line);
    }
    memset(&r_dt, 0, sizeof r_dt);
    memset(&r_trail, 0, sizeof r_trail);
    memset(&r_char, 0, sizeof r_char);
    memset(&r_eye, 0, sizeof r_eye);
    calls = 0;
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_cam_mgr(const unsigned char *mgr, int dt)
{
    g_cs_now += (float)dt;
    g_cs_dt = dt;
    /* lateness: beyond 2 ticks and beyond the recent average, so a stretch of slower drawing (which
       the Mac's 30 frames/s would not have) is not counted frame after frame -- only spikes are */
    float on_time = ceilf(g_cs_avg) > g_cs_nominal ? ceilf(g_cs_avg) : g_cs_nominal;
    g_cs_late = (float)dt > on_time ? (float)dt - on_time : 0.0f;
    if (dt > 0 && dt < 250)
        g_cs_avg += ((float)dt - g_cs_avg) / 16.0f;
    if (g_cam_trace)
        cam_trace_frame(mgr, dt);
}

/* Hook a function whose entry is `55 89 e5 53 57 56`: call fn([ch,] arg1..argN) with the first N
   stack arguments (ch < 0 = no leading constant), store its eax over stack argument `ret_arg`
   (1-based; 0 = discard), then run the prologue and continue. */
static int cam_entry_hook(uint32_t va, void *fn, int ch, int nargs, int ret_arg)
{
    static const unsigned char pro[6] = { 0x55, 0x89, 0xe5, 0x53, 0x57, 0x56 };
    if (memcmp((const void *)(uintptr_t)va, pro, sizeof pro))
        return 0;
    unsigned char *st = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!st)
        return 0;
    size_t o = 0;
    uint32_t a;
    for (int i = 0; i < nargs; i++) {   /* each push moves the next one's source to the same offset */
        st[o++] = 0xff; st[o++] = 0x74; st[o++] = 0x24; st[o++] = (unsigned char)(4 * nargs);
    }
    if (ch >= 0) {
        st[o++] = 0x6a; st[o++] = (unsigned char)ch;                    /* push ch       */
    }
    st[o++] = 0xb8;                                                     /* mov eax,fn    */
    a = (uint32_t)(uintptr_t)fn; memcpy(st + o, &a, 4); o += 4;
    st[o++] = 0xff; st[o++] = 0xd0;                                     /* call eax      */
    st[o++] = 0x83; st[o++] = 0xc4;                                     /* add esp,n     */
    st[o++] = (unsigned char)(4 * (nargs + (ch >= 0)));
    if (ret_arg > 0) {
        st[o++] = 0x89; st[o++] = 0x44; st[o++] = 0x24;                 /* mov [esp+4k],eax */
        st[o++] = (unsigned char)(4 * ret_arg);
    }
    memcpy(st + o, pro, sizeof pro);
    o += sizeof pro;
    emit_jmp(st, &o, va + sizeof pro);
    FlushInstructionCache(GetCurrentProcess(), st, o);
    unsigned char site[6] = { 0xe9, 0, 0, 0, 0, 0x90 };
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)st - (va + 5));
    memcpy(site + 1, &rel, 4);
    return patch_bytes(va, site, sizeof site);
}

/* Re-aim the compare at 0x5f1308 (PIC base `pic`) at the host float `gate`. */
static int cam_sync_aim(uint32_t site, uint32_t pic, float *gate)
{
    static const unsigned char op[3] = { 0x0f, 0x2e, 0x8e };
    uint32_t disp;
    memcpy(&disp, (const void *)(uintptr_t)(site + 3), 4);
    if (memcmp((const void *)(uintptr_t)site, op, 3) || pic + disp != 0x005f1308u)
        return 0;
    disp = (uint32_t)(uintptr_t)gate - pic;
    return patch_bytes(site + 3, (const unsigned char *)&disp, 4);
}

/* Camera sync for a tick of ms: installed the first time it is below 60 ticks/s' 16.6 ms, and
   from then on on below it and passing the original through above (the options menu's FRAME RATE). */
int cs_mgr_hooked, cs_installed;

static void cam_sync_in(double ms)
{
    static int failed;
    const char *e = gmv_getenv("GMV_CAMERA_SYNC");
    int want = !(e && *e == '0') && ms < 16.6;
    g_ct_tick = (float)ms;
    g_cs_nominal = ms > 0.0 ? ceilf((float)(2.0 * ms)) : FLT_MAX;   /* uncapped: no on-time length */
    if (((want && !cs_installed) || g_cam_trace) && !cs_mgr_hooked && !failed) {
        /* [esp+4]=mgr [esp+8]=dt -> gmv_cam_mgr(mgr, dt): the game clock for the sync, and the trace */
        if (!cam_entry_hook(CT_MGR_UPDATE, (void *)&gmv_cam_mgr, -1, 2, 0)) {
            gmv_log("[gamepatch] camera manager 0x%08x not the expected bytes; camera sync skipped\n",
                    CT_MGR_UPDATE);
            failed = 1;
        } else {
            cs_mgr_hooked = 1;
            if (g_cam_trace)
                gmv_log("[camtrace] hooked the camera manager update 0x%08x\n", CT_MGR_UPDATE);
        }
    }
    if (want && !cs_installed && cs_mgr_hooked && !failed) {
        /* [esp+4]=mgr [esp+8]=&wanted [esp+0xc]=0.1f -> gmv_cs_follow(ch, mgr, wanted), result over the
           0.1f.  The compares are re-aimed only once both hooks are in; a partial install leaves
           g_cam_sync 0, so the hooks hand back the original 0.1 s. */
        g_cam_sync = 1;
        if (!cam_entry_hook(CS_EYE_FN, (void *)&gmv_cs_follow, 0, 2, 3) ||
            !cam_entry_hook(CS_TGT_FN, (void *)&gmv_cs_follow, 1, 2, 3) ||
            !cam_sync_aim(CS_EYE_CMP, CS_EYE_PIC, &g_cs_gate[0]) ||
            !cam_sync_aim(CS_TGT_CMP, CS_TGT_PIC, &g_cs_gate[1])) {
            gmv_log("[gamepatch] camera follow code (0x%08x, 0x%08x) not the expected bytes; camera sync skipped\n",
                    CS_EYE_FN, CS_TGT_FN);
            g_cam_sync = 0;
            failed = 1;
            return;
        }
        cs_installed = 1;
        gmv_log("[gamepatch] camera sync installed: the camera's follow starts and holds as at the Mac's "
                "30 drawn frames/s (%.3f ms ticks)\n", ms);
        return;
    }
    if (cs_installed && g_cam_sync != want)
        gmv_log("[gamepatch] camera sync %s (%.3f ms ticks)\n", want ? "on" : "off", ms);
    g_cam_sync = want && cs_installed;
}

void cam_sync_set(double ms)
{
    patch_undo *keep = patch_undo_now;
    patch_undo_now = &mac_undo;
    cam_sync_in(ms);
    patch_undo_now = keep;
}

void apply_cam_follow(void)
{
#if GMV_DEV
    const char *e = gmv_dbgenv("GMV_CAM_TRACE");
    g_cam_trace = e && *e && *e != '0';
#endif
    cam_sync_set(gmv_pace_frame_ms());
}
