/**
 * td5_roam_mode.h -- GEO FREE ROAM: the ROAM game mode (PORT-ONLY, F2 task A).
 * See docs/plans/GEO_FREE_ROAM.md (F2 row) and GEO_FREE_ROAM_DESIGN.md 0 item 7, 2.
 *
 * ROAM is a new mode: off unless entered through the FREE ROAM button of the geo
 * selector (td5_fe_geo.c) or the dev CLI --Roam=1 --RoamSpawn=<lat>,<lon>. In roam
 * the strip is unloaded, the ring length is 0, every actor span field is -1 and the
 * roam street-piece id lives in a per-slot side table (not in the int16 fields).
 *
 * THE SHARED CONTRACT BELOW IS FROZEN for the F2-1 siblings:
 *   task A (this file)  td5_roam_mode.c    session, entry/exit, gating
 *   task B              td5_roam_physics.c fills td5_roam_resident() via td5_roam_set_resident()
 *   task C              td5_roam_render.c  reads td5_roam_active() / td5_roam_world_origin()
 * A change goes through _archive/roam/F2/IFACE_REQUESTS.txt.
 *
 * LIFE CYCLE. td5_roam_begin() ARMS the mode (frontend, or the dev CLI at boot).
 * td5_roam_active() turns 1 when the race session that follows starts
 * (td5_roam_session_start, top of td5_game_init_race_session) and stays 1 until
 * td5_roam_end(). So the car-select screens and the menus in between never see
 * an active roam. A pause-menu RESTART re-inits the session without ending it.
 */
#ifndef TD5_ROAM_MODE_H
#define TD5_ROAM_MODE_H

#include <stdint.h>
#include <stddef.h>

struct TD5_RoamResident;   /* td5_roam_locate.h */

typedef struct {
    char    place[64];         /* "la_plata_partido" */
    int32_t spawn_px, spawn_pz;/* spawn point, PLACE frame units (430/m) */
    int32_t spawn_yaw;         /* 0..4095, or -1 = along the nearest street (0 when no locator) */
    int     car_index;         /* chosen car */
} TD5_RoamSpawn;

int  td5_roam_active(void);                  /* 1 while a free-roam session runs */
void td5_roam_begin(const TD5_RoamSpawn *s); /* called by the frontend; sets up the session */
void td5_roam_end(void);

/* world origin (design 0 item 7): fixed at the spawn tile origin for the whole session in F2 */
void td5_roam_world_origin(int32_t *ox, int32_t *oz);    /* PLACE units; world = place - origin */

/* resident tile set, owned by task B. NULL until B registers one. */
const struct TD5_RoamResident *td5_roam_resident(void);

/* ---- hooks beyond the frozen contract (additive, task A) ---- */
void td5_roam_set_resident(const struct TD5_RoamResident *rs);   /* B registers / clears (NULL) */
const TD5_RoamSpawn *td5_roam_spawn(void);                       /* the session spawn; NULL unless armed or active */

/* per-slot roam street-piece id (side table, design 2.2 / F0b "span fields -1"):
 * 0 = none. Slots 0..31 (TD5_MAX_TOTAL_ACTORS). */
uint64_t td5_roam_slot_edge(int slot);
void     td5_roam_set_slot_edge(int slot, uint64_t edge_key);

/* ---- entry / session plumbing (frontend + game flow) ---- */
int  td5_roam_armed(void);                   /* begin() called, session not ended */
void td5_roam_cancel(void);                  /* = td5_roam_end(), for "back out of car select" */
int  td5_roam_host_track(void);              /* track index of the level whose textures/sky host the roam world */
int  td5_roam_host_is_geo(void);             /* 1 when the host is the place's own geo level (F2-F) */
void td5_roam_host_pages(int *road, int *ground, int *kerb);   /* page ids in the host's TEXTURES.DAT; kerb -1 = HUD white */
int  td5_roam_host_sky_path(char *out, size_t n);              /* sky file override for the host, 0 = level's own */
void td5_roam_session_start(void);           /* race init: armed -> active, force the single-player overrides */
void td5_roam_apply_overrides(void);         /* the overrides alone (idempotent; re-asserted after the mode setup) */
void td5_roam_session_spawn(int slot, void *actor);    /* places the player actor (td5_game.c spawn loop) */
/* Metres the session's spawn point was moved onto the drivable surface (0 = the click was already
 * on the road, -1 = no road found near it). The click is snapped by td5_roam_session_spawn (F2-D). */
double td5_roam_spawn_snap_m(void);

/* lat/lon -> PLACE frame units of `slug` (PLACE.JSON projection). 1 on success. */
int  td5_roam_latlon_to_place(const char *slug, double lat, double lon, int32_t *px, int32_t *pz);
/* [F5-A] PLACE frame units -> lat/lon of `slug` (the inverse of the above). 1 on success. */
int  td5_roam_place_to_latlon(const char *slug, double px, double pz, double *lat, double *lon);
/* 1 when `slug` has road tiles to roam in (roam/ROAMGRID.JSON present). */
int  td5_roam_place_has_tiles(const char *slug);
/* The place a map point can be roamed in (F2 A, FREE ROAM button): the first known place whose
 * ROAMGRID covers lat/lon AND whose ROAMTILE file for that tile exists. Fills `slug` (cap bytes)
 * and the PLACE-frame point. 0 when the point is outside every built tile. */
int  td5_roam_place_for_latlon(double lat, double lon, char *slug, size_t cap,
                               int32_t *px, int32_t *pz);

#ifndef TD5RE_RELEASE
/* Dev CLI: --Roam=1 --RoamSpawn=<lat>,<lon> [--RoamYaw=n] [--RoamPlace=slug]. Arms roam and
 * the AutoRace path with the default car. Call after the INI + CLI overrides are applied. */
void td5_roam_dev_cli(const char *cmdline);
/* Self-test entry (task F2-D, `roam` rows): arm a session at lat/lon of la_plata_partido with the
 * current ini car, yaw along the street. Does not touch the AutoRace ini (the director owns it).
 * 1 when armed. */
int td5_roam_selftest_arm(double lat, double lon);
#endif

#endif /* TD5_ROAM_MODE_H */
