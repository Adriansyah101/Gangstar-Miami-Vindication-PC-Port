/* patch.h -- what the game's patches share with each other. */
#ifndef GMV_PATCH_H
#define GMV_PATCH_H

#include "compat.h"
#include <stdint.h>

/* patch_common.c */
/* What a group of patches replaced, so that it can be taken out again: the hooks that exist only for
   what ORIGINAL_MAC_EXPERIENCE turns off go in and out with its in-game switch. */
#define PATCH_UNDO_MAX 160
typedef struct {
    int n;
    struct { uint32_t va; unsigned n; unsigned char was[16]; } e[PATCH_UNDO_MAX];
} patch_undo;
extern patch_undo *patch_undo_now;
extern patch_undo mac_undo;
#if GMV_DEV
#define MA_SITES 256
typedef struct { uint32_t va; unsigned n; unsigned char was[16]; } ma_site_t;
extern ma_site_t ma_site[MA_SITES];
extern int ma_sites;
#endif
int patch_bytes(uint32_t va, const unsigned char *buf, size_t n);
void patch_undo_all(patch_undo *u);
void emit_jmp(unsigned char *tr, size_t *o, uint32_t dst);

/* fix_vertex.c */
#if GMV_DEV
void apply_bakershare_log(void);
#endif
void apply_vtxfix(void);

/* diag_vehicle.c */
extern volatile float g_carsnd_last_speed;
void apply_carsnd_trace(void);
void apply_veh_trace(void);

/* fix_crash58.c */
void apply_crash58_guard(void);

/* fix_stoplock.c */
int stoplock_set(int on);
void apply_stoplock_fix(void);

/* fix_menu.c */
#define MAIN_MENU_VT   0x00659f28u
int menu_fade_clicks_set(int on);
void apply_menu_fade_clicks(void);

/* fix_fade_edge.c */
int fade_edge_set(int on);
void apply_fade_edge_fix(void);

/* fix_load.c */
int load_sleep_skip_set(int on);
void apply_load_sleep_skip(void);
int load_budget_set(int n);
void apply_load_budget(void);
void apply_fast_load(void);

/* diag_audio.c */
void apply_engvol_trace(void);
void apply_engsrc_trace(void);
void apply_engdtor_trace(void);

/* diag_loop.c */
void apply_frametick_probe(void);

/* diag_fade.c */
void apply_fade_trace(void);
void apply_cine_trace(void);

/* sync_physics.c */
extern int g_phys_on;
extern int kick_tried, grip_tried;
extern int phys_installed;
void physics_sync_set(double ms);
void apply_physics_sync(void);

/* sync_camera.c */
extern int g_cam_sync;
extern int cs_mgr_hooked, cs_installed;
void cam_sync_set(double ms);
void apply_cam_follow(void);

/* sync_walk.c */
#define WS_MAC_MS     33.333        /* a game update at the Mac's 60 ticks/s */
extern double g_ws_tick;
extern double g_ws_qpf;
extern int g_ws_on, g_as_on;
double ws_now_ms(void);
extern int ws_installed;
void walk_sync_set(double ms);
void apply_walk_sync(void);

/* sync_ai.c */
extern int as_installed;
void ai_sync_set(double ms);
void apply_ai_sync(void);

/* test_solocar.c */
void apply_solo_car(void);

/* radio_logo.c */
#define RADIO_PICK_FN   0x0005f3e8u   /* (mm, station) -> slot of the next song/jingle/ad   */
#define RADIO_TUNE_FN   0x0006000cu   /* (mm, station): switch station                       */
#define RADIO_LOGO_FN   0x0017357eu   /* (widget, station): HUD radio logo                   */
__attribute__((cdecl, force_align_arg_pointer))
int gmv_radio_pick(unsigned char *mm, int station);
void apply_radio_trace(void);
#define RADIO_APP_FN     0x0015e4fau   /* the app object ([+0x214] HUD, [+0x218] HUD state)      */
#define RADIO_MM_PP      0x0063e030u   /* -> global -> music manager (NULL until first use)       */
int radio_logo_set(int on);
void apply_radio_logo_fix(void);

