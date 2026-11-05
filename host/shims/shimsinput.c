/* shimsinput.c -- the game's keyboard and mouse, from SDL3's events.

   The game takes QUIT/keyboard through SDL events (joysticks it polls, shimsjoy.c), but MOUSE/TOUCH through Cocoa: its
   EAGLView (an NSView subclass) implements -mouseDown:/-mouseUp:/-mouseDragged:/-mouseMoved:,
   which Cocoa would invoke with an NSEvent.  There is no Cocoa event loop here, so we synthesise
   them: SDL_PumpEvents (which the game calls every frame) pumps SDL's events and then forwards the
   activity to the EAGLView via objc_msgSend, passing a fake NSEvent whose -locationInWindow returns the
   point in Cocoa window coordinates (origin bottom-left, so SDL's y is flipped). */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "log.h"
#include "layout.h"
#include "shims.h"
#include "veh.h"

typedef void *(*msg_t)(void *self, const char *sel, ...);

extern void *gmv_objc_last_eaglview;            /* shimsobjc.c */
extern void *gmv_objc_last_gamewindow;          /* shimsobjc.c */
extern void *gmv_objc_last_gamedelegate;        /* shimsobjc.c */

/* A game's state trace sets it when a screen waiting for a tap is on top; gmv_input_pump injects a centre tap
   there (GMV_CLICK_S3) so timing never matters. */
int gmv_input_wants_tap;

/* CUTSCENE_KEYS: the keys whose press a cutscene took (their release is kept from the game too). */
static unsigned char g_cs_keys[SDL_SCANCODE_COUNT];
static int g_alt_enter;                 /* Alt+Enter pressed: switch full screen at the next pump */

static int g_client_w = 1024, g_client_h = 768; /* for the Y flip + auto-click centre */
static int g_key_pending;                       /* +1 = became key, -1 = resigned, 0 = nothing */
static int g_key_inited;                         /* forced the initial "became key" once */

/* A small ring of pending mouse actions, drained in gmv_input_pump.
   kind: 1=Ldown 2=Lup 3=move/Ldrag 4=Rdown 5=Rup 6=Rdrag 8=scroll (d = scroll deltaY). */
struct minput { int kind; float x, y, d; };
#define MAXIN 128
static struct minput g_q[MAXIN];
static int g_head, g_tail;
static int g_button_down;                        /* left button held  */

/* Mouse-look (the camera control) -- hands-free, like a normal PC game.  The Mac build turns the
   camera from the RELATIVE mouse motion: the per-frame engine input poll (low 0x3f4a) calls
   CGGetLastMouseDelta(&dx,&dy) EVERY frame and drives the camera from that delta.  (The EAGLView
   -mouseMoved: path feeds a SEPARATE touch/pointer queue at 0x59b700 -- the menu's on-screen pointer
   -- NOT the camera.)

   The game hides the OS cursor once at startup and keeps it hidden the whole session (it draws its
   own pointer), so cursor visibility does NOT distinguish gameplay from menu.  The game's own pointer
   does (gmv_game_pointer_shown): while it shows, the mouse works as a normal one, with the system's
   speed; while it does not, the mouse is in SDL's relative mode, whose deltas are independent of the
   cursor position (even past the screen edge).  The motion goes to the engine in both, and SDL keeps
   a position in relative mode too, so a click always has a place.  While focused the cursor stays
   inside the picture, so clicks land in-game and the menu pointer never sticks at an edge; SDL lets it
   go when the window loses the focus. */
static int   g_look_dx, g_look_dy;   /* accumulated relative look delta (CG convention, y down) */
static int   g_game_cursor_hidden;   /* the game's cursor-visibility intent (NSCursor hide/unhide) */
static int   g_focus;                /* our window is the active/focused window */

/* Confine the OS cursor to the game's picture in the window.  The picture is the whole client area, or
   the scaled picture between the black bars (gmv_gl_view_rect). */
static void look_clip(void)
{
    SDL_Window *w = gmv_gl_window();
    if (!w)
        return;
    SDL_Rect r;
    gmv_gl_view_rect(&r.x, &r.y, &r.w, &r.h, NULL, NULL);
    SDL_SetWindowMouseRect(w, &r);
}

/* The game's cursor-visibility intent, set from the NSCursor hide/unhide shims (shimsfnd.c) and the
   CGDisplayHide/ShowCursor shims.  Tracked so CGCursorIsVisible can report it truthfully (which
   -[EAGLView hideCursore:] branches on); it does NOT gate the camera (the game keeps it hidden all
   session, so it is not a gameplay/menu signal). */
