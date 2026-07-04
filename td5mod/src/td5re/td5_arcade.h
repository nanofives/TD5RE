/**
 * td5_arcade.h -- ARCADE mode: 3x-collision launch + collectible road power-ups
 *
 * PORT-ONLY feature (no original-binary analog). Active only when the race is
 * launched in ARCADE dynamics mode (td5_physics_get_dynamics() == 0). In
 * SIMULATION mode every query here is inert (mode_active() == 0), so the faithful
 * physics path is untouched.
 *
 * Power-up model: pads are placed along the track ring at race start; each pad's
 * kind is deterministic (pad_index % TD5_PU_KINDS) so netplay/replay stay in
 * lockstep with NO RNG in the sim path. Driving a car over an active pad triggers
 * that kind's effect INSTANTLY (no inventory / no activation button), then the pad
 * goes dormant for a respawn cooldown.
 *
 *   NITRO         -- sustained accel boost + raised top-speed cap; speed-trail particles
 *   GHOST         -- phase through cars + traffic (collision skipped), car renders
 *                    translucent; also can't be acquired as a NEW cop-chase target
 *   INDESTRUCTIBLE -- (was WRECK) immune to hits, everything you ram launches harder;
 *                    common spawn, only offered when car damage is enabled
 *   HAZARD        -- [REMOVED 2026-06-28] formerly dropped a rear oil-slick; no longer spawned
 *   SHIELD        -- [REMOVED 2026-07-04] no longer exists
 *   FREEZE        -- [REWORKED 2026-07-04] while held, ramming a car cuts ITS speed to
 *                    1/3 instantly, then eases back to full over ~3s; the victim glows
 *   MAGNET        -- drags nearby traffic toward you; only spawns in Traffic Battle mode
 *   ROCKET        -- [REMOVED 2026-07-04] no longer exists
 *   REPAIR        -- fully repairs the car (health + dents); only offered when car
 *                    damage is enabled
 *
 * Determinism: pad layout, pad kinds, pickup detection, and every effect run off
 * REPLICATED actor state only. The only entropy is td5_msvc_rand (seeded
 * identically across peers), and it is NOT used on the sim path.
 */

#ifndef TD5_ARCADE_H
#define TD5_ARCADE_H

struct TD5_Actor;   /* forward decl — full struct in re/include/td5_actor_struct.h */

/* ======================================================================
 * Power-up kinds (also the active-effect id reported to the HUD)
 * ====================================================================== */
enum {
    TD5_PU_NONE   = 0,
    TD5_PU_NITRO  = 1,   /* sustained accel boost + raised top speed + speed trail */
    TD5_PU_GHOST  = 2,   /* phase through cars/traffic + translucent; cop-chase immune */
    TD5_PU_INDESTRUCTIBLE = 3,  /* [RENAMED 2026-07-04, was TD5_PU_WRECK] immune;
                          * rammed cars get a boosted launch; damage-gated, common     */
    TD5_PU_HAZARD = 4,   /* [REMOVED 2026-06-28] oil-slick trap — kept as a valid
                          * effect id for the dormant render/HUD branches, but it
                          * is NO LONGER spawned (weight 0 in arc_pick_kind).      */
    /* TD5_PU_SHIELD = 5 -- [REMOVED 2026-07-04] retired, id not reused. */
    TD5_PU_FREEZE = 6,   /* [REWORKED 2026-07-04] holder buff: your rams cut the
                          * victim's speed to 1/3, easing back over ~3s            */
    TD5_PU_MAGNET = 7,   /* drags nearby traffic toward you; Traffic Battle only   */
    /* TD5_PU_ROCKET = 8 -- [REMOVED 2026-07-04] retired, id not reused. */
    TD5_PU_REPAIR = 9,   /* damage-gated: fully repairs health + dents (also clears
                          * your own oil-drift / freeze-victim state)              */
    TD5_PU_KINDS  = 9    /* highest effect id; per-kind SPAWN weighting (incl.
                          * HAZARD=0/SHIELD-removed/ROCKET-removed) lives in
                          * arc_pick_kind                                          */
};

/* ======================================================================
 * Lifecycle (driven from td5_game.c)
 * ====================================================================== */

/* Place pads + clear all per-slot/hazard state. Call once at race start, AFTER
 * the track + actor table are ready. No-op (and clears state) outside arcade mode. */
void td5_arcade_init_race(void);

/* Per sim-tick update: pickup detection, effect-timer decay, per-tick effect
 * application (nitro thrust), hazard TTL + hazard-vs-car spinout. Call once per
 * genuine race tick AFTER td5_physics_tick() (td5_game.c). No-op outside arcade. */
void td5_arcade_tick(void);

/* Drop all arcade state (race teardown). Safe to call unconditionally. */
void td5_arcade_shutdown_race(void);

/* ======================================================================
 * Mode gate
 * ====================================================================== */

/* 1 = wild ARCADE mode is active for the current race, 0 = SIMULATION (or no
 * race). Cheap; reads the committed dynamics flag. Every other query below is
 * forced inert when this returns 0. */
int td5_arcade_mode_active(void);

/* ======================================================================
 * Physics-side queries (collision response in td5_physics.c consults these)
 * ====================================================================== */

/* Horizontal knockback multiplier in 24.8 fixed-point applied to the V2V
 * collision impulse. Arcade returns a PERCENT of faithful (default 140 -> 0x166,
 * knob TD5RE_ARCADE_COLLISION_MULT_PCT); simulation returns 0x100 (1.0) so the
 * response is byte-identical to the faithful path. */
