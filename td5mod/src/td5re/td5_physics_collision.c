/* ========================================================================
 * td5_physics_collision.c -- V2V + V2W collision (byte-faithful + hitbox layer)
 *
 * Split out of td5_physics.c (P1-B step 2, 2026-07-02). Contents, in original
 * core order:
 *   - Wall collision response (FUN_00406980) + V2W tuning knobs
 *   - OBB corner test (FUN_00408570)
 *   - Collision response / ApplyVehicleCollisionImpulse (FUN_004079c0)
 *   - [MESH HITBOX] model-derived V2V box + convex-hull silhouette
 *   - Simple + full collision detection (FUN_00408f70 / FUN_00408a60)
 *   - Anti-tunnel depenetration (S17, PORT-ONLY)
 *   - ResolveVehicleContacts broadphase (0x409150) + traffic crash spin
 * Cross-TU seam: td5_physics_internal.h (PRIVATE).
 * ======================================================================== */

#include "td5_physics.h"
#include "td5_ai.h"
#include "td5_track.h"
#include "td5_render.h"   /* td5_render_get_vehicle_mesh */
#include "td5_sound.h"    /* td5_sound_play_at_position (Tier 2 recovery SFX) */
#include "td5_input.h"    /* td5_input_ff_collision (wall/prop impact FF) */
#include "td5_vfx.h"      /* td5_vfx_queue_prop_break (TD6 prop debris) */
#include "td5_game.h"     /* td5_game_get_total_actor_count, td5_game_is_wanted_mode */
#include "td5_arcade.h"   /* arcade collision mult / ghost / wrecking-ball / launch */
#include "td5_damage.h"   /* [CAR DAMAGE] health from impacts, knockout freeze, handling penalty */
#include "td5_laneassist.h" /* optional lane-assist steering aid (port-only, default OFF) */
#include "td5_platform.h"
#include "td5_trace.h"    /* inner-tick physics_trace stages */
#include "td5_carparam.h" /* shared carparam field map + heaviness math (weight mechanics) */
#include "td5_config.h"   /* shared TD5RE_* env-knob accessors (td5_env_int/float/flag_*) */
/* V2V trace headers are included unconditionally: their obb_corner_test /
 * collision_detect_full call sites below are ungated, so the snapshot types
 * must always be declared. The emitters self-stub to no-ops under
 * TD5RE_RELEASE (release build does not link the v2v trace modules). */
#include "td5re.h"
#include "td5_physics_internal.h"  /* PRIVATE core<->assists seam (SAR_RZ, tuning accessors, assist decls) */

/* Include the full actor struct for field-level access.
 * The build system must add TD5RE/re/include to the include path (-I). */
#include "../../../re/include/td5_actor_struct.h"

#include <string.h>  /* memset, memcpy */
#include <math.h>    /* cos, sin */
#include <stdlib.h>  /* abs */
#include <stdio.h>   /* FILE/fopen/fprintf — used by the [Trace] TrafficEdgePen
                      * CSV mirror at traffic_edge_pen (no-op when flag is 0).
                      * [TRACE 2026-05-24 traffic-edge-pen-cluster] */

#define LOG_TAG "physics"


extern void *g_actor_base;
extern uint8_t *g_actor_table_base;

/* ======== [split] OBB typedef + collision fwd decls (moved verbatim from td5_physics.c) ======== */
/* OBB corner test output: per-corner penetration data */
typedef struct OBB_CornerData {
    int16_t proj_x;     /* corner position in TARGET's local frame (X) */
    int16_t proj_z;     /* corner position in TARGET's local frame (Z) */
    int16_t pen_x;      /* penetration depth along X axis */
    int16_t pen_z;      /* penetration depth along Z axis */
    int16_t own_x;      /* rotated corner offset from penetrator's center, in TARGET's frame (X) */
    int16_t own_z;      /* rotated corner offset from penetrator's center, in TARGET's frame (Z) */
} OBB_CornerData;

static void resolve_collision_pair(TD5_Actor *a, TD5_Actor *b, int idx_a, int idx_b);
static void collision_detect_full(TD5_Actor *a, TD5_Actor *b, int idx_a, int idx_b);
static void collision_detect_simple(TD5_Actor *a, TD5_Actor *b);
static int  obb_corner_test(TD5_Actor *a, TD5_Actor *b,
                            int32_t pos_a_x_fp, int32_t pos_a_z_fp,
                            int32_t pos_b_x_fp, int32_t pos_b_z_fp,
                            int32_t yaw_a_acc, int32_t yaw_b_acc,
                            OBB_CornerData corners[8]);
static void apply_collision_response(TD5_Actor *penetrator, TD5_Actor *target,
                                     int corner_idx, OBB_CornerData *corner,
                                     int32_t heading_target, int32_t impactForce);

/* [tasks #9/#14] V2V traffic-taming knob accessors + ground clamp, defined
 * lower in the file but referenced from obb_corner_test / collision_detect_*. */
static int   traffic_hit_tame_enabled(void);
static int   traffic_mass_pct(void);
int   wreck_immobile_enabled(void);
static int   wreck_mass_pct(void);
static float traffic_hitbox_scale(void);
static void  traffic_clamp_above_ground(TD5_Actor *t);

/* [item #4] Racer (slot 0..g_traffic_slot_base-1) OBB hitbox-fit scale, defined
 * lower in the file but referenced from obb_corner_test / apply_collision_response.
 * Brings the player/AI collision box in line with the visible chassis (the cardef
 * OBB is authored larger than the mesh) so V2V contacts no longer fire across a
 * visible gap. Returns 1.0 (no shrink) when TD5RE_HITBOX_FIT=0. */
static float racer_hitbox_scale(void);
static float actor_hitbox_scale(const TD5_Actor *act);

/* [MESH HITBOX 2026-06-25] Model-derived V2V collision box. actor_collision_box
 * returns each actor's collision half-extents from the PURE mesh AABB (no fixed
 * scale, no padding) the suspension envelope already computed; mesh_box_store is
 * how the envelope feeds it. Defined lower; referenced from obb_corner_test /
 * apply_collision_response / compute_suspension_envelope. */
void  mesh_box_store(int slot, int32_t half_w, int32_t front_z);
static void  actor_collision_box(const TD5_Actor *act,
                                 int32_t *half_w, int32_t *front_z, int32_t *rear_z);

/* [#1 WRECK STAND-STILL 2026-06-21] Free-slide window thresholds; the per-slot
 * counter g_wreck_push_ticks lives in td5_physics.c (its traffic-update reader). */
#define WRECK_PUSH_TICKS       45      /* ~1.5s of free slide after a ram */
#define WRECK_PUSH_MIN_IMPACT  0x400   /* min V2V impact_mag that counts as a real shove */

/* V2V inertia constant = 500,000 (DAT_00463204) */
#define V2V_INERTIA_K       500000

/* Per-actor AABB table for broadphase (stride 20 bytes: xMin, zMin, xMax, zMax, chain) */
static int32_t g_actor_aabb[TD5_MAX_TOTAL_ACTORS][5];

/* Spatial grid broadphase (FUN_00409150).
 * Bucket array indexed by (segment_index >> 2).
 * Each entry is a chain head (actor index, or 0xFF sentinel).
 * Chain links stored in g_actor_aabb[][4]. */
#define COLLISION_GRID_SIZE     256
#define COLLISION_CHAIN_END     0xFF
/* Per-bucket chain-walk cap (Phase 2 of td5_physics_resolve_vehicle_contacts).
 * Must be >= the maximum number of actors that can land in one span bucket,
 * which is the full active-actor count. The original binary capped the field
 * at 12 (6 racers + 6 traffic), so a fixed 17 always cleared the real chain.
 * The N-way expansion raised TD5_MAX_TOTAL_ACTORS to 22, so a tight pack of
 * >17 cars in a single ~12-span window silently dropped the trailing chain
 * nodes -> some pairs were never tested -> cars passed through each other.
 * Scaling the cap with the actor cap guarantees no real chain is truncated.
 * [S08 car-vs-car coverage 2026-06-04] */
#define COLLISION_MAX_WALK      TD5_MAX_TOTAL_ACTORS
static uint8_t s_collision_grid[COLLISION_GRID_SIZE];

/* --- Anti-tunnel car-vs-car depenetration (S17 2026-06-05) ---
 * Number of outer relaxation rounds the position-only separation pass runs
 * per frame. Each round pushes every still-overlapping pair fully apart along
 * the line of centres; extra rounds let multi-car packs (A shoved into C while
 * separating from B) settle. This is Gauss-Seidel relaxation of separation
 * constraints, so a dense N-car pile-up needs several sweeps to converge --
 * 24 fully clears even a 12-racer heap in one frame. The loop breaks as soon
 * as no pair moves, so the common 1-2 contact case still costs only 1-2 rounds. */
#define ANTITUNNEL_RELAX_ROUNDS  24
/* Clamp on the per-call separation magnitude (display units). Bounds the move
 * for a pathological deep overlap so the reposition can't teleport a car;
 * normal car overlaps are well under this, so they separate in one round. */
#define ANTITUNNEL_MAX_DEPTH     256

/* ======== [split] wall collision response (moved verbatim from td5_physics.c) ======== */
/* ========================================================================
 * Wall collision response -- FUN_00406980
 *
 * Called when a probe detects the car is outside a track span edge.
 * Pushes the car back onto the road and reflects velocity with damping.
 *
 * Parameters:
 *   actor       - the vehicle actor
 *   wall_angle  - 12-bit angle of the wall edge direction
 *   penetration - signed distance (negative = outside wall)
 *   side        - 0=left boundary, 1=left inner, 2=right inner
 * ======================================================================== */

/* Wall-to-vehicle inertia constant = 1,500,000 [CONFIRMED @ DAT_00463200] */
#define V2W_INERTIA_K       1500000
/* Angular divisor shared with V2V = 0x28C (652) [CONFIRMED @ 0x406a66] */
/* ANGULAR_DIVISOR_W moved to td5_physics_internal.h (player dynamics also uses it) */
/* Impulse numerator scale factor [CONFIRMED @ 0x406aae] */
#define V2W_NUM_SCALE       0x1100

/* [#10 NEAR-WALL CAMERA ZOOM 2026-06-19] Per-racer-slot countdown (in sim ticks)
 * set when a wheel probe clips a wall deeply. The chase camera zooms in while
 * this is > 0 so the wall geometry cannot occlude the car. Cosmetic (camera
 * only) — decremented in the camera per-tick update. Defined here because the
 * wall-contact response is the producer. */
int16_t g_actor_near_wall[TD5_MAX_RACER_SLOTS];

void td5_physics_wall_response(TD5_Actor *actor, int32_t wall_angle,
                               int32_t penetration, int side,
                               int32_t probe_x_fp8, int32_t probe_z_fp8)
{
    /* [#10] A deep wall clip arms the chase-cam zoom for this racer. penetration
     * is negative when the probe is outside the rail; the more negative, the
     * deeper the clip. Racer slots only (traffic has no camera). Knob
     * TD5RE_WALL_CAM_ZOOM (default on); "0" disables the zoom entirely. */
    if (actor->slot_index >= 0 && actor->slot_index < TD5_MAX_RACER_SLOTS) {
        static int s_wall_cam = -1;
        static int s_wall_cam_pen = 0;
        if (s_wall_cam < 0) {
            s_wall_cam = td5_env_flag_on("TD5RE_WALL_CAM_ZOOM");
            /* [#R3-4 2026-06-19] Lowered 100 -> 40: the zoom only fired sometimes
             * (esp. reversing into a wall, where contact is gentler -> shallower
             * penetration). 40 arms on almost any real wall contact. Original had
             * only a lower clamp (>=10); generous hi cannot change realistic arming. */
            s_wall_cam_pen = td5_env_int("TD5RE_WALL_CAM_PEN", 40, 10, 100000);   /* min penetration to arm */
        }
        /* [#R11 2026-06-19] Lowered the arm threshold (was -250, rarely hit) and
         * lengthened the hold (was 8 ticks — too short for the radius spring to
         * visibly settle) so the zoom actually engages on a normal wall scrape. */
        if (s_wall_cam && penetration < -s_wall_cam_pen)
            g_actor_near_wall[actor->slot_index] = 22;  /* ~0.7s hold at 30Hz */
    }

    /* Pre-impulse attitude snapshot (slot 0) — lets us see whether wall
     * response is the proximate trigger of a pitch/roll spike. */
    int32_t pre_av_roll  = actor->angular_velocity_roll;
    int32_t pre_av_pitch = actor->angular_velocity_pitch;
    int32_t pre_av_yaw   = actor->angular_velocity_yaw;
    int32_t pre_disp_r   = actor->display_angles.roll;
    int32_t pre_disp_p   = actor->display_angles.pitch;
    int32_t pre_pos_y    = actor->world_pos.y;
    uint8_t pre_bm       = actor->wheel_contact_bitmask;

    /* wall tangent direction (cos,sin); the wall normal is (-sin, cos). */
    int32_t cos_w = cos_fixed12(wall_angle);
    int32_t sin_w = sin_fixed12(wall_angle);

    /* Push actor position out of wall along the wall normal.
     * [CONFIRMED @ 0x4069a1-0x4069d7]: push = penetration - 4, negative →
     * outward. Arithmetic-right-shift by 4 with sign-bit round mask. */
    int32_t push = penetration - 4;
    {
        int64_t px = (int64_t)sin_w * push;
        int64_t pz = (int64_t)cos_w * push;
        actor->world_pos.x -= (int32_t)((px + ((px >> 63) & 0xF)) >> 4);
        actor->world_pos.z += (int32_t)((pz + ((pz >> 63) & 0xF)) >> 4);
    }

    /* Lever arm iVar9 = (actor_center - probe) dot wall_tangent, both sides
     * divided by 256 first to avoid overflow before the >>12.
     * [CONFIRMED @ 0x00406A04-0x00406A60]. POST-push positions are used
     * because actor->world_pos was already updated above.
     *
     * Original uses Borland-style round-toward-zero on the >>12: adds 0xFFF
     * to negative values before SAR (asm: `CDQ; AND EDX,0xFFF; ADD EAX,EDX;
     * SAR EAX,0xC`). GCC's plain `>> 12` rounds toward -inf for negatives,
     * producing 1-unit-too-negative results. D4 audit. */
    int32_t arm_x_int = (actor->world_pos.x - probe_x_fp8) >> 8;
    int32_t arm_z_int = (actor->world_pos.z - probe_z_fp8) >> 8;
    int64_t iVar9_raw = (int64_t)arm_z_int * sin_w + (int64_t)arm_x_int * cos_w;
    int32_t iVar9 = (int32_t)((iVar9_raw + ((iVar9_raw >> 63) & 0xFFF)) >> 12);

    /* Decompose velocity into wall-tangent (v_para, iVar4) and
     * wall-normal (v_perp, iVar10) components [CONFIRMED @ 0x4069cc].
     * Same round-toward-zero correction on the >>12 (D2/D3/D4/D6 family). */
    int32_t vx = actor->linear_velocity_x;
    int32_t vz = actor->linear_velocity_z;
    int64_t v_para_raw = (int64_t)vx * cos_w + (int64_t)vz * sin_w;
    int64_t v_perp_raw = (int64_t)vz * cos_w - (int64_t)vx * sin_w;
    int32_t v_para = (int32_t)((v_para_raw + ((v_para_raw >> 63) & 0xFFF)) >> 12);
    int32_t v_perp = (int32_t)((v_perp_raw + ((v_perp_raw >> 63) & 0xFFF)) >> 12);

    /* iVar11 = contact-point normal velocity (center normal vel + rotation
     * contribution at the lever arm). [CONFIRMED @ 0x00406A60-0x00406A66] */
    int32_t iVar11 = (actor->angular_velocity_yaw / ANGULAR_DIVISOR_W) * iVar9 + v_perp;

    int32_t impulse = 0;
    int32_t new_v_para = v_para;
    int32_t new_v_perp = v_perp;
    int32_t pre_clamp_delta = 0;
    int     clamp_fired = 0;

    /* Early-out when separating [CONFIRMED @ 0x00406A72-0x00406A78]:
     * JS 0x00406CBA — when iVar11 < 0 the original jumps to the function
     * epilogue and returns WITHOUT updating linear_velocity_x/z or
     * angular_velocity_yaw. D1 audit: port previously fell through and
     * rotated (new_v_para, new_v_perp) back to world basis, introducing
     * round-off in lv_x/lv_z on separating contacts. Faithful behavior is
     * to skip the entire writeback block when iVar11 < 0. */
    if (iVar11 >= 0) {
        /* [POLICE rewrite 2026-06-19] A high-speed head-on wall hit breaks down
         * a CHASED racer (ending the pursuit) or a CHASING cop. iVar11 is the
         * approach speed into the wall; the threshold is high so only a genuine
         * crash counts. Ordinary traffic is left alone. */
        {
            int wslot = actor->slot_index;
            /* [POLICE 2026-06-24] A cop withstands 2.5x the wall approach speed
             * that breaks plain traffic, so a fast chase scraping a wall doesn't
             * instantly end the pursuit. Plain traffic / chased racers keep the
             * stock 20000 floor. */
            int32_t wall_gate = COP_WALL_BREAK_VPERP;
            if (wslot >= 0 && td5_ai_actor_is_cop(wslot)) {
                wall_gate = (int32_t)((int64_t)COP_WALL_BREAK_VPERP * cop_durability_pct() / 100);
                /* [COP OVERHAUL 2026-06-29] An actively CHASING cop is driving fast
                 * and WILL scrape walls; those self-inflicted scrapes must not end
                 * the pursuit, so raise its wall gate much higher. Player V2V rams
                 * are unaffected (only this wall gate is relaxed); a catastrophic
                 * head-on still exceeds the gate and wrecks the cop. */
                if (td5_ai_cop_is_chasing(wslot))
                    wall_gate = (int32_t)((int64_t)wall_gate * cop_chase_wall_factor_pct() / 100);
            }
            if (iVar11 > wall_gate) {
                /* Any TRAFFIC car / cop that slams a wall hard breaks down (halt +
                 * smoke); so does a chased racer (ends the pursuit). Un-chased racers
                 * keep control. */
                if (wslot >= g_traffic_slot_base || td5_ai_actor_is_pursued(wslot)) {
                    td5_ai_mark_actor_broken_down(wslot);
                    TD5_LOG_I(LOG_TAG, "wall_break: slot=%d vperp=%d gate=%d (broke down)",
                              wslot, iVar11, wall_gate);
                }
            }
        }

        /* [CAR DAMAGE 2026-06-28] Wall/barrier hit damages + dents the car on the
         * face that struck the wall. iVar11 is the approach speed into the wall;
         * the hit region comes from the inward wall normal (-sin_w, cos_w) rotated
         * into the car's model frame by its heading (signs only, so the 12-bit
         * fixed scale cancels). No-op when [Game] CarDamage=0. */
        if (td5_damage_enabled() && actor->slot_index >= 0) {
            int32_t h     = actor->display_angles.yaw & 0xFFF;
            int32_t cos_h = cos_fixed12(h);
            int32_t sin_h = sin_fixed12(h);
            int32_t Dx = -sin_w, Dz = cos_w;                         /* world dir into wall */
            int64_t lf = (int64_t)Dx * sin_h + (int64_t)Dz * cos_h;  /* local forward comp */
            int64_t ll = (int64_t)Dx * cos_h - (int64_t)Dz * sin_h;  /* local lateral comp */
            int64_t alf = lf < 0 ? -lf : lf, all_ = ll < 0 ? -ll : ll;
            TD5_DamageHit whit;
            whit.fwd     = (lf > 0) - (lf < 0);
            whit.lat     = (ll > 0) - (ll < 0);
            whit.is_side = (all_ > alf);
            td5_damage_on_wall_impact(actor, iVar11, &whit);
        }

        /* Impulse numerator = ((K>>8) * -0x1100) >> 12 with Borland round-
         * toward-zero on the negative >>12. D2 audit. The compiler-time
         * constant evaluation here yields a different result from the
         * runtime add-and-shift form, but since iVar6 = -25497168 is
         * negative, we apply (val + 0xFFF) >> 12 explicitly.
         * [CONFIRMED @ 0x00406A86-0x00406ACB] */
        int32_t iVar6_num = (V2W_INERTIA_K >> 8) * -V2W_NUM_SCALE;  /* -25497168 */
        int32_t num = (iVar6_num + ((iVar6_num >> 31) & 0xFFF)) >> 12;

        /* Denominator = (iVar9^2 + K) >> 8. [CONFIRMED @ 0x00406AAE-0x00406AD2]
         * iVar9^2 + K is always non-negative (K>0, square>=0) so no sign-
         * round needed; matches port's prior code. */
        int64_t denom64 = ((int64_t)iVar9 * iVar9 + (int64_t)V2W_INERTIA_K) >> 8;
        if (denom64 == 0) denom64 = 1;
        impulse = (int32_t)((int64_t)num * iVar11 / denom64);

        new_v_perp = v_perp + impulse;

        /* Tangential damping — corrected asm reading [CONFIRMED via asm
         * 0x00406B0B-0x00406B69, re-verified 2026-05-02]:
         *   v_para >  0 branch: new_v_para = v_para - delta. If sign flips
         *                       negative (SUB result < 0), JNS at 0x00406B33
         *                       falls through to JMP 0x00406B69 → XOR ECX,ECX
         *                       → zero. So result is clamped to 0 on neg-flip.
         *   v_para <= 0 branch: new_v_para = v_para + delta. TEST/JLE at
         *                       0x00406B65 takes the jump when result <= 0
         *                       (preserve), else falls through to
         *                       XOR ECX,ECX at 0x00406B69 → zero. So result
         *                       is clamped to 0 on pos-flip.
         *
         * Both branches hard-zero on sign-flip. The asymmetry is purely in
         * *which* sign-flip each side cares about (v_para>0 →
         * v_para-delta, clamp to 0 on negative-flip; v_para<=0 →
         * v_para+delta, clamp to 0 on positive-flip).
         *
         * 2026-05-10 — reverted to faithful behaviour on the v_para<=0
         * branch. A prior port version used a magnitude-only soft clamp
         * here (allowing sign-flip up to |incoming v_para|) under the
         * theory that hard-zero killed wall-friction reorient. A Frida
         * long-pass on Moscow (StartSpanOffset=175, PlayerIsAI=1, branch
         * fix-1778394064-46915 long_span280_pass) showed the opposite
         * effect: with the soft clamp, AI cars hit walls, then carry a
         * sign-flipped tangential velocity that prevents them from
         * settling parallel to the wall — they roll back, get hit again,
         * and stall permanently at span ~430. Original AI cars in the
         * same geometry cruise through unobstructed (slot 3 reached
         * span 1373 vs port's 430 in 2882 ticks).
         *
         * Ghidra agent re-read of 0x00406B65-69 (verbatim disasm: TEST/
         * JLE + XOR ECX,ECX) confirmed both branches hard-zero on
         * sign-flip in the original. The earlier "no clamp on v_para<=0"
         * memory note was a misread.
         *
         * Branch order matches the asm (positive first). */
        /* `v_para_round` mirrors the original `(v_para + (v_para>>31 &
         * 0x3F)) >> 6` round-toward-zero pattern. D3 audit: GCC `>> 6` on
         * negative values rounds toward -inf, off by 1 unit; original adds
         * 0x3F before SAR. */
        int32_t v_para_round = (v_para + ((v_para >> 31) & 0x3F)) >> 6;
        int32_t tmp;
        if (v_para > 0) {
            tmp = v_para_round + 0x800 + iVar11 * 2;
            /* D3 round-toward-zero on the >>11 of (tmp * 0x180). */
            int32_t mul_raw = tmp * 0x180;
            int32_t delta = (mul_raw + ((mul_raw >> 31) & 0x7FF)) >> 11;
            pre_clamp_delta = delta;
            new_v_para = v_para - delta;
            if (new_v_para < 0) { new_v_para = 0; clamp_fired = 1; }
        } else {
            tmp = (iVar11 * 2 + 0x800) - v_para_round;
            int32_t mul_raw = tmp * 0x180;
            int32_t delta = (mul_raw + ((mul_raw >> 31) & 0x7FF)) >> 11;
            pre_clamp_delta = delta;
            new_v_para = v_para + delta;
            /* Faithful port of 0x00406B65-69: TEST EAX,EAX / JLE
             * (jump if signed less-or-equal preserves) / fall through to
             * XOR ECX,ECX. → clamp to 0 on positive-flip. */
            if (new_v_para > 0) { new_v_para = 0; clamp_fired = 1; }
        }

        /* Angular velocity update: ω += (impulse * iVar9) / (K / 0x28C).
         * K / 0x28C = 1500000 / 652 = 2300 (integer). This is the yaw
         * alignment kick: with iVar9 non-zero (probe not at CoM along wall),
         * the sign of impulse drives the car toward tangential alignment.
         * [CONFIRMED @ 0x00406B6B-0x00406B7A] */
        int32_t ang_div = V2W_INERTIA_K / ANGULAR_DIVISOR_W;  /* 2300 */
        if (ang_div == 0) ang_div = 1;
        actor->angular_velocity_yaw += (impulse * iVar9) / ang_div;

        /* Rotate (new_v_para, new_v_perp) back to world basis.
         * [CONFIRMED @ 0x406a10]. D6 audit: round-toward-zero on the >>12,
         * matching original (val + (val>>31 & 0xFFF)) >> 12 pattern. */
        int64_t lvx_raw = (int64_t)new_v_para * cos_w - (int64_t)new_v_perp * sin_w;
        int64_t lvz_raw = (int64_t)new_v_para * sin_w + (int64_t)new_v_perp * cos_w;
        actor->linear_velocity_x = (int32_t)((lvx_raw + ((lvx_raw >> 63) & 0xFFF)) >> 12);
        actor->linear_velocity_z = (int32_t)((lvz_raw + ((lvz_raw >> 63) & 0xFFF)) >> 12);

        /* No ±6000 yaw clamp here — original has none inside 0x406980.
         * [CONFIRMED — no write to actor+0x1C4 after LAB_00406B6B] */

        /* Track contact flag: Forward/Reverse handlers pass side=-1 (no write).
         * The original wrote nothing of this kind directly inside the iVar11>=0
         * block either — track_contact_flag is a port-only field used by the
         * port's lateral-handler dispatch (which we have disabled). It's
         * still useful as a per-tick "did wall fire" marker. Gated inside
         * the iVar11>=0 branch per D1: separating contacts shouldn't flag.
         * [CONFIRMED @ 0x406d7e/0x406e4e on the side-encoding mapping] */
        if (side >= 0)
            actor->track_contact_flag = (uint8_t)(side + 1);

        /* [FIX 2026-05-24 OVERSIGHT: wall_impact_sfx_ff_missing; orig 0x00406B70..0x00406CB7]
         * After computing the wall impulse, the orig plays a sound + FF rumble
         * if v_perp (= iVar10) > 0x3200. Two SFX tiers based on impulse
         * magnitude; FF rumble channel selected by route heading delta.
         * Visual / audio only — no sim feedback. */
        if (v_perp > 0x3200) {
            /* [#10 telemetry] A meaningful wall impact (same threshold the
             * original uses to play the crunch SFX). Flag a collision for this
             * racer's per-tick edge-detector; the accumulate pass turns a
             * contiguous run of flagged ticks into a single counted collision. */
            td5_physics_mark_collision((int)actor->slot_index);

            int32_t pitch_arg = v_perp - 0x2000;
            if (pitch_arg < 0x400) pitch_arg = 0x400;
            else if (pitch_arg > 0x800) pitch_arg = 0x800;
            int variant = (v_perp < 0x19001) ? 0x16 : 0x1b;
            int mag     = (v_perp < 0x19001) ? 0x5622 : 0x2198;
            int volume  = (v_perp < 0x19001) ? 1 : 4;
            int32_t wpos[3] = { actor->world_pos.x, actor->world_pos.y, actor->world_pos.z };
            /* Port td5_sound_play_at_position param order matches orig:
             * (variant, pitch, mag, &pos, volume_or_variant_count). */
            td5_sound_play_at_position(variant, pitch_arg, mag, wpos, volume);

            /* DecayUltimateVariantTimer(actor, 1) [orig 0x0040A440]: inlined to
             * match existing call sites at td5_physics.c:5410/5468. */
            if (g_td5.special_encounter_enabled == 4 && actor->finish_time == 0) {
                if (actor->clean_driving_score > 0) actor->clean_driving_score -= 1;
                if (actor->clean_driving_score < 0) actor->clean_driving_score  = 0;
            }

            /* Force-feedback rumble: magnitude = (v_perp + sign-bit&3) >> 2,
             * capped at 100000. Channel chosen by route heading delta XOR'd
             * with the contact side when |heading_delta - 0x800| < 0x400. */
            int32_t ff_mag = (v_perp + ((v_perp >> 31) & 3)) >> 2;
            if (ff_mag > 99999) ff_mag = 100000;

            /* [FIX 2026-05-25 crash-compute-heading-delta-stride]: the route-state
             * stride is RS_STRIDE_DWORDS = 0x47 DWORDS (= 0x11C bytes), not 0x47
             * BYTES.  Previous "(uint8_t *)g_route_data + slot*0x47" landed in
             * garbage for any slot > 0; td5_compute_heading_delta then read
             * rs[0x35] (RS_SLOT_INDEX) from that garbage, multiplied by 0x388
             * (actor stride) and crashed at "actor + 0x1F4".  Reproduced as
             * "CRASH at EIP=0071CB97" on Scotland span 191/203 and Moscow
             * span 224 (high-velocity wall impacts triggering this tail block).
             * Use td5_ai_get_route_state() which already applies the correct
             * stride. */
            extern int32_t *td5_ai_get_route_state(int slot);
            uint32_t hd = 0;
            {
                int32_t *rs = td5_ai_get_route_state((int)actor->slot_index);
                if (rs) hd = td5_compute_heading_delta(rs);
            }
            uint32_t local_flags = (side < 0) ? 0u : (uint32_t)(side + 1);
            if ((int32_t)hd > 0x3FF && (int32_t)hd < 0xC00) local_flags ^= 3;
            /* [item #5(1)(2)] Route the wall impact through td5_input_ff_collision
             * (decaying directional pulse) instead of the old persistent
             * td5_input_ff_play_effect: local_flags 1/2 encode the contact side, so
             * map them to the LEFT (0x10) / RIGHT (0x40) contact-side bits the FF
             * layer uses to pick the impacted-side motor. actor_b_slot = -1 (single
             * actor, ignored). The raw ff_mag is in the same 0..100000 domain
             * td5_input_ff_collision expects (it divides by COLLISION_DIV). */
            if (local_flags == 1) {
                td5_input_ff_collision(0x10, (int)actor->slot_index, -1, ff_mag);
            } else if (local_flags == 2) {
                td5_input_ff_collision(0x40, (int)actor->slot_index, -1, ff_mag);
            }
        }
    }

    TD5_LOG_I(LOG_TAG,
              "wall_response: side=%d pen=%d angle=%d arm=%d iVar11=%d imp=%d vpara=%d vperp=%d yaw=%d delta=%d",
              side, penetration, wall_angle, iVar9, iVar11, impulse,
              new_v_para, new_v_perp, actor->angular_velocity_yaw,
              pre_clamp_delta);

    if (clamp_fired && actor->slot_index == 0) {
        TD5_LOG_I(LOG_TAG,
                  "wall_response: soft-clamp fired slot0 vpara_in=%d delta=%d vpara_out=%d",
                  v_para, pre_clamp_delta, new_v_para);
    }

    if (actor->slot_index == 0) {
        TD5_LOG_I(LOG_TAG,
                  "wall_delta slot0: bm=0x%02x pos_y=%d pre{ar=%d ap=%d ay=%d dr=%d dp=%d} "
                  "post{ar=%d ap=%d ay=%d dr=%d dp=%d}",
                  (int)pre_bm, pre_pos_y,
                  pre_av_roll, pre_av_pitch, pre_av_yaw,
                  pre_disp_r, pre_disp_p,
                  actor->angular_velocity_roll, actor->angular_velocity_pitch,
                  actor->angular_velocity_yaw,
                  (int)actor->display_angles.roll, (int)actor->display_angles.pitch);
    }

    /* Pilot trace: capture post-state and emit row. */
}