void gmv_input_set_cursor_hidden(int hidden)
{
    hidden = hidden ? 1 : 0;
    if (hidden != g_game_cursor_hidden) {
        g_game_cursor_hidden = hidden;
        static int tr = -1;
        if (tr < 0) { const char *e = gmv_dbgenv("GMV_INPUT_TRACE"); tr = (e && *e && *e != '0') ? 1 : 0; }
        if (tr) gmv_trace("[input] game cursor %s\n", hidden ? "HIDDEN" : "SHOWN");
    }
}

int gmv_input_is_cursor_hidden(void)
{
    return g_game_cursor_hidden;
}

/* Accumulate one relative mouse motion sample as camera look delta. */
static void look_add_raw(int dx, int dy)
{
    g_look_dx += dx;         /* CG screen convention: y DOWN, same as SDL's, so no flip */
    g_look_dy += dy;
    static int tr = -1;
    if (tr < 0) { const char *e = gmv_dbgenv("GMV_INPUT_TRACE"); tr = (e && *e && *e != '0') ? 1 : 0; }
    if (GMV_DEV && tr) { static int lc; if ((lc++ % 30) == 0)
        gmv_log("[input] raw look delta=(%d,%d) accum=(%d,%d)\n", dx, dy, g_look_dx, g_look_dy); }
}

/* Consumed by shimscg.c's CGGetLastMouseDelta: the relative mouse motion since the last call.
   Called on the game/main thread -- the same thread that pumps the events -- so no locking needed. */
void gmv_input_take_mouse_delta(int *dx, int *dy)
{
    if (dx) *dx = g_look_dx;
    if (dy) *dy = g_look_dy;
    g_look_dx = g_look_dy = 0;
}

static float g_ptr_x = 512.0f, g_ptr_y = 384.0f; /* the pointer's last place, game view units (PAD_MENU) */
static int g_pad_moved;                         /* the pad moved the pointer last, not the mouse */

static void q_push(int kind, float x, float y)
{
    int n = (g_head + 1) % MAXIN;
    if (kind <= 3) {
        g_ptr_x = x;
        g_ptr_y = y;
    }
    if (n == g_tail)
        return;                                 /* full: drop oldest-first by not pushing */
    g_q[g_head].kind = kind;
    g_q[g_head].x = x;
    g_q[g_head].y = y;
    g_q[g_head].d = 0.0f;
    g_head = n;
}

/* Queue a scroll-wheel event (deltaY in "notches", +1 per wheel step away from the user). */
static void q_push_scroll(float x, float y, float dy)
{
    int n = (g_head + 1) % MAXIN;
    if (n == g_tail)
        return;
    g_q[g_head].kind = 8;
    g_q[g_head].x = x;
    g_q[g_head].y = y;
    g_q[g_head].d = dy;
    g_head = n;
}

void gmv_input_set_client_size(int w, int h)
{
    if (w > 0) g_client_w = w;
    if (h > 0) g_client_h = h;
}

void gmv_input_view_changed(void)
{
    look_clip();
}

/* The view frame the game sees (shimsfnd.c fnd_rect_full answers -[NSView frame] with it). */

/* A client-area point -> the game's Cocoa window point.  First into the picture's own pixels (a point
   on a black bar goes to the nearest edge).  The game's -[EAGLView transformMousePoint:] (0x19a4d0)
   then turns a view point p into picture pixels as x = p.x * backingW / frame.w and
   y_down = (frame.h - p.y) * backingH / frame.h, and the frame here is always 1024x768 (on the Mac it
   is the window's size).  So hand it the point in that frame's units, bottom-left origin; then the
   game gets the right pixel at any resolution (at 640x480 it used to stop at 400,180). */
static void to_game(int x, int y, float *gx, float *gy)
{
    int vx = 0, vy = 0, vw = g_client_w, vh = g_client_h, sw = g_client_w, sh = g_client_h;
    gmv_gl_view_rect(&vx, &vy, &vw, &vh, &sw, &sh);
    float fx = (float)(x - vx) * (float)sw / (float)vw;
    float fy = (float)(y - vy) * (float)sh / (float)vh;
    if (fx < 0.0f) fx = 0.0f;
    if (fx > (float)(sw - 1)) fx = (float)(sw - 1);
    if (fy < 0.0f) fy = 0.0f;
    if (fy > (float)(sh - 1)) fy = (float)(sh - 1);
    *gx = fx * GMV_VIEW_FRAME_W / (float)sw;
    *gy = GMV_VIEW_FRAME_H - fy * GMV_VIEW_FRAME_H / (float)sh;
}

static void q_push_at(int kind, int x, int y)
{
    float gx, gy;
    to_game(x, y, &gx, &gy);
    q_push(kind, gx, gy);
}

