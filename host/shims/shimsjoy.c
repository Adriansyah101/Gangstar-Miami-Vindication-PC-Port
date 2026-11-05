/* shimsjoy.c -- the SDL 1.3 joystick API the game calls, over SDL3.

   The game reads controllers only by polling SDL (input device 0x1a466): every frame 0x191a0 drains
   the SDL joystick events (0x600..0x604) unread, calls SDL_JoystickUpdate, then for each opened
   joystick turns GetButton / GetAxis / GetHat into its own input events (button 0x1e, axis 0x1c,
   stick pair 0x1b, hat 0x1d).  0x1a230 rescans: Init(JOYSTICK), close 0..3, QuitSubSystem,
   InitSubSystem, open 0..3 (only four are ever opened).  It runs at startup, whenever the input mode
   flips between keyboard/mouse and pad, and when the gamepad options page opens; the "Gamepad RESET"
   function 0x1a2d8 that would rescan otherwise is never called, so a pad plugged in later is picked
   up at one of those points, as on the Mac.

   The layout the game expects is a DirectInput-style pad (Logitech Dual Action, DualShock 4):
     - axes 0/1 = left stick (walk, steer), 2/3 = right stick (camera), down/right positive (0x12e400);
     - buttons 0..11 = X A B Y LB RB LT RT Back Start LS RS: the calibration page (0x17ede0) asks for
       them in that order and the default calibration of every control scheme is identity (0x188d82);
     - the d-pad as hat 0 with the SDL bits up 1, right 2, down 4, left 8 (jump table at 0x19b2c).
   Calibration only accepts buttons, so triggers are reported as buttons 6/7.  A pad SDL knows the
   buttons of (its gamepad API) is put in that layout, whatever it is and however it is connected;
   SDL's west/south/east/north are X A B Y and Square Cross Circle Triangle alike.  Any other device
   is passed through as its first four axes, its buttons and its first hat.

   The game's own dead zone is +-1000 of 32767, made for 8-bit pads that rest at 0; modern sticks rest
   further out, and any reading past it switches the game to pad mode.  So sticks get a radial dead
   zone here (XInput's recommended values on Xbox pads) and are rescaled to full range outside it. */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "log.h"
#include "shims.h"

#define GAME_INIT_JOYSTICK 0x200u       /* the flag as the game passes it */
#define JOY_MAX      8
#define JOY_AXES     4
#define JOY_BUTTONS  32
#define XBOX_LEFT_DEADZONE  7849        /* of 32767, XInput's recommended values */
#define XBOX_RIGHT_DEADZONE 8689
#define OTHER_DEADZONE      4000
#define TRIGGER_DOWN (30 * 32767 / 255) /* XInput's threshold, on SDL's 0..32767 */
#define LAYOUT_LT 6
#define LAYOUT_RT 7

typedef struct joy {
    SDL_JoystickID id;                  /* 0 = a free entry */
    SDL_Gamepad *pad;                   /* a pad SDL knows the buttons of */
    SDL_Joystick *raw;                  /* any other device */
    int style;                          /* see joy_style */
    int naxes, nbuttons, nhats;
    int opened;                         /* by the game */
    int16_t axis[JOY_AXES];
    uint8_t button[JOY_BUTTONS];
    uint8_t hat;
} joy;

static joy g_joy[JOY_MAX];              /* entries past g_njoy may still hold an SDL handle, kept for the next scan */
static int g_njoy, g_inited, g_sdl;
static char g_last_list[512];
static int g_style = -1;                /* gmv_joy_style; -1 = no pad seen yet */

/* PAD_ICONS: 1 Xbox, 2 DualShock 4 (and Sony's older pads), 3 DualSense, 0 any other pad. */
static int joy_style(SDL_JoystickID id)
{
    switch (SDL_GetGamepadTypeForID(id)) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE:
        return 1;
    case SDL_GAMEPAD_TYPE_PS4:
        return 2;
    case SDL_GAMEPAD_TYPE_PS5:
        return 3;
    default:
        return SDL_GetJoystickVendorForID(id) == 0x054C ? 2 : 0;
    }
}

