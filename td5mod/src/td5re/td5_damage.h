/* ========================================================================
 * td5_damage.h -- GTA4-style car damage system (PORT-ONLY)
 *
 * Global, opt-in vehicle damage: per-vertex body deformation, health driven
 * down by collision impact magnitude, knockout (wreck) at zero health, a
 * handling penalty, tiered damage smoke, and a player HUD damage bar.
 *
 * The original Test Drive 5 has NO damage model, so every formula/constant
 * here is a port-only engineering choice (all TD5RE_DAMAGE_* knob-tunable) —
 * NOT reverse-engineered. The only RE-derived facts this rests on:
 *   - actor +0x1D8..+0x1EF is free padding (health stored there)            [CONFIRMED]
 *   - the software vertex transform (TransformMeshVerticesToView 0x0043DD60)
 *     reads model-space pos fresh each frame from the shared per-mesh blob,
 *     so a per-slot mutated model-space copy needs no GPU re-upload          [CONFIRMED]
 *
 * Master toggle [Game] CarDamage (default 0 = OFF). When off, every
 * entrypoint here is inert and the faithful Sim is byte-unchanged.
 * ======================================================================== */
#ifndef TD5_DAMAGE_H
#define TD5_DAMAGE_H

#include <stdint.h>
#include "td5_types.h"   /* TD5_Actor (fwd), TD5_MeshHeader; actor fields are only
                          * touched in td5_damage.c, which pulls the full struct. */