/* ======== [split] V2V cluster: OBB test .. ResolveVehicleContacts (moved verbatim from td5_physics.c) ======== */
/* ========================================================================
 * OBB Corner Test -- FUN_00408570
 *
 * Tests 8 corners (4 of each car) against the other car's OBB.
 * Returns bitmask: bits 0-3 = B corners inside A, bits 4-7 = A corners inside B.
 * For each penetrating corner, stores {projX, projZ, penX, penZ} in corners[].
 *
 * Car bbox from carData pointer at actor+0x1B8 [CONFIRMED @ 0x407ADB-0x407ADF]:
 *   carData+0x04 = front_z  (int16, positive)  — forward Z extent from center
 *   carData+0x08 = half_w   (int16, positive)  — lateral X extent
 *   carData+0x14 = rear_z   (int16, negative)  — backward Z extent (stored signed)
 *
 * Prior port had these SWAPPED (0x04 → "halfWidth", 0x08 → "halfLength"),
 * which produced corners laid out with front/rear-Z along the X axis and
 * half-width along the Z axis. The degenerate bounds check
 * `cx in [-rear_z, front_z]` ≈ `[160, 156]` (empty range) made nearly every
 * test fail except for corners whose integer math collapsed to cx=0. That
 * left ~12k spurious "contacts" per lap with cx=0 and tiny rel_vel → imp=0.
 * The mapping above is verified by how 0x4079C0's case split consumes
 * them: `side_extent = cardef[0x08] - |cx_A|` requires 0x08 = half-width.
 * ======================================================================== */

/* [SILHOUETTE HITBOX 2026-06-26] Per-car 2D convex hull of the model's top-down
 * (X,Z) silhouette, in the car's LOCAL frame, model units (same units as the
 * mesh box). Built once per race from every mesh vertex (gift-wrapping, integer
 * cross products -> fully deterministic for netplay/replay). The V2V collision
 * tests hull-vs-hull so a contact registers when the actual outlines touch, not
 * when the looser bounding boxes do. Falls back to the mesh box when a car's
 * hull can't be built (mesh missing, too many points, degenerate). */
#define HULL_MAX_PTS 32
static int16_t s_hull[TD5_MAX_TOTAL_ACTORS][HULL_MAX_PTS][2];     /* [i] = {x, z} local */
static int16_t s_hull_nrm[TD5_MAX_TOTAL_ACTORS][HULL_MAX_PTS][2]; /* edge i inward unit normal, Q12 */
static uint8_t s_hull_n[TD5_MAX_TOTAL_ACTORS];                    /* vertex count (>=3) */
static uint8_t s_hull_valid[TD5_MAX_TOTAL_ACTORS];

/* Silhouette (convex-hull) V2V collision — default ON; TD5RE_SILHOUETTE_HITBOX=0
 * reverts to the looser mesh-AABB box test. */
static int silhouette_hitbox_enabled(void)
{
    static int s_init = 0, s_on = 1;
    if (!s_init) {
        s_on = td5_env_flag_on("TD5RE_SILHOUETTE_HITBOX");
        s_init = 1;
        TD5_LOG_I(LOG_TAG, "silhouette_hitbox: %s (TD5RE_SILHOUETTE_HITBOX)",
                  s_on ? "ON (hull-vs-hull)" : "OFF (box)");
    }
    return s_on;
}

/* Penetration of point (px,pz) [target local frame, model units] into target
 * slot's convex hull. Returns 1 + depth (>=0 = distance to the nearest hull
 * edge) when inside; 0 when outside. Pure integer (edge normals Q12) -> det. */
static int hull_point_penetration(int slot, int32_t px, int32_t pz, int32_t *pen_out)
{
    int n, i;
    int32_t minpen = 0x7FFFFFFF;
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS || !s_hull_valid[slot]) return 0;
    n = (int)s_hull_n[slot];
    for (i = 0; i < n; i++) {
        int32_t vx = s_hull[slot][i][0], vz = s_hull[slot][i][1];
        int32_t nx = s_hull_nrm[slot][i][0], nz = s_hull_nrm[slot][i][1];
        int32_t d  = (int32_t)((((int64_t)(px - vx) * nx) + ((int64_t)(pz - vz) * nz)) >> 12);
        if (d < 0) return 0;             /* beyond this edge -> outside the hull */
        if (d < minpen) minpen = d;
    }
    *pen_out = (minpen == 0x7FFFFFFF) ? 0 : minpen;
    return 1;
}

static int obb_corner_test(TD5_Actor *a, TD5_Actor *b,
                           int32_t pos_a_x_fp, int32_t pos_a_z_fp,
                           int32_t pos_b_x_fp, int32_t pos_b_z_fp,
                           int32_t yaw_a_acc, int32_t yaw_b_acc,
                           OBB_CornerData corners[8])
{
    int result = 0;

    /* [MESH HITBOX 2026-06-25] Collision half-extents from each car's OWN model
     * (pure mesh AABB, no fixed scale — see actor_collision_box). Replaces the
     * old cardef*0.85/0.70 fudge so a contact registers precisely when the
     * meshes touch. Falls back to the legacy cardef*scale box when no mesh box
     * exists for the slot / the knob is off. Both cars draw from the same source
     * so every corner / penetration value below stays self-consistent.
     * (Mapping per the header comment: front_z +, half_w +, rear_z stored -.) */
    int32_t half_w_a, front_z_a, rear_z_a;
    int32_t half_w_b, front_z_b, rear_z_b;
    actor_collision_box(a, &half_w_a, &front_z_a, &rear_z_a);
    actor_collision_box(b, &half_w_b, &front_z_b, &rear_z_b);

    /* [LISTING ALIGNMENT 0x00408570]: the original's
     *   CollectVehicleCollisionContacts(int param_1, int param_2,
     *                                   int *param_3, int *param_4, short *param_5)
     * receives raw 24.8-fp position triplets (param_3 = &local_80 = pos triplet of A,
     * etc.) and raw 24.8-fp euler accumulators (param_4 = &local_94 = yaw triplet),
     * then computes:
     *   iVar3  = (param_3[5] - param_3[2]) >> 8;          // (pos_b.z - pos_a.z) >> 8
     *   iVar24 = (param_3[3] - *param_3) >> 8;            // (pos_b.x - pos_a.x) >> 8
     *   uVar25 = (uint)(short)((*param_4 - param_4[1]) >> 8); // (yaw_a - yaw_b) >> 8
     *   CosFixed12bit(*param_4 >> 8);                     // yaw_a >> 8
     *   CosFixed12bit(param_4[1] >> 8);                   // yaw_b >> 8
     * — i.e. the >>8 happens on the DELTA (positions) or on the raw accumulator
     * (headings), INSIDE the callee. The prior port wrapper required the caller
     * to pre-shift, so two callers in resolve_vehicle_collision_pair both did
     * (pos_X >> 8) before calling. That introduced an off-by-1-LSB divergence
     * for any pair whose fractional bytes underflowed on subtract
     * (e.g. pos_a=255, pos_b=0: orig (0-255)>>8 = -1; pre-shifted (0>>8)-(255>>8)=0).
     *
     * Fix (this commit): match the listing — wrapper now takes raw 24.8 fp pos +
     * raw 24.8 fp yaw accumulators, computes the same deltas-then-shift and
     * raw-then-shift internally. */
    int32_t delta_world_x = (pos_b_x_fp - pos_a_x_fp) >> 8;
    int32_t delta_world_z = (pos_b_z_fp - pos_a_z_fp) >> 8;
    int32_t heading_a = yaw_a_acc >> 8;            /* matches `*param_4 >> 8` */
    int32_t heading_b = yaw_b_acc >> 8;            /* matches `param_4[1] >> 8` */
    /* dheading derived from raw accumulator subtract then shift, matching
     * `uVar25 = (uint)(short)((*param_4 - param_4[1]) >> 8)`. The (short) cast
     * means the LOW 16 bits are then treated as int — equivalent in our path
     * because cos_fixed12/sin_fixed12 mask with & 0xFFF internally. */
    int32_t dheading_raw_shift = (yaw_a_acc - yaw_b_acc) >> 8;

    /* Pool15 V2V pilot trace — capture inputs at entry. */
    /* Pilot snapshot stores raw 24.8 fp coords for direct comparison with the
     * Frida capture of the original callee. */

    /* Precompute sin/cos for each heading. cos_fixed12/sin_fixed12 mask with
     * & 0xFFF internally, so passing the raw (yaw_acc >> 8) is safe and matches
     * the listing's CosFixed12bit(*param_4 >> 8) / CosFixed12bit(param_4[1] >> 8). */
    int32_t cos_a = cos_fixed12(heading_a);
    int32_t sin_a = sin_fixed12(heading_a);
    int32_t cos_b = cos_fixed12(heading_b);
    int32_t sin_b = sin_fixed12(heading_b);

    /* Delta heading. Listing uses `(*param_4 - param_4[1]) >> 8` =
     * (yaw_a - yaw_b) >> 8 = +dheading_inv (rotation from A to B).
     * The original then feeds that uVar25 into CosFixed12bit/SinFixed12bit and
     * uses the resulting cos/sin to rotate B's CORNERS into A's frame
     * (= "+dheading_inv" rotates B-frame vectors into A-frame; the inverse of
     * the orientation difference). Port's dheading_inv variable is the same. */
    int32_t dheading_inv = dheading_raw_shift & 0xFFF;
    int32_t cos_di = cos_fixed12(dheading_inv);
    int32_t sin_di = sin_fixed12(dheading_inv);

    /* For the symmetric second half (A's corners into B's frame), the rotation
     * is the opposite sign: dheading = -dheading_inv. */
    int32_t dheading = (-dheading_raw_shift) & 0xFFF;
    int32_t cos_d = cos_fixed12(dheading);
    int32_t sin_d = sin_fixed12(dheading);

    /* B's 4 corners in B's local frame. Layout (X=lateral, Z=forward):
     *   0 = FL  (-half_w, front_z)
     *   1 = FR  (+half_w, front_z)
     *   2 = RR  (+half_w, rear_z)   ← rear_z is stored negative
     *   3 = RL  (-half_w, rear_z)                                         */
    int32_t b_corners_lx[4] = { -half_w_b, +half_w_b, +half_w_b, -half_w_b };
    int32_t b_corners_lz[4] = {  front_z_b, front_z_b,  rear_z_b,  rear_z_b };

    /* A's 4 corners in A's local frame (same layout as B's) */
    int32_t a_corners_lx[4] = { -half_w_a, +half_w_a, +half_w_a, -half_w_a };
    int32_t a_corners_lz[4] = {  front_z_a, front_z_a,  rear_z_a,  rear_z_a };

    /* --- Test B's corners in A's OBB (bits 0-3) --- */
    /* World-space delta from A to B, in display units. Listing computes this
     * as (param_3[3] - *param_3) >> 8 and (param_3[5] - param_3[2]) >> 8,
     * i.e. subtraction on the raw 24.8 fp THEN >>8. The previously pre-shifted
     * (pos_b>>8) - (pos_a>>8) form drifts by 1 LSB on fractional-underflow. */
    int32_t delta_x = delta_world_x;
    int32_t delta_z = delta_world_z;

    /* Rotate delta into A's local frame.
     * [CONFIRMED @ 0x004086D0-0x004086F6]: game uses "CW from +Z" convention
     *   iVar20 = iVar6 * iVar24 - iVar7 * iVar3  (cos*delta_x - sin*delta_z)
     *   iVar6  = iVar6 * iVar3  + iVar7 * iVar24 (sin*delta_x + cos*delta_z)
     * Earlier port had BOTH sin terms inverted (CCW from +X / world rotated
     * by -yaw_a). That produced false bitmask=0x28 contacts on the Jarash
     * stationary spawn — exactly the visible-slide symptom in memory
     * `reference_obb_corner_test_rotation_sign`. Algorithmic re-validation
     * vs 501 captured original inputs (tools/validate_pool15_v2v_contact_math.py)
     * dropped bitmask divergence from 66.7% to 0% with the sign flip. */
    /* Rotate WORLD delta into A's local frame.
     * [CONFIRMED @ 0x00408570 CollectVehicleCollisionContacts]:
     *   local_dx = cos(A.yaw)*delta_x - sin(A.yaw)*delta_z
     *   local_dz = sin(A.yaw)*delta_x + cos(A.yaw)*delta_z
     * The game stores yaw in "CW from +Z" convention (matches AngleFromVector12
     * argument order (dz, dx)). The earlier port formula (cos*x+sin*z, -sin*x+cos*z)
     * was the standard math "CCW from +X" rotation and inverted slot 1's apparent
     * position in slot 0's frame. */
    int32_t local_dx = (delta_x * cos_a - delta_z * sin_a) >> 12;
    int32_t local_dz = (delta_x * sin_a + delta_z * cos_a) >> 12;

    /* [SILHOUETTE HITBOX 2026-06-26] Hull-vs-hull path. Test each car's convex
     * silhouette vertices against the OTHER car's outline (instead of the looser
     * box corners) so a contact registers when the models actually touch. The
     * deepest penetrating vertex per direction collapses into corners[0]/[4]
     * (bits 0/4) carrying the TRUE hull penetration depth, so the unchanged
     * downstream bisection / depenetration / impulse response runs as before
     * (apply_collision_response reads proj/own for the contact; v2v_depenetrate
     * reads min(|pen_x|,|pen_z|) for the push depth). Both directions use the
     * SAME rotations the box test uses below. Falls through to the faithful box
     * test when disabled or either hull is unavailable. */
    if (silhouette_hitbox_enabled() &&
        a->slot_index >= 0 && a->slot_index < TD5_MAX_TOTAL_ACTORS &&
        b->slot_index >= 0 && b->slot_index < TD5_MAX_TOTAL_ACTORS &&
        s_hull_valid[a->slot_index] && s_hull_valid[b->slot_index]) {
        int a_slot = (int)a->slot_index, b_slot = (int)b->slot_index;
        /* A-to-B delta in B's frame for the second direction (matches the
         * local2_dx/local2_dz the box loop computes further below). */
        int32_t l2dx = ((-delta_world_x) * cos_b - (-delta_world_z) * sin_b) >> 12;
        int32_t l2dz = ((-delta_world_x) * sin_b + (-delta_world_z) * cos_b) >> 12;
        int32_t best_pen, bcx, bcz, pen = 0;
        int cnt, k;
        /* The depenetration subtracts anti_tunnel_slop to compensate the BOX-vs-
         * mesh margin. Our penetration is already mesh-exact (hull), so add the
         * slop back here; (depth - slop) then nets the true hull depth and cars
         * settle at real outline contact with no gap and no overlap. */
        int32_t slop = g_td5.ini.anti_tunnel_slop;

        /* Direction 1: B's silhouette vertices into A's hull -> bit 0. */
        best_pen = -1; bcx = 0; bcz = 0; cnt = (int)s_hull_n[b_slot];
        for (k = 0; k < cnt; k++) {
            int32_t hx = s_hull[b_slot][k][0], hz = s_hull[b_slot][k][1];
            int32_t cx = (hx * cos_d - hz * sin_d) >> 12;
            int32_t cz = (hx * sin_d + hz * cos_d) >> 12;
            cx += local_dx; cz += local_dz;
            if (hull_point_penetration(a_slot, cx, cz, &pen) && pen > best_pen) {
                best_pen = pen; bcx = cx; bcz = cz;
            }
        }
        if (best_pen >= 0) {
            result |= 1;
            corners[0].proj_x = (int16_t)bcx;          corners[0].proj_z = (int16_t)bcz;
            corners[0].own_x  = (int16_t)(bcx - local_dx); corners[0].own_z = (int16_t)(bcz - local_dz);
            corners[0].pen_x  = (int16_t)(best_pen + slop); corners[0].pen_z = (int16_t)(best_pen + slop);
        }

        /* Direction 2: A's silhouette vertices into B's hull -> bit 4. */
        best_pen = -1; bcx = 0; bcz = 0; cnt = (int)s_hull_n[a_slot];
        for (k = 0; k < cnt; k++) {
            int32_t hx = s_hull[a_slot][k][0], hz = s_hull[a_slot][k][1];
            int32_t cx = (hx * cos_di - hz * sin_di) >> 12;
            int32_t cz = (hx * sin_di + hz * cos_di) >> 12;
            cx += l2dx; cz += l2dz;
            if (hull_point_penetration(b_slot, cx, cz, &pen) && pen > best_pen) {
                best_pen = pen; bcx = cx; bcz = cz;
            }
        }
        if (best_pen >= 0) {
            result |= (1 << 4);
            corners[4].proj_x = (int16_t)bcx;          corners[4].proj_z = (int16_t)bcz;
            corners[4].own_x  = (int16_t)(bcx - l2dx);  corners[4].own_z  = (int16_t)(bcz - l2dz);
            corners[4].pen_x  = (int16_t)(best_pen + slop); corners[4].pen_z = (int16_t)(best_pen + slop);
        }
        return result;
    }

    for (int i = 0; i < 4; i++) {
        /* Rotate B's corner from B's local frame into A's local frame */
        int32_t cx = (b_corners_lx[i] * cos_d - b_corners_lz[i] * sin_d) >> 12;
        int32_t cz = (b_corners_lx[i] * sin_d + b_corners_lz[i] * cos_d) >> 12;

        /* Translate by A-to-B delta in A's frame */
        cx += local_dx;
        cz += local_dz;

        /* Test if within A's box: |cx| <= half_w_a and rear_z_a <= cz <= front_z_a */
        if (cx >= -half_w_a && cx <= half_w_a &&
            cz >= rear_z_a  && cz <= front_z_a) {
            result |= (1 << i);
            corners[i].proj_x = (int16_t)cx;
            corners[i].proj_z = (int16_t)cz;
            /* contactData[2,3] = rotated corner offset in target's frame
             * [CONFIRMED @ 0x00408763/0x00408771]: original stores
             *   contactData[2] = iVar27 - sVar1 = corner_in_A - B_center_in_A
             *   contactData[3] = iVar28 - sVar2
             * i.e. the penetrator's static corner ROTATED by (heading_b-heading_a)
             * but WITHOUT the A→B translation. Equivalent to (cx - local_dx,
             * cz - local_dz). Previously stored raw unrotated b_corners_lx/lz,
             * which gave the impulse solver a wrong-orientation lever arm for
             * any non-aligned-heading contact. */
            corners[i].own_x = (int16_t)(cx - local_dx);
            corners[i].own_z = (int16_t)(cz - local_dz);
            /* Penetration depth along each face normal (signed).
             * Minimum |pen| identifies the closest face. */
            int32_t pen_right = half_w_a  - cx;     /* to +X face */
            int32_t pen_left  = cx + half_w_a;      /* to -X face */
            int32_t pen_front = front_z_a - cz;     /* to +Z face */
            int32_t pen_back  = cz - rear_z_a;      /* to -Z face */
            corners[i].pen_x = (int16_t)((pen_right < pen_left) ? pen_right : -pen_left);
            corners[i].pen_z = (int16_t)((pen_front < pen_back) ? pen_front : -pen_back);
        }
    }

    /* --- Test A's corners in B's OBB (bits 4-7) --- */
    /* World-space delta from B to A = negation of the A-to-B delta. The
     * listing's second half re-uses the same iVar3/iVar24 with sign-flipped
     * sin/cos terms (see iVar15 = sin_b*dz - cos_b*dx at 0x004088E0). */
    int32_t delta2_x = -delta_world_x;
    int32_t delta2_z = -delta_world_z;

    /* Rotate delta into B's local frame.
     * [CONFIRMED @ second half of 0x00408570]: symmetric sign convention
     * — same "CW from +Z" world→local form as the B-in-A half above. */
    /* Rotate WORLD delta into B's local frame — same "CW from +Z" convention. */
    int32_t local2_dx = (delta2_x * cos_b - delta2_z * sin_b) >> 12;
    int32_t local2_dz = (delta2_x * sin_b + delta2_z * cos_b) >> 12;

    for (int i = 0; i < 4; i++) {
        /* Rotate A's corner from A's local frame into B's local frame */
        int32_t cx = (a_corners_lx[i] * cos_di - a_corners_lz[i] * sin_di) >> 12;
        int32_t cz = (a_corners_lx[i] * sin_di + a_corners_lz[i] * cos_di) >> 12;

        /* Translate by B-to-A delta in B's frame */
        cx += local2_dx;
        cz += local2_dz;

        if (cx >= -half_w_b && cx <= half_w_b &&
            cz >= rear_z_b  && cz <= front_z_b) {
            result |= (1 << (i + 4));
            corners[i + 4].proj_x = (int16_t)cx;
            corners[i + 4].proj_z = (int16_t)cz;
            /* Symmetric second half: rotated corner offset in B's frame.
             * [CONFIRMED @ 0x004089EB] — original applies the same subtraction
             * with sVar1/sVar2 = A_center in B's frame. */
            corners[i + 4].own_x = (int16_t)(cx - local2_dx);
            corners[i + 4].own_z = (int16_t)(cz - local2_dz);
            int32_t pen_right = half_w_b  - cx;
            int32_t pen_left  = cx + half_w_b;
            int32_t pen_front = front_z_b - cz;
            int32_t pen_back  = cz - rear_z_b;
            corners[i + 4].pen_x = (int16_t)((pen_right < pen_left) ? pen_right : -pen_left);
            corners[i + 4].pen_z = (int16_t)((pen_front < pen_back) ? pen_front : -pen_back);
        }
    }

    /* Pool15 V2V pilot trace — capture outputs at exit. */
    {
        for (int _i = 0; _i < 8; _i++) {
        }
    }

    return result;
}

