/* ========================================================================
 * td5_physics_internal.h — PRIVATE shared internals of the physics module
 *
 * Included ONLY by td5_physics.c (faithful core) and td5_physics_assists.c
 * (PORT-ONLY tuning assists split out of the core, P1-B 2026-07-02). Nothing
 * outside the physics TUs may include this header — the public surface stays
 * td5_physics.h.
 *
 * Contents:
 *   1. SAR-RZ fixed-point shift helpers (shared arithmetic vocabulary).
 *   2. Per-car tuning-table accessors + field offsets (get_phys/get_cardef,
 *      the PHYS_ and CDEF_ offset tables, ACTOR_I16/I32).
 *   3. Cross-TU macros whose values gate core fast-paths (Q8/Q12 identity
 *      constants, acute-crash threshold).
 *   4. extern decls for the state + functions each TU provides the other.
 * ======================================================================== */
#ifndef TD5_PHYSICS_INTERNAL_H
#define TD5_PHYSICS_INTERNAL_H

#include <stdint.h>
#include "td5_types.h"
#include "../../../re/include/td5_actor_struct.h"   /* TD5_Actor field access */

/* ------------------------------------------------------------------------
 * SAR-RZ (signed arithmetic shift with round-toward-zero) helpers.
 *
 * x86 encodes signed division by a power-of-two via:
 *     CDQ                  ; EDX = sign(EAX) ? -1 : 0
 *     AND EDX, (2^N - 1)   ; EDX = (sign ? 2^N-1 : 0)
 *     ADD EAX, EDX         ; bias negative values up by (2^N-1)
 *     SAR EAX, N           ; signed shift right
 *
 * Net result: divide-by-2^N with round-toward-zero (matches C signed `/`).
 * Plain `x >> N` in C is implementation-defined; on GCC/clang it is
 * arithmetic-shift-right which rounds toward -infinity for negative `x`,
 * differing from x86's SAR-RZ by 1 LSB when `x < 0` and the low N bits
 * are nonzero.
 *
 * Use SAR_RZ_8 / SAR_RZ_12 / SAR_RZ_6 / SAR_RZ_15 instead of plain `>>`
 * at any site sourced from a `CDQ; AND EDX,mask; ADD; SAR` idiom in the
 * original listing.
 *
 * `x` is evaluated once; safe to use with arbitrary signed int32 args.
 * Audit citations: pilot_00404030_audit.md (D1/D2/D5/D10/D14).
 * ------------------------------------------------------------------------ */
#define SAR_RZ_6(x)   (((int32_t)(x) + (((int32_t)(x) >> 31) & 0x3F))   >> 6)
#define SAR_RZ_8(x)   (((int32_t)(x) + (((int32_t)(x) >> 31) & 0xFF))   >> 8)
#define SAR_RZ_12(x)  (((int32_t)(x) + (((int32_t)(x) >> 31) & 0xFFF))  >> 12)
#define SAR_RZ_15(x)  (((int32_t)(x) + (((int32_t)(x) >> 31) & 0x7FFF)) >> 15)

/* ------------------------------------------------------------------------
 * Tuning data access helpers
 *
 * The tuning pointer at actor+0x1BC points to the physics table.
 * The car definition pointer at actor+0x1B8 points to the tuning table
 * (bounding box, wheel positions, collision mass).
 * ------------------------------------------------------------------------ */
static inline int16_t *get_phys(TD5_Actor *a)
{
    return (int16_t *)a->tuning_data_ptr;
}

static inline int16_t *get_cardef(TD5_Actor *a)
{
    return (int16_t *)a->car_definition_ptr;
}

/* Read a short from the physics table at byte offset */
#define PHYS_S(a, off) (*(int16_t*)((uint8_t*)get_phys(a) + (off)))
/* Read an int from the physics table at byte offset */
#define PHYS_I(a, off) (*(int32_t*)((uint8_t*)get_phys(a) + (off)))
/* Read a short from the car definition at byte offset */
#define CDEF_S(a, off) (*(int16_t*)((uint8_t*)get_cardef(a) + (off)))
/* Named field offsets for the two per-car tuning tables the accessors above
 * read. PHYS_* index the vehicle-physics table entry (get_phys, mostly int16
 * fields); CDEF_* index the cardef entry (get_cardef). Values mirror the
 * original binary's table layouts — the listing-anchored comments at the use
 * sites are the provenance. */