/* --- Keyboard -------------------------------------------------------------------------------
   The game takes keyboard through SDL (not Cocoa): its per-frame event drainer (0x18a9e) loops
   SDL_PeepEvents(&ev, 1, SDL_GETEVENT, 0x300, 0x301) and, for each returned event, reads the
   type at ev+0 (0x300 KEYDOWN => pressed, else released) and the SDL *keycode* at ev+0x10
   (keysym.sym) which it looks up in its own key map (member this+0x1038) to set a key-state byte.
   So we take SDL3's key events, translate them to the keycode of the game's older SDL, queue them
   here, and hand them back through SDL_PeepEvents.  Faithful 1:1 key translation -- no
   game-specific rebinding (that would be a post-v1.0 extra; see the pure-port scope). */
struct kinput { unsigned type; int sym, scancode; };   /* type: 0x300 down / 0x301 up */
#define MAXKIN 64
static struct kinput g_kq[MAXKIN];
static int g_khead, g_ktail;

static void kq_push(unsigned type, int sym, int sc)
{
    int n = (g_khead + 1) % MAXKIN;
    if (n == g_ktail)
        return;                                 /* full: drop rather than overwrite */
    g_kq[g_khead].type = type;
    g_kq[g_khead].sym = sym;
    g_kq[g_khead].scancode = sc;
    g_khead = n;
}

/* A key of SDL3 -> SDL 1.3 keycode (keysym.sym) and USB HID scancode.  Letters and digits go by their
   label, as the Mac's SDL gave them, with the shift state left out: their ASCII keycode, and the scancode
   of the US key with that label.  Navigation / modifier / function keys map to the SDLK_* form
   (scancode | (1<<30)); left and right modifiers count as the same key.  Returns 0 (SDLK_UNKNOWN) for keys
   we don't translate. */
#define GMV_SDLK_SCAN(s)  ((int)((unsigned)(s) | (1u << 30)))

static int gmv_key_to_sdl(SDL_Scancode code, int *out_scancode)
{
    int sym = 0, sc = 0;
    SDL_Keycode k = SDL_GetKeyFromScancode(code, SDL_KMOD_NONE, true);
    if (!(SDL_GetModState() & SDL_KMOD_NUM)) {  /* without NumLock the keypad is the arrows and Delete */
        code = code == SDL_SCANCODE_KP_4 ? SDL_SCANCODE_LEFT : code == SDL_SCANCODE_KP_6 ? SDL_SCANCODE_RIGHT
             : code == SDL_SCANCODE_KP_8 ? SDL_SCANCODE_UP : code == SDL_SCANCODE_KP_2 ? SDL_SCANCODE_DOWN
             : code == SDL_SCANCODE_KP_PERIOD ? SDL_SCANCODE_DELETE : code;
    }
    if (k >= 'a' && k <= 'z') {
        sym = (int)k;
        sc  = SDL_SCANCODE_A + (int)(k - 'a');
    } else if (k >= '1' && k <= '9') {
        sym = (int)k;
        sc  = SDL_SCANCODE_1 + (int)(k - '1');
    } else if (k == '0') {
        sym = '0'; sc = SDL_SCANCODE_0;
    } else if (code >= SDL_SCANCODE_F1 && code <= SDL_SCANCODE_F12) {
        sc  = (int)code;
        sym = GMV_SDLK_SCAN(sc);
    } else switch (code) {
        case SDL_SCANCODE_SPACE:     sym = ' ';  sc = 44; break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:  sym = '\r'; sc = 40; break;
        case SDL_SCANCODE_ESCAPE:    sym = 0x1b; sc = 41; break;
        case SDL_SCANCODE_BACKSPACE: sym = '\b'; sc = 42; break;
        case SDL_SCANCODE_TAB:       sym = '\t'; sc = 43; break;
        case SDL_SCANCODE_DELETE:    sym = 0x7f; sc = 76; break;
        case SDL_SCANCODE_LEFT:      sym = GMV_SDLK_SCAN(80);  sc = 80;  break;
        case SDL_SCANCODE_RIGHT:     sym = GMV_SDLK_SCAN(79);  sc = 79;  break;
        case SDL_SCANCODE_UP:        sym = GMV_SDLK_SCAN(82);  sc = 82;  break;
        case SDL_SCANCODE_DOWN:      sym = GMV_SDLK_SCAN(81);  sc = 81;  break;
        case SDL_SCANCODE_LSHIFT:
        case SDL_SCANCODE_RSHIFT:    sym = GMV_SDLK_SCAN(225); sc = 225; break;
        case SDL_SCANCODE_LCTRL:
        case SDL_SCANCODE_RCTRL:     sym = GMV_SDLK_SCAN(224); sc = 224; break;
        case SDL_SCANCODE_LALT:
        case SDL_SCANCODE_RALT:      sym = GMV_SDLK_SCAN(226); sc = 226; break;
        default:
            if (k == ',')      { sym = ',';  sc = 54; }
            else if (k == '.') { sym = '.';  sc = 55; }
            else if (k == '-') { sym = '-';  sc = 45; }
            else if (k == '=' || k == '+') { sym = '=';  sc = 46; }
            break;
    }
    if (out_scancode) *out_scancode = sc;
    return sym;
}

