/* diag_state.c -- diagnostic (GMV_STATE_TRACE=1): the game's state stack, its render gates and the menu's predicates. */
#include "compat.h"
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"

#if GMV_DEV
typedef void *(*msg_t)(void *self, const char *sel, ...);
extern void *gmv_objc_last_eaglview;
extern void *gmv_objc_last_gamewindow;
extern void *gmv_objc_last_gamedelegate;
extern int gmv_input_wants_tap;

/* The main-menu state (0x659f28) update runs its show/animate logic only when the front-end is
   idle + fully loaded; two game predicates decide it.  Call them to see which one blocks (all are
   high-code, run in place at nominal VAs): fe = 0x35d08() front-end accessor; 0x32de0(fe) returns
   "front-end is in a TRANSITION state 1..6" (TRUE blocks the menu logic); 0x1adbc4() returns "all
   screens/resources loaded" (FALSE blocks).  These are read-only predicates the game itself calls
   every frame; noinline + force_align so the realignment survives for the game's SSE stack. */
__attribute__((noinline, force_align_arg_pointer))
static void gmv_menu_gates(int *fe_trans, int *fe_state, int *all_loaded)
{
    void *(*fe_get)(void)  = (void *(*)(void))(uintptr_t)0x35d08u;
    int   (*fe_tr)(void *) = (int (*)(void *))(uintptr_t)0x32de0u;
    int   (*loaded)(void)  = (int (*)(void))(uintptr_t)0x1adbc4u;
    void *fe = fe_get();
    *fe_trans   = fe ? (fe_tr(fe) & 0xff) : -1;
    *fe_state   = fe ? *(const int *)((const unsigned char *)fe + 0xc) : -0x1000;
    *all_loaded = loaded() & 0xff;
    /* Front-end screen table: container onEnter 0x1b5b52 does mainMenu = fe->screens[0][0x51]
       (0x32d98) and stores it at [0x949ad8]; [0x63e5e4] is set to 1 when that onEnter ran.  If the
       screen table isn't populated up to 0x51, the lookup returns 0 and the menu page is empty. */
    int scount = fe ? *(const int *)((const unsigned char *)fe + 0x2ec) : -1;
    void *sarr = fe ? *(void *const *)((const unsigned char *)fe + 0x2dc) : NULL;
    void *mainmenu = *(void *const *)(uintptr_t)0x949ad8u;
    unsigned char e5e4 = *(const unsigned char *)(uintptr_t)0x63e5e4u;
    /* The RENDER manager is a DIFFERENT object from the screen registry fe: G1 = *[0x63e038] and the
       menu update 0x1a1752 renders its CURRENT PAGE G1[+0x94].  Activating MainMenu in fe's table
       (screen[0x18]=2) must also make G1[+0x94] point at it, or there is nothing for the per-frame
       front-end render to draw.  Also read the MainMenu screen's own fields (state [0x18], context
       binding [0x20]) and the manager context [[0x689ea8]+0x75c] that activation binds to. */
    /* G1 needs TWO derefs: the menu update does eax=*(0x63e038) (=0x685668, a static slot), then
       edi=*(eax) = the real render-mgr object, then edi[0x94] = current page. */
    void *g1slot = *(void *const *)(uintptr_t)0x63e038u;          /* = 0x685668 */
    void *g1     = g1slot ? *(void *const *)g1slot : NULL;        /* real render-mgr object */
    void *g1pg   = g1 ? *(void *const *)((const unsigned char *)g1 + 0x94) : (void *)-1;
    void *eamgr = *(void *const *)(uintptr_t)0x689ea8u;
    int   ctx   = eamgr ? *(const int *)((const unsigned char *)eamgr + 0x75c) : -0x1000;
    int   mmSt  = mainmenu ? *(const int *)((const unsigned char *)mainmenu + 0x18) : -0x1000;
    int   mmCtx = mainmenu ? *(const int *)((const unsigned char *)mainmenu + 0x20) : -0x1000;
    /* MainMenu fe-screen is a transient builder: it makes 4 widgets (list top [+8], count [+4]),
       then at scr[8]==scr[4] switches away and activates its successor [scr+0x1c].  Log those. */
    int   mm4   = mainmenu ? *(const int *)((const unsigned char *)mainmenu + 0x4) : -0x1000;
    int   mm8   = mainmenu ? *(const int *)((const unsigned char *)mainmenu + 0x8) : -0x1000;
    void *mm1c  = mainmenu ? *(void *const *)((const unsigned char *)mainmenu + 0x1c) : NULL;
    void *mm1cVt = (mm1c && !IsBadReadPtr(mm1c, 4)) ? *(void *const *)mm1c : NULL;
    int   mm1cSt = (mm1c && !IsBadReadPtr(mm1c, 0x20)) ? *(const int *)((const unsigned char *)mm1c + 0x18) : -0x1000;
    gmv_log("[state] MMSCR count[+4]=%d top[+8]=%d successor[+0x1c]=%p succVt=%p succState[+0x18]=%d\n",
            mm4, mm8, mm1c, mm1cVt, mm1cSt);
    /* Widgets are NOT freed on teardown (switchaway only calls widget->vt[0x10]), so scr[0x10][0..2]
       + the widget objects persist.  Read each widget's vtable + vt[8] (the "is-done" predicate the
       advance 0x32706 gates on) so we can disassemble it and see why it is immediately true. */
    void *warr = mainmenu ? *(void *const *)((const unsigned char *)mainmenu + 0x10) : NULL;
    if (warr && !IsBadReadPtr(warr, 12)) {
        for (int i = 0; i < 3; i++) {
            void *w = ((void *const *)warr)[i];
            void *wvt = (w && !IsBadReadPtr(w, 4)) ? *(void *const *)w : NULL;
            void *w8  = (wvt && !IsBadReadPtr(wvt, 0xc)) ? ((void *const *)wvt)[2] : NULL;
            void *w0  = (wvt && !IsBadReadPtr(wvt, 4)) ? ((void *const *)wvt)[0] : NULL;
            gmv_log("[state] WIDGET[%d]=%p vt=%p vt0=%p vt8(isDone)=%p\n", i, w, wvt, w0, w8);
        }
    } else {
        gmv_log("[state] WIDGET array=%p (unreadable)\n", warr);
    }
    /* WIDGET[0] is a fade-in: global timer [0x63e6c8] starts at widget[0x10] (duration) and is
       decremented each frame by the dt passed to its vt[8]; done when timer<=0.  If the per-frame
       dt >= duration, the fade completes in ONE frame -> premature screen switchaway -> blank.
       Measure the fade duration (WIDGET[0][+0x10]=0xa9dc78), the frame dt [App+0x17ad8], and the
       live fade globals alpha[0x63e6c4]/timer[0x63e6c8]. */
    {
        void *app3 = *(void *const *)(uintptr_t)0x6853a0u;
        int dt   = app3 ? *(const int *)((const unsigned char *)app3 + 0x17ad8) : -1;
        int w0dur = *(const int *)(uintptr_t)0xa9dc78u;   /* WIDGET[0][+0x10] fade duration */
        int alpha = *(const int *)(uintptr_t)0x63e6c4u;
        int ftmr  = *(const int *)(uintptr_t)0x63e6c8u;
        gmv_log("[state] FADE w0dur[+0x10]=%d frameDt[App+0x17ad8]=%d  alpha[0x63e6c4]=%d timer[0x63e6c8]=%d\n",
                w0dur, dt, alpha, ftmr);
    }
    const unsigned char *g6 = (const unsigned char *)(uintptr_t)0x949a58u;
    gmv_log("[state] FE fe=%p screens[0].count=%d arr=%p  mainMenu=%p st18=%d ctx20=%d onEnterRan=%u\n"
            "        G1slot[0x685668]=%p G1obj=%p curPage[G1+0x94]=%p  eamgr[0x689ea8]=%p ctx[+0x75c]=%d  gate6=%02x%02x%02x%02x%02x%02x\n",
            fe, scount, sarr, mainmenu, mmSt, mmCtx, e5e4,
            g1slot, g1, g1pg, eamgr, ctx, g6[0], g6[1], g6[2], g6[3], g6[4], g6[5]);
    /* One-shot: resolve the scene/resource sub-manager behind the "MainMenu" load.  onEnter does
       core=0x15e4fa(); core[0xb4]->vtable[0x28]("MainMenu") (changeScene) -- the widget build is
       behind that virtual slot.  Log core, the sub-manager core[0xb4], its vtable, and slot 0x28
       (and neighbours) so we can disassemble the actual loader. */
    static int once;
    if (!once) {
        once = 1;
        void *(*core_get)(void) = (void *(*)(void))(uintptr_t)0x15e4fau;
        void *core = core_get();
        void *mgr  = core ? *(void *const *)((const unsigned char *)core + 0xb4) : NULL;
        void *vt   = mgr  ? *(void *const *)mgr : NULL;
        void *s28  = vt   ? *(void *const *)((const unsigned char *)vt + 0x28) : NULL;
        /* Scene manager layout (from loader 0x3005a0): [0x104]/[0x108] = scene DEFINITION list
           (searched by name = scene+8), [0x114]/[0x118] = active scene STACK, [0xf8] = flags.
           Dump the definition count + whether a "MainMenu" definition exists, and the active
           stack top (name + state [scene+0x58]).  This tells us if changeScene("MainMenu")
           actually found + loaded the scene, or bailed on an empty/!found definition list. */
        int   defN  = mgr ? *(const int *)((const unsigned char *)mgr + 0x108) : -0x1000;
        void *defA  = mgr ? *(void *const *)((const unsigned char *)mgr + 0x104) : NULL;
        int   stkN  = mgr ? *(const int *)((const unsigned char *)mgr + 0x118) : -0x1000;
        void *stkA  = mgr ? *(void *const *)((const unsigned char *)mgr + 0x114) : NULL;
        unsigned f8 = mgr ? *(const unsigned char *)((const unsigned char *)mgr + 0xf8) : 0;
        void *top   = (stkA && stkN > 0) ? ((void *const *)stkA)[stkN - 1] : NULL;
        const char *topNm = top ? (const char *)((const unsigned char *)top + 8) : "<none>";
        int   topSt = top ? *(const int *)((const unsigned char *)top + 0x58) : -0x1000;
        void *topVt = top ? *(void *const *)top : NULL;        /* scene object's own vtable */
        int   top54 = top ? *(const int *)((const unsigned char *)top + 0x54) : -0x1000;
        int   mmDef = 0; char mmName[32] = "";
        if (defA && defN > 0 && defN < 4096)
            for (int i = 0; i < defN; i++) {
                void *d = ((void *const *)defA)[i];
                if (!d) continue;
                const char *nm = (const char *)((const unsigned char *)d + 8);
                if (!IsBadReadPtr(nm, 9) && !memcmp(nm, "MainMenu", 9)) { mmDef = 1; break; }
                if (i == 0 && !IsBadReadPtr(nm, 1)) { lstrcpynA(mmName, nm, 31); }
            }
        gmv_log("[state] SCENE core=%p mgr=%p vt=%p vt[0x28]=%p  defN=%d defArr=%p hasMainMenuDef=%d def[0]Name=\"%s\"\n"
                "        stackN=%d top=%p topVt=%p topName=\"%s\" topState[0x58]=%d top[0x54]=%d flags[0xf8]=0x%02x\n",
                core, mgr, vt, s28, defN, defA, mmDef, mmName, stkN, top, topVt, top ? topNm : "<none>", topSt, top54, f8);
        /* The menu update 0x1a1752 ticks a subsystem sub=*(0x68a100); sub->vtable[8](dt) every
           frame, then "renders" the front-end current page via 0x124ab0(G1[0x94]).  Log the
           subsystem + its vt[8] (the per-frame tick target to disassemble), and G1[0x94] again. */
        void *sub   = *(void *const *)(uintptr_t)0x68a100u;
        void *subVt = sub ? *(void *const *)sub : NULL;
        void *sub8  = subVt ? *(void *const *)((const unsigned char *)subVt + 8) : NULL;
        void *sub18 = subVt ? *(void *const *)((const unsigned char *)subVt + 0x18) : NULL;
        gmv_log("[state] SUBSYS *(0x68a100)=%p vt=%p vt[8]=%p vt[0x18]=%p  G1curPage[+0x94]=%p\n",
                sub, subVt, sub8, sub18, g1pg);
        /* The front-end draw 0x1a9488 (called unconditionally by the 0x4600 bracket) EARLY-OUTS
           unless the drawable vector [0x68a768(begin)..0x68a76c(end)] has >=4 bytes (8-byte
           entries).  This is the real "front-end has content to draw" gate -- measure it. */
        void *vb = *(void *const *)(uintptr_t)0x68a768u;
        void *ve = *(void *const *)(uintptr_t)0x68a76cu;
        long  vn = (vb && ve) ? (long)(((const char *)ve - (const char *)vb) / 8) : -1;
        void *vb2 = *(void *const *)(uintptr_t)0x68a778u;
        void *ve2 = *(void *const *)(uintptr_t)0x68a77cu;
        long  vn2 = (vb2 && ve2) ? (long)(((const char *)ve2 - (const char *)vb2) / 8) : -1;
        gmv_log("[state] FEDRAW vec[0x68a768]=%p..%p count=%ld   vec2[0x68a778]=%p..%p count=%ld\n",
                vb, ve, vn, vb2, ve2, vn2);
        /* The 0x4600 bracket, right before the front-end flush 0x1a9488, walks the circular
           intrusive list [App+0x17a88] calling each node's object->vtable[8]() -- the prime
           candidate for the per-frame front-end DRAW pass that emits widget geometry.  Walk it
           (node link = [node+0], node object = [node+8]) and log each object's vtable so we can
           see whether the menu's renderer is registered to draw at all. */
        void *app = *(void *const *)(uintptr_t)0x6853a0u;
        if (app) {
            void *sent80 = (void *)((const unsigned char *)app + 0x17a80);
            void *sent88 = (void *)((const unsigned char *)app + 0x17a88);
            int k = 0; void *node = *(void *const *)sent80;
            while (node && node != sent80 && k < 10) {
                void *obj = *(void *const *)((const unsigned char *)node + 8);
                void *ovt = (obj && !IsBadReadPtr(obj, 4)) ? *(void *const *)obj : NULL;
                gmv_log("[state] A80[%d] node=%p obj=%p objvt=%p\n", k, node, obj, ovt);
                node = *(void *const *)node; k++;
            }
            if (k == 0) gmv_log("[state] A80 list EMPTY\n");
            k = 0; node = *(void *const *)sent88;
            while (node && node != sent88 && k < 10) {
                void *obj = *(void *const *)((const unsigned char *)node + 8);
                void *ovt = (obj && !IsBadReadPtr(obj, 4)) ? *(void *const *)obj : NULL;
                gmv_log("[state] A88[%d] node=%p obj=%p objvt=%p\n", k, node, obj, ovt);
                node = *(void *const *)node; k++;
            }
            if (k == 0) gmv_log("[state] A88 list EMPTY\n");
            /* The layer vid->[0x10] is what state1's working draw renders (layer->vt[0x14]/vt[0x18])
               and what the menu draw reads ([layer+0xb4]).  Dump its vtable + first 0x40 dwords to
               find its drawable-list field (begin/end/cur pointers), so we can find addDrawable. */
            void *vid = *(void *const *)((const unsigned char *)app + 0x17aa4);
            void *layer = (vid && !IsBadReadPtr(vid, 0x14)) ? *(void *const *)((const unsigned char *)vid + 0x10) : NULL;
            if (layer && !IsBadReadPtr(layer, 0xc0)) {
                const uint32_t *L = (const uint32_t *)layer;
                gmv_log("[state] LAYER vid=%p layer=%p vt=%08x  [+0xb4]=%08x [+0xb8]=%08x [+0xbc]=%08x\n",
                        vid, layer, L[0], L[0xb4/4], L[0xb8/4], L[0xbc/4]);
            }
        }
        /* Dump the live current-page object (vtable + first 32 dwords) so we can identify its class
           and spot its widget list / visible-active flags.  g1pg is a heap pointer this run. */
        if (g1pg && (uintptr_t)g1pg > 0x10000 && !IsBadReadPtr(g1pg, 32 * 4)) {
            const uint32_t *pg = (const uint32_t *)g1pg;
            gmv_log("[state] PAGE obj=%p vt=%08x\n", g1pg, pg[0]);
            for (int r = 0; r < 4; r++)
                gmv_log("        +%02x: %08x %08x %08x %08x %08x %08x %08x %08x\n", r * 0x20,
                        pg[r*8+0], pg[r*8+1], pg[r*8+2], pg[r*8+3],
                        pg[r*8+4], pg[r*8+5], pg[r*8+6], pg[r*8+7]);
        }
    }
}