/* ========================================================================
 * Collision Response -- FUN_004079c0 ApplyVehicleCollisionImpulse
 *
 * Rewrite (2026-04-11) to match the original's case-split impulse model.
 * Prior port used a 2D minimum-penetration-axis model; the original does NOT.
 *
 * Algorithm [CONFIRMED @ 0x4079C0 via Ghidra pass]:
 *   1. Save angular velocities (prologue).
 *   2. Rotate both actors' linear velocities into A's local frame using
 *      cos/sin of `angle` (= target's world yaw).
 *        local_54 = A tangent, local_50 = A normal
 *        local_4c = B tangent, local_44 = B normal
 *   3. Case split on contactData[1] (cz_A) sign vs. A's box extents to pick
 *      SIDE or FRONT/REAR impact branch.
 *   4. Branch body (either side or front/rear):
 *      - Apply positional push along ±sin/cos of angle.
 *      - Compute mass polynomial using z² (side) or x² (front) terms.
 *      - Angular contribution: (cz_B*ω_B - cz_A*ω_A)/0x28C for side, (cx ...)
 *        for front.
 *      - Impulse scalar = (500000>>8 * 0x1100) / (poly>>8) * rel_vel, >>12.
 *      - Sign rejection: if ((cx_B - cx_A) ^ impulse) < 0 → return (separating).
 *      - Update tangent/normal velocity channels and angular deltas.
 *   5. TOI rollback: pos/euler -= (0x100 - impactForce) * vel >> 8 for both.
 *   6. Commit new angular velocity (saved + delta).
 *   7. Rotate linear velocities back to world frame from tangent/normal.
 *   8. TOI re-advance: pos/euler += (0x100 - impactForce) * new_vel >> 8.
 *   9. UpdateVehiclePoseFromPhysicsState on both actors.
 *  10. Impact magnitude = |(mass_A + mass_B) * impulse|; apply damage/sfx.
 *
 * Key constants:
 *   INERTIA_K      = 500000 (DAT_00463204)
 *   NUM_SCALE      = 0x1100 (4352)
 *   ANG_DIVISOR    = 0x28C  (652)
 *   INERTIA_PER_ANG = 500000/652 = 766 (integer)
 *   Mass: cardef+0x88 (int16)
 *   Extents: cardef+0x04 (front-Z), cardef+0x08 (half-width),
 *            cardef+0x14 (rear-Z, stored negative).
 *
 * Caller interface: (penetrator, target, corner_idx, corner, heading, impactForce).
 * impactForce is now passed from the caller's position-based binary search.
 * Internally, "A" = target (frame owner whose yaw = angle), "B" = penetrator.
 * Contact data from OBB corner test [CONFIRMED @ 0x00408570]:
 *   cx_A, cz_A = corner->proj_x, proj_z   (corner in TARGET's frame, WITH translation)
 *   cx_B, cz_B = corner->own_x, own_z     (rotated corner OFFSET from penetrator's
 *                                          center, expressed in TARGET's frame —
 *                                          proj minus A→B center translation)
 * ======================================================================== */

#define V2V_NUM_SCALE        0x1100
#define V2V_ANG_DIVISOR      0x28C
#define V2V_INERTIA_PER_ANG  (V2V_INERTIA_K / V2V_ANG_DIVISOR)  /* 766 */

/* Signed round-to-zero divide by 256 — matches the original's
 * [CDQ; AND EDX,0xff; ADD EAX,EDX; SAR EAX,8] idiom used 12 times in the
 * TOI rollback/advance halves at 0x00407F31-0x004080A6 and 0x004080C8-0x0040816B.
 * For positive x: equivalent to x >> 8.
 * For negative x not divisible by 256: x >> 8 rounds toward -inf, this rounds toward 0. */
static inline int32_t v2v_sar8_rz(int32_t x) {
    return ((x < 0) ? (x + 0xFF) : x) >> 8;
}

/* Signed round-to-zero divide by 4096 — matches the original's
 * [CDQ; AND EDX,0xfff; ADD EAX,EDX; SAR EAX,0xc] idiom used at the four
 * velocity-rotation sites in the prologue and the four velocity writeback
 * sites in the tail, plus twice at the impulse-scale step. */
static inline int32_t v2v_sar12_rz(int32_t x) {
    return ((x < 0) ? (x + 0xFFF) : x) >> 12;
}

/* 64-bit signed round-to-zero divide by 4096 — used at the impulse final
 * scale where (NUM_CONST / denom) * rel_vel can exceed 31 bits before the
 * >>12 truncation. */
static inline int32_t v2v_sar12_rz_64(int64_t x) {
    return (int32_t)(((x < 0) ? (x + 0xFFF) : x) >> 12);
}

/* [task #9] TD5RE_TRAFFIC_HIT_TAME (default ON) — when a TRAFFIC car (slot >=
 * g_traffic_slot_base) is involved in a V2V collision, constrain the response so
 * it does NOT get launched into the air and cannot sink below the ground:
 *   - zero/clamp the vertical (Y) component injected by the collision impulse,
 *   - clamp the overall impulse magnitude that drives the crash-spin lift,
 *   - clamp post-collision world_pos.y so a traffic car never ends up under the
 *     track surface (the "clips through the ground" half of the report).
 * "0" reverts to the byte-faithful (launch + lift) response. */
static int traffic_hit_tame_enabled(void)
{
    static int s = -1;
    if (s < 0) {
        s = td5_env_flag_on("TD5RE_TRAFFIC_HIT_TAME");
        TD5_LOG_I(LOG_TAG, "traffic_hit_tame: TD5RE_TRAFFIC_HIT_TAME=%d", s);
    }
    return s;
}

/* [#1 WRECK IMMOBILE 2026-06-21] A broken-down (wrecked) car must sit still, not
 * reverse. The brake path's port-only low-speed REVERSE hand-off (gear=REVERSE +
 * clear brake) turns the wreck's negative stop-command into sustained backward
 * drive — the reported "broken cars reverse and keep accelerating backwards".
 * Suppress that hand-off for a broken car so it just brakes to rest and stays
 * (it is still pushable by a fresh collision). Default ON; TD5RE_WRECK_IMMOBILE=0
 * reverts. */
int wreck_immobile_enabled(void)
{
    static int s = -1;
    if (s < 0) {
        s = td5_env_flag_on("TD5RE_WRECK_IMMOBILE");
        TD5_LOG_I(LOG_TAG, "wreck_immobile: TD5RE_WRECK_IMMOBILE=%d", s);
    }
    return s;
}

/* [#2 LIGHT WRECK 2026-06-21] Make a broken-down car easy to shove out of the
 * way: scale its V2V mass term (+0x88, which behaves as an INVERSE mass here —
 * a bigger value = lighter / more mobile, so the wreck gains MORE velocity from
 * a hit while the car that hits it loses LESS and plows through). Verified:
 * dv = impulse*mass is written straight to linear_velocity (td5_physics.c ~6060),
 * and |dv_wreck| rises monotonically with the wreck's mass term. Percentage of
 * the stock mass; default 400 (= 4x lighter). TD5RE_WRECK_MASS_PCT=100 disables.
 * Clamped to [100,2000] so a wreck is never made HEAVIER than stock. */
static int wreck_mass_pct(void)
{
    static int v = -1;
    if (v < 0) {
        v = td5_env_int("TD5RE_WRECK_MASS_PCT", 400, 100, 2000);
        TD5_LOG_I(LOG_TAG, "wreck_mass_pct: TD5RE_WRECK_MASS_PCT=%d", v);
    }
    return v;
}

/* [traffic-lighter 2026-06-24] Make ORDINARY (non-wrecked) traffic easier to shove
 * out of the way when the player crashes into it. Same mechanism as wreck_mass_pct
 * (above), applied to live traffic instead of broken-down cars: cardef+0x88 behaves
 * as an INVERSE mass in the V2V impulse math [CONFIRMED @ 0x004079C0], so scaling a
 * traffic car's mass term UP gives it MORE velocity from a hit and bleeds LESS speed
 * off the car that hits it — the player plows through and the traffic flies aside.
 * Stock traffic mass is 32 (0x20) [CONFIRMED @ 0x0042F235]; the default 250 (= 2.5x
 * lighter) is noticeable but well short of the 4x wreck boost. Percentage of stock;
 * TD5RE_TRAFFIC_MASS_PCT=100 disables. Clamped [100,2000] so traffic is never made
 * HEAVIER than stock. Only applies to a traffic slot that is NOT broken down (a wreck
 * already gets wreck_mass_pct, which takes precedence so the two never compound). */
static int traffic_mass_pct(void)
{
    static int v = -1;
    if (v < 0) {
        v = td5_env_int("TD5RE_TRAFFIC_MASS_PCT", 250, 100, 2000);
        TD5_LOG_I(LOG_TAG, "traffic_mass_pct: TD5RE_TRAFFIC_MASS_PCT=%d", v);
    }
    return v;
}

/* [task #14 / #16] TD5RE_TRAFFIC_HITBOX_SCALE (default 0.70) — multiplier applied
 * to a TRAFFIC car's OBB half-extents (half-width / front-Z / rear-Z) in the V2V
 * corner test so the collision box matches the visible model instead of being
 * noticeably larger (the player "crashes" onto traffic without visually
 * touching). Lowered from 0.8 -> 0.70 (#16: traffic hitbox still felt too big).
 * Scales only when at least one of the pair is traffic, so racer-on-racer
 * contacts stay byte-faithful. "1"/"1.0" disables (no shrink). */
static float traffic_hitbox_scale(void)
{
    static int   s_init = 0;
    static float s_scale = 0.70f;
    if (!s_init) {
        s_scale = td5_env_float("TD5RE_TRAFFIC_HITBOX_SCALE", 0.70f, 0.1f, 1.0f);   /* floor 0.1; never inflate past 1.0 */
        s_init = 1;
        TD5_LOG_I(LOG_TAG, "traffic_hitbox_scale: TD5RE_TRAFFIC_HITBOX_SCALE=%.3f", s_scale);
    }
    return s_scale;
}

/* [item #4] TD5RE_HITBOX_FIT (default ON) + TD5RE_HITBOX_SCALE (default 0.85) —
 * multiplier applied to a RACER's OBB half-extents (half-width / front-Z / rear-Z)
 * in the V2V corner test + impulse branch-split, the analog of
 * TD5RE_TRAFFIC_HITBOX_SCALE but for the player and AI opponents (slots
 * 0..g_traffic_slot_base-1). The cardef OBB is authored noticeably larger than the
 * visible chassis mesh (see the "playability slop" in v2v_depenetrate_pair and the
 * traffic-shrink note in obb_corner_test), so without this the player takes a
 * collision response — impulse / speed loss / the acute-crash shake — while there is
 * still a visible gap to the other car ("invisible crash"). Conservative default
 * (0.85): cars still collide on genuine contact, just not across the gap.
 *
 * TD5RE_HITBOX_FIT=0 returns 1.0 (no shrink) -> byte-faithful racer boxes.
 * TD5RE_HITBOX_SCALE overrides the magnitude (clamped to [0.5, 1.0] — never inflate,
 * sane floor so cars cannot pass through each other). This affects vehicle-vs-vehicle
 * only; the player-vs-wall path is probe-based (wheel corners cross the rail, no
 * half-width added) so it is not inflated and is intentionally left faithful. */
static float racer_hitbox_scale(void)
{
    static int   s_init = 0;
    static float s_scale = 0.85f;
    if (!s_init) {
        const char *fit = getenv("TD5RE_HITBOX_FIT");
        if (fit && fit[0] == '0') {
            s_scale = 1.0f;   /* knob OFF -> faithful (no shrink) */
        } else {
            /* floor 0.5 (still collides); never inflate past 1.0 */
            s_scale = td5_env_float("TD5RE_HITBOX_SCALE", 0.85f, 0.5f, 1.0f);
        }
        s_init = 1;
        TD5_LOG_I(LOG_TAG, "racer_hitbox_scale: TD5RE_HITBOX_FIT/SCALE -> %.3f", s_scale);
    }
    return s_scale;
}

/* [item #4] Effective OBB half-extent scale for one actor in a V2V pair: traffic
 * actors keep their existing TD5RE_TRAFFIC_HITBOX_SCALE shrink, racers (player + AI)
 * use the new racer_hitbox_scale. A single helper so obb_corner_test and
 * apply_collision_response derive identical scaled extents (the impulse branch-split
 * re-reads the cardef, so it must match the corner test or the side/front decision
 * would use a different box than the overlap that produced the contact). */
static float actor_hitbox_scale(const TD5_Actor *act)
{
    if (act && act->slot_index >= g_traffic_slot_base)
        return traffic_hitbox_scale();
    return racer_hitbox_scale();
}

/* ========================================================================
 * [MESH HITBOX 2026-06-25] Model-derived V2V collision box.
 *
 * The original derives each car's collision box from its 3D model: at race
 * init ComputeVehicleSuspensionEnvelope (0x0042F6D0) scans EVERY mesh vertex
 * for the axis-aligned extents, then stores the box into the cardef — half_w =
 * max|x|+20 at 0x08, front/rear_z = ±(max|z|-20) at 0x04/0x14 — overwriting the
 * authored carparam bytes before any collision reader fires [CONFIRMED @0x42F6D0,
 * readers @0x408570/0x4079C0]. The port runs that envelope, but obb_corner_test
 * and apply_collision_response then ALSO multiplied those extents by a fixed
 * shrink (TD5RE_HITBOX_SCALE 0.85 / traffic 0.70) — a one-size-fits-all fudge
 * layered on an already-padded box, so the effective collision box matched
 * neither the model nor any single car (e.g. a car whose mesh is 343 wide ended
 * up with a 309 box, so it visibly overlapped before a contact registered).
 *
 * This replaces that fudge with the PURE mesh axis-aligned bounds: the V2V
 * collision box is EXACTLY the model's |x|/|z| silhouette (no +20/-20 padding,
 * no scale), so cars AND traffic register a crash precisely when their meshes
 * touch. The box is recomputed from each actor's own mesh, so it tracks the
 * model with no fixed tuning parameter. The padded 8-corner box, sphere radius
 * (0x80) and height (0x86) the envelope also writes are LEFT INTACT, so the
 * other consumers (AI lane bounds, suspension Y, wheel placement, the airborne
 * sphere-separation path) stay byte-faithful.
 *
 * Populated by compute_suspension_envelope (which already has the per-vertex
 * max) via mesh_box_store; read by actor_collision_box, which falls back to the
 * legacy cardef*scale box when no mesh box exists for the slot (mesh failed to
 * load) or TD5RE_MESH_HITBOX=0. Indexed by actor slot_index. */
static int16_t s_mesh_box[TD5_MAX_TOTAL_ACTORS][3];    /* {half_w, front_z, rear_z(neg)} */
static uint8_t s_mesh_box_valid[TD5_MAX_TOTAL_ACTORS];

static int mesh_hitbox_enabled(void)
{
    static int s_init = 0, s_on = 1;   /* default ON */
    if (!s_init) {
        s_on = td5_env_flag_on("TD5RE_MESH_HITBOX");
        s_init = 1;
        TD5_LOG_I(LOG_TAG, "mesh_hitbox: %s (TD5RE_MESH_HITBOX)",
                  s_on ? "ON (model-derived collision box)"
                       : "OFF (legacy cardef*scale box)");
    }
    return s_on;
}

/* Record the PURE mesh AABB extents for a slot. Called from the suspension
 * envelope with the same max|x| / max|z| it already scanned (before the +20/-20
 * padding). half_w / front_z are positive magnitudes; rear_z is stored negative
 * to mirror the cardef layout obb_corner_test expects. */
void mesh_box_store(int slot, int32_t half_w, int32_t front_z)
{
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return;
    if (half_w  < 1) half_w  = 1;          /* never degenerate */
    if (front_z < 1) front_z = 1;
    if (half_w  > 0x7FFF) half_w  = 0x7FFF;
    if (front_z > 0x7FFF) front_z = 0x7FFF;
    s_mesh_box[slot][0] = (int16_t)half_w;
    s_mesh_box[slot][1] = (int16_t)front_z;
    s_mesh_box[slot][2] = (int16_t)(-front_z);
    s_mesh_box_valid[slot] = 1;
}

/* [SILHOUETTE HITBOX] Build the 2D (X,Z) convex hull of a car's mesh vertices in
 * its local frame and cache it for slot. Gift-wrapping (Jarvis march): start at
 * the leftmost-lowest point and repeatedly take the most-clockwise next vertex.
 * All math is integer (float verts truncated to model units, the same units the
 * mesh box uses) with int64 cross products, so it is bit-identical across peers.
 * Leaves s_hull_valid[slot]=0 (box fallback) if the mesh is degenerate or the
 * hull exceeds HULL_MAX_PTS. */