#define PHYS_INERTIA_YAW        0x20   /* int32 */
#define PHYS_HALF_WHEELBASE     0x24   /* int32 */
#define PHYS_FRONT_WEIGHT       0x28
#define PHYS_REAR_WEIGHT        0x2A
#define PHYS_TIRE_GRIP_COEFF    0x2C
#define PHYS_GEAR_RATIO_BASE    0x2E   /* int16[gear], stride 2 */
#define PHYS_GEAR_UPSHIFT_BASE  0x3E   /* int16[gear] RPM threshold, stride 2 */
#define PHYS_GEAR_DOWNSHIFT_BASE 0x4E  /* int16[gear] RPM threshold, stride 2 */
#define PHYS_SUSP_POS_DAMP      0x5E
#define PHYS_SUSP_VEL_DAMP      0x60
#define PHYS_SUSP_SPRING        0x62
#define PHYS_SUSP_TRAVEL_LIM    0x64
#define PHYS_SUSP_LOAD_SCALE    0x66
#define PHYS_DRIVE_TORQUE_MULT  0x68
#define PHYS_DAMP_COEFF_TURN    0x6A   /* picked when steering hard in gear >= 2 */
#define PHYS_DAMP_COEFF_BASE    0x6C   /* picked when near-straight or gear < 2 */
#define PHYS_BRAKE_FRONT        0x6E
#define PHYS_BRAKE_REAR         0x70
#define PHYS_REDLINE_RPM        0x72
#define PHYS_TOP_SPEED          0x74
#define PHYS_DRIVETRAIN_TYPE    0x76
#define PHYS_SPEED_SCALE        0x78
#define PHYS_HANDBRAKE_MOD      0x7A
#define PHYS_SLIP_COUPLING      0x7C

#define CDEF_FRONT_Z_EXTENT     0x04   /* positive */
#define CDEF_HALF_WIDTH         0x08   /* positive */
#define CDEF_REAR_Z_EXTENT      0x14   /* negative */
#define CDEF_COLLISION_RADIUS   0x80
#define CDEF_SUSP_REF_HEIGHT    0x82
#define CDEF_HEIGHT_OFFSET      0x86
#define CDEF_WHEEL_Y_BASE       0x42   /* per-wheel, stride 8 */

#define ACTOR_I16(base, off) (*(int16_t *)((uint8_t *)(base) + (off)))
#define ACTOR_I32(base, off) (*(int32_t *)((uint8_t *)(base) + (off)))

/* ------------------------------------------------------------------------
 * Cross-TU macros. These gate fast-paths in BOTH TUs (e.g. "mult != ONE"
 * guards in the core torque pipeline), so the identity constants live here.
 * ------------------------------------------------------------------------ */
#define MP_CATCHUP_Q8_ONE     0x100    /* Q8 1.0 — catch-up/draft/weight mults */
#define SLOPE_LIGHT_Q12_ONE   0x1000   /* Q12 1.0 — slope-decel scale */
#define CRASH_FX_ACUTE_MAG    250000   /* impact_mag above which a player hit is "acute" */
/* [POLICE rewrite 2026-06-19] Approach-speed (iVar11) into a wall above which a
 * traffic car / cop / chased racer "breaks down" (halt + smoke; ends a pursuit).
 * Deliberately high so only a genuine head-on crash counts, not a scrape.
 * Read by the collision TU's wall response AND scaled by the assists' cop
 * durability. */
#define COP_WALL_BREAK_VPERP  20000
/* Angular-velocity divisor shared by the wall response (collision TU) and the
 * player-dynamics yaw-correction path (core). From the original listing. */
#define ANGULAR_DIVISOR_W   0x28C

/* ------------------------------------------------------------------------
 * Core state shared with the assists TU (defined in td5_physics.c unless
 * noted). All of it is replicated deterministic sim state.
 * ------------------------------------------------------------------------ */
extern int32_t  g_race_slot_state[TD5_MAX_RACER_SLOTS]; /* 1=human, 0=AI per slot */
extern int32_t  g_difficulty_hard;
extern int32_t  g_game_paused;                          /* DAT_004AAD60 */
/* [#1 WRECK STAND-STILL] free-slide window per slot: armed by the V2V impulse
 * resolver (collision TU), counted down by td5_physics_update_traffic (core).
 * Defined in td5_physics.c. */