/* custom_radio_hooks.c */
#define CR_GAME_STATIONS 5
extern int g_cr_hooked, g_cr_on;
extern int g_cr_row_want, g_cr_row;
#define CR_VISIBLE     0x002fdf80u      /* (flash, path, visible) */
#define CR_TITLE0      (-1397)          /* twips: the first row's title and bar, 80 units higher */
#define CR_BAR0        (-868)
#define CR_STEP        1760             /* 88 units between rows */
#define CR_OFF         40000            /* our rows in the menu file: this far below the stage */
void cr_page_mac(void);
void cr_mac_switch(int mac);
void cr_logo_hide_tick(void);
void cr_file_wants(void);
void apply_custom_radio(void);

/* short_warning.c */
void apply_short_warning(void);

/* cutscene_keys.c */
#define CS_TEXTS       0x0016fbeeu              /* slot 6 */
#define CS_ARROW_SHOW  0x0015ebfau
#define CS_ARROW_HIDE  0x0015eb48u
#define CS_CURSOR_SET  0x00181d14u              /* (cursor, on) */
#define CS_APP         0x00a064a0u              /* the menu manager 0x15e4fa answers (its static): widgets at +0x214, count +0x218 */
#define CS_APP_GUARD   0x00a066d8u              /* its guard: set once it is built */
void cs_btn_visible(void *fx, int on);
extern const uint32_t cs_hide_sites[2];
extern volatile int g_cs_on, g_cs_hooked;
extern volatile int g_cs_line;
extern volatile int g_cs_skip, g_cs_next;
unsigned char *cs_widget(void);
__attribute__((cdecl, force_align_arg_pointer))
void gmv_cs_texts(unsigned char *w);
__attribute__((cdecl, force_align_arg_pointer))
void gmv_cs_arrow_show(void *app);
__attribute__((cdecl, force_align_arg_pointer))
void gmv_cs_arrow_hide(void *app);
__attribute__((cdecl, force_align_arg_pointer))
void gmv_cs_cursor(void *cursor, int on);

/* tutorial_keys.c */
extern volatile int g_tk_on, g_tk_ok, g_tk_press, g_tk_answer;
int tk_set(int on);
void apply_tutorial_keys(void);
int cs_set(int on);
void apply_cutscene_keys(void);

/* pad_layout.c */
#define PL_AIM        0x100         /* ours: lock on while held                                        */
#define PL_BTNS       16
enum { PL_FOOT, PL_CAR, PL_FLY, PL_SNIPER, PL_KINDS };
extern const int pl_map[PL_KINDS][PL_BTNS];
extern volatile int g_pl_on, g_pl_ok, g_pl_aim_btn;
__attribute__((noinline, force_align_arg_pointer, optimize("no-optimize-sibling-calls")))
uint32_t pl_call1(uint32_t fn, uint32_t a);
extern int g_pl_cam_hooked;
#define PL_MOTO_VT  0x00656228u
int pl_set(int on);
#define PL_LABEL_FN    0x0008f426u
extern const uint32_t pl_scheme_vt[PL_KINDS];
__attribute__((noinline, force_align_arg_pointer, optimize("no-optimize-sibling-calls")))
uint32_t pl_call2(uint32_t fn, uint32_t a, uint32_t b);
__attribute__((noinline, force_align_arg_pointer, optimize("no-optimize-sibling-calls")))
uint32_t pl_call3(uint32_t fn, uint32_t a, uint32_t b, uint32_t c);
void apply_pad_layout(void);

/* pad_menu.c */
int pm_set(int on);
int pointer_shown(void);
void apply_pad_menu(void);

/* pad_hints.c */
extern int g_ph_on, g_ph_ok;
int ph_set(int on);
void apply_pad_hints(void);

/* key_move.c */
extern volatile int g_km_on, g_km_ok;
int km_set(int on);
void apply_key_move(void);

/* switch_save.c */
#define SSF_MAIN_PP   0x00689ea4u    /* the main player (Johnny)                                     */
#define SSF_CTRL_PP   0x00689ea8u    /* the player in control (= *0x63e010)                          */
#define SSF_PLAYER_VT 0x00655de8u
extern volatile int g_ssf_on, g_ssf_ok;
int ssf_set(int on);
__attribute__((cdecl, force_align_arg_pointer))
uint32_t gmv_ssf_write(uint32_t save, uint32_t path);
void apply_switch_save_fix(void);
/* fix_mission_save.c */
int msf_set(int on);
void apply_mission_save_fix(void);