static int joy_used(const joy *j)
{
    for (int i = 0; i < j->nbuttons; i++)
        if (j->button[i])
            return 1;
    for (int i = 0; i < j->naxes; i++)
        if (j->axis[i] > 8000 || j->axis[i] < -8000)
            return 1;
    return j->hat != 0;
}

#if GMV_DEV
/* PAD_TEST (dev builds): a virtual pad, so that tools\playtest can play without a real one.  xbox,
   ds4 and ds5 are gamepads with those ids, raw a device SDL has no mapping for.  Its state is the
   text of cache\padtest.txt, read as it changes: names of what is held (X A B Y LB RB LT RT BACK
   START LS RS PS TOUCHPAD UP DOWN LEFT RIGHT), lx= ly= rx= ry= (-32768..32767), and plug / unplug.
   Each change the game's readings show is logged. */
static const char *g_test_kind;
static SDL_Joystick *g_test;

static void test_plug(void)
{
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    int gamepad = strcmp(g_test_kind, "raw") != 0;
    d.type = gamepad ? SDL_JOYSTICK_TYPE_GAMEPAD : SDL_JOYSTICK_TYPE_UNKNOWN;
    d.naxes = gamepad ? SDL_GAMEPAD_AXIS_COUNT : 4;
    d.nbuttons = gamepad ? SDL_GAMEPAD_BUTTON_COUNT : 12;
    d.nhats = gamepad ? 0 : 1;
    d.vendor_id = !strcmp(g_test_kind, "xbox") ? 0x045E : gamepad ? 0x054C : 0xF00D;
    d.product_id = !strcmp(g_test_kind, "xbox") ? 0x028E : !strcmp(g_test_kind, "ds4") ? 0x09CC
                 : !strcmp(g_test_kind, "ds5") ? 0x0CE6 : 0x0001;
    d.name = "PAD_TEST";
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
    g_test = id ? SDL_OpenJoystick(id) : NULL;
    gmv_log("[joy] test: virtual %s pad %s\n", g_test_kind, g_test ? "plugged in" : SDL_GetError());
}