void hull_build_store(int slot, const TD5_MeshVertex *verts, int n)
{
    int start, hcount, cur_safe;
    int32_t sx, sz, cx, cz;

    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return;
    s_hull_valid[slot] = 0;
    if (!verts || n < 3) return;

    /* Leftmost, then lowest, vertex — guaranteed on the hull. */
    start = 0;
    sx = (int32_t)verts[0].pos_x; sz = (int32_t)verts[0].pos_z;
    for (int i = 1; i < n; i++) {
        int32_t x = (int32_t)verts[i].pos_x, z = (int32_t)verts[i].pos_z;
        if (x < sx || (x == sx && z < sz)) { sx = x; sz = z; start = i; }
    }
    (void)start;

    hcount = 0;
    cx = sx; cz = sz;
    /* Walk the hull. cur_safe caps iterations against any degeneracy. */
    for (cur_safe = 0; cur_safe <= HULL_MAX_PTS + 1; cur_safe++) {
        int nxt = -1;
        int32_t nx = 0, nz = 0;
        if (hcount >= HULL_MAX_PTS) return;          /* too complex -> box fallback */
        s_hull[slot][hcount][0] = (int16_t)cx;
        s_hull[slot][hcount][1] = (int16_t)cz;
        hcount++;
        for (int i = 0; i < n; i++) {
            int32_t x = (int32_t)verts[i].pos_x, z = (int32_t)verts[i].pos_z;
            if (x == cx && z == cz) continue;        /* skip the current vertex/dups */
            if (nxt < 0) { nxt = i; nx = x; nz = z; continue; }
            {
                /* cross of (cand-cur) x (p-cur); <0 => p is more clockwise. */
                int64_t cr = (int64_t)(nx - cx) * (z - cz) -
                             (int64_t)(nz - cz) * (x - cx);
                if (cr < 0) { nxt = i; nx = x; nz = z; }
                else if (cr == 0) {
                    /* collinear: keep the farther point (extreme endpoint). */
                    int64_t dn = (int64_t)(x - cx) * (x - cx) + (int64_t)(z - cz) * (z - cz);
                    int64_t doo = (int64_t)(nx - cx) * (nx - cx) + (int64_t)(nz - cz) * (nz - cz);
                    if (dn > doo) { nxt = i; nx = x; nz = z; }
                }
            }
        }
        if (nxt < 0) return;                          /* degenerate -> box fallback */
        cx = nx; cz = nz;
        if (cx == sx && cz == sz) break;              /* wrapped back to start */
    }

    if (hcount >= 3) {
        /* Precompute each edge's INWARD unit normal (Q12) for point-in-hull /
         * penetration. Orient toward the centroid so winding doesn't matter. */
        int32_t csx = 0, csz = 0, cenx, cenz;
        s_hull_n[slot] = (uint8_t)hcount;
        for (int i = 0; i < hcount; i++) { csx += s_hull[slot][i][0]; csz += s_hull[slot][i][1]; }
        cenx = csx / hcount; cenz = csz / hcount;
        for (int i = 0; i < hcount; i++) {
            int j = (i + 1) % hcount;
            int32_t ex  = (int32_t)s_hull[slot][j][0] - s_hull[slot][i][0];
            int32_t ez  = (int32_t)s_hull[slot][j][1] - s_hull[slot][i][1];
            int32_t nrx = ez, nrz = -ex;           /* perpendicular to the edge */
            int32_t len;
            if ((int64_t)nrx * (cenx - s_hull[slot][i][0]) +
                (int64_t)nrz * (cenz - s_hull[slot][i][1]) < 0) { nrx = -nrx; nrz = -nrz; }
            len = td5_isqrt(nrx * nrx + nrz * nrz);
            if (len < 1) len = 1;
            s_hull_nrm[slot][i][0] = (int16_t)((nrx << 12) / len);
            s_hull_nrm[slot][i][1] = (int16_t)((nrz << 12) / len);
        }
        s_hull_valid[slot] = 1;
    }
}

/* Effective V2V collision half-extents for one actor. With the model-derived
 * box available (default) it is returned verbatim — precise to the mesh, no
 * fixed scale. Otherwise falls back to the cardef extents * actor_hitbox_scale
 * (legacy behaviour) so a car whose mesh never loaded still collides. */
static void actor_collision_box(const TD5_Actor *act,
                                int32_t *half_w, int32_t *front_z, int32_t *rear_z)
{
    int slot = act ? (int)act->slot_index : -1;
    if (mesh_hitbox_enabled() && slot >= 0 && slot < TD5_MAX_TOTAL_ACTORS &&
        s_mesh_box_valid[slot]) {
        *half_w  = (int32_t)s_mesh_box[slot][0];
        *front_z = (int32_t)s_mesh_box[slot][1];
        *rear_z  = (int32_t)s_mesh_box[slot][2];
        return;
    }
    /* Legacy fallback: cardef extents * fixed scale (pre-mesh-hitbox path). */
    {
        TD5_Actor *m = (TD5_Actor *)act;   /* get_cardef/CDEF_S take non-const */
        float s = actor_hitbox_scale(m);
        int32_t hw = (int32_t)CDEF_S(m, CDEF_HALF_WIDTH);
        int32_t fz = (int32_t)CDEF_S(m, CDEF_FRONT_Z_EXTENT);
        int32_t rz = (int32_t)CDEF_S(m, CDEF_REAR_Z_EXTENT);
        if (s < 1.0f) {
            hw = (int32_t)((float)hw * s);
            fz = (int32_t)((float)fz * s);
            rz = (int32_t)((float)rz * s);
        }
        *half_w = hw; *front_z = fz; *rear_z = rz;
    }
}

/* Clamp a (presumed traffic) actor's world_pos.y so it never sits below the
 * track surface at its current span — the "traffic clips through the ground"
 * recovery for task #9. Uses the same barycentric ground query the traffic pose
 * integrator uses (td5_track_compute_contact_height_with_normal). No-op if the
 * car is already at/above ground or the query is unavailable. */
static void traffic_clamp_above_ground(TD5_Actor *t)
{
    int span, lane;
    int16_t snrm[3] = {0, 1, 0};
    int32_t ground_y;
    if (!t) return;
    span = (int)t->track_span_raw;
    lane = (int)t->track_sub_lane_index;
    ground_y = td5_track_compute_contact_height_with_normal(
        span, lane, t->world_pos.x, t->world_pos.z, snrm);
    /* world_pos is Y-up 24.8 FP; "below ground" = world_pos.y < ground_y. */
    if (t->world_pos.y < ground_y) {
        t->world_pos.y = ground_y;
        if (t->linear_velocity_y < 0) t->linear_velocity_y = 0;
    }
}

/* [FIX 2026-05-28 — traffic crash-spin animation] Port of the heavy-impact
 * TRAFFIC branch of ApplyVehicleCollisionImpulse (orig 0x00408289+). When a
 * traffic vehicle (slot >= 6) takes a heavy hit it enters scripted recovery
 * (vehicle_mode=1): a per-tick spin matrix is latched into collision_spin_matrix,
 * the current orientation is snapshotted to saved_orientation, the car is popped
 * up (linear_velocity_y = impact/6), and the recovery animation plays for ~0x3B
 * frames before ResetVehicleActorState rights it (handled by the existing
 * vehicle_mode==1 dispatch + td5_physics_integrate_scripted_motion).
 *
 * The spin-angle MAGNITUDE is byte-faithful to the original (GetDamageRulesStub
 * @0x0042C8D0 returns 0, so the original's spin is deterministic, not random) and
 * is NOT cosmetic: it is precisely what keeps the recovery's per-frame angular
 * velocity above the early-exit threshold so the tumble stays visible for the full
 * ~0x3B-frame window. The per-tick angle formula is documented inline below. */
static void td5_physics_apply_traffic_crash_spin(TD5_Actor *t, int32_t impact_mag)
{
    if (!t) return;

    /* [FIX 2026-06-02 traffic-tumble-visible] Per-tick spin angles derived from the
     * impact magnitude exactly like the original traffic branch (orig 0x408289+),
     * NOT a state LCG. orig: scatter = max(impact_mag/4, 0x7FFF) [floor, same as the
     * racer scatter]; iVar = scatter >> 8; each of the 3 spin-matrix angles =
     * GetDamageRulesStub() % iVar - iVar/2. GetDamageRulesStub @0x0042C8D0 returns 0,
     * so the byte-faithful deterministic value is -(iVar/2).
     *
     * Why this fixes the INVISIBLE tumble: RefreshScriptedVehicleTransforms re-derives
     * angular_velocity_{roll,yaw,pitch} from this spin matrix every tick, and the
     * recovery exit (td5_physics_check_and_update_actor_collision_alignment, orig
     * CheckAndUpdateActorCollisionAlignment @0x00409520) calls ResetVehicleActorState
     * at frame_counter==3 if the car is still upright (bin 0) AND |ang_vel_roll| < 0x20
     * AND |ang_vel_pitch| < 0x20. The OLD state-LCG produced angles in [-128,127]
     * INDEPENDENT of impact and frequently < 0x20, so a fresh hit reset out of scripted
     * mode in ~3 frames -> no visible tumble. The original's -(iVar/2) is always >= 63
     * (scatter >= 0x7FFF -> iVar >= 127), so |ang_vel| > 0x20 on every axis, the
     * early-exit can never fire, and the car tumbles the full ~0x3B frames (until the
     * unconditional frame_counter > 0x3B reset at the tail of integrate_scripted_motion).
     * Magnitude scales with impact, so harder hits spin faster. */
    int32_t scatter = impact_mag / 4;
    if (scatter < 0x7FFF) scatter = 0x7FFF;            /* floor (orig 0x4082A2-C9) */
    int16_t spin_ang = (int16_t)(-((scatter >> 8) / 2));  /* -(iVar/2), <= -63 */
    int16_t ang[3] = { spin_ang, spin_ang, spin_ang };

    float spin[9];
    BuildRotationMatrixFromAngles(spin, ang);

    /* Latch per-tick spin + snapshot current orientation. Mirror the orig's
     * 12-float (48-byte) copies to +0x180 / +0x150 (trailing 3 floats are
     * residue but the orig writes them, so we match). */
    memcpy(&t->collision_spin_matrix, spin, 9 * sizeof(float));
    memcpy(((uint8_t *)&t->collision_spin_matrix) + 9 * sizeof(float),
           spin, 3 * sizeof(float));
    memcpy(&t->saved_orientation, &t->rotation_matrix, 9 * sizeof(float));
    memcpy(((uint8_t *)&t->saved_orientation) + 9 * sizeof(float),
           &t->render_pos, 3 * sizeof(float));

    int32_t lift = impact_mag / 6;
    if (lift > 200000) lift = 200000;
    /* [task #9] Tame the vertical launch: keep just enough pop to read as a
     * crash but stop the car flying off the road. The spin animation stays
     * (driven by the angular spin matrix above, not by Y velocity), so the
     * tumble is unaffected — only the height is curbed. */
    if (traffic_hit_tame_enabled()) {
        const int32_t TRAFFIC_LIFT_CAP = 24000;   /* ~93 world-u/tick (was up to 200000) */
        if (lift > TRAFFIC_LIFT_CAP) lift = TRAFFIC_LIFT_CAP;
    }
    /* [POLICE 2026-06-24] A cop must NEVER launch into the air on a collision
     * (user: "they shouldn't go up in the air after a collision"). Kill the
     * vertical pop entirely for a cop — the in-place spin animation still plays,
     * but the wreck stays glued to the road instead of flying off it. */
    if (t->slot_index >= 0 && td5_ai_actor_is_cop((int)t->slot_index))
        lift = 0;
    t->linear_velocity_y = lift;

    t->vehicle_mode = 1;     /* route to scripted-recovery dispatch next tick */
    t->frame_counter = 0;    /* start the ~0x3B-frame recovery animation */

    TD5_LOG_I(LOG_TAG, "traffic_crash_spin: slot=%d mag=%d lift=%d ang=[%d,%d,%d]",
              t->slot_index, impact_mag, lift, ang[0], ang[1], ang[2]);
}

/* True when `slot` is a HUMAN-driven racer (race_slot_state == 1). AI racers,
 * traffic, and out-of-range slots return 0. Used by the S08 rear-impact
 * softening so only the player's response is tuned, never AI/traffic.
 * g_race_slot_state is sized [TD5_MAX_RACER_SLOTS]; a human is always a racer
 * slot (< g_traffic_slot_base <= TD5_MAX_RACER_SLOTS), so the bound is safe. */
static inline int v2v_slot_is_human(int slot)
{
    return slot >= 0 && slot < g_traffic_slot_base &&
           slot < TD5_MAX_RACER_SLOTS && g_race_slot_state[slot] == 1;
}

/* Scale x to `pct`% (round-to-zero), used to soften a rear-ended player's
 * angular response. pct==100 is identity (byte-faithful pass-through). */
static inline int32_t v2v_scale_pct(int32_t x, int32_t pct)
{
    if (pct >= 100) return x;
    if (pct <= 0)   return 0;
    return (int32_t)(((int64_t)x * pct) / 100);
}

/* [MP GAME MODES: TIME TRIAL 2026-06-22] In time trial every player races the
 * clock and PASSES THROUGH the others (ghosts). True only for human-vs-human
 * pairs, so car-vs-traffic and car-vs-wall still resolve normally and each
 * viewport interacts with traffic on its own. Applied to BOTH V2V paths (the
 * impulse resolver and the anti-tunnel depenetration). Knob
 * TD5RE_TT_NO_COLLISION=0 restores solid player-vs-player contact. */
static int tt_pair_passthrough(int slot_a, int slot_b)
{
    static int knob = -1;
    if (knob < 0) {
        knob = td5_env_flag_on("TD5RE_TT_NO_COLLISION");   /* default ON */
    }
    if (!knob) return 0;
    if (g_td5.mp_mode_config.mode != TD5_MP_MODE_TIME_TRIAL) return 0;
    /* Both are HUMAN players (slots 0..num_human_players-1) — the same definition
     * the ghost RENDER uses, so what you see (a ghost) and what you collide with
     * agree. Don't key off g_race_slot_state here (not reliably set for local
     * split-screen humans). AI opponents are NOT humans -> they still collide. */
    return slot_a >= 0 && slot_a < g_td5.num_human_players &&
           slot_b >= 0 && slot_b < g_td5.num_human_players;
}

/* [TRAFFIC BATTLE 2026-06-28] In battle mode a WRECKED traffic car is intangible
 * — both the V2V impulse and the depenetration push skip any pair that includes
 * a broken-down traffic actor, so the player drives clean through wrecks (which
 * also render translucent, like ghost mode). No-op outside battle. */
static int battle_wreck_intangible(const TD5_Actor *a, const TD5_Actor *b)
{
    if (!td5_game_battle_mode_active()) return 0;
    int sa = a ? (int)a->slot_index : -1;
    int sb = b ? (int)b->slot_index : -1;
    if (sa >= g_traffic_slot_base && td5_ai_actor_is_broken_down(sa)) return 1;
    if (sb >= g_traffic_slot_base && td5_ai_actor_is_broken_down(sb)) return 1;
    return 0;
}

/* [TRAFFIC BATTLE 2026-06-28] Capped CLOSING-speed impact for a battle
 * racer-vs-traffic pair — the speed-based wreck magnitude that replaces the
 * normal-projected impulse so a fast crash totals the car at ANY angle. Returns
 * 0 when it is not a battle racer-vs-traffic pair. avx..bvz are the
 * PRE-collision velocities; mass_sum = mass_A + mass_B. */
static int32_t battle_speed_impact(const TD5_Actor *A, const TD5_Actor *B,
                                   int32_t avx, int32_t avz,
                                   int32_t bvx, int32_t bvz, int64_t mass_sum)
{
    if (!A || !B || !td5_game_battle_mode_active()) return 0;
    int sa = (int)A->slot_index, sb = (int)B->slot_index;
    int rvt = (sa >= 0 && sa < g_traffic_slot_base && sb >= g_traffic_slot_base) ||
              (sb >= 0 && sb < g_traffic_slot_base && sa >= g_traffic_slot_base);
    if (!rvt) return 0;
    int64_t rvx = (int64_t)avx - bvx;
    int64_t rvz = (int64_t)avz - bvz;
    uint64_t s2 = (uint64_t)(rvx * rvx + rvz * rvz);
    if (s2 > 0xFFFFFFFFull) s2 = 0xFFFFFFFFull;       /* isqrt takes u32 */
    int32_t closing = (int32_t)td5_isqrt((uint32_t)s2);
    int64_t si64 = mass_sum * closing * battle_ram_pct() / 100;
    int32_t cap = battle_ram_cap();
    if (si64 > cap) si64 = cap;
    return (int32_t)si64;
}