/* TISGetInputSourceProperty.  The game's only use is 0x19f0b0, which asks whether the layout's id is
   com.apple.keylayout.French; the 4 input handlers then bind Z/Q to forward/left and A to what Q does
   (0x18ac4e).  Keys reach the game by their label here (gmv_key_to_sdl) as through the Mac's SDL, so this
   answer is all an AZERTY keyboard needs.  Any AZERTY counts, found by where its keys are: the Mac's test
   left out Belgian.  NULL compares unequal, the game's usual keys. */
static int kbd_is_azerty(void)
{
    static int seen = -1;
    int azerty = SDL_GetKeyFromScancode(SDL_SCANCODE_Q, SDL_KMOD_NONE, false) == 'a' &&
                 SDL_GetKeyFromScancode(SDL_SCANCODE_W, SDL_KMOD_NONE, false) == 'z';
    if (azerty != seen) {
        seen = azerty;
        gmv_log("[input] keyboard layout: %s\n", azerty ? "AZERTY, the game's French keys" : "the game's usual keys");
    }
    return azerty;
}

void *gmv_tis_input_source_property(void *source, void *key)
{
    static void *french;
    (void)source;
    (void)key;
    if (!kbd_is_azerty())
        return NULL;
    if (!french)
        french = gmv_make_nsstring("com.apple.keylayout.French");
    return french;
}

/* Fill one queued keyboard event into the game's SDL_Event buffer (see shims.h).  The game reads
   type@0 and keysym.sym@0x10; we also fill state@8 and keysym.scancode@0xc for faithfulness. */
int gmv_input_get_sdl_event(void *buf, unsigned minType, unsigned maxType)
{
    if (!buf)
        return 0;
    if (!(minType <= 0x301u && maxType >= 0x300u))   /* range must overlap keyboard events */
        return 0;
    if (g_ktail == g_khead)
        return 0;                               /* nothing queued */
    struct kinput *e = &g_kq[g_ktail];
    g_ktail = (g_ktail + 1) % MAXKIN;
    unsigned char *p = (unsigned char *)buf;
    memset(p, 0, 28);                           /* SDL_KeyboardEvent is 28 bytes through keysym */
    *(unsigned *)(p + 0x00) = e->type;                        /* type: 0x300 / 0x301 */
    p[0x08] = (unsigned char)(e->type == 0x300u ? 1 : 0);     /* state: PRESSED / RELEASED */
    *(int *)(p + 0x0c) = e->scancode;                         /* keysym.scancode */
    *(int *)(p + 0x10) = e->sym;                              /* keysym.sym (the game reads this) */
    {
        static int ktr = -1;
        if (ktr < 0) { const char *en = gmv_dbgenv("GMV_INPUT_TRACE"); ktr = (en && *en && *en != '0') ? 1 : 0; }
        if (GMV_DEV && ktr) gmv_log("[input] deliver SDL key %s sym=0x%x sc=%d\n",
                         e->type == 0x300u ? "DOWN" : "UP", (unsigned)e->sym, e->scancode);
    }
    return 1;
}

/* PAD_HINTS: when the keyboard or the mouse buttons were last used. */
static unsigned g_kbm_tick;
static unsigned g_presses;              /* keys and mouse buttons gone down, modifiers and Tab left out */

unsigned gmv_input_last_key(void) { return g_kbm_tick; }
unsigned gmv_input_tick(void) { return (unsigned)SDL_GetTicks(); }
unsigned gmv_input_presses(void) { return g_presses; }
int gmv_input_scancode_held(int scancode)
{
    int n = 0;
    const bool *keys = SDL_GetKeyboardState(&n);
    return scancode >= 0 && scancode < n && keys[scancode];
}

/* GMV_SHOW_CURSOR=1 keeps the system's arrow visible even when the game hid its own cursor
   (debugging aid); by default we hide it to avoid the double-cursor during play. */
static int show_os_cursor(void)
{
    static int v = -1;
    if (v < 0) { const char *e = gmv_getenv("GMV_SHOW_CURSOR"); v = (e && *e && *e != '0') ? 1 : 0; }
    return v;
}

/* Hide the system's arrow over the window whenever the game has hidden its own cursor, so the user never
   sees two cursors at once.  On the title bar / borders (windowed) the system draws its arrow as usual. */
static void cursor_sync(void)
{
    static int off;
    int want = gmv_input_is_cursor_hidden() && !show_os_cursor();
    if (want != off && gmv_gl_window()) {
        off = want;
        if (want)
            SDL_HideCursor();
        else
            SDL_ShowCursor();
    }
}