static void test_apply(char *text)
{
    static const struct { const char *name; int pad, raw; } names[] = {
        {"X", SDL_GAMEPAD_BUTTON_WEST, 0}, {"A", SDL_GAMEPAD_BUTTON_SOUTH, 1},
        {"B", SDL_GAMEPAD_BUTTON_EAST, 2}, {"Y", SDL_GAMEPAD_BUTTON_NORTH, 3},
        {"LB", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 4}, {"RB", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 5},
        {"BACK", SDL_GAMEPAD_BUTTON_BACK, 8}, {"START", SDL_GAMEPAD_BUTTON_START, 9},
        {"LS", SDL_GAMEPAD_BUTTON_LEFT_STICK, 10}, {"RS", SDL_GAMEPAD_BUTTON_RIGHT_STICK, 11},
        {"PS", SDL_GAMEPAD_BUTTON_GUIDE, -1}, {"TOUCHPAD", SDL_GAMEPAD_BUTTON_TOUCHPAD, -1},
        {"UP", SDL_GAMEPAD_BUTTON_DPAD_UP, -1}, {"DOWN", SDL_GAMEPAD_BUTTON_DPAD_DOWN, -1},
        {"LEFT", SDL_GAMEPAD_BUTTON_DPAD_LEFT, -1}, {"RIGHT", SDL_GAMEPAD_BUTTON_DPAD_RIGHT, -1},
    };
    static const struct { const char *name; int axis; } sticks[] = {{"lx=", 0}, {"ly=", 1}, {"rx=", 2}, {"ry=", 3}};
    int gamepad = strcmp(g_test_kind, "raw") != 0;
    int held[sizeof names / sizeof names[0]] = {0}, ax[4] = {0}, lt = 0, rt = 0, hat = 0;
    for (char *t = strtok(text, " \t\r\n"); t; t = strtok(NULL, " \t\r\n")) {
        if (!strcmp(t, "unplug") && g_test) {
            SDL_JoystickID id = SDL_GetJoystickID(g_test);
            SDL_CloseJoystick(g_test);
            g_test = NULL;
            SDL_DetachVirtualJoystick(id);
            gmv_log("[joy] test: virtual pad unplugged\n");
        } else if (!strcmp(t, "plug") && !g_test) {
            test_plug();
        }
        lt |= !strcmp(t, "LT");
        rt |= !strcmp(t, "RT");
        for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
            held[i] |= !strcmp(t, names[i].name);
        for (unsigned i = 0; i < 4; i++)
            if (!strncmp(t, sticks[i].name, 3))
                ax[sticks[i].axis] = atoi(t + 3);
    }
    if (!g_test)
        return;
    /* SDL's order for a gamepad is left x y, right x y; a raw device is the layout itself */
    for (int i = 0; i < 4; i++)
        SDL_SetJoystickVirtualAxis(g_test, i, (Sint16)ax[i]);
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (gamepad)
            SDL_SetJoystickVirtualButton(g_test, names[i].pad, held[i]);
        else if (names[i].raw >= 0)
            SDL_SetJoystickVirtualButton(g_test, names[i].raw, held[i]);
        else if (held[i] && names[i].pad >= SDL_GAMEPAD_BUTTON_DPAD_UP)
            hat |= names[i].pad == SDL_GAMEPAD_BUTTON_DPAD_UP ? SDL_HAT_UP
                 : names[i].pad == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? SDL_HAT_DOWN
                 : names[i].pad == SDL_GAMEPAD_BUTTON_DPAD_LEFT ? SDL_HAT_LEFT
                 : names[i].pad == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ? SDL_HAT_RIGHT : 0;
    }
    if (gamepad) {
        SDL_SetJoystickVirtualAxis(g_test, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, lt ? 32767 : -32768);
        SDL_SetJoystickVirtualAxis(g_test, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, rt ? 32767 : -32768);
    } else {
        SDL_SetJoystickVirtualButton(g_test, LAYOUT_LT, lt);
        SDL_SetJoystickVirtualButton(g_test, LAYOUT_RT, rt);
        SDL_SetJoystickVirtualHat(g_test, 0, (Uint8)hat);
    }
}

static void test_poll(void)
{
    static char path[640], last[256];
    static Uint64 next;
    char text[256];
    if (!g_test_kind || SDL_GetTicks() < next)
        return;
    next = SDL_GetTicks() + 20;
    if (!path[0])
        snprintf(path, sizeof path, "%scache/padtest.txt", SDL_GetBasePath());
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    if (strcmp(text, last)) {
        strcpy(last, text);
        test_apply(text);
        SDL_UpdateGamepads();           /* a virtual pad's new state shows at the next update */
    }
}

static void test_log(const joy *j)
{
    static joy seen;
    if (!g_test_kind || (!memcmp(seen.axis, j->axis, sizeof j->axis) && !memcmp(seen.button, j->button, sizeof j->button) && seen.hat == j->hat))
        return;
    seen = *j;
    unsigned b = 0;
    for (int i = 0; i < j->nbuttons && i < 32; i++)
        b |= (unsigned)j->button[i] << i;
    gmv_log("[joy] test: buttons=0x%x hat=%d axes=%d,%d,%d,%d style=%d\n", b, j->hat, j->axis[0], j->axis[1], j->axis[2], j->axis[3], g_style);
}
#else
#define test_poll() ((void)0)
#define test_log(j) ((void)0)
#endif

/* Every entry that was found before keeps its handle if the device is still there: the game rescans
   on each keyboard <-> pad switch, and opening a pad again is not free. */