extern int16_t  g_wreck_push_ticks[TD5_MAX_TOTAL_ACTORS];
extern int32_t  g_collisions_enabled;                   /* 0=on, 1=off (inverted) */
extern uint8_t *g_actor_table_base;                     /* td5_game.c */
extern int      g_actorSlotForView[TD5_MAX_VIEWPORTS];  /* td5_game.c */

/* Core-provided helpers the assists call. */
int32_t phys_top_speed_rating(TD5_Actor *actor);   /* effective top-speed rating (+cop/suspect/MP-cap rules) */
int32_t sin_fixed12(int32_t angle);                /* fixed-point trig (12-bit angle, 12-bit result) */
int32_t cos_fixed12(int32_t angle);

/* Core-provided helpers the collision TU calls. */
void    td5_physics_mark_collision(int slot);            /* per-tick collision metric marker */
void    update_vehicle_pose_from_physics(TD5_Actor *actor); /* pose re-snap after depenetration/impulse */

/* ------------------------------------------------------------------------
 * Assists-provided API consumed by the faithful core (td5_physics_assists.c).
 * All of these are inert/identity when their feature is disabled, keeping the
 * default sim byte-faithful — see the banners at each definition.
 * ------------------------------------------------------------------------ */
/* Race-init hook: clears per-race assist state (manual-recovery cooldowns). */
void    td5_physics_assists_race_reset(void);

/* MP catch-up (rubber-banding for HUMAN players) */
void    td5_physics_update_mp_catchup(void);
int32_t td5_physics_mp_catchup_mult(int slot);
int32_t td5_physics_mp_catchup_ts_mult(int slot);

/* Manual gearbox + boost */
int     td5_physics_actor_is_manual_gearbox(const TD5_Actor *actor);
int     td5_physics_actor_should_auto_shift(const TD5_Actor *actor);
int32_t td5_physics_actor_manual_boost_q8(const TD5_Actor *actor);
int32_t td5_physics_apply_speed_limit_boost(int32_t speed_limit, int32_t q8);

/* Slope / hill assists */
int32_t td5_physics_slope_light_scale_q12(int32_t top_speed);
int32_t td5_physics_hill_assist_q12(int32_t up_mag);

/* Car-weight mechanics */
int32_t td5_physics_weight_slope_q12(const TD5_Actor *a);
int32_t td5_physics_weight_accel_q8(const TD5_Actor *a);
int32_t td5_physics_weight_lift_q8(const TD5_Actor *a);
int32_t td5_physics_scale_lift_q8(int32_t lift, int32_t q8);

/* Slipstream / draft + downforce */
void    td5_physics_update_draft(void);
int32_t td5_physics_draft_mult(int slot);
void    td5_physics_apply_downforce(TD5_Actor *actor, int32_t sin_h, int32_t cos_h);

/* Hard-difficulty AI catch-up */
void    td5_physics_update_hard_catchup(void);
int32_t td5_physics_hard_catchup_mult(int slot);

/* Crash FX / battle / police knob getters (V2V + wall response read these) */
void    td5_physics_apply_acute_crash_fx(TD5_Actor *actor, int32_t impact_mag);
int32_t npc_fatal_mag(void);
int     battle_ram_pct(void);
int32_t battle_ram_cap(void);
int32_t battle_ram_min_speed(void);
int32_t battle_airborne_vy(void);
int     cop_durability_pct(void);
int     cop_chase_wall_factor_pct(void);
int32_t cop_break_mag(void);

/* Gentle flip-recovery (core recovery-animation path defers to these) */
int     recovery_gentle_for_actor(const TD5_Actor *actor);
void    td5_physics_gentle_recovery_coast(TD5_Actor *actor);

/* ------------------------------------------------------------------------
 * Collision-TU API consumed by the core (td5_physics_collision.c).
 * hull_build_store(slot, verts, n) is declared at its core call site instead
 * (its TD5_MeshVertex parameter type is not visible from this header).
 * ------------------------------------------------------------------------ */
int     wreck_immobile_enabled(void);                       /* traffic wreck anchoring knob */
void    mesh_box_store(int slot, int32_t half_w, int32_t front_z);
void    td5_physics_hitbox_invalidate(int slot);            /* clear model box + hull for slot */
int     td5_physics_hull_points(int slot);                  /* 0 when no valid silhouette */

#endif /* TD5_PHYSICS_INTERNAL_H */