/* The mouse is the camera while the game shows no pointer, a pointer while it does. */
static void look_mode_sync(void)
{
    static int relative;
    SDL_Window *w = gmv_gl_window();
    int want = !gmv_game_pointer_shown();
    if (!w || want == relative)
        return;
    relative = want;
    if (!SDL_SetWindowRelativeMouseMode(w, want))
        gmv_log("[input] relative mouse failed (%s)\n", SDL_GetError());
    gmv_trace("[input] the mouse is the %s\n", want ? "camera" : "pointer");
}

static int g_move_pending, g_move_x, g_move_y;  /* the last move not queued yet */

/* Moves come far more often than frames in relative mode, and the game needs the last one only. */
static void move_flush(void)
{
    if (g_move_pending) {
        g_move_pending = 0;
        /* input_dispatch picks -mouseDragged: when the left button is down, else -mouseMoved:. */
        q_push_at(3, g_move_x, g_move_y);
    }
}

static void input_key_down(const SDL_KeyboardEvent *e)
{
    if (e->repeat || e->scancode >= SDL_SCANCODE_COUNT) /* the game is edge/state based: the first press only */
        return;
    int quiet = e->scancode == SDL_SCANCODE_TAB || e->scancode == SDL_SCANCODE_CAPSLOCK ||
                (e->scancode >= SDL_SCANCODE_LCTRL && e->scancode <= SDL_SCANCODE_RGUI);
    if (!quiet)
        g_presses++;
    /* Alt+Enter: full screen <-> window, done in gmv_input_pump after the queue is drained (the mode
       switch resizes this window).  The Enter is kept from the game, its release too, so it does not
       also press a menu button. */
    if ((e->scancode == SDL_SCANCODE_RETURN || e->scancode == SDL_SCANCODE_KP_ENTER) && (e->mod & SDL_KMOD_ALT)) {
        g_alt_enter = 1;
        g_cs_keys[e->scancode] = 1;
        return;
    }
    g_kbm_tick = gmv_input_tick() | 1;
    int sc = 0, sym = gmv_key_to_sdl(e->scancode, &sc);
    if (sym && gmv_game_key((unsigned)sym)) {
        g_cs_keys[e->scancode] = 1;             /* CUTSCENE_KEYS took it: its release stays ours too */
        sym = 0;
    }
    if (sym)
        kq_push(0x300u, sym, sc);
}

static void input_key_up(const SDL_KeyboardEvent *e)
{
    if (e->scancode >= SDL_SCANCODE_COUNT)
        return;
    int sc = 0, sym = gmv_key_to_sdl(e->scancode, &sc);
    if (g_cs_keys[e->scancode]) {
        g_cs_keys[e->scancode] = 0;
        sym = 0;
    }
    if (sym)
        kq_push(0x301u, sym, sc);
}

static void input_button(const SDL_MouseButtonEvent *e)
{
    int x = (int)e->x, y = (int)e->y;
    move_flush();
    if (e->down) {
        g_presses++;
        if (e->button == SDL_BUTTON_LEFT || e->button == SDL_BUTTON_RIGHT)
            g_kbm_tick = gmv_input_tick() | 1;
    }
    if (e->button == SDL_BUTTON_LEFT) {
        g_button_down = e->down;
        q_push_at(e->down ? 1 : 2, x, y);
    } else if (e->button == SDL_BUTTON_RIGHT) {
        /* The game's view implements -rightMouseDown: (IMP 0x199800) -> sendMouseEvent:sendedButton:1
           isDown:1, i.e. the engine's auto-target / lock-on action used in on-foot and drive-by combat
           (left click is sendedButton:0).  Deliver it like the left button (same Y-flip). */
        q_push_at(e->down ? 4 : 5, x, y);
    }
}

static void input_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        input_key_down(&e->key);
        break;
    case SDL_EVENT_KEY_UP:
        input_key_up(&e->key);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        input_button(&e->button);
        break;
    case SDL_EVENT_MOUSE_MOTION: {
        static int last_mx = -1, last_my = -1;
        int mx = (int)e->motion.x, my = (int)e->motion.y;
        look_add_raw((int)e->motion.xrel, (int)e->motion.yrel);
        /* The system also reports the mouse when it has not moved, as windows change; that must not
           take the pointer back from the pad (PAD_MENU). */
        if (g_pad_moved && mx == last_mx && my == last_my)
            break;
        g_pad_moved = 0;
        last_mx = mx;
        last_my = my;
        /* Forward the moves for the front-end menu's on-screen cursor (the yellow pointer), which
           tracks -mouseMoved: (absolute position) and selects the button under it -- independent of
           the camera's relative-delta path above. */
        g_move_pending = 1;
        g_move_x = mx;
        g_move_y = my;
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        float gx, gy;
        move_flush();
        to_game((int)e->wheel.mouse_x, (int)e->wheel.mouse_y, &gx, &gy);
        q_push_scroll(gx, gy, e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e->wheel.y : e->wheel.y);
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        g_focus = e->type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        g_key_pending = g_focus ? 1 : -1;
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        gmv_input_set_client_size(e->window.data1, e->window.data2);
        look_clip();                            /* the picture may lie elsewhere now */
        break;
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        /* Alt+F4 or the window's close button.  The Mac's close (-windowWillClose: 0x19af50) sends
           terminate:, which ends the process without saving; the same here (fnd_NSApp_terminate). */
        gmv_log("[input] window closed (Alt+F4 or the close button): quitting\n");
        log_close();
        _Exit(0);
    default:
        break;
    }
}