static void joy_scan(void)
{
    char list[512];
    int len = 0, count = 0;
    joy old[JOY_MAX];
    memcpy(old, g_joy, sizeof old);
    memset(g_joy, 0, sizeof g_joy);
    g_njoy = 0;
    list[0] = 0;

    SDL_UpdateGamepads();
    SDL_JoystickID *ids = g_sdl ? SDL_GetJoysticks(&count) : NULL;
    for (int i = 0; i < count && g_njoy < JOY_MAX; i++) {
        joy *j = &g_joy[g_njoy];
        const char *name = SDL_GetJoystickNameForID(ids[i]);
        for (int k = 0; k < JOY_MAX; k++)
            if (old[k].id == ids[i]) {
                j->pad = old[k].pad;
                j->raw = old[k].raw;
                old[k].id = 0;
            }
        if (!j->pad && !j->raw) {
            if (SDL_IsGamepad(ids[i]))
                j->pad = SDL_OpenGamepad(ids[i]);
            else
                j->raw = SDL_OpenJoystick(ids[i]);
        }
        if (!j->pad && !j->raw) {
            gmv_log("[joy] could not open \"%s\": %s\n", name ? name : "?", SDL_GetError());
            continue;
        }
        j->id = ids[i];
        j->style = joy_style(ids[i]);
        if (j->pad) {
            j->naxes = 4;
            j->nbuttons = j->style >= 2 ? 14 : 12;  /* Sony's pads also have PS and the touchpad */
            j->nhats = 1;
        } else {
            int a = SDL_GetNumJoystickAxes(j->raw), b = SDL_GetNumJoystickButtons(j->raw);
            j->naxes = a < JOY_AXES ? a : JOY_AXES;
            j->nbuttons = b < JOY_BUTTONS ? b : JOY_BUTTONS;
            j->nhats = SDL_GetNumJoystickHats(j->raw) > 0 ? 1 : 0;
        }
        if (len < (int)sizeof list)
            len += snprintf(list + len, sizeof list - len, " %d=\"%s\" (%04x:%04x, %s, %d axes, %d buttons%s)",
                            g_njoy, name ? name : "?", SDL_GetJoystickVendorForID(ids[i]), SDL_GetJoystickProductForID(ids[i]),
                            j->pad ? "gamepad" : "joystick", j->naxes, j->nbuttons, j->nhats ? ", hat" : "");
        g_njoy++;
    }
    SDL_free(ids);
    for (int k = 0; k < JOY_MAX; k++)   /* unplugged since the last scan */
        if (old[k].id) {
            if (old[k].pad)
                SDL_CloseGamepad(old[k].pad);
            else
                SDL_CloseJoystick(old[k].raw);
        }
    /* The game rescans on every keyboard <-> pad switch; log only when the list changes. */
    if (strcmp(list, g_last_list)) {
        gmv_log("[joy] %d controller(s):%s\n", g_njoy, g_njoy ? list : " none");
        strcpy(g_last_list, list);
    }
    if (g_style < 0 && g_njoy)
        g_style = g_joy[0].style;
}

/* Radial dead zone, rescaled so the edge of the zone reads 0 and full deflection 32767. */
static void stick(int *x, int *y, int dz)
{
    double fx = *x, fy = *y, m = sqrt(fx * fx + fy * fy);
    if (m <= dz) {
        *x = *y = 0;
        return;
    }
    double k = ((m > 32767.0 ? 32767.0 : m) - dz) / (32767.0 - dz) * 32767.0 / m;
    *x = (int)lrint(fx * k);
    *y = (int)lrint(fy * k);
}