int td5_arcade_collision_mult_q8(void);

/* Percent (0..100) of the faithful angular crash-scatter (roll/yaw/pitch
 * kicks) applied on a heavy arcade hit. <100 tames the nose-over flip that
 * launches rammed cars into the air; 100 = faithful, 0 = no spin kick.
 * The collision solver scales kick_ry/kick_p by this in arcade mode. */
int td5_arcade_scatter_pct(void);

/* 1 = this racer slot is currently GHOSTing — the collision solver should skip
 * ALL contact for it (V2V both directions). 0 otherwise / outside arcade.
 * [2026-07-04] Also consulted by td5_ai.c's cop-chase logic in two places:
 * a ghosting car cannot be newly ACQUIRED as a pursuit target, and any chase
 * already in progress ENDS outright the moment its target ghosts ("police
 * should not be able to see you anymore") — every cop pursuing that target
 * drops it, not just one. */
int td5_arcade_slot_is_ghost(int slot);

/* 1 = this racer slot is currently INDESTRUCTIBLE (was WRECK) — the collision
 * solver should leave THIS slot's velocity unchanged and give the OTHER car the
 * boosted launch. */
int td5_arcade_slot_is_wrecking(int slot);

/* Q8 (0x100 = 1.0) drive-torque (ACCELERATION) multiplier for this racer slot:
 * the NITRO boost (default 2.5x = 0x280) while NITRO is active, else 1.0. 1.0
 * outside arcade mode. Read at the physics drive-torque chokepoint so a boosting
 * car accelerates harder for the NITRO duration. */
int td5_arcade_slot_accel_q8(int slot);

/* [ARCADE NITRO 2026-07-04] Effective top-speed multiplier for this racer slot,
 * as a percent (100 = unchanged). Raised (default 150) while NITRO is active so
 * the boosted acceleration can actually be exploited instead of hitting the same
 * faithful cap sooner. Read at the physics top-speed-rating chokepoint
 * (phys_top_speed_rating). 100 outside arcade mode or when NITRO isn't active. */
int td5_arcade_slot_topspeed_pct(int slot);

/* [ARCADE FREEZE 2026-07-04] 1 = this slot (racer OR traffic) is currently
 * recovering from being rammed by a FREEZE holder — its speed was cut to 1/3 on
 * impact and is ramping back to normal over the recovery window. Render should
 * draw a glow on the car while this is true. 0 outside arcade / not a victim. */
int td5_arcade_slot_is_freeze_victim(int slot);

/* Airborne-launch tuning for the arcade V2V heavy-impact branch. The faithful
 * gate launches vel_y = impact_mag/6 only when impact_mag > 90000. In arcade we
 * lower the gate and raise the launch so ordinary crashes send cars flying. */
int td5_arcade_launch_active(void);     /* 1 = apply arcade launch on V2V hits */
int td5_arcade_launch_gate(void);       /* impact_mag threshold (lower than 90000) */
int td5_arcade_launch_div(void);        /* vel_y = impact_mag / div (smaller = higher) */

/* Notify the arcade layer that racer `aggressor` rammed `victim` with the given
 * impact magnitude (called from the V2V heavy-impact branch, for every heavy
 * racer/traffic hit — not gated on battle mode). Lets battle-mode wreck-scoring
 * AND the FREEZE crash-speed-cut both react to ram events. Safe no-op outside
 * arcade / when neither applies. */
void td5_arcade_note_ram(int aggressor, int victim, int impact_mag);

/* ======================================================================
 * Render-side queries (td5_render.c draws pads/hazards; translucency for ghost)
 * ====================================================================== */

int  td5_arcade_pad_count(void);
/* Fill world position (RENDER units = world_pos/256, the scale render_pos uses) +
 * active flag + kind for pad i. Returns 1 if i is valid, 0 otherwise. */
int  td5_arcade_pad_get(int i, float *wx, float *wy, float *wz,
                        int *active, int *kind);

/* Floating item-box visual half-size in RENDER units (auto-scaled from the track
 * span length at race init). Renderer sizes the rotating cube / icon / glow off
 * this. 0 outside arcade mode. */
float td5_arcade_box_half_world(void);

/* Oil-slick (HAZARD) radius in RENDER units = 1.5 lanes, so the slick is 3 lanes
 * wide and matches the spin-out trigger area. 0 outside arcade mode. */
float td5_arcade_hazard_radius_world(void);

int  td5_arcade_hazard_count(void);
int  td5_arcade_hazard_get(int i, float *wx, float *wy, float *wz, int *owner);

/* Render alpha 0..1 for a racer slot: < 1.0 while GHOSTing (translucent), 1.0
 * otherwise. Render reads this to fade ghosting cars. */
float td5_arcade_slot_render_alpha(int slot);

/* ======================================================================
 * HUD-side queries (td5_hud.c draws the active-effect chip per viewport)
 * ====================================================================== */

int td5_arcade_active_effect(int slot);     /* TD5_PU_* currently active, or NONE */
int td5_arcade_active_frames(int slot);     /* frames remaining on the active effect */
/* Frames the CURRENTLY-active effect started with (its full duration). The HUD
 * timer bar uses this as the "100%" reference so it depicts the WHOLE duration —
 * not a stale hardcoded nominal. Tracks the live duration knobs automatically.
 * 0 when no effect is active / outside arcade mode. */
int td5_arcade_active_max_frames(int slot);

#endif /* TD5_ARCADE_H */