static void apply_collision_response(TD5_Actor *penetrator, TD5_Actor *target,
                                     int corner_idx, OBB_CornerData *corner,
                                     int32_t heading_target, int32_t impactForce)
{
    if (!penetrator || !target) return;
    if (!penetrator->car_definition_ptr || !target->car_definition_ptr) return;
    (void)corner_idx;

    /* [ARCADE] A GHOSTing racer is intangible — skip the whole V2V interaction so
     * either party passes cleanly through the other. No-op outside arcade mode. */
    if (td5_arcade_slot_is_ghost((int)penetrator->slot_index) ||
        td5_arcade_slot_is_ghost((int)target->slot_index)) {
        return;
    }

    /* [TRAFFIC BATTLE] Drive straight through wrecked traffic. */
    if (battle_wreck_intangible(penetrator, target)) return;

    /* [#10 telemetry] A dispatched OBB corner contact = a real car-to-car
     * collision; flag both actors for this tick's edge-detector (idempotent —
     * multiple corners in one tick still count as a single collision). */
    td5_physics_mark_collision((int)penetrator->slot_index);
    td5_physics_mark_collision((int)target->slot_index);

    TD5_Actor *A = target;      /* frame owner (yaw drives `angle`) */
    TD5_Actor *B = penetrator;  /* other actor */
    int32_t    angle       = heading_target;

    /* impactForce is now passed from the caller's position-based binary search
     * (matching ResolveVehicleCollisionPair @ 0x408A60). Range [0x10, 0xF0].
     * Higher = contact near end of tick (less rollback).
     * Lower = contact early in tick (more rollback). */

    /* pool14_v2v pilot trace: capture pre-state. The Frida probe captures
     * args[0]=actorA (=slot_a=frame owner) and args[1]=actorB (=slot_b).
     * Port's A=target=caller's `a`=slot_a → maps to Frida's actorA. */

    /* --- 1. Prologue: save angular velocities for delta application --- */
    int32_t saved_omega_A = A->angular_velocity_yaw;
    int32_t saved_omega_B = B->angular_velocity_yaw;

    /* [ARCADE wrecking ball] If a party is a WRECKING BALL it is immune: it plows
     * through unaffected while the OTHER car takes the (3x, low-gate) launch. We
     * snapshot its full pre-collision motion+pose here and restore it at function
     * exit, so the symmetric solver below stays byte-faithful for the victim. */
    const int A_wreck = td5_arcade_slot_is_wrecking((int)A->slot_index);
    const int B_wreck = td5_arcade_slot_is_wrecking((int)B->slot_index);
    const int32_t Awx = A->world_pos.x, Awz = A->world_pos.z;
    const int32_t Avx = A->linear_velocity_x, Avy = A->linear_velocity_y, Avz = A->linear_velocity_z;
    const int32_t Aroll = A->angular_velocity_roll, Apitch = A->angular_velocity_pitch;
    const int32_t Aeyaw = A->euler_accum.yaw;
    const int32_t Bwx = B->world_pos.x, Bwz = B->world_pos.z;
    const int32_t Bvx = B->linear_velocity_x, Bvy = B->linear_velocity_y, Bvz = B->linear_velocity_z;
    const int32_t Broll = B->angular_velocity_roll, Bpitch = B->angular_velocity_pitch;
    const int32_t Beyaw = B->euler_accum.yaw;

    /* Arcade collision multiplier in 24.8 (0x100 == 1.0 outside arcade, so the
     * impulse math below is byte-identical to the faithful path when off). */
    const int arc_coll_q8 = td5_arcade_collision_mult_q8();

    /* Mass from cardef+0x88 (int16). [CONFIRMED @ 0x00407BE7, 0x00407BFE,
     * 0x00407DA0, 0x00407DB4]: original ApplyVehicleCollisionImpulse loads
     * mass via MOVSX with NO clamp. Upstream writers guarantee mass > 0
     * before V2V fires: (a) racing slots 0..5 load carparam.dat into
     * car_definition (positive int16); (b) traffic slots 6+ get an explicit
     * mass=0x20 write at InitializeRaceVehicleRuntime+0xF5 (`MOV word ptr
     * [EAX + 0x88], 0x20` @ 0x0042F235), mirrored in the port's
     * traffic-init path (td5_physics.c:8116-8118). The earlier port-only
     * `if (mass <= 0) mass = 0x20;` defensive clamp was never reachable
     * and diverged from the byte-faithful listing. Removed per
     * audit-v2v-mass-clamp 2026-05-14. */
    int32_t mass_A = (int32_t)CDEF_S(A, 0x88);
    int32_t mass_B = (int32_t)CDEF_S(B, 0x88);

    /* [#2 LIGHT WRECK 2026-06-21] Boost a broken-down car's inverse-mass term so
     * it shoves aside easily and barely slows the car that hits it. See
     * wreck_mass_pct(). slot_index>=0 guards the (rare) sentinel; the broken-down
     * flag is replicated, so this stays netplay-deterministic. */
    {
        int wp = wreck_mass_pct();
        int tp = traffic_mass_pct();
        /* Wreck boost takes precedence over the ordinary-traffic boost so a
         * broken-down traffic car keeps its (heavier) 4x shove and the two
         * multipliers never compound. A live traffic slot (>= traffic base, not
         * broken) gets the lighter-traffic boost instead. Racers (slot <
         * g_traffic_slot_base) are never scaled — byte-faithful. */
        if (A->slot_index >= 0) {
            if (wp != 100 && td5_ai_actor_is_broken_down((int)A->slot_index))
                mass_A = (mass_A * wp) / 100;
            else if (tp != 100 && (int)A->slot_index >= g_traffic_slot_base)
                mass_A = (mass_A * tp) / 100;
        }
        if (B->slot_index >= 0) {
            if (wp != 100 && td5_ai_actor_is_broken_down((int)B->slot_index))
                mass_B = (mass_B * wp) / 100;
            else if (tp != 100 && (int)B->slot_index >= g_traffic_slot_base)
                mass_B = (mass_B * tp) / 100;
        }
    }

    /* --- 2. Rotate both velocities into A's local (contact) frame --- */
    int32_t cos_a = cos_fixed12(angle);
    int32_t sin_a = sin_fixed12(angle);

    int32_t vxA = A->linear_velocity_x;
    int32_t vzA = A->linear_velocity_z;
    int32_t vxB = B->linear_velocity_x;
    int32_t vzB = B->linear_velocity_z;

    /* [CONFIRMED @ 0x00407A19-29, 0x00407A3E-4D, 0x00407A69-72, 0x00407A95-AA]:
     * each rotation step uses CDQ; AND EDX,0xfff; ADD EAX,EDX; SAR EAX,0xc —
     * signed round-to-zero divide by 0x1000. Plain `>>12` on a negative product
     * diverges by 1 LSB. */
    int32_t local_54 = v2v_sar12_rz(vxA * cos_a - vzA * sin_a);  /* A tangent */
    int32_t local_50 = v2v_sar12_rz(vxA * sin_a + vzA * cos_a);  /* A normal  */
    int32_t local_4c = v2v_sar12_rz(vxB * cos_a - vzB * sin_a);  /* B tangent */
    int32_t local_44 = v2v_sar12_rz(vxB * sin_a + vzB * cos_a);  /* B normal  */

    /* --- Unpack contactData [CONFIRMED @ 0x00408570 stores] --- */
    /* cx_A, cz_A = corner position in TARGET's (A's) frame (WITH A→B translation).
     * cx_B, cz_B = rotated corner OFFSET from penetrator's (B's) center, in A's frame.
     *              i.e. the penetrator's static carbox corner rotated by
     *              (heading_b - heading_a), no translation.
     * Original stores 'contactData[2] = contactData[0] - B_center_in_A.x' at
     * 0x00408763 (SUB), i.e. the pre-translation rotated corner. Using the
     * unrotated static corner here gave the mass polynomial and angular
     * contribution the wrong lever-arm orientation whenever cars impacted at
     * a heading delta, leaving relative velocity and impulse sign noisy. */
    int32_t cx_A = (int32_t)corner->proj_x;
    int32_t cz_A = (int32_t)corner->proj_z;
    int32_t cx_B = (int32_t)corner->own_x;
    int32_t cz_B = (int32_t)corner->own_z;

    /* --- 3. Case split: side vs front/rear --- */
    /* [CONFIRMED @ 0x407ADB]: cardef+0x04 = front-Z extent (positive),
     *                         cardef+0x08 = half-width,
     *                         cardef+0x14 = rear-Z extent (stored negative). */
    /* [MESH HITBOX 2026-06-25] Use the SAME model-derived box obb_corner_test
     * used to generate this contact (actor_collision_box). The side-vs-front/rear
     * split below compares |side_extent| = half_w_A - |cx_A| against the
     * front/rear depth; cx_A/cz_A were computed from these extents, so the box
     * here must match the overlap that produced the contact or the branch
     * decision (and its push direction) would reference a different box. */
    int32_t half_w_A, front_z_A, rear_z_A;
    actor_collision_box(A, &half_w_A, &front_z_A, &rear_z_A);

    int32_t abs_cx_A   = cx_A < 0 ? -cx_A : cx_A;
    int32_t side_extent = half_w_A - abs_cx_A;
    /* [CONFIRMED @ 0x00407B0D-10 (front) + 0x00407B65-6E (rear)]: the listing
     * applies a CDQ; XOR; SUB abs() idiom to side_extent before comparing it
     * against |rear_diff| or |front_diff|. Without this abs, a corner outside
     * the box laterally (abs(cx_A) > half_w_A → side_extent < 0) would
     * unconditionally take the SIDE branch in the port while the original
     * would still pick FRONT/REAR based on |negative| vs depth magnitudes. */
    int32_t abs_side_extent = side_extent < 0 ? -side_extent : side_extent;

    int is_side_branch;
    if (cz_A < 1) {
        /* Rear half (cz_A <= 0). [CONFIRMED @ 0x00407B7B-7D]:
         *   CMP ECX, EAX ; ECX=|side_extent|, EAX=|cz_A - rear_z_raw|
         *   JGE LAB_00407B2D (FRONT/REAR) — SIDE if |side_extent| < rear_depth. */
        int32_t rear_depth = cz_A - rear_z_A;
        if (rear_depth < 0) rear_depth = -rear_depth;
        is_side_branch = (rear_depth > abs_side_extent);
    } else {
        /* Front half (cz_A > 0). [CONFIRMED @ 0x00407B29-2B]:
         *   CMP ECX, EAX ; ECX=|side_extent|, EAX=|front_z_A - cz_A|
         *   JL  LAB_00407B7F (SIDE) — SIDE if |side_extent| < front_depth. */
        int32_t front_depth = front_z_A - cz_A;
        if (front_depth < 0) front_depth = -front_depth;
        is_side_branch = (abs_side_extent < front_depth);
    }

    /* [S08 rear-impact softening 2026-06-04] When a HUMAN player (always the
     * target A inside this function) is struck on its REAR face, the angular
     * response below — and the heavy-impact scatter/lift in step 10 — is scaled
     * down so getting rear-ended destabilises but stays recoverable instead of
     * spinning the car out. `cz_A < 1` is A's rear half (same predicate as the
     * side-vs-front/rear split above); a rear face contact is never the side
     * branch, so require the front/rear branch too. Only the victim's spin is
     * softened: the attacker B, front/side contacts on A, and every AI/traffic
     * response stay byte-faithful. rear_retain==100 disables the softening. */
    int rear_hit_on_player = (!is_side_branch) && (cz_A < 1) &&
                             v2v_slot_is_human(A->slot_index);
    int32_t rear_retain = rear_hit_on_player ? g_td5.ini.rear_impact_response : 100;
    if (rear_retain < 0)   rear_retain = 0;
    if (rear_retain > 100) rear_retain = 100;

    /* --- 4. Branch-specific impulse math --- */
    int32_t impulse      = 0;
    int32_t omega_A_delta = 0;
    int32_t omega_B_delta = 0;
    int     rejected     = 0;
    int32_t push_x = 0, push_z = 0;

    const int64_t INERTIA_K_64 = (int64_t)V2V_INERTIA_K;
    /* NUM_CONST = (500000 >> 8) * 0x1100 = 8,499,456 [CONFIRMED @ 0x4079C0].
     * The `>> 8` on INERTIA_K is NOT a compiler overflow workaround — it is
     * intentional scaling paired with the denom `>> 8` below. Together they
     * produce small integer impulses (typically 1-200) such that
     * `impulse * mass` — the velocity channel update — lands in the right
     * 24.8-FP range.
     *
     * An earlier commit tried full-width NUM_CONST = 500000 * 0x1100 to
     * "recover precision"; with the OBB corner fix making rel_vel realistic,
     * it produced impulse magnitudes in the 10^8..10^9 range. That's a
     * clear signal the shift is load-bearing, not cosmetic. The earlier
     * symptom of impulse∈[-4..1] was unrelated — it came from obb_corner_test
     * having swapped cardef offsets (fixed in 4e8860e), which made rel_vel
     * collapse to near-zero. */
    const int64_t NUM_CONST    = (INERTIA_K_64 >> 8) * V2V_NUM_SCALE;

    if (is_side_branch) {
        /* --- SIDE BRANCH (LAB_00407B7F) --- */
        /* [CONFIRMED @ 0x00407B7F-0x00407BB5]: predicate is
         *     cx_A == cx_B || cx_A - cx_B < 0   (i.e. cx_A <= cx_B)
         * iVar6 holds the cos channel, iVar14 holds the sin channel.
         * Push writes at 0x00407BB7-0x00407BDB are A.x -= iVar6, A.z -= iVar14,
         * B.x += iVar6, B.z += iVar14 (see apply block below).
         * push_x mirrors iVar6, push_z mirrors iVar14. */
        if (cx_A <= cx_B) { push_x = -cos_a / 2; push_z =  sin_a / 2; }
        else              { push_x =  cos_a / 2; push_z = -sin_a / 2; }

        int64_t denom = ((int64_t)cz_B * cz_B + INERTIA_K_64) * mass_A
                      + ((int64_t)cz_A * cz_A + INERTIA_K_64) * mass_B;
        denom >>= 8;
        if (denom == 0) denom = 1;

        int32_t ang_contrib =
            (int32_t)(((int64_t)cz_B * saved_omega_B) / V2V_ANG_DIVISOR) -
            (int32_t)(((int64_t)cz_A * saved_omega_A) / V2V_ANG_DIVISOR);
        int32_t rel_vel = ang_contrib - local_54 + local_4c;

        /* [CONFIRMED @ 0x00407CB2-C1]: CDQ; AND EDX,0xfff; ADD; SAR 0xc —
         * signed round-to-zero divide by 0x1000. */
        int64_t impulse_raw = (NUM_CONST / denom) * rel_vel;
        impulse = v2v_sar12_rz_64(impulse_raw);
        /* [ARCADE] Boosted horizontal knockback (default 1.4x, was 3x). Positive
         * multiply preserves sign, so the XOR rejection below is unchanged.
         * NOTE: impact_mag is derived from this impulse, so the multiplier also
         * scales the heavy-crash scatter/lift and how readily the heavy gate
         * trips — kept modest now so routine rams don't go airborne. */
        impulse = (int32_t)(((int64_t)impulse * arc_coll_q8) >> 8);

        /* [CONFIRMED @ 0x4079C0 side branch]: XOR sign rejection. */
        if (((cx_B - cx_A) ^ impulse) < 0) {
            rejected = 1;
        } else {
            local_54 += impulse * mass_A;
            local_4c -= impulse * mass_B;
            omega_A_delta =  (int32_t)(((int64_t)impulse * mass_A * cz_A) / V2V_INERTIA_PER_ANG);
            omega_B_delta = -(int32_t)(((int64_t)impulse * mass_B * cz_B) / V2V_INERTIA_PER_ANG);
        }
    } else {
        /* --- FRONT/REAR BRANCH (LAB_00407B2D) --- */
        /* [CONFIRMED @ 0x00407B41 (predicate JLE), 0x00407B47-0x00407B54
         *  (else: cz_A > cz_B → +sin/2, +cos/2),
         *  0x00407D5F-0x00407D6E (taken: cz_A <= cz_B → NEG; NEG → -sin/2, -cos/2),
         *  0x00407D70-0x00407D94 (apply A -= ECX/EAX, B += ECX/EAX)]:
         *
         * Predicate is `cz_A == cz_B || cz_A - cz_B < 0` (i.e. cz_A <= cz_B).
         * In ASM: ECX holds the iVar14 sin channel (loaded from [ESP+0x7c]=sin_a),
         *         EAX holds the iVar6 cos channel (loaded from EDI=cos_a, where
         *         EDI was set by CosFixed12bit @ 0x004079EB and never reloaded
         *         through the FRONT path).
         * Push writes at 0x00407D70-0x00407D94 are A.x -= iVar14 (sin channel),
         * A.z -= iVar6 (cos channel), B.x += iVar14, B.z += iVar6.
         * The /2 idiom (SUB EAX,EDX after CDQ; SAR 1) is signed
         * round-toward-zero division by 2, which equals plain C `/2` for int32
         * (C99/C11 truncation toward zero).
         * Precise-port audit 2026-05-14 re-verified against decomp
         * ApplyVehicleCollisionImpulse and listing — already byte-faithful. */
        if (cz_A <= cz_B) { push_x = -sin_a / 2; push_z = -cos_a / 2; }
        else              { push_x =  sin_a / 2; push_z =  cos_a / 2; }

        /* [CONFIRMED FRONT impulse/omega vs decomp ApplyVehicleCollisionImpulse]:
         *   denom = (cx_B^2 + K) * mass_A + (cx_A^2 + K) * mass_B
         *   NUM_CONST / (denom >> 8) * rel_vel  →  sar12_rz → impulse
         *   reject if  (cz_B - cz_A) ^ impulse < 0   (XOR sign mismatch)
         *   local_50 += impulse * mass_A
         *   local_44 -= impulse * mass_B
         *   omega_A_delta = -(imp * mass_A * cx_A) / (K / 0x28C)   [iVar6 in decomp]
         *   omega_B_delta =  (imp * mass_B * cx_B) / (K / 0x28C)   [iVar8 in decomp]
         * Note the omega signs are FLIPPED vs SIDE: SIDE has +A/-B, FRONT has -A/+B.
         * V2V_INERTIA_PER_ANG = 500000/0x28C = 766 (compile-time fold of the
         * runtime magic-divide DAT_00463204 / 0x28C at 0x00407EC4-D7). */
        int64_t denom = ((int64_t)cx_B * cx_B + INERTIA_K_64) * mass_A
                      + ((int64_t)cx_A * cx_A + INERTIA_K_64) * mass_B;
        denom >>= 8;
        if (denom == 0) denom = 1;

        int32_t ang_contrib =
            (int32_t)(((int64_t)cx_B * saved_omega_B) / V2V_ANG_DIVISOR) -
            (int32_t)(((int64_t)cx_A * saved_omega_A) / V2V_ANG_DIVISOR);
        int32_t rel_vel = ang_contrib - local_50 + local_44;

        /* [CONFIRMED @ 0x00407E68-7A]: same round-to-zero idiom as SIDE branch. */
        int64_t impulse_raw = (NUM_CONST / denom) * rel_vel;
        impulse = v2v_sar12_rz_64(impulse_raw);
        /* [ARCADE] Boosted horizontal knockback (default 1.4x; see SIDE note). */
        impulse = (int32_t)(((int64_t)impulse * arc_coll_q8) >> 8);

        if (((cz_B - cz_A) ^ impulse) < 0) {
            rejected = 1;
        } else {
            local_50 += impulse * mass_A;
            local_44 -= impulse * mass_B;
            omega_A_delta = -(int32_t)(((int64_t)impulse * mass_A * cx_A) / V2V_INERTIA_PER_ANG);
            omega_B_delta =  (int32_t)(((int64_t)impulse * mass_B * cx_B) / V2V_INERTIA_PER_ANG);
            /* [S08] Tame the yaw spin imparted to a rear-ended player; the
             * linear impulse (local_50/local_44) is left faithful so the hit
             * still shoves the car forward like a real bump. */
            omega_A_delta = v2v_scale_pct(omega_A_delta, rear_retain);
        }
    }

    /* Apply positional push BEFORE rejection check.
     * [CONFIRMED @ 0x00407BB7-0x00407BDB (SIDE) + 0x00407D70-0x00407D94 (FRONT)]:
     *     A.x -= iVar6/iVar14;  A.z -= iVar14/iVar6;
     *     B.x += iVar6/iVar14;  B.z += iVar14/iVar6;
     * push_x/push_z carry the iVar sign per the per-branch assignments above
     * (cosmetic line-for-line match to the decomp; supersedes the earlier
     * algebraically-equivalent A+=, B-= encoding). The XOR rejection below
     * only gates the velocity impulse, not the push. */
    A->world_pos.x -= push_x;
    A->world_pos.z -= push_z;
    B->world_pos.x += push_x;
    B->world_pos.z += push_z;

    /* [AI UNSTICK] Tell the AI both cars are touching this tick (grind detect). */
    td5_ai_note_v2v_contact((int)A->slot_index, (int)B->slot_index);
    td5_ai_note_v2v_contact((int)B->slot_index, (int)A->slot_index);

    if (rejected) {
        /* [TRAFFIC BATTLE 2026-06-28] A fast ANGLED / glancing crash into traffic
         * registers as "separating" by the normal-impulse sign test and would
         * bail here with no wreck — this is the user's "I hit cars at speed but
         * it doesn't wreck because I'm not hitting them the right way". If a racer
         * is closing on the traffic car FAST, total it anyway (score + break it
         * down + a small pop so the wreck reads), then fall through to the normal
         * separating return. The broken-down car then goes translucent +
         * intangible (pass-through) like every other battle wreck. Deduped: once
         * the victim is broken-down, td5_arcade_note_ram skips it and the pair is
         * intangible next tick. */
        int32_t si = battle_speed_impact(A, B, Avx, Avz, Bvx, Bvz,
                                         (int64_t)mass_A + mass_B);
        if (si >= td5_physics_npc_fatal_mag()) {
            int sa = (int)A->slot_index, sb = (int)B->slot_index;
            int a_is_racer = (sa >= 0 && sa < g_traffic_slot_base);
            int aggressor  = a_is_racer ? sa : sb;
            int victim     = a_is_racer ? sb : sa;
            TD5_Actor *vic = a_is_racer ? B : A;
            if (!td5_ai_actor_is_broken_down(victim)) {
                td5_arcade_note_ram(aggressor, victim, si);   /* score (before break) */
                td5_ai_mark_actor_broken_down(victim);        /* total it */
                int32_t lift = si / 20;                       /* modest pop */
                if (lift > 120000) lift = 120000;
                if (vic->linear_velocity_y < lift) vic->linear_velocity_y = lift;
                TD5_LOG_I(LOG_TAG, "battle_ram(reject): aggr=%d victim=%d impact=%d -> WRECK",
                          aggressor, victim, si);
            }
        }
        /* Separating contact — push applied above, but no velocity impulse.
         * Original returns 0 (XOR EAX,EAX at 0x00407CCD / 0x00407E86). */
        TD5_LOG_I(LOG_TAG, "v2v_reject: slot_A=%d slot_B=%d side=%d cxA=%d czA=%d cxB=%d czB=%d imp=%d push=(%d,%d)",
                  A->slot_index, B->slot_index, is_side_branch, cx_A, cz_A, cx_B, cz_B, impulse, push_x, push_z);
        return;
    }

    /* --- 5. TOI rollback (before committing new velocities) ---
     * [CONFIRMED @ 0x00407F31-F4C (A.x), F50-6E (A.z), F6F-8E (B.x),
     *  F8F-AE (B.z), FAF-D6 (A.yaw_eul), FDE-FFD (B.yaw_eul)]:
     * each rollback uses IMUL; CDQ; AND EDX,0xff; ADD EAX,EDX; SAR EAX,8; NEG;
     * ADD [field], EAX — i.e. `field += -sar8_rz(toi_frac * vel)` which equals
     * `field -= sar8_rz(toi_frac * vel)`. Plain `>>8` on a negative product
     * diverges by 1 LSB. */
    int32_t toi_frac = 0x100 - impactForce;

    A->world_pos.x -= v2v_sar8_rz(toi_frac * A->linear_velocity_x);
    A->world_pos.z -= v2v_sar8_rz(toi_frac * A->linear_velocity_z);
    A->euler_accum.yaw -= v2v_sar8_rz(toi_frac * A->angular_velocity_yaw);
    B->world_pos.x -= v2v_sar8_rz(toi_frac * B->linear_velocity_x);
    B->world_pos.z -= v2v_sar8_rz(toi_frac * B->linear_velocity_z);
    B->euler_accum.yaw -= v2v_sar8_rz(toi_frac * B->angular_velocity_yaw);

    /* --- 6. Commit new angular velocities --- */
    A->angular_velocity_yaw = saved_omega_A + omega_A_delta;
    B->angular_velocity_yaw = saved_omega_B + omega_B_delta;

    /* --- 7. Rotate tangent/normal channels back to world frame ---
     * [CONFIRMED @ 0x00408027-37, 0x00408048-58, 0x00408068-78, 0x00408088-98]:
     * each writeback uses the same CDQ; AND EDX,0xfff; SAR 0xc idiom — signed
     * round-to-zero divide by 0x1000. */
    A->linear_velocity_x = v2v_sar12_rz(local_50 * sin_a + local_54 * cos_a);
    A->linear_velocity_z = v2v_sar12_rz(local_50 * cos_a - local_54 * sin_a);
    B->linear_velocity_x = v2v_sar12_rz(local_44 * sin_a + local_4c * cos_a);
    B->linear_velocity_z = v2v_sar12_rz(local_44 * cos_a - local_4c * sin_a);

    /* --- 8. TOI re-advance (with the new post-impulse velocities) ---
     * [CONFIRMED @ 0x004080C8 onwards]: same round-to-zero idiom but the NEG
     * disappears so `field += sar8_rz(toi_frac * vel)`. */
    A->world_pos.x += v2v_sar8_rz(toi_frac * A->linear_velocity_x);
    A->world_pos.z += v2v_sar8_rz(toi_frac * A->linear_velocity_z);
    A->euler_accum.yaw += v2v_sar8_rz(toi_frac * A->angular_velocity_yaw);
    B->world_pos.x += v2v_sar8_rz(toi_frac * B->linear_velocity_x);
    B->world_pos.z += v2v_sar8_rz(toi_frac * B->linear_velocity_z);
    B->euler_accum.yaw += v2v_sar8_rz(toi_frac * B->angular_velocity_yaw);

    /* --- 9. Post-impulse pose update on both --- */
    update_vehicle_pose_from_physics(A);
    update_vehicle_pose_from_physics(B);

    /* --- 10. Impact magnitude and damage effects --- */
    int64_t impact_signed = (int64_t)(mass_A + mass_B) * impulse;
    int32_t impact_mag = (int32_t)(impact_signed < 0 ? -impact_signed : impact_signed);

    /* [CAR DAMAGE 2026-06-28] Drive each car's health down + deform its body at
     * the struck region. No-op when [Game] CarDamage=0 (faithful sim unchanged).
     * Hit region is taken from the contact corner the solver already computed:
     * A's corner is (cx_A,cz_A) in A's frame; B's is (cx_B,cz_B) in B's frame
     * (model axes: x=lateral +right, z=forward +front). Uses the REAL impact (the
     * actual contact force), BEFORE the battle boost below. */
    if (td5_damage_enabled()) {
        TD5_DamageHit hitA, hitB;
        hitA.lat = (cx_A > 0) - (cx_A < 0);
        hitA.fwd = (cz_A > 0) - (cz_A < 0);
        hitA.is_side = is_side_branch;
        hitB.lat = (cx_B > 0) - (cx_B < 0);
        hitB.fwd = (cz_B > 0) - (cz_B < 0);
        hitB.is_side = (cx_B < 0 ? -cx_B : cx_B) > (cz_B < 0 ? -cz_B : cz_B);
        td5_damage_on_impact(A, impact_mag, &hitA);
        td5_damage_on_impact(B, impact_mag, &hitB);
    }

    /* [TRAFFIC BATTLE 2026-06-28] SPEED-based wreck trigger. impact_mag above is
     * the NORMAL-projected impulse, so a fast GLANCING / corner crash into traffic
     * barely registers and never crosses the wreck gate — the user's "I hit cars
     * at speed but it won't wreck unless I hit them just right". For a
     * racer-vs-traffic pair in battle, recompute the impact from the full
     * pre-collision CLOSING speed (relative-velocity magnitude). Same
     * (mass_A+mass_B)*velocity units as impact_mag, so it drops straight into the
     * existing npc_fatal_mag gate + the scatter/lift below; take the larger so a
     * hard crash at ANY angle totals the car. (Applied AFTER car-damage so the
     * visual dents stay tied to the real contact force.) */
    {
        int32_t si = battle_speed_impact(A, B, Avx, Avz, Bvx, Bvz,
                                         (int64_t)mass_A + mass_B);
        if (si > impact_mag) {
            TD5_LOG_I(LOG_TAG, "battle_ram: A=%d B=%d impact %d -> %d (speed wreck)",
                      A->slot_index, B->slot_index, impact_mag, si);
            impact_mag = si;
        }
    }

    /* [#1 WRECK PUSH WINDOW 2026-06-21] A real ram on a broken-down (anchored)
     * wreck opens its free-slide window so it can be shoved aside — otherwise the
     * stand-still anchor in td5_physics_update_traffic re-freezes it next tick.
     * Gated on a meaningful impact so merely resting against a wreck does not keep
     * nudging it. */
    if (impact_mag > WRECK_PUSH_MIN_IMPACT) {
        if (A->slot_index >= 0 && A->slot_index < TD5_MAX_TOTAL_ACTORS &&
            td5_ai_actor_is_broken_down((int)A->slot_index))
            g_wreck_push_ticks[A->slot_index] = WRECK_PUSH_TICKS;
        if (B->slot_index >= 0 && B->slot_index < TD5_MAX_TOTAL_ACTORS &&
            td5_ai_actor_is_broken_down((int)B->slot_index))
            g_wreck_push_ticks[B->slot_index] = WRECK_PUSH_TICKS;
    }

    TD5_LOG_I(LOG_TAG, "v2v_impulse: side=%d slot_A=%d slot_B=%d mA=%d mB=%d "
              "cxA=%d czA=%d cxB=%d czB=%d imp=%d mag=%d toi=%d",
              is_side_branch, A->slot_index, B->slot_index, mass_A, mass_B,
              cx_A, cz_A, cx_B, cz_B, impulse, impact_mag, impactForce);

    /* [orig ApplyVehicleCollisionImpulse 0x004079c0, pre-LAB_00408289]: car/
     * traffic collision SFX. A racer-involved impact (at least one racer slot)
     * above 0x3201 plays a positional one-shot: LHit1-5 (light) below 0xc801,
     * HHit1-4 (hard) at/above it. Volume 0x1000; pitch per tier (0xd02 / 0x2198).
     * Reached only on an APPROACHING impact (the separating early-out above
     * returns before this), so it fires once per real hit, not while resting.
     * [N-way 2026-06-04] racer boundary g_traffic_slot_base, not hardcoded 6. */
    if ((A->slot_index < g_traffic_slot_base || B->slot_index < g_traffic_slot_base) &&
        impact_mag >= 0x3201) {
        int hit_slot, hit_pitch, hit_variants;
        if (impact_mag < 0xc801) {
            hit_slot = 0x1F; hit_pitch = 0xd02;  hit_variants = 5; /* LHit1-5 */
        } else {
            hit_slot = 0x1B; hit_pitch = 0x2198; hit_variants = 4; /* HHit1-4 */
        }
        int32_t hit_pos[3] = { A->world_pos.x, A->world_pos.y, A->world_pos.z };
        td5_sound_play_at_position(hit_slot, 0x1000, hit_pitch, hit_pos, hit_variants);
    }

    /* Wanted mode (cop chase): player<->cop collision awards damage score.
     * Mirrors ApplyVehicleCollisionImpulse @ 0x40817A/0x4081BE → AwardWantedDamageScore.
     * [CONFIRMED]: both A-is-player and B-is-player paths call AwardWantedDamageScore
     * on the cop slot with impact magnitude. */
    if (td5_game_is_wanted_mode()) {
        /* [MP COP CHASE] Generalized from the SP "slot 0 is the cop" gate: a ram
         * between ANY cop and any SUSPECT (non-cop racer) decrements that suspect's
         * damage bar. SP (cop=0) reproduces the old slot-0-vs-1..5 behaviour exactly.
         * [multi-cop 2026-06-24] Uses is_cop (mask-aware) so a SECOND human cop's rams
         * also count — previously only the single primary cop_slot dealt damage. */
        int sa = (int)A->slot_index, sb = (int)B->slot_index;
        /* Pass BOTH the ramming cop and the rammed suspect so the bust is
         * credited to the actual cop (per-cop arrest tally in multi-cop), and
         * capture whether THIS ram completed the arrest. */
        int arrested = 0, cop_s = -1, susp_s = -1;
        if (td5_game_cop_chase_is_cop(sa) && td5_game_cop_chase_is_suspect(sb)) {
            arrested = td5_ai_wanted_cop_hit(sa, sb, impact_mag); cop_s = sa; susp_s = sb;
        } else if (td5_game_cop_chase_is_cop(sb) && td5_game_cop_chase_is_suspect(sa)) {
            arrested = td5_ai_wanted_cop_hit(sb, sa, impact_mag); cop_s = sb; susp_s = sa;
        }
        /* [COP CHASE ARREST FF 2026-06-25] When this ram completed the arrest, fire a
         * strong short jolt on BOTH the arresting cop and the busted suspect so each
         * player feels the bust (slots out of human/device range are no-ops). */
        if (arrested) {
            td5_input_ff_jolt(cop_s,  TD5_FF_ARREST_JOLT_MAG);
            td5_input_ff_jolt(susp_s, TD5_FF_ARREST_JOLT_MAG);
        }
    }

    /* Traffic recovery escalation (> 50000 and traffic slot). [N-way 2026-06-04]
     * traffic boundary g_traffic_slot_base, not hardcoded 6 — a human racer in
     * an expanded slot must not pick up the traffic-only lockout escalation. */
    if (A->slot_index >= g_traffic_slot_base && impact_mag > 50000 &&
        A->damage_lockout > 0 && A->damage_lockout < 7) {
        A->damage_lockout++;
    }
    if (B->slot_index >= g_traffic_slot_base && impact_mag > 50000 &&
        B->damage_lockout > 0 && B->damage_lockout < 7) {
        B->damage_lockout++;
    }

    /* Heavy impact (> 90000) with collisions enabled: per-actor angular scatter
     * + vertical lift, byte-faithful to the original high-impact branch.
     *
     * [CONFIRMED @ 0x408289-0x40855D, ApplyVehicleCollisionImpulse]: the branch
     * runs an INDEPENDENT pass over each actor of the pair -- actor A (base EBX,
     * 0x4082CB-0x408362) then actor B (base EBP, 0x40842B-0x408497). For a racer
     * slot (<6) each pass writes, via a fresh GetDamageRulesStub() call:
     *     angular_velocity_roll  (+0x1C0) += stub %  scatter    - scatter/2
     *     angular_velocity_yaw   (+0x1C4) += stub %  scatter    - scatter/2
     *     angular_velocity_pitch (+0x1C8) += stub % (2*scatter) - scatter
     *     linear_velocity_y      (+0x1D0)  = impact_mag / 6
     * scatter = (impact_mag >> 2) FLOORED to a MINIMUM of 0x7FFF
     * [CONFIRMED @ 0x4082A2-0x4082C9]. GetDamageRulesStub @0x0042C8D0 literally
     * `return 0` [CONFIRMED], so every `stub % N` term is 0 and the writes fold
     * to a DETERMINISTIC result with the SAME sign on BOTH actors:
     *     roll -= scatter/2;  yaw -= scatter/2;  pitch -= scatter;
     *     linear_velocity_y = impact_mag / 6;
     *
     * [FIX 2026-06-02 v2v-heavy-scatter-faithfulness] The prior port diverged on
     * three counts, all corrected here: (a) it clamped scatter with a CEILING
     * (`if > 0x7FFF`) -- inverted vs the original's FLOOR; (b) it used ad-hoc
     * kick magnitudes (scatter/8, -scatter/2, scatter/4 on roll/pitch/yaw) that
     * matched neither the original's axis mapping nor its magnitude; and (c) it
     * applied OPPOSITE signs to A (+kick) and B (-kick), whereas the original
     * writes the SAME sign to both. The vertical lift keeps the port-only 200000
     * clamp (the original has none) -- that clamp is a #bounce concern deferred
     * to a manual A/B; it only touches linear_velocity_y, not the spin.
     *
     * Gate semantics [VERIFIED 2026-05-17]: original at 0x00408289 reads
     * `if (90000 < iVar10 && g_cameraMode == 0)`. The port's `g_collisions_enabled`
     * lives at the SAME address (DAT_00463188) as the original's `g_cameraMode`;
     * `td5_physics_set_collisions(1)` writes 0, so `== 0` means collisions ON --
     * byte-faithful to the original's gate. */
    /* [#5 2026-06-20] Heavy-crash gate. Racer-vs-racer keeps the faithful 90000
     * floor; a pair that includes a TRAFFIC or COP car trips the dramatic 3D
     * collision (scatter+lift) AND the wreck at the lower npc_fatal_mag() so
     * those hits feel fatal and total the NPC much more readily. */
    int a_is_npc = (A->slot_index >= g_traffic_slot_base);
    int b_is_npc = (B->slot_index >= g_traffic_slot_base);
    int32_t heavy_gate = (a_is_npc || b_is_npc) ? npc_fatal_mag() : 90000;
    /* [POLICE 2026-06-24] Cop durability: a cop only takes the dramatic crash
     * reaction (scatter + break-down) above 2.5x the traffic floor. A cop hit
     * between heavy_gate and cop_gate is "tough" — it shrugs the collision off
     * entirely (no scatter, no air-launch, no wreck) and keeps chasing. This is
     * the core of "they shouldn't break down so easily" / "80% crash" fix:
     * routine bumps with traffic no longer end the pursuit. The player can still
     * total a cop by ramming it past cop_gate ("breakable by us"). */
    int a_is_cop = (A->slot_index >= 0) && td5_ai_actor_is_cop((int)A->slot_index);
    int b_is_cop = (B->slot_index >= 0) && td5_ai_actor_is_cop((int)B->slot_index);
    int32_t cop_gate = cop_break_mag();
    int a_tough_cop = a_is_cop && impact_mag <= cop_gate;
    int b_tough_cop = b_is_cop && impact_mag <= cop_gate;
    /* [ARCADE] Lower the heavy-impact gate so ordinary crashes launch cars into
     * the air (faithful gate is 90000 racer / npc_fatal_mag for NPC pairs). */
    if (td5_arcade_launch_active()) {
        int ag = td5_arcade_launch_gate();
        if (ag < heavy_gate) heavy_gate = ag;
    }
    /* [ARCADE] Boosted vertical launch: smaller divisor + higher clamp than the
     * faithful impact_mag/6 (200000-clamped). 1.0/no-change values outside arcade.
     * [2026-06-26] Arcade clamp lowered 800000 -> 160000 across two tuning
     * passes (each halving the mega-launch ceiling on huge hits); pairs with
     * the launch-divisor bump (3 -> 16) in td5_arcade_launch_div(). Net
     * airborne launch is ~5x lower than the original arcade feel. */
    const int     arc_launch_div   = td5_arcade_launch_active() ? td5_arcade_launch_div() : 6;
    const int32_t arc_launch_clamp = td5_arcade_launch_active() ? 160000 : 200000;
    if (impact_mag > heavy_gate && g_collisions_enabled == 0) {
        int32_t scatter = impact_mag / 4;
        if (scatter < 0x7FFF) scatter = 0x7FFF;   /* FLOOR (orig 0x4082A2-C9) */
        int32_t kick_ry = scatter / 2;            /* roll & yaw delta magnitude */
        int32_t kick_p  = scatter;                /* pitch delta magnitude      */
        /* [ARCADE 2026-06-26] Tame the angular crash-scatter so a hard hit
         * shoves/spins a car instead of flipping it nose-over into the air.
         * The faithful pitch kick (up to 0x7FFF) is what launches rammed cars;
         * scaling it down keeps crashes dramatic but playable. Vertical lift
         * (div/clamp) and horizontal impulse are separate levers. Applied at
         * the source so BOTH A and B scale, and A's rear_retain composes on
         * top. Knob TD5RE_ARCADE_SCATTER_PCT (default 35). */
        if (td5_arcade_launch_active()) {
            int sp = td5_arcade_scatter_pct();
            kick_ry = v2v_scale_pct(kick_ry, sp);
            kick_p  = v2v_scale_pct(kick_p,  sp);
        }
        /* [N-way coverage 2026-06-04] The racer/traffic split was hardcoded
         * `< 6`, the ORIGINAL racer count. With the N-way expansion a human
         * racer can sit in slots 6..15, so `< 6` wrongly routed it into the
         * traffic scripted crash-spin (vehicle_mode=1 takeover). Use the live
         * racer/traffic boundary g_traffic_slot_base — byte-identical to `< 6`
         * in a legacy 6-racer race, correct for the expanded field. */
        if (a_tough_cop) {
            /* [POLICE] Durable cop below cop_gate: no scatter / no lift / no
             * spin-out — it absorbs the hit and stays on the chase. */
        } else if (A->slot_index < g_traffic_slot_base) {
            /* [S08] Soften the rear-ended player's launch/spin (rear_retain is
             * 100 for everyone else, so this is a no-op for AI/front/side). */
            int32_t a_kick_ry = v2v_scale_pct(kick_ry, rear_retain);
            int32_t a_kick_p  = v2v_scale_pct(kick_p,  rear_retain);
            int32_t lift_a = impact_mag / arc_launch_div;
            if (lift_a > arc_launch_clamp) lift_a = arc_launch_clamp;
            lift_a = v2v_scale_pct(lift_a, rear_retain);
            /* [WEIGHT fly-away 2026-06-25] Lighter cars get a bigger vertical pop,
             * heavier cars stay planted. Per-car Q8 from collision_mass; the
             * horizontal knockback is already mass-driven by the faithful impulse.
             * Knob TD5RE_WEIGHT_LIFT_PCT (master TD5RE_WEIGHT_MECH). */
            lift_a = td5_physics_scale_lift_q8(lift_a, td5_physics_weight_lift_q8(A));
            /* [POLICE] A racer-slot cop (MP cop chase) never goes airborne. */
            if (a_is_cop) lift_a = 0;
            A->angular_velocity_roll  -= a_kick_ry;
            A->angular_velocity_yaw   -= a_kick_ry;
            A->angular_velocity_pitch -= a_kick_p;
            A->linear_velocity_y  = lift_a;
        } else {
            /* Traffic: scripted crash-spin recovery, orig 0x00408289+.
             * (A cop here gets lift suppressed inside the helper.) */
            td5_physics_apply_traffic_crash_spin(A, impact_mag);
        }
        if (b_tough_cop) {
            /* [POLICE] Durable cop below cop_gate: absorb and keep chasing. */
        } else if (B->slot_index < g_traffic_slot_base) {
            /* B is the penetrator (never the rear-contacted victim here), so it
             * always takes the byte-faithful kick — the attacker is unchanged. */
            int32_t lift_b = impact_mag / arc_launch_div;
            if (lift_b > arc_launch_clamp) lift_b = arc_launch_clamp;
            /* [WEIGHT fly-away 2026-06-25] Lighter cars pop up more, heavier stay
             * planted. Per-car Q8 from collision_mass, composed on top of the
             * arcade/sim launch scaling above. Knob TD5RE_WEIGHT_LIFT_PCT. */
            lift_b = td5_physics_scale_lift_q8(lift_b, td5_physics_weight_lift_q8(B));
            if (b_is_cop) lift_b = 0;             /* [POLICE] no airborne for a cop */
            B->angular_velocity_roll  -= kick_ry;
            B->angular_velocity_yaw   -= kick_ry;
            B->angular_velocity_pitch -= kick_p;
            B->linear_velocity_y  = lift_b;
        } else {
            /* Traffic: scripted crash-spin recovery, orig 0x00408289+. */
            td5_physics_apply_traffic_crash_spin(B, impact_mag);
        }
        /* [CRASH FX 2026-06-15, item #12] On a genuinely high-speed hit, give a
         * PLAYER actor an extra forward-speed scrub and record a crash-fx event
         * other modules can poll (HUD shake / VFX / audio). Players only (slot <
         * g_traffic_slot_base); both A and B are scrubbed if both are players.
         * Gated by TD5RE_CRASH_FX (default ON) inside the helper; off => no-op.
         * The faithful angular scatter + vertical lift above are unchanged. */
        if (impact_mag > CRASH_FX_ACUTE_MAG) {
            if (A->slot_index < g_traffic_slot_base)
                td5_physics_apply_acute_crash_fx(A, impact_mag);
            if (B->slot_index < g_traffic_slot_base)
                td5_physics_apply_acute_crash_fx(B, impact_mag);
        }
        /* [#5 2026-06-20, was POLICE rewrite 2026-06-19] This heavy crash WRECKS
         * every involved TRAFFIC/COP car: it breaks down -> parks, smokes from
         * the roof, and can never drive itself again (permanent). A chased racer
         * is also flagged so its "broke down" ENDS the pursuit, but racers/the
         * player are never immobilised or smoked (handled downstream). Because we
         * are already inside the (NPC-lowered) heavy gate, no separate magnitude
         * test is needed — a fatal hit on traffic/cops always totals them. */
        /* [MP TRAFFIC FAIRNESS 2026-06-22] In fair split-screen MP, don't
         * PERMANENTLY total plain background traffic (keep the shared stream
         * deterministic so every viewport sees the same cars — it free-slides
         * then recovers). Cops and pursued racers still wreck normally. */
        {
            int fair   = td5_game_mp_traffic_fair();
            int battle = td5_game_battle_mode_active();
            int sa = (int)A->slot_index, sb = (int)B->slot_index;
            int plain_a = (sa >= g_traffic_slot_base) &&
                          !td5_ai_actor_is_pursued(sa) && !td5_ai_cop_is_chasing(sa);
            int plain_b = (sb >= g_traffic_slot_base) &&
                          !td5_ai_actor_is_pursued(sb) && !td5_ai_cop_is_chasing(sb);
            /* [TRAFFIC BATTLE 2026-06-28] Score the wreck BEFORE marking the
             * victim broken-down, so td5_arcade_note_ram can dedup on the
             * not-yet-broken victim (each destroyed traffic car counts once).
             * Both directions; note_ram self-gates which side is a racer-vs-
             * traffic fatal hit. No-op outside battle mode. */
            td5_arcade_note_ram(sa, sb, impact_mag);   /* A rammed B */
            td5_arcade_note_ram(sb, sa, impact_mag);   /* B rammed A */
            /* [POLICE 2026-06-24] A tough cop (hit below cop_gate) is NOT totalled
             * — only a deliberate hard ram past 2.5x the traffic floor wrecks it.
             * [TRAFFIC BATTLE 2026-06-28] In battle mode plain traffic is always
             * wreckable (the MP traffic-fair recover rule is ignored) so a
             * destroyed car is marked broken-down — that flag is note_ram's dedup
             * key, and a fresh kill stream is the whole point of the mode. */
            int fair_a = fair && plain_a && !battle;
            int fair_b = fair && plain_b && !battle;
            if ((a_is_npc || td5_ai_actor_is_pursued(sa)) && !fair_a && !a_tough_cop)
                td5_ai_mark_actor_broken_down(sa);
            if ((b_is_npc || td5_ai_actor_is_pursued(sb)) && !fair_b && !b_tough_cop)
                td5_ai_mark_actor_broken_down(sb);
        }
        TD5_LOG_I(LOG_TAG, "v2v_heavy_scatter: A=%d B=%d mag=%d scatter=%d kick_ry=%d kick_p=%d rear_retain=%d "
                  "copA=%d copB=%d cop_gate=%d toughA=%d toughB=%d",
                  A->slot_index, B->slot_index, impact_mag, scatter, kick_ry, kick_p, rear_retain,
                  a_is_cop, b_is_cop, cop_gate, a_tough_cop, b_tough_cop);
    }

    /* [task #9] After the full impulse + (optional) heavy scatter, keep any
     * traffic actor in the pair at/above the track surface so it can never sink
     * through the ground following a hard hit. Racers are left untouched (their
     * own ground-snap runs in the pose integrator). No-op when tame is off. */
    if (traffic_hit_tame_enabled()) {
        if (A->slot_index >= g_traffic_slot_base) traffic_clamp_above_ground(A);
        if (B->slot_index >= g_traffic_slot_base) traffic_clamp_above_ground(B);
    }

    /* [ARCADE wrecking ball] Restore the immune wrecker(s) to their pre-collision
     * motion+pose so they plow straight through, while the victim keeps the full
     * (3x, low-gate) launch computed above. If BOTH are wrecking they both pass
     * through each other untouched. */
    if (A_wreck) {
        A->world_pos.x = Awx; A->world_pos.z = Awz;
        A->linear_velocity_x = Avx; A->linear_velocity_y = Avy; A->linear_velocity_z = Avz;
        A->angular_velocity_roll = Aroll; A->angular_velocity_yaw = saved_omega_A;
        A->angular_velocity_pitch = Apitch; A->euler_accum.yaw = Aeyaw;
        update_vehicle_pose_from_physics(A);
        td5_arcade_note_ram((int)A->slot_index, (int)B->slot_index, impact_mag);
    }
    if (B_wreck) {
        B->world_pos.x = Bwx; B->world_pos.z = Bwz;
        B->linear_velocity_x = Bvx; B->linear_velocity_y = Bvy; B->linear_velocity_z = Bvz;
        B->angular_velocity_roll = Broll; B->angular_velocity_yaw = saved_omega_B;
        B->angular_velocity_pitch = Bpitch; B->euler_accum.yaw = Beyaw;
        update_vehicle_pose_from_physics(B);
        td5_arcade_note_ram((int)B->slot_index, (int)A->slot_index, impact_mag);
    }

    /* pool14_v2v pilot trace: capture post-state at function exit.
     * Original returns int impact_mag at 0x004084A2 RET. */
}