/* While the player drags the window's border, Windows keeps this thread inside SDL_PumpEvents until
   the mouse is let go, so the game draws nothing and the new part of the window would stay black.  SDL
   sends expose events from inside that loop, and a watch sees them at once.  Only during the pump: the
   screen FBO holds a whole frame there, and SDL also sends them from calls the game makes mid-frame. */
static volatile int g_pumping;

static bool SDLCALL redraw_watch(void *userdata, SDL_Event *e)
{
    (void)userdata;
    if (e->type == SDL_EVENT_WINDOW_EXPOSED && g_pumping)
        gmv_gl_redraw();
    return true;
}

/* The events since the last call.  With skip (the intro movie): a skip key or a click sets it and is kept
   from the game. */
void gmv_input_drain(int *skip)
{
    static int watching;
    SDL_Event e;
    if (!watching)
        watching = SDL_AddEventWatch(redraw_watch, NULL);
    g_pumping = 1;
    SDL_PumpEvents();
    g_pumping = 0;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 1) {
        if (skip) {
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && (e.key.scancode == SDL_SCANCODE_ESCAPE ||
                e.key.scancode == SDL_SCANCODE_SPACE || e.key.scancode == SDL_SCANCODE_RETURN ||
                e.key.scancode == SDL_SCANCODE_KP_ENTER)) {
                *skip = 1;
                continue;
            }
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                *skip = 1;
                continue;
            }
        }
        input_event(&e);
    }
    move_flush();
}

/* Deliver the captured mouse actions to the game's EAGLView.  Called on the main thread (from
   SDL_PumpEvents), so running game code here is fine.  noinline + force_align so the realignment
   actually happens at a real call boundary (an inlined force-aligned fn loses its realignment). */
__attribute__((noinline, force_align_arg_pointer))
/* The game gates mouse input on the window being "key": -[GameWindow windowDidBecomeKey:] sets a
   byte flag (this address) to 1, -windowDidResignKey: to 0, and -sendMouseEvent: drops the event
   unless it is 1.  macOS calls those via the Cocoa event loop on activation; we have none, so we
   write the flag directly to mirror the window's active state. (Fixed address in the game's
   __DATA, from the instruction `mov byte [pic+0x4ef670], 1` at 0x19b03c.) */
#define GMV_KEYWINDOW_FLAG ((volatile unsigned char *)(uintptr_t)GMV_KEYWINDOW_FLAG_ADDR)

static void input_dispatch(void)
{
    msg_t msg = (msg_t)(void *)gmv_objc_msgSend;

    if (!g_key_inited && gmv_objc_last_gamewindow) {
        g_key_inited = 1;
        g_key_pending = 1;                      /* our single window starts active */
    }
    if (g_key_pending) {
        if (GMV_KEYWINDOW_FLAG_ADDR)
            *GMV_KEYWINDOW_FLAG = (g_key_pending > 0) ? 1 : 0;
        g_key_pending = 0;
    }

    void *view = gmv_objc_last_eaglview;
    if (!view)
        return;
#if GMV_DEV
    static int itrace = -1;
    if (itrace < 0) { const char *e = gmv_dbgenv("GMV_INPUT_TRACE"); itrace = (e && *e && *e != '0') ? 1 : 0; }
#else
    enum { itrace = 0 };
#endif
    while (g_tail != g_head) {
        struct minput *e = &g_q[g_tail];
        g_tail = (g_tail + 1) % MAXIN;
        const char *sel;
        void *ev;
        if (e->kind == 8) {
            sel = "scrollWheel:";
            ev  = gmv_make_nsscroll(e->x, e->y, e->d);
        } else {
            ev  = gmv_make_nsevent(e->x, e->y);
            sel = e->kind == 1 ? "mouseDown:" :
                  e->kind == 2 ? "mouseUp:" :
                  e->kind == 4 ? "rightMouseDown:" :
                  e->kind == 5 ? "rightMouseUp:" :
                  e->kind == 6 ? "rightMouseDragged:" :
                  (g_button_down ? "mouseDragged:" : "mouseMoved:");
        }
        /* Only deliver a selector the view actually implements: an EAGLView has -mouseDown:/-mouseUp:/
           -mouseDragged:/-mouseMoved: but NOT -rightMouse*:/-scrollWheel:, and our objc runtime stops
           the process on an unhandled message.  So right-mouse/scroll become harmless no-ops here
           rather than a force-close, and work only if some view does implement them. */
        if (!gmv_objc_responds(view, sel)) {
            static int warned_kind[9];
            if (itrace && e->kind < 9 && !warned_kind[e->kind]) {
                warned_kind[e->kind] = 1;
                gmv_log("[input] view does not implement -%s; dropping (no-op)\n", sel);
            }
            continue;
        }
        if (itrace && e->kind != 3 && e->kind != 6)   /* log taps/scroll; skip the noisy moves/drags */
            gmv_log("[input] %-18s cocoa=(%.0f,%.0f) d=%.2f view=%p keyWin=%u\n",
                    sel, e->x, e->y, e->d, view, GMV_KEYWINDOW_FLAG_ADDR ? (unsigned)*GMV_KEYWINDOW_FLAG : 0u);
        msg(view, sel, ev);
    }
}