/* Diagnostic (GMV_STATE_TRACE=1): the engine's render is gated in its main loop by flags in the
   Application object (GameDelegate m_App @+56).  Log a few candidate state words each second and
   on change, to see whether the render guard is stuck (vs an empty scene). */
void gmv_state_trace(void)
{
    static int on = -1;
    if (!GMV_DEV)
        return;
    if (on < 0) {
        const char *e = gmv_dbgenv("GMV_STATE_TRACE");
        on = e && *e && *e != '0';
    }
    if (!on)
        return;
    /* The engine Application is a singleton at __DATA 0x6853a0 (from the accessor at low 0x31b0:
       `mov eax,[pic+0x6821e8]`, pic 0x31b8 -> 0x6853a0).  The per-frame loop gates its render on
       byte flags [App+0x17af0] and [App+0x17b00]. */
    void *app = *(void **)(uintptr_t)0x6853a0u;
    static unsigned n;
    static unsigned pf0 = 0xffffffffu, pb0 = 0xffffffffu;
    if (!app) {
        if (n == 0) gmv_log("[state] App singleton [0x6853a0] still NULL\n");
        n++;
        return;
    }
    if (n == 0) {
        /* -[GameDelegate update]@0x19c600 ticks the engine via theApp->vtable[0x24](theApp).
           Log that fn pointer (and neighbours) once so we can disassemble the real per-frame tick. */
        void **vt = *(void ***)app;
        gmv_log("[state] theApp=%p vtable=%p tick vt[0x24]=%p (vt[0x20]=%p vt[0x28]=%p)\n",
                app, (void *)vt, vt[9], vt[8], vt[10]);
    }
    const unsigned char *a = app;
    unsigned af0 = a[0x17af0], b00 = a[0x17b00];
    /* State machine: -[GameDelegate ...]@0x19c630 reads state = [[App+0x17ac4]+4] (enum 1..8).
       Watch it to see whether the game advances out of its current (loading?) state. */
    void *sobj = *(void *const *)(a + 0x17ac4);
    int state = sobj ? *(const int *)((const unsigned char *)sobj + 4) : -1;
    static int pstate = -2;
    /* The per-frame update (low 0x4508) runs [App+0x17ac0]->vtable[2](dt) -- the active game/scene
       manager -- then walks the intrusive list at [App+0x17a80] calling each node's vtable[2](dt).
       The render (0x43a060) drains a command deque at [vid+0xb8 .. +0xc8]; empty => nothing drawn.
       Probe all three to see whether a scene is actually active and whether the draw list ever
       fills, which decides "stuck with no scene" vs "scene present but emitting no geometry". */
    void *mgr = *(void *const *)(a + 0x17ac0);
    const void *lh = *(const void *const *)(a + 0x17a80);
    int list_empty = (lh == (const void *)(a + 0x17a80));
    const unsigned char *vc = *(const unsigned char *const *)(a + 0x17aa4);
    int deque_empty = -1;
    if (vc) {
        const void *dh = *(const void *const *)(vc + 0xb8);
        const void *dt = *(const void *const *)(vc + 0xc8);
        deque_empty = (dh == dt);
    }
    static int pdeque = -2;
    /* mgr is a state/screen STACK: [mgr+0xc]=base ptr, [mgr+0x10]=top ptr; its update (vt[2]) does
       nothing while the stack is empty (top-base < 4).  An empty stack => no screen => black. */
    int stackN = -1;
    if (mgr) {
        unsigned base = *(const unsigned *)((const unsigned char *)mgr + 0xc);
        unsigned top  = *(const unsigned *)((const unsigned char *)mgr + 0x10);
        stackN = (int)((top - base) / 4);
    }
    static int pstk = -2;
    /* The sole StateMachine wait-state (its update 0x1b81a8) polls a readiness gate: with the core
       singleton S at fixed __bss 0xa064a0, when [S+0x218] > 62 it reads obj = [[S+0x214]+0xf8] and
       the gate byte obj[0x19].  Read these directly to see exactly what the state waits on. */
    int s218 = *(const int *)(uintptr_t)0xa066b8u;              /* S+0x218 */
    void *s214 = *(void *const *)(uintptr_t)0xa066b4u;          /* S+0x214 */
    void *rdyobj = s214 ? *(void *const *)((const unsigned char *)s214 + 0xf8) : NULL;
    int rdy = rdyobj ? *(const unsigned char *)((const unsigned char *)rdyobj + 0x19) : -1;
    static int prdy = -2;
    /* changeState (0x12d96) pops the whole stack and pushes the new state, so stackN stays 1 across
       a transition -- only the top-state pointer changes.  Track it to detect transitions. */
    void *topState = NULL;
    if (mgr && stackN >= 1) {
        void *base = *(void *const *)((const unsigned char *)mgr + 0xc);
        if (base)
            topState = *(void *const *)base;        /* bottom==top when stackN==1 */
    }
    static void *ptop = (void *)-1;
    if (topState && topState != ptop) {
        void **svt = *(void ***)topState;
        gmv_log("[state] f%u topState=%p vtable=%p update(vt6)=%p vt3=%p vt4=%p vt5=%p vt7=%p\n",
                n, topState, (void *)svt, svt[6], svt[3], svt[4], svt[5], svt[7]);
    }
    if (n == 0 && mgr) {
        void **mvt = *(void ***)mgr;                /* the active manager's vtable */
        gmv_log("[state] mgr=%p vtable=%p update vt[2]=%p\n", mgr, (void *)mvt, mvt[2]);
    }
    /* EXPERIMENT (GMV_DISMISS_SPLASH=1): the game rests in the western-region rating-splash state
       (vtable 0x65a388), whose transition-to-menu is gated by the byte [state+0xc] that never gets
       set for region 0.  Force it to 1 once to confirm the splash is the SOLE blocker and that the
       menu then loads.  SDL_PumpEvents (which calls us) runs before the state update in the tick,
       so the same frame's update sees it.  Reversible / opt-in; off unless the env var is set. */
    {
        static int dismiss = -1;
        if (dismiss < 0) {
            const char *e = gmv_dbgenv("GMV_DISMISS_SPLASH");
            dismiss = e && *e && *e != '0';
        }
        if (dismiss && topState && *(void **)topState == (void *)(uintptr_t)0x65a388u) {
            unsigned char *g = (unsigned char *)topState + 0xc;
            if (*g == 0) {
                *g = 1;
                gmv_log("[state] >>> DISMISS_SPLASH: forced [state+0xc]=1 on %p (f%u)\n", topState, n);
            }
        }
    }
    /* EXPERIMENT (GMV_FORCE_DRAW=1): the draw bracket (0x4600) skips the main-layer draw
       (R10->vt[0x1c]=0x521a70) unless BOTH draw-enable CVars [0x68541a] and [0x685438] are
       non-zero; both read 0 the whole run.  Force them to 1 each frame to test whether these
       are the lever that unblocks rendering.  Reversible / opt-in; off unless the env var set. */
    {
        static int force = -1;
        if (force < 0) {
            const char *e = gmv_dbgenv("GMV_FORCE_DRAW");
            force = e && *e && *e != '0';
        }
        if (force) {
            *(volatile unsigned char *)(uintptr_t)0x68541au = 1;
            *(volatile unsigned char *)(uintptr_t)0x685438u = 1;
            if (n == 0)
                gmv_log("[state] >>> FORCE_DRAW: forcing [0x68541a]=[0x685438]=1 each frame\n");
        }
    }
    /* EXPERIMENT (GMV_FORCE_GATE6=1): the menu draw 0x1a2590 early-outs unless any of the 6 bytes
       [0x949a58..0x949a5d] ("layer g has content") is non-zero; all 6 are never written.  Force
       them to 1 each frame to test whether the menu draw then renders the live current page (GL
       calls appear) or draws nothing / faults (page content not actually ready).  Opt-in. */
    {
        static int fg = -1;
        if (fg < 0) {
            const char *e = gmv_dbgenv("GMV_FORCE_GATE6");
            fg = e && *e && *e != '0';
        }
        if (fg) {
            volatile unsigned char *g6f = (volatile unsigned char *)(uintptr_t)0x949a58u;
            for (int i = 0; i < 6; i++) g6f[i] = 1;
            if (n == 0)
                gmv_log("[state] >>> FORCE_GATE6: forcing [0x949a58..5d]=1 each frame\n");
        }
    }
    if (n < 8 || n % 120 == 0 || af0 != pf0 || b00 != pb0 || state != pstate || deque_empty != pdeque
        || stackN != pstk || rdy != prdy) {
        int gate = topState ? *(const unsigned char *)((const unsigned char *)topState + 0xc) : -1;
        gmv_log("[state] f%u App=0x%08x [+0x17af0]=%u [+0x17b00]=%u [+0x17ad8]=%d vid=0x%08x state=%d sobj=%p"
                " mgr=%p listEmpty=%d dequeEmpty=%d stackN=%d S218=%d rdyObj=%p rdy=%d top=%p gate=%d\n",
                n, (unsigned)(uintptr_t)app, af0, b00,
                *(const int *)(a + 0x17ad8), *(const unsigned *)(a + 0x17aa4), state, sobj,
                mgr, list_empty, deque_empty, stackN, s218, rdyobj, rdy, topState, gate);
        /* The draw bracket (low 0x4600) runs each tick when 0x127f8 (top-state->vt[0x2c]) is true;
           it clears via vid->[0x1c]->vt[0x18] (unconditional) and draws the main layer via
           vid->[0x10]->vt[0x1c], the latter gated by the draw-enable CVars [0x68541a]&&[0x685438].
           Read those + the clear/draw IMPs to see whether the engine's draw path is live. */
        unsigned char cv1a = *(const unsigned char *)(uintptr_t)0x68541au;
        unsigned char cv38 = *(const unsigned char *)(uintptr_t)0x685438u;
        unsigned char cv1c = *(const unsigned char *)(uintptr_t)0x68541cu;
        unsigned char ae4  = *(const unsigned char *)(a + 0x17ae4);
        void *r1c = vc ? *(void *const *)(vc + 0x1c) : NULL;
        void *r10 = vc ? *(void *const *)(vc + 0x10) : NULL;
        void *r1c_vt18 = r1c ? (*(void ***)r1c)[6] : NULL;   /* vt offset 0x18 */
        void *r10_vt1c = r10 ? (*(void ***)r10)[7] : NULL;   /* vt offset 0x1c */
        gmv_log("[state] f%u draw: cv1a=%u cv38=%u cv1c=%u ae4=%u  R1c=%p clr.vt18=%p  "
                "R10=%p draw.vt1c=%p\n",
                n, cv1a, cv38, cv1c, ae4, r1c, r1c_vt18, r10, r10_vt1c);
        /* When the MAIN-MENU state (vtable 0x659f28) is on top, dump its own state: visibility
           byte [self+0x1d] (its vt[0x2c] returns it), [self+0x24]/[self+0x25]; the 6 menu
           draw-gate bytes [0x949a58..0x949a5d] (draw 0x1a2590 early-outs if all 0); and the
           global front-end object G1=[0x63e038] that holds the actual screen/widgets. */
        if (topState && *(void **)topState == (void *)(uintptr_t)0x659f28u) {
            const unsigned char *mg = (const unsigned char *)(uintptr_t)0x949a58u;
            void *g1 = *(void *const *)(uintptr_t)0x63e038u;
            void *g1v = g1 ? *(void *const *)((const unsigned char *)g1 + 0x94) : NULL;
            gmv_log("[state] f%u MENU self=%p [+1d]=%u [+24]=%u [+25]=%u  gate6=%02x%02x%02x%02x%02x%02x"
                    "  G1=%p [G1+0x94]=%p\n",
                    n, topState,
                    *((const unsigned char *)topState + 0x1d),
                    *((const unsigned char *)topState + 0x24),
                    *((const unsigned char *)topState + 0x25),
                    mg[0], mg[1], mg[2], mg[3], mg[4], mg[5], g1, g1v);
            int ftr = -2, fst = -2, ld = -2;
            gmv_menu_gates(&ftr, &fst, &ld);
            gmv_log("[state] f%u MENU gates: feTrans=%d (state=%d; 1..6 blocks) allLoaded=%d "
                    "(both must be feTrans=0 & allLoaded=1 for menu logic to run)\n",
                    n, ftr, fst, ld);
        }
    }
    gmv_input_wants_tap = (topState && *(void **)topState == (void *)(uintptr_t)0x65a388u);
    pf0 = af0;
    pb0 = b00;
    pstate = state;
    pdeque = deque_empty;
    pstk = stackN;
    prdy = rdy;
    ptop = topState;
    n++;
}
void gmv_state_trace_present(void)
{
    unsigned frames = gmv_gl_frame_no();
    /* Load-loop job probe (GMV_STATE_TRACE): the menu's page load queues jobs into the loader
       manager at __bss 0xa058e0 (its job vector is at +0x18..+0x1c, entries are job pointers,
       [job+0x14]==1 means done).  0x1adbc4 blocks the menu until all are done.  This runs in
       the nested loading loop (which presents but stops the normal SDL pump), so dump the jobs
       here to see which never complete.  All reads are guarded; harmless before the manager
       is constructed (bss is zero -> begin NULL). */
    {
        static int st = -2;
        if (st == -2) { const char *e = gmv_dbgenv("GMV_STATE_TRACE"); st = (e && *e && *e != '0') ? 1 : 0; }
        if (st && frames % 60 == 0) {
            unsigned char *mgr = (unsigned char *)(uintptr_t)0xa058e0u;
            void **beg = *(void ***)(mgr + 0x18);
            void **end = *(void ***)(mgr + 0x1c);
            long njob = beg ? (long)(end - beg) : -1;
            gmv_log("[gl] f%u JOBS mgr=%p vec=%p..%p n=%ld\n",
                    frames, (void *)mgr, (void *)beg, (void *)end, njob);
            if (beg && njob >= 0 && njob <= 128) {
                for (long i = 0; i < njob; i++) {
                    unsigned char *job = (unsigned char *)beg[i];
                    if (!job) { gmv_log("     job[%ld]=NULL\n", i); continue; }
                    gmv_log("     job[%ld]=%p done14=%d  vt=%08x f4=%08x f8=%08x fc=%08x f10=%08x\n",
                            i, (void *)job, *(int *)(job + 0x14),
                            *(unsigned *)(job + 0), *(unsigned *)(job + 4), *(unsigned *)(job + 8),
                            *(unsigned *)(job + 0xc), *(unsigned *)(job + 0x10));
                }
            }
        }
    }
}
#endif