/* ========================================================================
 * Simple Collision -- FUN_00408f70
 *
 * For crashed/flipped cars -- sphere overlap with simple impulse.
 * Combined radius = (radiusA + radiusB) * 3/4
 * ======================================================================== */

static void collision_detect_simple(TD5_Actor *a, TD5_Actor *b)
{
    if (!a || !b) return;
    if (!a->car_definition_ptr || !b->car_definition_ptr) return;

    /* [ARCADE] A GHOSTing racer passes through everything — this sphere path is
     * the player-vs-TRAFFIC resolver (and the scripted-pair path) and has no
     * other ghost gate, so a ghost would still bump traffic without this. */
    if (td5_arcade_slot_is_ghost((int)a->slot_index) ||
        td5_arcade_slot_is_ghost((int)b->slot_index)) return;

    /* [TRAFFIC BATTLE] Wrecked traffic is intangible here too — this crashed-car
     * sphere resolver was a second path that still bumped wrecks (the user's
     * "ghost/wrecked cars should not have collision"). */
    if (battle_wreck_intangible(a, b)) return;

    int32_t radius_a = (int32_t)CDEF_S(a, CDEF_COLLISION_RADIUS);
    int32_t radius_b = (int32_t)CDEF_S(b, CDEF_COLLISION_RADIUS);
    int32_t combined = ((radius_a + radius_b) * 3) >> 2;

    int32_t dx = (a->world_pos.x >> 8) - (b->world_pos.x >> 8);
    int32_t dy = (a->world_pos.y >> 8) - (b->world_pos.y >> 8);
    int32_t dz = (a->world_pos.z >> 8) - (b->world_pos.z >> 8);

    int32_t dist_sq = dx * dx + dy * dy + dz * dz;
    if (dist_sq >= combined * combined) return;

    int32_t dist = td5_isqrt(dist_sq);
    if (dist == 0) return;

    /* Normalize separation vector (12-bit) */
    int32_t nx = (dx << 12) / dist;
    int32_t ny = (dy << 12) / dist;
    int32_t nz = (dz << 12) / dist;

    /* Project relative velocity onto normal */
    int32_t rel_vx = a->linear_velocity_x - b->linear_velocity_x;
    int32_t rel_vy = a->linear_velocity_y - b->linear_velocity_y;
    int32_t rel_vz = a->linear_velocity_z - b->linear_velocity_z;
    int32_t v_dot = (rel_vx * nx + rel_vy * ny + rel_vz * nz) >> 12;

    /* Only apply if closing (dot < 0) */
    if (v_dot >= 0) return;

    /* Faithful port of ResolveSimpleActorSeparation @ 0x00408F70
     * [CONFIRMED 2026-05-12 via Ghidra decomp]:
     *   iVar6 = (nx_12bit * v_proj_16) / 32
     *   A->vel += iVar6;  B->vel -= iVar6;
     * No mass divide in orig. Operand order is multiply-then-divide so the
     * small projection magnitude doesn't truncate to zero.
     *
     * Previous port did `impulse = -v_dot >> 5` FIRST (truncating tiny
     * closing-velocity projections to 0 or -1), then `imp_x = (impulse * nx)
     * >> 12`, then divided by mass. That made the impulse ~260x weaker than
     * orig at typical race speeds (orig: ~6 wu/tick separation; port: 0.023
     * wu/tick). Cars in continuous grounded contact (slot 0+slot 4 logged
     * 31k V2V events on Moscow) never decelerated enough to separate.
     *
     * Scale match: orig stores rel_v as int16 (vel>>8). Port keeps 24.8 FP.
     * v_dot_port = v_dot_orig * 256, so divide by 8192 (instead of orig's 32)
     * to land on the same 24.8 FP delta magnitude. */
    int32_t impulse_scalar = -v_dot;  /* positive when closing, in 24.8 FP */
    int32_t delta_x = (nx * impulse_scalar) >> 13;
    int32_t delta_y = (ny * impulse_scalar) >> 13;
    int32_t delta_z = (nz * impulse_scalar) >> 13;

    /* [task #9] Constrain the response to the horizontal plane for traffic.
     * This sphere path runs for player-vs-traffic (the explicit S20 collide)
     * and scripted pairs; its separation vector includes a vertical (Y)
     * component, so a player ramming a traffic car at speed can punt it
     * upward. When tame is on and EITHER actor is traffic, drop the Y impulse
     * for the pair so the bump stays in-plane (the player still gets shoved
     * sideways/forward exactly as before). */
    if (traffic_hit_tame_enabled() &&
        (a->slot_index >= g_traffic_slot_base || b->slot_index >= g_traffic_slot_base)) {
        delta_y = 0;
    }

    a->linear_velocity_x += delta_x;
    a->linear_velocity_y += delta_y;
    a->linear_velocity_z += delta_z;
    b->linear_velocity_x -= delta_x;
    b->linear_velocity_y -= delta_y;
    b->linear_velocity_z -= delta_z;

    /* [#10 telemetry] A closing sphere overlap that produced a separation
     * impulse is a real car-to-car contact — flag both actors for this tick. */
    td5_physics_mark_collision((int)a->slot_index);
    td5_physics_mark_collision((int)b->slot_index);

    /* [task #9] Belt-and-suspenders: keep any traffic actor in the pair from
     * ending up below the track surface this tick. */
    if (traffic_hit_tame_enabled()) {
        if (a->slot_index >= g_traffic_slot_base) traffic_clamp_above_ground(a);
        if (b->slot_index >= g_traffic_slot_base) traffic_clamp_above_ground(b);
    }
}

/* ========================================================================
 * Full Collision Detection -- FUN_00408a60
 *
 * Binary search refinement using OBB corner test:
 * 1. AABB pre-test from grid bounds
 * 2. Initial OBB test at full size
 * 3. Binary search 7 iterations: halve half-extents each step
 * 4. Penetration depth = final_adjustment - 0x10
 * 5. Dispatch collision response based on corner bitmask
 * ======================================================================== */

