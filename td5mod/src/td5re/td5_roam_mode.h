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
 */
#ifndef TD5_ROAM_MODE_H
#define TD5_ROAM_MODE_H

#include <stdint.h>

struct TD5_RoamResident;   /* td5_roam_locate.h */

typedef struct {
    char    place[64];         /* "la_plata_partido" */
    int32_t spawn_px, spawn_pz;/* spawn point, PLACE frame units (430/m) */
    int32_t spawn_yaw;         /* 0..4095 */
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
const TD5_RoamSpawn *td5_roam_spawn(void);                       /* the session spawn, NULL when not active */

/* per-slot roam street-piece id (side table, design 2.2 / F0b "span fields -1"):
 * 0 = none. Slots 0..31 (TD5_MAX_TOTAL_ACTORS). */
uint64_t td5_roam_slot_edge(int slot);
void     td5_roam_set_slot_edge(int slot, uint64_t edge_key);

#endif /* TD5_ROAM_MODE_H */