static int16_t clamp16(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

/* Reads a pad into j; one that was unplugged reads as everything at rest. */
static void joy_read(joy *j)
{
    int a[4];
    memset(j->button, 0, sizeof j->button);
    j->hat = 0;
    if (j->pad) {
        static const int8_t order[14] = {
            SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_NORTH,
            SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, -1, -1,
            SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
            SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_TOUCHPAD,
        };
        a[0] = SDL_GetGamepadAxis(j->pad, SDL_GAMEPAD_AXIS_LEFTX);
        a[1] = SDL_GetGamepadAxis(j->pad, SDL_GAMEPAD_AXIS_LEFTY);
        a[2] = SDL_GetGamepadAxis(j->pad, SDL_GAMEPAD_AXIS_RIGHTX);
        a[3] = SDL_GetGamepadAxis(j->pad, SDL_GAMEPAD_AXIS_RIGHTY);
        for (int i = 0; i < j->nbuttons; i++)
            j->button[i] = order[i] >= 0 && SDL_GetGamepadButton(j->pad, (SDL_GamepadButton)order[i]);
        j->button[LAYOUT_LT] = SDL_GetGamepadAxis(j->pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > TRIGGER_DOWN;
        j->button[LAYOUT_RT] = SDL_GetGamepadAxis(j->pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > TRIGGER_DOWN;
        j->hat = (SDL_GetGamepadButton(j->pad, SDL_GAMEPAD_BUTTON_DPAD_UP) ? SDL_HAT_UP : 0) |
                 (SDL_GetGamepadButton(j->pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ? SDL_HAT_RIGHT : 0) |
                 (SDL_GetGamepadButton(j->pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN) ? SDL_HAT_DOWN : 0) |
                 (SDL_GetGamepadButton(j->pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT) ? SDL_HAT_LEFT : 0);
    } else {
        for (int i = 0; i < 4; i++)
            a[i] = i < j->naxes ? SDL_GetJoystickAxis(j->raw, i) : 0;
        for (int i = 0; i < j->nbuttons; i++)
            j->button[i] = SDL_GetJoystickButton(j->raw, i);
        if (j->nhats)
            j->hat = SDL_GetJoystickHat(j->raw, 0);
    }
    stick(&a[0], &a[1], j->style == 1 ? XBOX_LEFT_DEADZONE : OTHER_DEADZONE);
    stick(&a[2], &a[3], j->style == 1 ? XBOX_RIGHT_DEADZONE : OTHER_DEADZONE);
    for (int i = 0; i < JOY_AXES; i++)
        j->axis[i] = i < j->naxes ? clamp16(a[i]) : 0;
}

static void joy_pump(void)
{
    test_poll();
    SDL_UpdateGamepads();
}

/* From the main thread, before the game.  SDL's joystick code must not leave a Bluetooth PlayStation
   pad in a report mode that lasts until the pad is turned off ("auto": only if the game asked for
   rumble or sensors, which it cannot); the game reads the pad whether it has the focus or not, and
   never reads SDL's event queue. */
void gmv_joy_start(void)
{
    SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "auto");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        gmv_log("[joy] SDL joystick support failed (%s); no pads\n", SDL_GetError());
        return;
    }
    SDL_SetJoystickEventsEnabled(false);
    SDL_SetGamepadEventsEnabled(false);
    g_sdl = 1;
#if GMV_DEV
    g_test_kind = gmv_dbgenv("GMV_PAD_TEST");
    if (g_test_kind && !*g_test_kind)
        g_test_kind = NULL;
    if (g_test_kind)
        test_plug();
#endif
}

/* ---- the SDL entry points (shimssdl.c's table points here) ---- */

void gmv_joy_init(uint32_t flags)
{
    if ((flags & GAME_INIT_JOYSTICK) && !g_inited) {
        joy_scan();
        g_inited = 1;
    }
}

void gmv_joy_quit(uint32_t flags)
{
    if (flags & GAME_INIT_JOYSTICK) {
        g_njoy = 0;
        g_inited = 0;
    }
}

int gmv_joy_count(void) { return g_njoy; }

void *gmv_joy_open(int index)
{
    if (index < 0 || index >= g_njoy)
        return NULL;
    g_joy[index].opened = 1;
    return &g_joy[index];
}

int gmv_joy_opened(int index) { return index >= 0 && index < g_njoy && g_joy[index].opened; }

void gmv_joy_close(void *jp)
{
    if (jp)
        ((joy *)jp)->opened = 0;
}

static unsigned g_used_tick;             /* gmv_input_tick of the last pad use (PAD_HINTS) */

unsigned gmv_joy_last_use(void) { return g_used_tick; }

void gmv_joy_update(void)
{
    int used = 0, keep = 0;
    joy_pump();
    for (int i = 0; i < g_njoy; i++) {
        joy *j = &g_joy[i];
        if (!j->opened)
            continue;
        joy_read(j);
        test_log(j);
        if (joy_used(j)) {
            /* the pad last used; one pad seen twice (DS4Windows' Xbox pad and the DualShock itself)
               keeps the kind it has instead of flipping between the two */
            keep |= j->style == g_style;
            if (!used)
                used = j->style + 1;
        }
    }
    if (used && !keep)
        g_style = used - 1;
    if (used)
        g_used_tick = gmv_input_tick() | 1;
}

int gmv_joy_style(void) { return g_style < 0 ? 0 : g_style; }

/* SHORT_WARNING: any button of any pad down now (every pad found is read here, opened or not). */
int gmv_joy_any_button(void)
{
    joy_pump();
    for (int i = 0; i < g_njoy; i++) {
        joy *j = &g_joy[i];
        joy_read(j);
        for (int b = 0; b < j->nbuttons; b++)
            if (j->button[b])
                return 1;
    }
    return 0;
}

/* PAD_MENU and the intro: every pad found, read into copies (the game's own readings stay as they
   are).  *x, *y = the left stick deflected furthest, -1..1, down positive; *buttons = bit i for
   button i (X A B Y LB RB LT RT Back Start LS RS) of any pad; *hat = the SDL hat bits of any pad.
   Returns the number of pads. */
int gmv_joy_peek(float *x, float *y, unsigned *buttons, int *hat)
{
    float bx = 0.0f, by = 0.0f, bm = 0.0f;
    unsigned b = 0;
    int h = 0;
    static int scanned;
    if (!g_inited && !scanned) {                /* before the game's first scan (the intro): once */
        scanned = 1;
        joy_scan();
    }
    joy_pump();
    for (int i = 0; i < g_njoy; i++) {
        joy j = g_joy[i];
        joy_read(&j);
        for (int k = 0; k < j.nbuttons && k < 32; k++)
            if (j.button[k])
                b |= 1u << k;
        h |= j.hat;
        if (j.naxes >= 2) {
            float fx = j.axis[0] / 32767.0f, fy = j.axis[1] / 32767.0f, m = fx * fx + fy * fy;
            if (m > bm) {
                bm = m;
                bx = fx;
                by = fy;
            }
        }
    }
    *x = bx;
    *y = by;
    *buttons = b;
    *hat = h;
    return g_njoy;
}

int gmv_joy_num_axes(void *jp) { return jp ? ((joy *)jp)->naxes : 0; }
int gmv_joy_num_buttons(void *jp) { return jp ? ((joy *)jp)->nbuttons : 0; }
int gmv_joy_num_hats(void *jp) { return jp ? ((joy *)jp)->nhats : 0; }

int gmv_joy_axis(void *jp, int i)
{
    const joy *j = jp;
    return j && i >= 0 && i < j->naxes ? j->axis[i] : 0;
}

int gmv_joy_button(void *jp, int i)
{
    const joy *j = jp;
    return j && i >= 0 && i < j->nbuttons ? j->button[i] : 0;
}

int gmv_joy_hat(void *jp, int i)
{
    const joy *j = jp;
    return j && i >= 0 && i < j->nhats ? j->hat : 0;
}