static void collision_detect_full(TD5_Actor *a, TD5_Actor *b, int idx_a, int idx_b)
{
    if (!a || !b) return;
    if (!a->car_definition_ptr || !b->car_definition_ptr) return;

    /* Pool15 V2V pilot trace — reset corner-test call counter so the next
     * 1+7 obb_corner_test calls inside this function get event_idx 1..8. */

    /* AABB pre-test from broadphase grid.
     * [CONFIRMED @ 0x00408A92-0x00408AB7]: original uses JGE/JLE — i.e.
     * `<=` / `>=` predicates. At exact equality the original rejects;
     * port previously used `<` and admitted the pair. One-LSB over-
     * approximation closed by switching `<` to `<=`. */
    if (g_actor_aabb[idx_a][2] <= g_actor_aabb[idx_b][0] ||
        g_actor_aabb[idx_b][2] <= g_actor_aabb[idx_a][0] ||
        g_actor_aabb[idx_a][3] <= g_actor_aabb[idx_b][1] ||
        g_actor_aabb[idx_b][3] <= g_actor_aabb[idx_a][1]) {
        return;
    }

    /* [precise-00408A60 byte-faithful port 2026-05-14]
     *
     * The original ResolveVehicleCollisionPair (0x00408A60) keeps the
     * bisection accumulators in RAW 24.8 fixed-point throughout — the
     * `>> 8` conversion to display units only happens INSIDE
     * CollectVehicleCollisionContacts at 0x00408570 and at the final
     * dispatch push (0x00408D58 SAR ECX,8 / 0x00408DD4 SAR EDX,8).
     *
     * Listing 0x00408AED-0x00408B23 reads the SIX accumulator seeds via
     * DWORD MOV directly out of the actor structs at full 24.8 scale:
     *   local_80 / [ESP+0x2C] = actor_A.world_pos.x  (+0x1FC)
     *   local_7c / [ESP+0x30] = actor_A.world_pos.y  (+0x200) (read but unused)
     *   local_78 / [ESP+0x34] = actor_A.world_pos.z  (+0x204)
     *   local_94 / [ESP+0x1C] = actor_A.euler_accum.yaw (+0x1F4)
     *   local_74..local_6c  = actor_B mirror (B.x/y/z @ +0x1FC..+0x204)
     *   local_90 / [ESP+0xa8 or saved] = actor_B.euler_accum.yaw
     *
     * Per-iter halving uses the CDQ/SUB/SAR pattern (0x00408B6F-0x00408BB1
     * pre-loop, 0x00408C10-0x00408C56 in-loop) which is C-style signed
     * divide-by-2 (truncate-toward-zero) — different from `>>= 1` on
     * negative odd values by 1 LSB. We use `/= 2` to match exactly.
     *
     * Bisection accumulator `local_84` (sum of ±local_8c) is the analog of
     * the port's old `frac`; impactForce = local_84 - 0x10. */

    /* Pre-load raw 24.8 accumulators from actor (listing 0x00408AED-0x00408B23). */
    int32_t pos_a_x = a->world_pos.x;
    int32_t pos_a_z = a->world_pos.z;
    int32_t pos_b_x = b->world_pos.x;
    int32_t pos_b_z = b->world_pos.z;
    int32_t yaw_a_acc = a->euler_accum.yaw;
    int32_t yaw_b_acc = b->euler_accum.yaw;

    /* Initial-state display-unit headings (used only for the diagnostic log
     * below; mirrors the prior heading_a/heading_b labels). */
    int32_t initial_heading_a = (yaw_a_acc >> 8) & 0xFFF;
    int32_t initial_heading_b = (yaw_b_acc >> 8) & 0xFFF;

    /* Initial OBB test at full (end-of-tick) position.
     * [CONFIRMED @ 0x00408B27-B48]: CollectVehicleCollisionContacts is
     * invoked with raw 24.8-fp position triplets and raw 24.8-fp euler
     * accumulators. The callee does (pos_b - pos_a) >> 8 and yaw_acc >> 8
     * internally — see the wrapper header comment. Pass raw, matching the
     * listing exactly. */
    OBB_CornerData corners[8];
    memset(corners, 0, sizeof(corners));
    int bitmask = obb_corner_test(a, b,
                                  pos_a_x, pos_a_z, pos_b_x, pos_b_z,
                                  yaw_a_acc, yaw_b_acc, corners);

    if (bitmask == 0) return;  /* No overlap at full size — early-return. */

    /* --- 7-iteration TOI binary search [listing 0x00408B5C-0x00408D23] ---
     *
     * Per-axis half-step accumulators (initial = velocity/2 via CDQ/SUB/SAR
     * at 0x00408B6F-0x00408BB1). These are raw 24.8 fp values; integer
     * signed division by 2 (truncate-toward-zero) matches the asm pattern.
     *
     * Pre-loop subtraction (listing 0x00408BB5-0x00408BF5) walks the
     * positions/headings BACK to the t=0.5 midpoint before the loop:
     *   local_80 -= iVar12; local_78 -= iVar13; local_94 -= iVar9
     *   local_74 -= iVar11; local_6c -= local_4; local_90 -= local_5c
     *
     * local_84 = local_8c = 0x80 (initial centre + initial step).
     * local_68 = 7 (loop counter). */
    int32_t step_ax = a->linear_velocity_x   / 2;  /* iVar12 */
    int32_t step_az = a->linear_velocity_z   / 2;  /* iVar13 */
    int32_t step_ah = a->angular_velocity_yaw / 2; /* iVar9 — yaw step */
    int32_t step_bx = b->linear_velocity_x   / 2;  /* iVar11 */
    int32_t step_bz = b->linear_velocity_z   / 2;  /* local_4 */
    int32_t step_bh = b->angular_velocity_yaw / 2; /* local_5c — B yaw step */

    pos_a_x -= step_ax;
    pos_a_z -= step_az;
    pos_b_x -= step_bx;
    pos_b_z -= step_bz;
    yaw_a_acc -= step_ah;
    yaw_b_acc -= step_bh;

    int32_t local_84  = 0x80;     /* bisection accumulator (-> impactForce) */
    int32_t local_8c  = 0x80;     /* current half-step magnitude (sign chosen per-iter) */

    /* Cache last-hit bitmask + corner data. Seeded from the pre-loop test
     * (listing 0x00408B52 MOV [ESP+0x24],EAX). Loop updates it ONLY on
     * the bitmask!=0 branch (listing 0x00408CD0). */
    int32_t        cached_bitmask = bitmask;
    OBB_CornerData cached_corners[8];
    memcpy(cached_corners, corners, sizeof(cached_corners));

    /* Display-unit heading snapshot for the dispatch path. Updated each
     * iter from yaw_a_acc/yaw_b_acc after the wrapper call. */
    int32_t test_ha = yaw_a_acc >> 8;
    int32_t test_hb = yaw_b_acc >> 8;

    for (int iter = 0; iter < 7; iter++) {
        /* Per-iter halving [listing 0x00408C0E-0x00408C56]:
         * each step accumulator is signed-divided by 2 (CDQ/SUB/SAR pattern).
         * local_8c is also signed-/2 — `(int)local_8c / 2` in the decompiler. */
        step_ax  /= 2;
        step_az  /= 2;
        step_ah  /= 2;
        step_bx  /= 2;
        step_bz  /= 2;
        step_bh  /= 2;
        local_8c /= 2;

        /* Pass current raw 24.8 accumulators to the wrapper, matching the
         * listing exactly — callee owns the >>8 shift on delta/yaw. */
        test_ha = yaw_a_acc >> 8;  /* snapshot for dispatch logging */
        test_hb = yaw_b_acc >> 8;

        memset(corners, 0, sizeof(corners));
        int32_t result = obb_corner_test(a, b,
                                         pos_a_x, pos_a_z, pos_b_x, pos_b_z,
                                         yaw_a_acc, yaw_b_acc, corners);

        /* Default direction (uVar5 == 0 / miss path, listing 0x00408CD6-0x00408D1F):
         *   move FORWARD in time — add the current half-step to positions
         *   and local_84 (uVar10 = local_8c, iVar1=iVar12, etc.).
         * Hit path (uVar5 != 0, listing 0x00408C83-0x00408CD0):
         *   negate all step deltas (uVar10 = -local_8c, iVar1 = -iVar12...),
         *   cache the bitmask, and apply — which steps BACK in time. */
        int32_t dir_ax = step_ax;
        int32_t dir_az = step_az;
        int32_t dir_ah = step_ah;
        int32_t dir_bx = step_bx;
        int32_t dir_bz = step_bz;
        int32_t dir_bh = step_bh;
        int32_t dir_8c = local_8c;
        if (result != 0) {
            cached_bitmask = result;
            memcpy(cached_corners, corners, sizeof(cached_corners));
            dir_ax = -step_ax;
            dir_az = -step_az;
            dir_ah = -step_ah;
            dir_bx = -step_bx;
            dir_bz = -step_bz;
            dir_bh = -step_bh;
            dir_8c = -local_8c;
        }

        pos_a_x  += dir_ax;
        pos_a_z  += dir_az;
        pos_b_x  += dir_bx;
        pos_b_z  += dir_bz;
        yaw_a_acc += dir_ah;
        yaw_b_acc += dir_bh;
        local_84  += dir_8c;
    }

    /* Post-loop: refresh display-unit yaws from the FINAL raw accumulators
     * for the dispatch impulse calls (listing 0x00408D58 SAR ECX,8 reads
     * local_94 — the raw accumulator — and shifts; ditto 0x00408DD4 for
     * local_90 with B's yaw). Without this the dispatch yaw would be stale
     * from the LAST iteration's read instead of from the FINAL post-update
     * accumulator. The original matches because it re-reads local_94/90
     * at the dispatch site after the loop has updated them. */
    test_ha = yaw_a_acc >> 8;
    test_hb = yaw_b_acc >> 8;

    /* impactForce = local_84 - 0x10 [listing 0x00408D34 SUB ESI,0x10].
     * Range after 7 iters from 0x80: [-0x10, 0xEF] (no clamp). */
    int32_t impactForce = local_84 - 0x10;

    /* Pool15 V2V pilot trace — emit toi-row at dispatch boundary. */

    /* Dispatch uses the cached last-non-zero bitmask + corners, NOT a final
     * re-test. `test_ha` / `test_hb` carry the final-state yaws (in 12-bit
     * units, possibly slightly outside [0,0xFFF] — handled by cos_fixed12/
     * sin_fixed12's internal mask) because the original's dispatch push uses
     * `local_94 >> 8` / `local_90 >> 8` which are the bisection accumulators'
     * final values [CONFIRMED @ 0x00408D58 SAR ECX,8; 0x00408DD4].
     * Final-state yaw sits within ±(last bisect_step) of the last-hit yaw. */
    TD5_LOG_I(LOG_TAG, "v2v_bisect: slotA=%d slotB=%d local_84=0x%02X impactForce=%d bitmask=0x%02X bisHA=0x%03X bisHB=0x%03X rawHA=0x%03X rawHB=0x%03X",
              a->slot_index, b->slot_index, local_84, impactForce, cached_bitmask,
              test_ha, test_hb, initial_heading_a, initial_heading_b);

    /* --- Dispatch collision response based on corner bitmask ---
     * 4th arg `angle` is the target's BISECTED yaw (>> 8).
     * [CONFIRMED @ 0x00408D58 SAR ECX,0x8; 0x00408D5B PUSH ECX]
     *   For cases where B is the penetrator (bits 0-3): angle = local_94 >> 8
     *     = (actor_A.euler_accum.yaw accumulator post-bisection) >> 8 = test_ha.
     *   For cases where A is the penetrator (bits 4-7): angle = local_90 >> 8
     *     = (actor_B.euler_accum.yaw accumulator post-bisection) >> 8 = test_hb. */
    /* Bits 0-3: B corners in A -> response(B, A, corner) -- B is penetrating A */
    for (int i = 0; i < 4; i++) {
        if (cached_bitmask & (1 << i)) {
            apply_collision_response(b, a, i, &cached_corners[i], test_ha, impactForce);
        }
    }
    /* Bits 4-7: A corners in B -> response(A, B, corner) -- A is penetrating B */
    for (int i = 0; i < 4; i++) {
        if (cached_bitmask & (1 << (i + 4))) {
            apply_collision_response(a, b, i, &cached_corners[i + 4], test_hb, impactForce);
        }
    }
}

/* ========================================================================
 * Collision impulse API (0x4079C0) -- Wrapper for backward compatibility
 * ======================================================================== */

void td5_physics_apply_collision_impulse(TD5_Actor *a, TD5_Actor *b)
{
    if (!a || !b) return;
    if (!a->car_definition_ptr || !b->car_definition_ptr) return;

    /* Use the simple sphere-based collision for API-level calls.
     * The full OBB system is invoked through resolve_vehicle_contacts. */
    collision_detect_simple(a, b);
}

/* ========================================================================
 * Anti-tunnel car-vs-car depenetration (S17 2026-06-05) -- PORT-ONLY
 *
 * The original v2v resolution (ApplyVehicleCollisionImpulse @ 0x004079C0)
 * applies a single FIXED half-cos/sin positional push -- NOT proportional to
 * how deeply the cars overlap -- and runs once per frame with no relaxation
 * loop. A high-speed / deep collision therefore stays visibly interpenetrated
 * for one or more frames before the impulse plus repeated small fixed pushes
 * finally separate the cars. That is the user-visible "at the moment of
 * crashing I can briefly go THROUGH cars".
 *
 * This pass restores hard non-overlap by pushing any genuinely OBB-overlapping
 * pair fully apart along their line of centres. It is purely POSITIONAL: it
 * writes world_pos only and never touches linear or angular velocity, so the
 * S08 rear/side/front impact response tuning (RearImpactResponse etc.) is
 * provably unaffected -- the cars bounce and spin exactly as before, they just
 * no longer clip through one another first.
 *
 * Overlap is detected with the SAME obb_corner_test the collision system uses,
 * so two cars racing side-by-side without actually touching are never pushed
 * apart (no ghost repulsion) and we separate exactly the pairs the game already
 * treats as in contact. The push direction is the world line of centres, which
 * for convex bodies monotonically reduces penetration (so it cannot oscillate)
 * and whose sign is trivially "away from each other".
 *
 * Returns the pre-push penetration depth in display units (> 0 means it moved
 * the pair apart), or 0 if the cars were not actually overlapping.
 * ======================================================================== */
static int32_t v2v_depenetrate_pair(TD5_Actor *a, TD5_Actor *b)
{
    OBB_CornerData corners[8];
    memset(corners, 0, sizeof(corners));

    /* [ARCADE] GHOSTing racer is intangible — don't depenetrate it out of a car
     * or traffic vehicle it's passing through. */
    if (a && b && (td5_arcade_slot_is_ghost((int)a->slot_index) ||
                   td5_arcade_slot_is_ghost((int)b->slot_index))) return 0;

    /* [TRAFFIC BATTLE] Don't push the player back out of a wrecked traffic car —
     * wrecks are intangible in battle so you slide clean through them. */
    if (battle_wreck_intangible(a, b)) return 0;

    /* Detect overlap at the cars' CURRENT positions (the faithful impulse and
     * earlier relaxation rounds may already have moved them). Raw 24.8 fp in;
     * the callee shifts internally. The pilot-trace emits inside obb_corner_test
     * self-gate to a no-op unless V2V contact tracing is explicitly enabled, so
     * these extra detection calls are free in normal and release builds. */
    int bm = obb_corner_test(a, b,
                             a->world_pos.x, a->world_pos.z,
                             b->world_pos.x, b->world_pos.z,
                             a->euler_accum.yaw, b->euler_accum.yaw,
                             corners);
    if (bm == 0) return 0;            /* not actually overlapping */

    /* Deepest minimal-axis penetration across all set corners (display units).
     * pen_x/pen_z are the signed depth to the nearest face on each local axis;
     * min(|pen_x|,|pen_z|) is that corner's minimum-translation distance, and
     * the max over corners is how far the pair must move to fully clear. */
    int32_t depth = 0;
    for (int c = 0; c < 8; c++) {
        if (!(bm & (1 << c))) continue;
        int32_t px = corners[c].pen_x; if (px < 0) px = -px;
        int32_t pz = corners[c].pen_z; if (pz < 0) pz = -pz;
        int32_t m  = (px < pz) ? px : pz;
        if (m > depth) depth = m;
    }
    if (depth <= 0) return 0;         /* exactly touching, nothing to clear */

    /* Playability slop: the collision OBB is slightly larger than the visible
     * car mesh, so depenetrating to ZERO OBB overlap parks the cars with a
     * visible gap (~the combined box-vs-mesh margin). Allow anti_tunnel_slop
     * units of OBB overlap to remain so the cars settle at ~mesh contact, and
     * push only the excess. The faithful impulse/bounce path is untouched --
     * only the resting separation changes. */
    int32_t push_depth = depth - g_td5.ini.anti_tunnel_slop;
    if (push_depth <= 0) return 0;    /* within the allowed slop -> leave it */
    if (push_depth > ANTITUNNEL_MAX_DEPTH) push_depth = ANTITUNNEL_MAX_DEPTH;

    /* World line of centres (display units), pointing A away from B. */
    int32_t dx = (a->world_pos.x - b->world_pos.x) >> 8;
    int32_t dz = (a->world_pos.z - b->world_pos.z) >> 8;
    int32_t nx, nz;                   /* 12-bit unit direction */
    int32_t len = td5_isqrt(dx * dx + dz * dz);
    if (len < 1) {
        /* Centres coincide -- eject along A's lateral axis as a fallback so we
         * still make progress instead of dividing by zero. */
        int32_t ha = a->euler_accum.yaw >> 8;
        nx =  cos_fixed12(ha);
        nz = -sin_fixed12(ha);
    } else {
        nx = (dx << 12) / len;
        nz = (dz << 12) / len;
    }

    /* Split the separation 50/50: each car moves half of (push_depth + 2)
     * display units along the unit normal, converted to 24.8 world position
     * units ((unit/4096) * half * 256 == (unit * half) >> 4). The +2 guard
     * clears the slop boundary by a couple of units so the next test settles. */
    int32_t half = (push_depth + 2) >> 1;
    int32_t mv_x = (nx * half) >> 4;
    int32_t mv_z = (nz * half) >> 4;

    a->world_pos.x += mv_x;  a->world_pos.z += mv_z;
    b->world_pos.x -= mv_x;  b->world_pos.z -= mv_z;
    return depth;
}

/* ========================================================================
 * ResolveVehicleContacts (0x409150) -- Spatial grid broadphase + OBB
 *
 * Phase 1: Build AABB grid, insert actors into spatial buckets
 * Phase 2: For each actor, walk adjacent buckets, test pairs
 * Phase 3: V2W wall collision
 * Phase 4: Reset grid
 * ======================================================================== */