/* PAD_MENU: while the game shows its menu pointer, the pad works it like a mouse.  The left stick or
   the D-pad moves it (speed grows with the push's square, up to PM_SPEED view units a second), A clicks
   (down while held, so a drag works too) and B is an Esc press.  The pad's own events still reach the
   game as before; the menus act only on Esc and pointer events. */
#define PM_SPEED 900.0f
#define PM_BTN_A 1
#define PM_BTN_B 2

static void pad_pointer(void)
{
    static uint64_t last;
    static unsigned prev;
    static int active, a_down;
    uint64_t now = SDL_GetPerformanceCounter();
    float x, y, dt, m;
    unsigned b;
    int hat, on = gmv_game_pad_pointer() && g_focus;

    dt = last ? (float)(now - last) / (float)SDL_GetPerformanceFrequency() : 0.0f;
    last = now;
    if (dt > 0.1f)
        dt = 0.1f;
    if (on != active) {
        gmv_trace("[padmenu] pointer %s\n", on ? "on (the game shows its pointer)" : "off");
        active = on;
        if (!on && a_down) {
            a_down = 0;
            g_button_down = 0;
            q_push(2, g_ptr_x, g_ptr_y);
        }
    }
    if (!on || !gmv_joy_peek(&x, &y, &b, &hat)) {
        prev = 0;
        return;
    }
    if (hat & 1) y = -1.0f;
    if (hat & 4) y = 1.0f;
    if (hat & 2) x = 1.0f;
    if (hat & 8) x = -1.0f;
    m = x * x + y * y;
    if (m > 0.0f) {
        float k = (m > 1.0f ? 1.0f / sqrtf(m) : sqrtf(m)) * PM_SPEED * dt;   /* |v| = push² */
        float nx = g_ptr_x + x * k, ny = g_ptr_y - y * k;                 /* the view is y up */
        nx = nx < 0.0f ? 0.0f : nx > GMV_VIEW_FRAME_W - 1.0f ? GMV_VIEW_FRAME_W - 1.0f : nx;
        ny = ny < 1.0f ? 1.0f : ny > GMV_VIEW_FRAME_H ? GMV_VIEW_FRAME_H : ny;
        if (nx != g_ptr_x || ny != g_ptr_y) {
            q_push(3, nx, ny);
            g_pad_moved = 1;
        }
    }
    if ((b & ~prev) & (1u << PM_BTN_A)) {
        a_down = 1;
        g_button_down = 1;
        q_push(1, g_ptr_x, g_ptr_y);
    } else if ((prev & ~b) & (1u << PM_BTN_A) && a_down) {
        a_down = 0;
        g_button_down = 0;
        q_push(2, g_ptr_x, g_ptr_y);
    }
    if ((b & ~prev) & (1u << PM_BTN_B))
        kq_push(0x300u, 0x1b, 41);
    if ((prev & ~b) & (1u << PM_BTN_B))
        kq_push(0x301u, 0x1b, 41);
    prev = b;
}

/* Pump SDL's events (keeps the window responsive), then deliver the captured mouse activity.
   Called from SDL_PumpEvents each frame.  GMV_TEST_CLICK=<frame> injects one synthetic click at the
   window centre at that frame (for testing without a real mouse). */