/* switch_inventory.c */
int si_set(int on);
void apply_playerswitch_inventory(void);

/* menu_hide.c */
extern int mg_more, mg_live;
int hide_main_menu_set(int more, int live);
void hide_main_menu_unhook(void);
void apply_hide_more_games(void);

/* weapon_sprint.c */
#define WS_PRESS_SITE   0x0011e269u     /* mov esi,[ebp-0x10]: start of the vt[0x78](1) call */
#define WS_RELEASE_SITE 0x0011e2aeu     /* mov eax,[esi+0x48]: start of the restore */
void apply_weapon_sprint(void);

/* minimap_shops.c */
int minimap_shops_set(int on);
void apply_minimap_shops(void);

/* sun_moon_hooks.c */
#define SM_TOD_FN     0x00142892u
#define SM_TRACK_FN   0x00142690u   /* (scene manager, disc, anchor, glow) */
#define SM_SKYBOX_PP  0x0068a6e8u   /* -> CustomSkyBoxSceneNode */
extern const uint32_t sm_track_sites[2];
extern int g_sm_on;
extern int g_sky_glow_iphone;
extern int g_sm_hooked;
typedef void (*sm_vfn_i)(void *, int);
void **sm_vt(void *obj);
__attribute__((cdecl, force_align_arg_pointer))
void gmv_sun_moon_tod(unsigned char *tod);
__attribute__((cdecl, force_align_arg_pointer))
void gmv_sun_moon_track(unsigned char *smgr, unsigned char *disc, unsigned char *anchor, void *glow);
/* The glows draw after the world with no depth test, so they showed through buildings.  The
   billboard render (vtable 0x676a70 slot 8, and its ISceneNode thunk) is wrapped to flag a glow's
   draw; the GL shim gives that draw a depth test behind everything of the world (g_sky_glow_draw).
   The discs need nothing: the skybox draws them before the world. */
#define SM_BB_RENDER       0x0046a580u
#define SM_BB_RENDER_THUNK 0x0046a570u   /* this = the ISceneNode at +4 */
__attribute__((cdecl, force_align_arg_pointer))
void gmv_sun_moon_render(unsigned char *n, int draw);
__attribute__((cdecl, force_align_arg_pointer))
void gmv_sun_moon_render_sn(unsigned char *sn, int draw);
void sm_discs_behind_farcity(void);
int sm_is_call(uint32_t site, uint32_t fn);
int sm_redirect(uint32_t site, void *hook);
int tab_live(void);
int tables_live(void);
int bloom_windows_on(void);
void sm_mode_set(int mode);
int sun_moon_set(int mode);

/* shadow_hooks.c */
void ds_show_all(void);
int ds_tree_has(int t, uint32_t node, int add);
void apply_sun_moon(void);

/* draw_distance.c */
#define DD_PROXY_FN    0x001b9e50u   /* b2BroadPhase::CreateProxy(bp, aabb, userData) */
#define DD_MAX_PROXIES 0x00649680u
#define DD_WORLD       0x006856c4u   /* the b2World (made once: a save's load keeps it, a run of 2026-11-02) */
#define DD_WORLD_BP    0x000191d8u   /* its broadphase */
#define DD_PROFILE     0x00949a00u   /* the profile read: +0x0c full detail, +0x10 low detail */
#define DD_RADIUS      0x00a06d78u   /* the streaming's (0x1a5d2a) statics: the full detail as a float, */
#define DD_RADIUS_SET  0x00a06d70u   /*   its guard, */
#define DD_KEEP        0x00a06d88u   /*   the cells' keep margin, 1.5 times it (0x5f096c), its guard */
#define DD_KEEP_SET    0x00a06d80u
extern const uint32_t dd_proxy_sites[2];
extern int dd_mac_profile[5];
extern double dd_scale;
__attribute__((cdecl, force_align_arg_pointer))
int gmv_create_proxy(char *bp, void *aabb, void *shape);
extern int dd_on;
extern int dd_live;
extern int dd_tables_in;
extern patch_undo dd_undo, dd_tables_undo;
void dd_tables_fit(double s);
int dd_set(double s);
void apply_draw_distance(void);
int dd_cells_aim(void);

