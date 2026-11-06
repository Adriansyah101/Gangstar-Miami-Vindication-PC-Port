/* gangstar.h -- what Gangstar's sources share that the generic host knows nothing of. */
#ifndef GANGSTAR_H
#define GANGSTAR_H

#include <stdint.h>
#include <stddef.h>

int gmv_set_aniso(float level);
void gmv_set_menuflash_fix(int on);
/* AMBIENT_OCCLUSION, BLOOM (0-2), FOG_MODE, WATER_REFLECTIONS, VEHICLE_REFLECTIONS or DYNAMIC_SHADOWS, taken at the
   next present, or FOG_HEIGHT (metres) at once; 0 for another key or without GAMEPATCH_TAB. */
int gmv_set_post(const char *key, float v);
int gmv_ds_active(void);            /* DYNAMIC_SHADOWS is on and its setup has not failed */
const float *gmv_ds_caster(void);   /* the node being drawn's absolute transformation, if a vehicle's or a person's */
/* vtable of the game state on top of the state stack, 0 if none (0x659f28 = main menu). */
uint32_t gmv_top_state_vtable(void);
void gmv_death_poll(void);
void gmv_mission_replay_tick(void);   /* MISSION_REPLAY: a replay's start, end and restore */
void gmv_playerswitch_inventory_tick(void);   /* PLAYERSWITCH_INVENTORY: Betty shares Johnny's cash and weapons */
/* SPAWN_FADE (the patches): for the draw about to be made, how many 64ths of it to show (64 = all,
   the usual answer; less while the scene node being drawn belongs to a car or pedestrian coming or going).
   add: the draw is additive (a light), counted for the log. */
int gmv_spawn_fade_level(int add);
/* UC_CRUSHER (ucveh.c): the Crusher from the player's add-on\crusher folder.  _init finds the folder (1 if
   complete); _redirect gives the fopen shim the file to open instead of a game path (NULL = none); _type
   and _res_base are the Crusher's type and its first sound id once the game has read those lists (-1
   before).  gmv_uc_tick (the patches) parks it after ON A LION HUNT; once per present. */
int gmv_uc_on(void);
int gmv_uc_init(void);
const char *gmv_uc_redirect(const char *path, char *buf, size_t n);
int gmv_uc_type(void);
int gmv_uc_res_base(void);
int gmv_uc_pick_type(void);
int gmv_uc_has_smoke(void);
void gmv_uc_tick(void);
int gmv_pak_unpack(const char *pak, const char *dir, const char *tag);
/* RADIO_SONGS (radiosongs.c): the songs the Mac lacks, from the player's add-on\radio folder.  _init finds them
   (1 if any); _redirect as gmv_uc_redirect; _counts gives a station's songs without and with them once the
   game has read its list (0 before). */
int gmv_rs_on(void);
int gmv_rs_have(void);
int gmv_rs_init(void);
const char *gmv_rs_redirect(const char *path, char *buf, size_t n);
int gmv_rs_counts(int st, int *mac, int *all);
int gmv_rs_is_song(int id);
/* ORIGINAL_MAC_EXPERIENCE switched in game: the collision tables of a broadphase in use made larger
   (broadphase.c; 0 = no memory); a new b2World about to be made (the patches, called from b2solve.c);
   our spawn pool copies that have come back from the city, and the [crowd] line (the patches; once
   per present). */
int gmv_bp_grow(void *bp, uint32_t proxies, uint32_t pairs);
int gmv_bp_shrink(void *bp, uint32_t proxies, uint32_t pairs);
void gmv_b2_world_new(void);
void gmv_pool_tick(void);
/* Test aid (GMV_MENU_LOOP_SCENE), the patches: restarts the looped menu scene and keeps the
   follow camera on its walker; called once per present. */
void gmv_menu_loop_tick(void);
void gmv_test_spawn_car_tick(void);     /* GMV_TEST_SPAWN_CAR test aid (dev build) */

/* broadphase.c: the faster, result-identical Box2D area query behind b2World::Query (a port
   performance fix, installed by gmv_apply_gamepatch), and its PERF_PROFILE line. */
void gmv_apply_broadphase(void);
void gmv_broadphase_report(double seconds);
void gmv_broadphase_tick(unsigned *made, unsigned *gone, int *bounds);

/* b2solve.c: b2World::Solve walking only the non-static bodies (same result; a port performance
   fix, installed by gmv_apply_gamepatch), and its PERF_PROFILE line. */
void gmv_apply_b2solve(void);
void gmv_b2solve_report(double seconds);

/* bbfast.c: the billboard turn 0x131f1e with its range tests before the mesh lookup (same result;
   a port performance fix, installed by gmv_apply_gamepatch), and its PERF_PROFILE line. */
void gmv_apply_bbfast(void);
void gmv_bbfast_report(double seconds);

/* hudbars.c: the HUD's life bars set by one UI walk instead of 38 searches (same result; a port
   performance fix, installed by gmv_apply_gamepatch), and its PERF_PROFILE line. */
void gmv_apply_hudbars(void);
void gmv_hudbars_report(double seconds);

/* vbostatic.c: STATIC_VBO, the game's unchanging vertex/index buffers kept in GL buffer objects
   (GL_STATIC_DRAW); dev: their counts by creating site (PERF_PROFILE). */
void gmv_vbostatic_install(void);
void gmv_vbostatic_frame(void);
const void *gmv_vbostatic_client(unsigned name, uintptr_t off);
#if GMV_DEV
void gmv_vbostatic_report(double seconds);
#else
#define gmv_vbostatic_report(s) ((void)0)
#endif

/* customradio.c: CUSTOM_RADIO, a sixth radio station (index GMV_CR_STATION) playing the files in the
   "custom radio" folder.  gmv_cr_enable turns its playback on/off and returns the songs found;
   gmv_cr_mix adds it to each rendered buffer (audio thread, after the game's render). */
#define GMV_CR_STATION 5
int gmv_cr_enable(int on);
int gmv_cr_songs(void);
void gmv_cr_set_volume(float v);
void gmv_cr_mix(int16_t *buf, uint32_t frames, uint32_t chans, uint32_t rate);
/* CUTSCENE_KEYS (the patches): a key going down that the cutscene takes (1 = keep it from the game),
   and the pump that acts on it after the messages. */
int gmv_cutscene_key(unsigned sym);
void gmv_cutscene_pump(void);
/* PAD_MENU (the patches): on, and the game shows its menu pointer. */
int   gmv_pad_menu_active(void);
int   gmv_pointer_up(void);
/* PAD_ICONS (padicons.c): the options menu's gamepad pictures drawn for the pad in use.  _on says
   whether the GL shim must put in the upload and delete wrappers, which call _upload after the
   glCompressedTexImage2D and _deleted before the glDeleteTextures; _sync runs after every present. */
int   gmv_padicons_on(void);
void  gmv_padicons_upload(int level, unsigned fmt, int w, int h, int size, const void *data);
void  gmv_padicons_deleted(int n, const unsigned *names);
void  gmv_padicons_sync(void);

#if GMV_DEV
void gmv_uifind_probe(void);                 /* PERF_PROFILE: the game's UI name lookups */
void gmv_b2step_probe(void);                 /* PERF_PROFILE: Box2D's world step, timed by part */
void gmv_billboard_probe(void);              /* PERF_PROFILE: the billboard turn 0x131f1e */
#else
#define gmv_uifind_probe() ((void)0)
#define gmv_b2step_probe() ((void)0)
#define gmv_billboard_probe() ((void)0)
#endif

#endif