__attribute__((force_align_arg_pointer))
void gmv_input_pump(void)
{
    /* One-shot (GMV_WATCH=1): arm a hw write-watchpoint on the MainMenu fe-screen STATE field
       [0xa9dc34+0x18]=0xa9dc4c, which goes 2 (active) -> 0 (switched away) within a frame of the
       menu loading.  Arm it LATE -- the moment the rating splash becomes top (gmv_input_wants_tap, set by
       state_trace the previous frame) -- so the ~100 captured writes are the menu-entry ones, not
       early-init noise.  Needs GMV_STATE_TRACE=1 (sets gmv_input_wants_tap) and GMV_CLICK_S3=1 (dismisses
       the splash so the menu actually loads).  GMV_WATCH_ADDR=<hex> overrides the watched field. */
#if GMV_DEV
    static int watch_armed, watch_mode = -1;
    if (watch_mode < 0) {
        const char *e = gmv_dbgenv("GMV_WATCH");
        watch_mode = (e && *e && *e != '0') ? 1 : 0;
    }
    if (watch_mode == 1 && !watch_armed && gmv_input_wants_tap) {
        const char *a = gmv_dbgenv("GMV_WATCH_ADDR");
        if (a && *a) gmv_watch_arm((uint32_t)strtoul(a, NULL, 16));
        watch_armed = 1;
    }
#endif
    gmv_input_drain(NULL);
    cursor_sync();
    look_mode_sync();
    if (g_alt_enter) {
        g_alt_enter = 0;
        gmv_game_alt_enter();
    }
    gmv_game_full_screen_tick();

#if GMV_DEV
    static long click_at = -2;
    static unsigned frame;
    if (click_at == -2) {
        const char *e = gmv_dbgenv("GMV_TEST_CLICK");
        click_at = e ? atol(e) : -1;
    }
    if (click_at >= 0) {
        float cx = GMV_VIEW_FRAME_W / 2.0f, cy = GMV_VIEW_FRAME_H / 2.0f;   /* view-frame units */
        if ((long)frame == click_at) {
            gmv_log("[input] TEST click down at centre (%.0f,%.0f)\n", cx, cy);
            g_button_down = 1;
            q_push(1, cx, cy);
        } else if ((long)frame == click_at + 6) {
            gmv_log("[input] TEST click up\n");
            g_button_down = 0;
            q_push(2, cx, cy);
        }
    }
    {   /* GMV_TEST_FATAL: each way of stopping, about 10 s into the game */
        static int fatal_kind = -1;
        if (fatal_kind < 0) {
            const char *e = gmv_dbgenv("GMV_TEST_FATAL");
            fatal_kind = e ? atoi(e) : 0;
        }
        if (fatal_kind && frame == 600) {
            void gmv_missing_import(uint32_t index, uint32_t ret);
            gmv_log("[input] TEST_FATAL=%d\n", fatal_kind);
            static volatile uintptr_t bad = 4;      /* volatile: the compiler must not see the address */
            if (fatal_kind == 1)
                *(volatile int *)bad = 1;
            else if (fatal_kind == 2)
                gmv_missing_import(0, 0);
            else if (fatal_kind == 3)
                __asm__("ud2");
            else if (fatal_kind == 4)
                SDL_CreateThread((SDL_ThreadFunction)(uintptr_t)4, "fatal", NULL);
        }
    }
    frame++;
    {   /* Direct pump-rate counter (independent of state_trace's logging) to resolve whether the
           SDL pump itself stalls vs just its logging. Compare to "presented frame N". */
        static int lg = -2;
        if (lg == -2) { const char *e = gmv_dbgenv("GMV_STATE_TRACE"); lg = (e && *e && *e != '0') ? 1 : 0; }
        if (lg && frame % 120 == 0)
            gmv_log("[input] PUMP call #%u\n", frame);
    }

    /* State-triggered tap (GMV_CLICK_S3=1): the rating-splash state3 waits for a tap to set
       [state3+0xc]=1 (dismiss).  Frame timing is unreliable (pump rate diverges from presents),
       so inject a centre down/up the moment state3 becomes the top state, regardless of frame. */
    static int s3_mode = -2, s3_phase;
    if (s3_mode == -2) {
        const char *e = gmv_dbgenv("GMV_CLICK_S3");
        s3_mode = (e && *e && *e != '0') ? 0 : -1;
    }
    if (s3_mode == 0) {
        float cx = GMV_VIEW_FRAME_W / 2.0f, cy = GMV_VIEW_FRAME_H / 2.0f;   /* view-frame units */
        if (s3_phase == 0 && gmv_input_wants_tap) {
            gmv_log("[input] S3 auto-click DOWN at centre (%.0f,%.0f)\n", cx, cy);
            g_button_down = 1;
            q_push(1, cx, cy);
            s3_phase = 1;
        } else if (s3_phase >= 1 && s3_phase < 8) {
            s3_phase++;
        } else if (s3_phase == 8) {
            gmv_log("[input] S3 auto-click UP\n");
            g_button_down = 0;
            q_push(2, cx, cy);
            s3_phase = 9;
        }
    }
#endif

    gmv_game_input_pump();
    pad_pointer();                              /* PAD_MENU */
    input_dispatch();
    gmv_game_input_tick();
}