/* fog_distance.c */
extern float sky_k;
extern int g_fog_all, g_sky_draw;
extern int fog_sky_wrapped;
void apply_fog(void);
void apply_sky_horizon(void);

/* origin_collision.c */
extern float oc_seed_r1, oc_seed_r2;
extern int dd_unhook;
void dd_take_out(void);
void apply_origin_collision_fix(void);

/* density.c */
#define SV_SITE      0x00082af8u   /* call 0x080272 (the spawn check) in the spawn point's update */
#define SV_WORLD     0x0068551cu   /* the world (counts at +0x414, lists from +0x418) */
#define PD_MAKE_SITE   0x00082bd1u   /* call 0x082504, the spawner (SPAWN_FADE's hook) */
#define PD_MAKE_FN     0x00082504u
typedef struct {
    const char *env;
    int kind, cap, prof;
    uint32_t cap_imm, rec, d0, d1;
} pd_kinds_t;
extern const pd_kinds_t pd_kinds[3];
extern double pd_scale[3];
extern double pd_user[3];
extern int pd_on, pd_racing;
int pd_write(void);
extern int sf_hooked;
__attribute__((cdecl, force_align_arg_pointer, noinline))
int gmv_spawn_make(uint32_t sp, uint32_t loc);
/* The spawn pool, moved to PD_POOL_N slots so the densities can put more copies in it */
#define PD_POOL_OLD   0x00689180u
#define PD_POOL_N     2048
extern uint32_t pd_pool[PD_POOL_N];
extern int pd_pool_on;
extern double pd_built[3];
extern int pd_extra_n;
int pd_is_extra(uint32_t o);
int pd_object_known(uint32_t o);
void cr_cars_off_sixth(void);
void pd_pool_top_up(void);
void pd_unmove_pool(void);
#if GMV_DEV
void pd_ws_left(uint32_t ch);
void pd_ws_report(uint32_t ch, uint32_t vt_was);
#endif
extern int pd_world_gen, pd_trim_gen;
extern int pd_centres_on;
__attribute__((cdecl, force_align_arg_pointer))
int gmv_can_spawn(uint32_t sp);
void apply_spawn_hook(void);

/* spawn_fade.c */
extern int sf_n, sf_live, sf_depth, sf_lost;
extern int sf_menu;
void sf_before_make(void);
void sf_after_make(void);
int spawn_fade_set(int on);
void apply_spawn_fade(void);
extern int mac_features_on, mac_out_pending, uc_loaded;

/* mac_hooks.c */
extern patch_undo pd_undo;
extern int dd_tables_back;
void dd_hooks_in(void);
void crowd_hooks_in(void);
void mac_hooks_in(void);
void mac_hooks_out(void);
void dd_tables_tick(void);

/* game_read.c */
#define MS_OWNER     0x00689e98u
#define MS_STRINGS   0x17ac4       /* app + this: the string table (0x8f426) */
#define MS_NONE_ID   0x1054
uint32_t ms_rd(uint32_t a);
const char *ms_text(uint32_t id, char *out, size_t n);
/* The world's scripts (0x35d08: the static at 0x94f300; +0x2dc the scripts, +0x2ec their count).  A
   script has its commands at +0x10 (+4 of them), the one it is at +8 and its state +0x18 (2 = running,
   as the MISSIONS page wants a trigger); a command's type is its vtable's +0x1c.  The MISSIONS page
   lists the scripts at a 0x8f (or 0x81) command with a mission id (0x32560: the +0x10 of the first
   0x63, else 0x67, command), and its START (0x180360) finds the 0x8f from there, faces its +0x14 name
   and sets the manager's +0x85. */
#define MS_SCRIPTS   0x0094f300u
#define MS_SC_MAX    1024
#define MS_CMD_MAX   400
int ms_cmd_type(uint32_t c);