#ifdef __cplusplus
extern "C" {
#endif

/* Sentinel written into actor.damage_magic once the damage fields have been
 * initialized for the current race (the original leaves +0x1D8..+0x1EF
 * uninitialized, so a magic guard distinguishes "never set" from "full hp"). */
#define TD5_DAMAGE_ACTOR_MAGIC  0x44414D47u   /* 'DAMG' */

/* Hit-region descriptor passed from the physics collision solver. Signs are in
 * the struck actor's MODEL frame: lat = +1 right / -1 left / 0 centred,
 * fwd = +1 front / -1 rear / 0 centred. is_side selects a lateral-face impact
 * vs a front/rear-face impact. Used only to place the deformation; health is
 * driven purely by impact magnitude. */
typedef struct TD5_DamageHit {
    int  lat;       /* -1, 0, +1 */
    int  fwd;       /* -1, 0, +1 */
    int  is_side;   /* 1 = side face, 0 = front/rear face */
} TD5_DamageHit;

int   td5_damage_init(void);     /* returns non-zero on success (module-table contract: !init()==fail) */
void  td5_damage_shutdown(void);

/* Master toggle — mirrors [Game] CarDamage. When 0, every call below is inert. */
int   td5_damage_enabled(void);

/* HUD damage bar + wreck/knockout sub-toggle — mirrors [Game] CarDamageBar AND
 * requires the master to be on. When this returns 0 the top-of-pane health bar
 * is hidden, a car can no longer be wrecked/eliminated by accumulated damage
 * (no race-ending knockout), and the health-driven handling penalty + damage
 * smoke are suppressed. Impact-driven mesh DEFORMATION (dents) and collision
 * physics are unaffected by this toggle — they live below the health meter. */
int   td5_damage_bar_enabled(void);

/* [DEFORM OFF 2026-07-05] DEFORMATION level sub-toggle — mirrors [Game]
 * CarDeform==3 ("OFF" folded into the DEFORMATION level cycle itself, same
 * pattern as CAR TOUGHNESS's OFF state) AND requires the master to be on.
 * When this returns 0, impact-driven mesh deformation (dents) and the
 * companion scuff darkening are suppressed; health/wreck (CAR TOUGHNESS) is
 * unaffected — the two axes are independent. */
int   td5_damage_deform_enabled(void);

/* Initialize per-slot health + clear all deformation at race start. MUST be
 * called once the race actors exist (ResetVehicleActorState does NOT touch the
 * damage padding, so this is the only initializer). */
void  td5_damage_reset_race(void);

/* [RESET-CAR REPAIR] Fully repair ONE slot mid-race: restore health, clear the
 * knockout/event state, and erase dents + scuff. Called by the arcade REPAIR
 * power-up (the one place a full, free repair is intended). Inert when
 * CarDamage is off. */
void  td5_damage_repair_actor(int slot);

/* [RESET-CAR REPAIR PARTIAL 2026-07-04] Partially repair ONE slot mid-race: add
 * `pct` percent of max health to the slot's CURRENT health (capped at max) and
 * clear the knockout/event state so a knocked-out car can drive again — but do
 * NOT erase dents/scuff (a partial repair gets the car moving, it doesn't undo
 * the visible damage). Called by the manual stuck-recovery (R/SELECT) path so
 * un-sticking a broken-down car isn't a free full heal. Inert when CarDamage
 * is off. */
void  td5_damage_repair_actor_pct(int slot, int pct);

/* Apply a collision of magnitude impact_mag (same units as the physics solver's
 * impact_mag) to actor. Drives health down and, when enabled, deforms the slot's
 * body mesh at the hit region. No-op when disabled or impact below the floor. */
void  td5_damage_on_impact(TD5_Actor *actor, int32_t impact_mag,
                           const TD5_DamageHit *hit);

/* Wall/barrier impact: `approach_speed` is the car's normal velocity into the
 * wall (the physics wall solver's iVar11). Scaled into the impact_mag domain and
 * routed through td5_damage_on_impact. No-op when disabled / below the floor. */
void  td5_damage_on_wall_impact(TD5_Actor *actor, int32_t approach_speed,
                                const TD5_DamageHit *hit);

/* Remaining health as 0..1 (1 = pristine). Returns -1 when the slot has no
 * actor or has not been initialized this race. */
float td5_damage_health01(int slot);

/* 1 if the slot is knocked out (enabled && health <= knockout threshold). */
int   td5_damage_slot_knocked_out(int slot);

/* End-of-race orbit camera (around a finished/wrecked car so the player can see
 * the damage while waiting for the others). Enabled only when CarDamage is on. */
int   td5_damage_finish_orbit_enabled(void);
int   td5_damage_finish_orbit_speed(void);   /* 12-bit angle increment per sim-tick */
int   td5_damage_finish_orbit_hold_ms(void); /* post-finish hold so the SP orbit can play */

/* Same test against an already-resolved actor (cheap path for the physics tick). */
int   td5_damage_actor_knocked_out(const TD5_Actor *actor);

/* Smoke tier for the slot: 0 none, 1 light, 2 black, 3 fire/sparks. 0 when
 * disabled or pristine. */
int   td5_damage_smoke_tier(int slot);

/* Steering-authority multiplier for the slot (1.0 pristine -> floor when
 * wrecked). Always 1.0 when disabled. */
float td5_damage_handling_scale(int slot);

/* Per-vertex model-space deformation deltas for the slot's current body mesh.
 * Returns 1 and fills the arrays (each *vcount long, indexed by mesh vertex)
 * when an active deformation exists that matches mesh; 0 otherwise. The render
 * transform adds these to model pos before the world->view multiply. */
int   td5_damage_get_deform(int slot, const TD5_MeshHeader *mesh,
                            const float **dx, const float **dy,
                            const float **dz, int *vcount);

/* Per-vertex damage "scuff" (0..1) for the slot's body mesh — drives a diffuse
 * darkening on the struck panels (a cheap "texture damage" look in the software
 * lighting pass). Returns 1 + fills the array when active for mesh, else 0. */
int   td5_damage_get_scuff(int slot, const TD5_MeshHeader *mesh,
                           const float **scuff, int *vcount);
float td5_damage_scuff_strength(void);   /* max diffuse darkening fraction at scuff=1 */

#ifdef __cplusplus
}
#endif
#endif /* TD5_DAMAGE_H */
