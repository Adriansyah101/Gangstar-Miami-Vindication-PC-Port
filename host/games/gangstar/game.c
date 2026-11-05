/* game.c -- Gangstar: Miami Vindication's side of game.h, apart from the settings (settings.c) and the GL hooks
   (render\draw_hooks.c). */
#include <stdint.h>
#include <stdlib.h>
#include "compat.h"

#include "game.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"

void gmv_vbostatic_fault(void);                 /* vbostatic.c: its buffers and the GL bindings */
void gmv_toggle_full_screen(void);              /* patch\tab.c */
void gmv_full_screen_tick(void);                /* patch\tab.c: makes sure a pending Alt+Enter switch happens */
#if GMV_DEV
void gmv_state_trace(void);                     /* patch\diag_state.c */
void gmv_state_trace_present(void);
#endif

void gmv_game_crash_note(const gmv_regs *r)
{
#if GMV_DEV
    gmv_vbostatic_fault();
    uint32_t eip = r->eip;
    if (eip == 0x00564e5au) {
        /* Targeted dump for the vertex-buffer-fill fault: identify the "VBO" object whose data
           pointer resolved into __cstring (0x6222ad).  Locals of the builder frame (from disasm):
           src attribute descriptor = [ebp-0x2fc]; its [0] = the buffer object passed to 0x517b10;
           dest = 0x517b10(vbo,4) + descriptor[4].  All reads guarded. */
        const unsigned char *L = (const unsigned char *)(uintptr_t)r->ebp;
        #define RDL(off) (!IsBadReadPtr((const void *)(L + (off)), 4) ? *(const uint32_t *)(L + (off)) : 0xBADBAD00u)
        uint32_t srcdesc = RDL(0 - 0x2fc), desc300 = RDL(0 - 0x300);
        gmv_log("        [vbfill] arg0c=%08x arg1c=%08x srcDesc[-0x2fc]=%08x desc[-0x300]=%08x dstStride[-0x2dc]=%08x\n",
                RDL(0x0c), RDL(0x1c), srcdesc, desc300, RDL(0 - 0x2dc));
        if (srcdesc > 0x1000 && !IsBadReadPtr((const void *)(uintptr_t)srcdesc, 24)) {
            const uint32_t *d = (const uint32_t *)(uintptr_t)srcdesc;
            gmv_log("        [vbfill] srcDesc: [0]vbo=%08x [1]=%08x [2]=%08x [3]=%08x [4]off=%08x\n",
                    d[0], d[1], d[2], d[3], d[4]);
            uint32_t vbo = d[0];
            if (vbo > 0x1000 && !IsBadReadPtr((const void *)(uintptr_t)vbo, 0x20)) {
                const uint32_t *v = (const uint32_t *)(uintptr_t)vbo;
                const unsigned char *vb = (const unsigned char *)v;
                gmv_log("        [vbfill] VBO=%08x vtable=%08x data[+8]=%08x [+0xc]=%08x [+0x10]=%08x "
                        "flags[+0x11]=%02x[+0x12]=%02x[+0x13]=%02x\n",
                        vbo, v[0], v[2], v[3], v[4], vb[0x11], vb[0x12], vb[0x13]);
            } else {
                gmv_log("        [vbfill] VBO=%08x UNREADABLE (garbage descriptor[0])\n", vbo);
            }
        }
        /* The drawable whose vertex declaration this is: arg [ebp+0x1c].  Its [0] is the attribute
           descriptor-array base (srcDesc = [obj]+0x14+idx*16).  Dump its vtable + fields + array so
           we can identify the object class (via vtable) and see if the whole object is garbage or
           just the descriptor array is uninitialised. */
        uint32_t obj = RDL(0x1c);
        if (obj > 0x1000 && !IsBadReadPtr((const void *)(uintptr_t)obj, 0x20)) {
            const uint32_t *o = (const uint32_t *)(uintptr_t)obj;
            gmv_log("        [vbfill] drawObj=%08x [0]descArr=%08x [1]=%08x [2]=%08x [3]=%08x [4]=%08x [5]=%08x\n",
                    obj, o[0], o[1], o[2], o[3], o[4], o[5]);
            uint32_t arr = o[0];
            if (arr > 0x1000 && !IsBadReadPtr((const void *)(uintptr_t)arr, 0x2c)) {
                const uint32_t *A = (const uint32_t *)(uintptr_t)arr;
                gmv_log("        [vbfill] descArr hdr: %08x %08x %08x %08x %08x | desc0: %08x %08x %08x %08x\n",
                        A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[8]);
            }
        }
        #undef RDL
    }
#else
    (void)r;
#endif
}

const char *gmv_game_redirect(const char *path, char *buf, size_t n)
{
    const char *uc = gmv_rs_redirect(path, buf, n);     /* RADIO_SONGS: the add-on's files */
    if (!uc)
        uc = gmv_uc_redirect(path, buf, n);             /* UC_CRUSHER: the add-on's files */
    return uc;
}

void gmv_game_audio_mix(int16_t *buf, uint32_t frames, uint32_t chans, uint32_t rate)
{
    gmv_cr_mix(buf, frames, chans, rate);               /* CUSTOM_RADIO (customradio.c) */
}

int gmv_game_key(unsigned sym)
{
    return gmv_cutscene_key(sym);
}

void gmv_game_input_pump(void)
{
    gmv_cutscene_pump();                                /* CUTSCENE_KEYS: what its keys asked for */
}

void gmv_game_input_tick(void)
{
#if GMV_DEV
    gmv_state_trace();
#endif
}

void gmv_game_alt_enter(void)
{
    gmv_toggle_full_screen();
}

void gmv_game_full_screen_tick(void)
{
    gmv_full_screen_tick();
}

int gmv_game_pad_pointer(void)
{
    return gmv_pad_menu_active();
}

int gmv_game_pointer_shown(void)
{
    return gmv_pointer_up();
}

void gmv_game_tick(void)
{
    gmv_death_poll();                                   /* arms the capture on the tick health reaches 0 */
    gmv_mission_poll();                                 /* MISSION_SURVEY */
    gmv_mission_replay_tick();                          /* MISSION_REPLAY */
    gmv_playerswitch_inventory_tick();                  /* PLAYERSWITCH_INVENTORY */
    gmv_uc_tick();                                      /* UC_CRUSHER */
    gmv_pool_tick();                                    /* the spawn pool after ORIGINAL_MAC_EXPERIENCE; [crowd] */
    gmv_menu_loop_tick();                               /* GMV_MENU_LOOP_SCENE test aid */
#if GMV_DEV
    gmv_state_trace_present();
#endif
}

void gmv_game_between_frames(void)
{
    gmv_vbostatic_frame();                              /* STATIC_VBO's checks, between frames */
}

const host_symbol gmv_game_host_symbols[] = {
    {NULL, NULL},
};