/* tab.c */
#define OM_SET_ENTRY   0x00172f0eu   /* (tab widget, slot, string id, enabled)                    */
#define OM_SET_VISIBLE 0x002fdf80u   /* (flash, path, visible)                                     */
#define OM_SET_TEXT    0x002fdea0u   /* (flash, path, text, 1)                                     */
#define OM_MOVE        0x002fdf30u   /* (flash, clip, x, y)                                        */
#define OM_STRING_FN   0x0008f426u   /* (string table, id): the text                               */
#define OM_CLICK_FN    0x0016eff4u
#define OM_REG_FN      0x002fd1f0u
#define OM_SWITCH_FN   0x0015e71au   /* (manager, state name, 0)                                   */
#define OM_LISTEN      0x0016eb12u   /* (widget, listener)                                         */
#define OM_TAB_SELECT  0x00180544u   /* (tab widget, slot, 1)                                       */
#define OM_VT_N        17
#define OM_VT_SHOW     5             /* 0x169a6a                                                   */
#define OM_VT_HIDE     6
#define OM_VT_EVENT    15            /* 0x169b62                                                   */
#define OM_ALLOC       0x000904e0u   /* (bytes, file, line, 1)                                     */
#define OM_CAT_VT      0x00659388u   /* widgetActionKeyTitle                                       */
#define OM_CAT_SLOTS   7
#define OM_CAT_EVENT   0x0017802au
#define OM_CAT_TEXTS   0x001720b0u
#define OM_PLAIN_SHOW  0x00177f58u   /* widgetActionKeyTitle's show and hide, which do no more     */
#define OM_PLAIN_HIDE  0x00177fbcu
#define OM_KEEP        0x0016e9e6u
#define OM_W_SHOW      1             /* widget vtable slots                                        */
#define OM_W_HIDE      2
#define OM_W_KEEP      3
#define OM_W_EVENT     5
#define OM_W_TEXTS     6
#define OM_W_TABS      1
#define OM_W_PAGER     0x12
#define OM_W_CAT       0x22
#define OM_LINES       6
#define MR_TAB_ID      0x7f01        /* MISSION_REPLAY's SERVICES tab, which shares the label lookup  */
#define OM_SEL_X0      1429          /* the category's ◀ value ▶ in widgetActionKeyTitle's twips:  */
#define OM_SEL_X1      7951          /* from the left arrow's tip to the right one's, over the bar  */
#define OM_SEL_Y0      21
#define OM_SEL_Y1      1202
extern int om_tab_files;
extern int om_credit;
extern int om_selectors;
extern void *(*om_opener)(const char *);
extern const char *const om_sel_names[4];
void *om_mgr(void);
unsigned char *om_widget(int i);
void *om_app(void);
void om_sound(int id);
void om_visible(void *fx, const char *path, int on);
void om_text(void *fx, const char *path, const char *text);
void om_line_visible(void *fx, int line, const char *field, int on);

/* tab_arrows.c */
uint32_t om_bits(const unsigned char *p, size_t *bit, int n);
int32_t om_sbits(const unsigned char *p, size_t *bit, int n);
void om_put_bits(unsigned char *p, size_t *bit, uint32_t v, int n);
int om_sbits_n(int32_t v);
typedef struct {
    int depth, cid, matrix, rotated;
    int32_t sx, sy, tx, ty;                     /* scale 16.16, translation in twips */
    const char *name;                           /* or NULL */
} om_place;
size_t om_tag(const unsigned char *s, size_t p, size_t end, int *code, size_t *data, size_t *len);
int om_sprite(const unsigned char *s, size_t start, size_t n, int cid, size_t *data, size_t *len);
int om_find_place(const unsigned char *s, size_t from, size_t end, const char *name, om_place *o);
size_t om_place_write(unsigned char *o, int depth, const om_place *m, const char *name);
unsigned char *om_swf_patch(const unsigned char *s, size_t n, size_t *out_n);

/* tab_swf.c */
#define OM_CREDIT   "PATCH BY ADRIANSYAH101"
int om_opener_hook(void);
void om_selectors_visible(void *fx, int line, int on);
unsigned om_resolution(int i, unsigned *h);
int om_density(const char *key, double v);
int om_set_sprint(const char *key, double v);
int om_set_minimap(const char *key, double v);
int om_set_spawn_fade(const char *key, double v);