void td5_physics_resolve_vehicle_contacts(void)
{
    int total;
    static uint32_t s_v2v_tick = 0;
    static uint32_t s_v2v_slot0_pair_count = 0;
    static uint32_t s_v2v_slot0_obb_enter = 0;

    if (!g_actor_table_base) {
        return;
    }

    /* [CORRECTED 2026-05-26 v2v-include-traffic; orig 0x00409150]
     * Earlier port comment claimed DAT_004aaf00 was g_racerCount and
     * excluded traffic. Ghidra re-audit confirms DAT_004aaf00 is the
     * FULL active-actor count written by 0x00432e60 as ((traffic_on)?12:6)
     * — so orig DOES include traffic slots 6..11 in V2V broadphase.
     * The dispatch at line ~4350 already gates per-slot on vehicle_mode
     * and wheel_contact_bitmask, so re-including traffic here surfaces
     * the user-reported missing traffic-vs-player collisions without
     * affecting racer-on-racer behavior. */
    total = (int)td5_game_get_total_actor_count();
    if (total > TD5_MAX_TOTAL_ACTORS) total = TD5_MAX_TOTAL_ACTORS;
    if (total < 2) {
        return;
    }


    s_v2v_tick++;

    /* Log slot 0's broadphase state once per second (60 ticks). Helps diagnose
     * why player rarely produces V2V events: either the grid isn't bucketing
     * it with AI (spatial separation) or the dispatch is filtering it out. */
    if ((s_v2v_tick % 60u) == 0u) {
        TD5_Actor *p0 = (TD5_Actor *)g_actor_table_base;
        if (p0->car_definition_ptr) {
            int32_t seg0 = p0->track_span_normalized < 0 ? 0 : p0->track_span_normalized;
            int bucket0 = (seg0 >> 2) & (COLLISION_GRID_SIZE - 1);
            TD5_LOG_I(LOG_TAG,
                      "v2v_tick slot0: total=%d span=%d bucket=%d pos=(%d,%d,%d) "
                      "vmode=%d dmg=%d pair_calls=%u aabb_hits=%u",
                      total, seg0, bucket0,
                      p0->world_pos.x >> 8, p0->world_pos.y >> 8, p0->world_pos.z >> 8,
                      p0->vehicle_mode, p0->damage_lockout,
                      s_v2v_slot0_pair_count, s_v2v_slot0_obb_enter);
            s_v2v_slot0_pair_count = 0;
            s_v2v_slot0_obb_enter = 0;
        }
    }

    /* [TRAFFIC BATTLE 2026-06-28] Reliable SWEPT wreck pass — runs FIRST, BEFORE
     * the broadphase/impulse touches anything, so it reads each car's true
     * PRE-collision velocity. (Running it last read the post-bounce velocity: a
     * light car bounced off the traffic, the reconstructed path no longer crossed
     * the car, and the sweep missed — which is exactly why heavier cars seemed to
     * wreck more.) For each racer moving above a floor speed we run the REAL
     * oriented-box test at NSUB+1 points along BOTH cars' paths this tick; a hit
     * at ANY sub-step means the bodies actually touched, so we total the traffic
     * car — independent of contact angle, frame-rate tunneling, AND car mass. The
     * car then goes broken-down => intangible (battle_wreck_intangible) so the
     * normal collision below passes the player straight through the husk. */
    if (td5_game_battle_mode_active()) {
        int base      = g_traffic_slot_base;
        int tmax2     = (total < TD5_MAX_TOTAL_ACTORS) ? total : TD5_MAX_TOTAL_ACTORS;
        int32_t mins  = battle_ram_min_speed();
        int32_t cap   = battle_ram_cap();
        int32_t lift0 = cap / 20; if (lift0 > 120000) lift0 = 120000;
        int32_t air_vy = battle_airborne_vy();
        /* Finer sub-sampling (12) so a fast HEAD-ON — where the cars cross in a
         * fraction of a tick — isn't stepped over between samples (the user's
         * "front-to-front aligned is harder to trigger"). */
        const int NSUB = 12;
        OBB_CornerData corners[8];
        for (int rsl = 0; rsl < base; rsl++) {
            TD5_Actor *rc = (TD5_Actor *)(g_actor_table_base + (size_t)rsl * TD5_ACTOR_STRIDE);
            if (!rc->car_definition_ptr || rc->finish_time != 0) continue;
            int32_t rvx = rc->linear_velocity_x, rvz = rc->linear_velocity_z;
            uint64_t rs2 = (uint64_t)((int64_t)rvx * rvx + (int64_t)rvz * rvz);
            if (rs2 > 0xFFFFFFFFull) rs2 = 0xFFFFFFFFull;
            if ((int32_t)td5_isqrt((uint32_t)rs2) < mins) continue;   /* not moving */
            int32_t rcx = rc->world_pos.x, rcz = rc->world_pos.z;     /* 24.8 */
            int32_t ryaw = rc->euler_accum.yaw;
            int32_t prad = (int32_t)CDEF_S(rc, CDEF_COLLISION_RADIUS);
            int32_t hw_a, fz_a, rz_a;
            actor_collision_box(rc, &hw_a, &fz_a, &rz_a);
            for (int t = base; t < tmax2; t++) {
                TD5_Actor *tr = (TD5_Actor *)(g_actor_table_base + (size_t)t * TD5_ACTOR_STRIDE);
                if (!tr->car_definition_ptr) continue;
                if (td5_ai_actor_is_broken_down(t)) continue;        /* already wrecked */
                if (td5_ai_traffic_dynamic_parked(t)) continue;      /* despawned */
                if (td5_ai_traffic_get_draw_alpha(t) == 0) continue; /* invisible */
                int32_t tcx = tr->world_pos.x, tcz = tr->world_pos.z;
                int32_t tvx = tr->linear_velocity_x, tvz = tr->linear_velocity_z;
                int32_t tyaw = tr->euler_accum.yaw;
                /* Coarse cull: skip far pairs (render-unit centres vs a generous
                 * bound = bounding radii + one tick of both cars' travel). */
                int32_t trad = (int32_t)CDEF_S(tr, CDEF_COLLISION_RADIUS);
                int64_t cdx = (int64_t)(rcx >> 8) - (tcx >> 8);
                int64_t cdz = (int64_t)(rcz >> 8) - (tcz >> 8);
                int64_t coarse = (int64_t)prad + trad
                               + (llabs((int64_t)rvx >> 8) + llabs((int64_t)tvx >> 8))
                               + (llabs((int64_t)rvz >> 8) + llabs((int64_t)tvz >> 8));
                if (cdx * cdx + cdz * cdz > coarse * coarse) continue;
                /* Precise: REAL oriented-box test at NSUB+1 points along BOTH
                 * cars' paths this tick. A hit at ANY sub-step = the bodies
                 * actually touched. */
                int hit = 0;
                for (int k = 0; k <= NSUB && !hit; k++) {
                    int32_t bk = NSUB - k;   /* back-offset weight: k=NSUB -> current pos */
                    int32_t rax = rcx - (int32_t)(((int64_t)rvx * bk) / NSUB);
                    int32_t raz = rcz - (int32_t)(((int64_t)rvz * bk) / NSUB);
                    int32_t tax = tcx - (int32_t)(((int64_t)tvx * bk) / NSUB);
                    int32_t taz = tcz - (int32_t)(((int64_t)tvz * bk) / NSUB);
                    if (obb_corner_test(rc, tr, rax, raz, tax, taz, ryaw, tyaw, corners))
                        hit = 1;
                }
                /* [aligned head-on fallback] The corner-in-box test can miss a
                 * perfectly aligned edge-to-edge contact (no corner penetrates).
                 * Catch it by the closest approach of the two centres over the
                 * tick: if they come within the summed half-WIDTHS they are in the
                 * same lateral line and crossing — a real head-on — without
                 * wrecking a car a full lane over (whose centre stays >1 width). */
                if (!hit) {
                    int32_t hw_b, fz_b, rz_b;
                    actor_collision_box(tr, &hw_b, &fz_b, &rz_b);
                    int64_t p1x = (int64_t)(tcx >> 8) - (rcx >> 8);
                    int64_t p1z = (int64_t)(tcz >> 8) - (rcz >> 8);
                    int64_t relvx = (int64_t)(tvx >> 8) - (rvx >> 8);
                    int64_t relvz = (int64_t)(tvz >> 8) - (rvz >> 8);
                    int64_t p0x = p1x - relvx, p0z = p1z - relvz;
                    int64_t sx = relvx, sz = relvz, cxx, czz;
                    int64_t seg2 = sx * sx + sz * sz;
                    if (seg2 <= 0) { cxx = p0x; czz = p0z; }
                    else {
                        int64_t tn = -(p0x * sx + p0z * sz);
                        if (tn <= 0)         { cxx = p0x; czz = p0z; }
                        else if (tn >= seg2) { cxx = p1x; czz = p1z; }
                        else                 { cxx = p0x + (sx * tn) / seg2; czz = p0z + (sz * tn) / seg2; }
                    }
                    int64_t lat = (int64_t)((hw_a + hw_b) >> 8);
                    if (cxx * cxx + czz * czz <= lat * lat) hit = 1;
                }
                if (!hit) continue;
                td5_arcade_note_ram(rsl, t, cap);                    /* score (before break) */
                td5_ai_mark_actor_broken_down(t);                    /* total it */
                if (tr->linear_velocity_y < lift0) tr->linear_velocity_y = lift0;
                TD5_LOG_I(LOG_TAG, "battle_wreck_sweep: racer=%d traffic=%d -> WRECK (swept OBB)", rsl, t);
            }
        }

        /* [TRAFFIC BATTLE 2026-06-28] Airborne traffic = wrecked. A car launched
         * into the air (vy above the threshold) counts as a kill (user: "trigger
         * wreck when cars get thrown out in the air"). Attribute it to the nearest
         * racer (the one that almost certainly knocked it up). */
        for (int t = base; t < tmax2; t++) {
            TD5_Actor *tr = (TD5_Actor *)(g_actor_table_base + (size_t)t * TD5_ACTOR_STRIDE);
            if (!tr->car_definition_ptr) continue;
            if (td5_ai_actor_is_broken_down(t)) continue;
            if (td5_ai_traffic_dynamic_parked(t)) continue;
            if (td5_ai_traffic_get_draw_alpha(t) == 0) continue;
            int32_t vy = tr->linear_velocity_y; if (vy < 0) vy = -vy;
            if (vy < air_vy) continue;
            int near_r = -1; int64_t nbest = 0;
            for (int rsl = 0; rsl < base; rsl++) {
                TD5_Actor *rc = (TD5_Actor *)(g_actor_table_base + (size_t)rsl * TD5_ACTOR_STRIDE);
                if (!rc->car_definition_ptr || rc->finish_time != 0) continue;
                int64_t dx = (int64_t)(rc->world_pos.x >> 8) - (tr->world_pos.x >> 8);
                int64_t dz = (int64_t)(rc->world_pos.z >> 8) - (tr->world_pos.z >> 8);
                int64_t d2 = dx * dx + dz * dz;
                if (near_r < 0 || d2 < nbest) { near_r = rsl; nbest = d2; }
            }
            if (near_r < 0) continue;
            td5_arcade_note_ram(near_r, t, cap);
            td5_ai_mark_actor_broken_down(t);
            TD5_LOG_I(LOG_TAG, "battle_wreck_air: traffic=%d vy=%d -> WRECK (airborne, credit racer=%d)",
                      t, tr->linear_velocity_y, near_r);
        }
    }

    /* --- Phase 1: Build AABB grid --- */
    /* Reset grid chains */
    memset(s_collision_grid, COLLISION_CHAIN_END, sizeof(s_collision_grid));

    for (int i = 0; i < total; ++i) {
        TD5_Actor *actor = (TD5_Actor *)(g_actor_table_base + (size_t)i * TD5_ACTOR_STRIDE);
        int32_t radius;

        if (!actor->car_definition_ptr ||
            /* [dynamic-traffic] parked cars are intangible — keep their ghost
             * pose out of the broadphase so nobody collides with a hidden car. */
            td5_ai_traffic_dynamic_parked(i)) {
            memset(g_actor_aabb[i], 0, sizeof(g_actor_aabb[i]));
            continue;
        }

        /* Compute AABB from position +/- bounding radius */
        radius = (int32_t)CDEF_S(actor, CDEF_COLLISION_RADIUS);
        g_actor_aabb[i][0] = (actor->world_pos.x >> 8) - radius;  /* xMin */
        g_actor_aabb[i][1] = (actor->world_pos.z >> 8) - radius;  /* zMin */
        g_actor_aabb[i][2] = (actor->world_pos.x >> 8) + radius;  /* xMax */
        g_actor_aabb[i][3] = (actor->world_pos.z >> 8) + radius;  /* zMax */

        /* Insert into bucket[segment >> 2] linked list */
        int32_t seg = actor->track_span_normalized;
        if (seg < 0) seg = 0;
        int bucket = (seg >> 2) & (COLLISION_GRID_SIZE - 1);

        /* Chain: actor's chain byte points to previous head */
        g_actor_aabb[i][4] = s_collision_grid[bucket];
        s_collision_grid[bucket] = (uint8_t)i;

    }

    /* --- Phase 2: Walk adjacent buckets for each actor --- */
    for (int i = 0; i < total; ++i) {
        TD5_Actor *a = (TD5_Actor *)(g_actor_table_base + (size_t)i * TD5_ACTOR_STRIDE);

        if (!a->car_definition_ptr) continue;
        if (td5_ai_traffic_dynamic_parked(i)) continue;  /* [dynamic-traffic] intangible */

        int32_t seg_a = a->track_span_normalized;
        if (seg_a < 0) seg_a = 0;
        int base_bucket = (seg_a >> 2) & (COLLISION_GRID_SIZE - 1);

        /* Walk 3 adjacent buckets: base-1, base, base+1 */
        for (int boff = -1; boff <= 1; boff++) {
            int bucket = (base_bucket + boff) & (COLLISION_GRID_SIZE - 1);
            int chain = s_collision_grid[bucket];
            int walk_count = 0;

            while (chain != COLLISION_CHAIN_END && walk_count < COLLISION_MAX_WALK) {
                int j = chain;
                walk_count++;

                /* Only test pairs where i < j to avoid duplicates */
                if (j <= i) {
                    chain = g_actor_aabb[j][4] & 0xFF;
                    continue;
                }

                TD5_Actor *b = (TD5_Actor *)(g_actor_table_base + (size_t)j * TD5_ACTOR_STRIDE);

                if (!b->car_definition_ptr) {
                    chain = g_actor_aabb[j][4] & 0xFF;
                    continue;
                }

                /* Dispatch rule from 0x00409150 ResolveVehicleContacts
                 * [CONFIRMED]: Full OBB path when BOTH actors have
                 * field_0x379 == 0 (normal) AND field_0x37c < 0x0F.
                 * Otherwise use sphere separation.
                 *
                 * Traffic (slots 6-11) runs the FULL OBB path in the
                 * original — `UpdateTrafficActorMotion`'s cVar3==0 branch
                 * leaves field_0x379 at 0. Gating on slot_index was wrong. */
                int a_scripted = (a->vehicle_mode != 0) ||
                                 (a->wheel_contact_bitmask >= 0x0F);
                int b_scripted = (b->vehicle_mode != 0) ||
                                 (b->wheel_contact_bitmask >= 0x0F);

                if (i == 0 || j == 0) {
                    s_v2v_slot0_pair_count++;

                    /* Inline AABB check so we only log the cases where the
                     * boxes actually overlap — the previous "v2v_obb_enter"
                     * was firing on every dispatch and missing the fact that
                     * collision_detect_full bails at the same AABB pre-check
                     * 588/601 times. The cases below are what we actually
                     * care about: slot 0 pairs whose AABBs touch but OBB
                     * still produces no impulse. */
                    int aabb_sep = (g_actor_aabb[i][2] < g_actor_aabb[j][0] ||
                                    g_actor_aabb[j][2] < g_actor_aabb[i][0] ||
                                    g_actor_aabb[i][3] < g_actor_aabb[j][1] ||
                                    g_actor_aabb[j][3] < g_actor_aabb[i][1]);
                    if (!aabb_sep) {
                        s_v2v_slot0_obb_enter++;
                        TD5_LOG_I(LOG_TAG,
                                  "v2v_aabb_hit: i=%d j=%d slot_i=%d slot_j=%d "
                                  "pos_i=(%d,%d) pos_j=(%d,%d) span_i=%d span_j=%d "
                                  "dispatch=%s a[vm=%d wm=%d scr=%d] b[vm=%d wm=%d scr=%d]",
                                  i, j, a->slot_index, b->slot_index,
                                  a->world_pos.x >> 8, a->world_pos.z >> 8,
                                  b->world_pos.x >> 8, b->world_pos.z >> 8,
                                  a->track_span_normalized, b->track_span_normalized,
                                  (a_scripted || b_scripted) ? "SIMPLE" : "FULL",
                                  a->vehicle_mode, a->wheel_contact_bitmask, a_scripted,
                                  b->vehicle_mode, b->wheel_contact_bitmask, b_scripted);
                    }
                }

                /* [TIME TRIAL] human-vs-human pairs pass THROUGH each other
                 * (ghosts) — skip the V2V impulse entirely. This is the real V2V
                 * dispatch (the broadphase), not resolve_collision_pair.
                 * [PER-VIEWPORT TRAFFIC] also skip pairs that belong to different
                 * viewports' traffic partitions (a player only touches its own
                 * traffic; cross-partition traffic twins never collide). */
                if (tt_pair_passthrough((int)a->slot_index, (int)b->slot_index) ||
                    td5_ai_traffic_pair_blocked((int)a->slot_index, (int)b->slot_index)) {
                    /* no contact */
                } else if (a_scripted || b_scripted) {
                    collision_detect_simple(a, b);
                } else {
                    collision_detect_full(a, b, i, j);
                }

                chain = g_actor_aabb[j][4] & 0xFF;
            }
        }
    }


    /* --- Phase 2.5: anti-tunnel position-only depenetration (S17) ---
     * Port-only robustness pass (gated by [GameOptions] AntiTunnel, default
     * on). The faithful resolution above can leave a fast/deep pair visibly
     * interpenetrated for a frame; this clears any residual OBB overlap by
     * pushing the cars apart, moving ONLY world position so the S08 impact
     * tuning is untouched. It walks every pair O(n^2) (n <= 22) over the LIVE
     * post-resolution positions, so unlike the span-bucket broadphase above it
     * has no bucket-aliasing blind spot for a fast closing pair. */
    if (g_td5.ini.anti_tunnel) {
        static uint32_t s_sep_frames = 0;   /* throttle for the summary log */
        int     moved_any_actor   = 0;
        int     separated_cleanly = 0;
        int32_t max_pre_depth     = 0;       /* deepest overlap seen this frame */
        uint8_t moved[TD5_MAX_TOTAL_ACTORS];
        memset(moved, 0, sizeof(moved));

        for (int round = 0; round < ANTITUNNEL_RELAX_ROUNDS; round++) {
            int moved_this_round = 0;

            for (int i = 0; i < total; i++) {
                TD5_Actor *a = (TD5_Actor *)(g_actor_table_base + (size_t)i * TD5_ACTOR_STRIDE);
                if (!a->car_definition_ptr) continue;

                /* Script-controlled vehicles (crash takeovers, AI script VM)
                 * own their own position -- leave them alone, the same set the
                 * faithful dispatch routes to the sphere (non-OBB) handler. */
                int a_scripted = (a->vehicle_mode != 0) ||
                                 (a->wheel_contact_bitmask >= 0x0F);
                if (a_scripted) continue;

                int32_t rad_a = (int32_t)CDEF_S(a, CDEF_COLLISION_RADIUS);
                int32_t ax = a->world_pos.x >> 8;
                int32_t az = a->world_pos.z >> 8;

                for (int j = i + 1; j < total; j++) {
                    TD5_Actor *b = (TD5_Actor *)(g_actor_table_base + (size_t)j * TD5_ACTOR_STRIDE);
                    if (!b->car_definition_ptr) continue;

                    int b_scripted = (b->vehicle_mode != 0) ||
                                     (b->wheel_contact_bitmask >= 0x0F);
                    if (b_scripted) continue;

                    /* Cheap live AABB reject before the OBB test. */
                    int32_t rad_b = (int32_t)CDEF_S(b, CDEF_COLLISION_RADIUS);
                    int32_t bx = b->world_pos.x >> 8;
                    int32_t bz = b->world_pos.z >> 8;
                    int32_t r  = rad_a + rad_b;
                    int32_t ddx = ax - bx; if (ddx < 0) ddx = -ddx;
                    int32_t ddz = az - bz; if (ddz < 0) ddz = -ddz;
                    if (ddx > r || ddz > r) continue;

                    /* [TIME TRIAL] don't separate two players — they ghost.
                     * [PER-VIEWPORT TRAFFIC] don't separate cross-partition traffic
                     * twins or a non-owner player from another viewport's traffic. */
                    if (tt_pair_passthrough((int)a->slot_index, (int)b->slot_index) ||
                        td5_ai_traffic_pair_blocked((int)a->slot_index, (int)b->slot_index)) continue;

                    int32_t pen = v2v_depenetrate_pair(a, b);
                    if (pen > 0) {
                        moved[i] = moved[j] = 1;
                        moved_this_round = 1;
                        moved_any_actor  = 1;
                        if (round == 0 && pen > max_pre_depth) max_pre_depth = pen;
                        /* a moved -- refresh its cached centre for the rest of
                         * this row so later j see the updated position. */
                        ax = a->world_pos.x >> 8;
                        az = a->world_pos.z >> 8;
                    }
                }
            }

            if (!moved_this_round) { separated_cleanly = 1; break; }
        }

        /* Resync render pose (render_pos + matrices) for any car we moved, the
         * same way apply_collision_response does after its push. Only XZ
         * position changed (angles untouched), but the shared helper is the
         * established idiom and keeps wheel/camera/HUD consumers consistent. */
        if (moved_any_actor) {
            for (int i = 0; i < total; i++) {
                if (!moved[i]) continue;
                TD5_Actor *a = (TD5_Actor *)(g_actor_table_base + (size_t)i * TD5_ACTOR_STRIDE);
                if (a->car_definition_ptr) update_vehicle_pose_from_physics(a);
            }
            s_sep_frames++;
            if (!separated_cleanly) {
                TD5_LOG_W(LOG_TAG, "v2v_antitunnel: pair(s) still overlapping after %d relax rounds (max_pre_depth=%d units, deep pile-up); continuing next frame",
                          ANTITUNNEL_RELAX_ROUNDS, max_pre_depth);
            } else if ((s_sep_frames % 30u) == 1u) {
                TD5_LOG_I(LOG_TAG, "v2v_antitunnel: cleared car overlap (max_pre_depth=%d units) in <=%d rounds; frames_with_sep=%u",
                          max_pre_depth, ANTITUNNEL_RELAX_ROUNDS, s_sep_frames);
            }
        }
    }

    /* [S20 PlayerCollide] Explicit player-vs-traffic collision by world proximity.
     * The span-bucket broadphase above only pairs actors within +/-1 bucket
     * (~4 track-spans). On curves/junctions a traffic car can be physically on
     * top of the player yet several spans apart in track-progress, so the pair
     * is never tested and the player drives THROUGH it (user-reported "traffic
     * doesn't collide"). Supplement here: test slot 0 against every traffic
     * actor whose bucket the broadphase did NOT cover, gated on a real world
     * overlap. resolve_collision_pair runs the same OBB test + impulse as the
     * faithful path, so the response is identical — just no longer skipped. */
    if (g_td5.ini.traffic_player_collide && total > g_traffic_slot_base) {
        int tmax = (total < TD5_MAX_TOTAL_ACTORS) ? total : TD5_MAX_TOTAL_ACTORS;
        /* [PER-VIEWPORT TRAFFIC] When active, run this supplement for EVERY human
         * viewport's player against ITS OWN traffic partition (so each player can
         * hit only its own traffic). Otherwise it stays the original slot-0 path. */
        int per_vp = td5_ai_traffic_per_viewport_active();
        int vc = per_vp ? g_td5.viewport_count : 1;
        int vp;
        if (vc > TD5_MAX_VIEWPORTS) vc = TD5_MAX_VIEWPORTS;
        for (vp = 0; vp < vc; vp++) {
            int pslot = per_vp ? td5_game_get_player_slot(vp) : 0;
            TD5_Actor *player;
            int32_t prad;
            if (pslot < 0 || pslot >= g_traffic_slot_base) continue;
            player = (TD5_Actor *)(g_actor_table_base + (size_t)pslot * TD5_ACTOR_STRIDE);
            if (!player->car_definition_ptr) continue;
            prad = (int32_t)CDEF_S(player, CDEF_COLLISION_RADIUS);
            for (int t = g_traffic_slot_base; t < tmax; ++t) {
                TD5_Actor *tr =
                    (TD5_Actor *)(g_actor_table_base + (size_t)t * TD5_ACTOR_STRIDE);
                int32_t dxp, dzp, rr;
                int64_t d2;
                if (per_vp && td5_ai_traffic_slot_owner_vp(t) != vp) continue; /* own partition only */
                if (!tr->car_definition_ptr) continue;
                /* [2026-06-12] Don't collide with INVISIBLE traffic. Dynamic
                 * traffic despawns by fading draw alpha to 0 (the renderer then
                 * skips the car), but the actor stays parked at its last span
                 * with a live car_definition_ptr -> the player rams an unrendered
                 * car = the "invisible wall on span 110" report (LOW volume cap=2
                 * left 4 despawned cars parked at span ~110). Gate collision on the
                 * SAME visibility the renderer uses, so you only ever hit traffic
                 * you can see. get_draw_alpha() returns 255 when dynamic traffic is
                 * off, so classic-mode collision is unchanged. [2026-06-12b]
                 * Also skip PARKED (inactive) slots outright — belt-and-suspenders
                 * in case a slot is mid-state with a transient non-zero alpha. */
                if (td5_ai_traffic_dynamic_parked(t) ||
                    td5_ai_traffic_get_draw_alpha(t) == 0) continue;
                dxp = (player->world_pos.x >> 8) - (tr->world_pos.x >> 8);
                dzp = (player->world_pos.z >> 8) - (tr->world_pos.z >> 8);
                rr  = prad + (int32_t)CDEF_S(tr, CDEF_COLLISION_RADIUS);
                d2  = (int64_t)dxp * dxp + (int64_t)dzp * dzp;
                if (d2 > (int64_t)rr * rr) continue;   /* not physically overlapping */
                /* Physically overlapping the player. The bucket broadphase
                 * misses player<->traffic pairs (verified 0 hits), so force the
                 * sphere collision response here regardless of track-span bucket
                 * or OBB orientation. Sphere only impulses when CLOSING, so this
                 * is a no-op once separated (won't fight the broadphase). */
                TD5_LOG_I(LOG_TAG,
                          "player_traffic_collide: player_slot=%d traffic_slot=%d dist=%d rr=%d span_p=%d span_t=%d "
                          "(world-overlap -> sphere collision)",
                          pslot, t, (int)td5_isqrt((int32_t)(d2 > 0x7FFFFFFF ? 0x7FFFFFFF : d2)), rr,
                          player->track_span_normalized, tr->track_span_normalized);
                collision_detect_simple(player, tr);
            }
        }
    }

    /* --- Phase 3: Grid reset is handled at start of next call --- */
}

/* Internal: dispatch collision between two actors (grid broadphase wrapper).
 * Same rule as the inline dispatch in td5_physics_resolve_vehicle_contacts
 * (see comment there). */
static void resolve_collision_pair(TD5_Actor *a, TD5_Actor *b, int idx_a, int idx_b)
{
    if (!a || !b) return;
    if (!a->car_definition_ptr || !b->car_definition_ptr) return;

    /* [TIME TRIAL] human-vs-human pairs pass through each other (ghost). */
    if (tt_pair_passthrough((int)a->slot_index, (int)b->slot_index)) return;

    int a_scripted = (a->vehicle_mode != 0) ||
                     (a->wheel_contact_bitmask >= 0x0F);
    int b_scripted = (b->vehicle_mode != 0) ||
                     (b->wheel_contact_bitmask >= 0x0F);

    if (a_scripted || b_scripted) {
        collision_detect_simple(a, b);
    } else {
        collision_detect_full(a, b, idx_a, idx_b);
    }
}


/* ------------------------------------------------------------------ hooks */
/* Race-init invalidation of the per-slot model box + hull (called from the
 * suspension-envelope scan in td5_physics.c before it reseeds them). */
void td5_physics_hitbox_invalidate(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return;
    s_mesh_box_valid[slot] = 0;
    s_hull_valid[slot] = 0;
}

/* Hull point count for a slot (0 when no valid silhouette) - envelope log. */
int td5_physics_hull_points(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return 0;
    return s_hull_valid[slot] ? (int)s_hull_n[slot] : 0;
}