/* tab.c */
void rs_tick(void);
void om_pages(unsigned char *pager, int n, int i);
const char *om_event_path(unsigned char *ev);
int tab_label_hook(void);
void apply_options_tab(void);

/* mission_replay.c */
#define MR_WORLD_REF   0x0063e170u   /* -> -> the world: object lists at +0x418, +0x430 and +0x424 (off) */
#define MR_PLAYER      0x00689ea8u   /* -> the Player under control (0x689ea4: the story's own)        */
#define MR_POS_BYTES   0x14
#define MR_FADE        0x006424b4u   /* the cutscene fader's black, 0..255, drawn over the frame by 0x34918; -999 none */
#define MR_FADE_NONE   (-999)
#define MR_OBJS        64
#define MR_VARS        18
extern uint32_t mr_unlock_prev;
/* A replay, from START to the scripts put back; what was before it. */
enum { MR_IDLE, MR_ARMED, MR_STARTING, MR_UNDER_WAY, MR_ENDING };
typedef struct {
    int phase, id, at, opener, last_stop, passed, failed, held, n;
    uint32_t sc, mgr, list;
    DWORD t;
    uint32_t vars[MR_VARS], flags[8], tries;
    uint32_t mission_sc;
    unsigned char tutorial;
    int cine;
    uint32_t player;
    unsigned char pos[MR_POS_BYTES];
    int no;
    uint32_t o_name[MR_OBJS], o_obj[MR_OBJS];
    uint16_t o_flags[MR_OBJS];
    unsigned char o_seen[MR_OBJS];
    DWORD watch;
    uint32_t s_sc[MS_SC_MAX], s_time[MS_SC_MAX];
    int s_st[MS_SC_MAX], s_pc[MS_SC_MAX];
} rp_t;
extern rp_t rp;
uint32_t mr_mgr(void);
typedef struct { int phase, a; DWORD t; } mf_t;
extern mf_t mf;
__attribute__((cdecl, force_align_arg_pointer))
int gmv_mr_save(void *save, void *path);
void mr_save_hooks(void);
void apply_mission_replay(void);
int om_set_replay(const char *key, double v);

/* test_menuloop.c */
#if GMV_DEV
__attribute__((cdecl, force_align_arg_pointer))
void gmv_ml_unlock(uint32_t c);
void apply_menu_loop(void);
#endif
#if !(GMV_DEV)
void apply_menu_loop(void);
#endif

/* crusher_hooks.c */
extern int uc_hooks;
extern patch_undo uc_undo;
typedef struct { float pos[3], ang; uint32_t cell, tri; int have; } uc_spot_t;
extern const uc_spot_t uc_beach;
typedef struct {
    uc_spot_t spot, cand;
    uint32_t obj, mgr, list, last_veh, survey_car;
    DWORD t;
    int said;
} uc_t;
extern uc_t uc;
__attribute__((cdecl, force_align_arg_pointer, noinline))
void gmv_uc_engine(uint32_t v);
__attribute__((cdecl, force_align_arg_pointer, noinline))
void uc_pos(uint32_t obj, float *out);
float uc_dist(const float *a, const float *b);
int uc_is_car(uint32_t o);
int uc_lion_done(uint32_t mgr);
__attribute__((cdecl, force_align_arg_pointer, noinline))
void uc_place(uint32_t o, const uc_spot_t *s, int fresh);
int uc_category(int type);
__attribute__((cdecl, force_align_arg_pointer, noinline))
uint32_t uc_make(uint32_t world, int type, const uc_spot_t *s);
__attribute__((cdecl, force_align_arg_pointer, noinline))
void uc_leave(uint32_t world, uint32_t p, int type);
void lc_pos(uint32_t o, float *p);

/* lc_switch.c */
int lc_set(int on);
void lc_tick(void);
void apply_lc_switch(void);
#if GMV_DEV
void ma_snapshot(void);
#endif
void uc_hooks_in(void);
void apply_uc_crusher(void);
void apply_radio_songs(void);

/* patch_apply.c */
void mac_features_in(void);
void mac_features_out(void);

#endif
