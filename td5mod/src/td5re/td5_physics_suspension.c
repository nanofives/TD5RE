/* ========================================================================
 * td5_physics_suspension.c -- Suspension, wheel contacts, pose integration
 *
 * Split out of td5_physics.c (P1-B step 4, 2026-07-02). Byte-faithful ports:
 *   - ApplyDampedSuspensionForce (0x4437C0, traffic)
 *   - IntegrateWheelSuspensionTravel (0x00403A20)
 *   - UpdateVehicleSuspensionResponse (0x4057F0)
 *   - Traffic edge containment + traffic pose integration (0x443CF0)
 *   - IntegrateVehiclePoseAndContacts (0x405E80)
 *   - UpdateVehiclePoseFromPhysicsState (0x4063A0)
 *   - TransformShortVec3ByRenderMatrixRounded (0x0042EB10)
 *   - RefreshVehicleWheelContactFrames (0x403720)
 *   - ClampVehicleAttitudeLimits (0x405B40)
 * Cross-TU seam: td5_physics_internal.h (PRIVATE).
 * ======================================================================== */

#include "td5_physics.h"
#include "td5_ai.h"
#include "td5_track.h"
#include "td5_render.h"   /* td5_render_get_vehicle_mesh */
#include "td5_sound.h"    /* td5_sound_play_at_position (Tier 2 recovery SFX) */
#include "td5_input.h"    /* td5_input_ff_collision (wall/prop impact FF) */
#include "td5_vfx.h"      /* td5_vfx_queue_prop_break (TD6 prop debris) */
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

/* ======== [split] per-slot wheel/pose state (moved verbatim from td5_physics.c) ======== */
/* [S18] Per-slot consecutive-ticks-over-attitude-limit counter, used ONLY on
 * migrated TD6 tracks to debounce the MODE-0 recovery latch (see
 * td5_physics_clamp_attitude). Faithful TD5 tracks never touch this. */
#define S18_TD6_RECOVERY_DEBOUNCE_TICKS 8   /* ~0.27s at 30Hz before recovery latches on TD6 */
static uint8_t s_td6_recovery_debounce[TD5_MAX_TOTAL_ACTORS];

/* Per-slot previous-frame wheel transform results (pre-snap) for gap_270 delta.
 * Using post-snap positions causes huge Y deltas because snap Y != transform Y. */
static int32_t s_prev_wheel_tx[TD5_MAX_TOTAL_ACTORS][4];  /* [slot][wheel] X transform result */
static int32_t s_prev_wheel_ty[TD5_MAX_TOTAL_ACTORS][4];  /* [slot][wheel] Y transform result */
static int32_t s_prev_wheel_tz[TD5_MAX_TOTAL_ACTORS][4];  /* [slot][wheel] Z transform result */
static uint8_t s_prev_wheel_valid[TD5_MAX_TOTAL_ACTORS];  /* per-slot: 1 if previous transform is valid */

/* Per-slot rotated wheel-Y world offset, captured INSIDE
 * td5_physics_refresh_wheel_contacts at the same moment wheel_contact_pos[i].y
 * is computed (pre-snap). Consumed by the chassis ground-snap tail in
 * update_vehicle_pose_from_physics to mirror the original's
 *   local_10 = sum(wheel_probe_y - rotated_wheel_y_offset) / count
 * form @ 0x004063A0 — replacing a port-invented re-probe loop that called
 * td5_track_probe_height(x, z, actor->track_span_raw, ...) for every
 * grounded wheel. The re-probe used the chassis span for all wheels and
 * a heuristic lane picker; with the per-wheel walker fixed (single-step,
 * 2026-05-01) the wheel_contact_pos values are already correct, so the
 * faithful sum form gives a stable chassis Y on slope onsets. Replacing
 * the re-probe alone (without the walker fix) made the launch worse
 * because the walker was over-advancing wheels and feeding bad ground_y. */
static int32_t s_wheel_offset_y_world[16][4];

/* Per-slot previous-frame grounded bitmask (1=grounded): snapshotted pre-refresh
 * in the suspension response, read by the ram check + landing detection here. */
static uint8_t s_prev_grounded_mask[16];

/* ======== [split] suspension .. attitude clamp (moved verbatim from td5_physics.c) ======== */
/* ========================================================================
 * ApplyDampedSuspensionForce (0x4437C0) -- Traffic only
 *
 * Simple 2-DOF spring-damper for roll and pitch.
 *
 * Byte-faithful port (pool8 precise-port pilot 2026-05-14).
 *
 * Original (per axis):
 *   new_pos = old_pos + old_vel                          [stored before damping]
 *   new_vel = old_vel
 *           + sar8_rz(-old_vel * 32)                     ; velocity damping (OLD vel)
 *           - sar8_rz( new_pos * 32)                     ; spring restore   (NEW pos)
 *           + sar8_rz( drive    * 128)                   ; external force
 *   if new_pos > +CLAMP: new_pos = +CLAMP, new_vel = 0
 *   if new_pos < -CLAMP: new_pos = -CLAMP, new_vel = 0
 *
 * `sar8_rz(x) = ((x < 0) ? (x + 0xFF) : x) >> 8`  — matches CDQ;AND EDX,0xFF;ADD;SAR 8.
 * ======================================================================== */

static inline int32_t sar8_rz(int32_t x) {
    /* Encodes original's CDQ + AND EDX,0xFF + ADD EAX,EDX + SAR EAX,8
     * (round-to-zero signed divide by 256). */
    return (x + (((int32_t)((uint32_t)x >> 31)) * 0xFF)) >> 8;
}

void apply_damped_suspension_force(TD5_Actor *actor, int32_t lateral, int32_t longitudinal)
{

    /* === Axis 0 (lateral-driven): pos @ +0x2DC, vel @ +0x2EC, clamp ±0x2000 ===
     * [CONFIRMED @ 0x004437C4-0x00443859] */
    {
        int32_t old_pos = actor->wheel_suspension_pos[0];
        int32_t old_vel = actor->wheel_spring_dv[0];

        /* new_pos = old_pos + old_vel  [0x004437D3-0x004437D5] */
        int32_t new_pos = old_pos + old_vel;
        actor->wheel_suspension_pos[0] = new_pos;

        /* new_vel = old_vel + sar8_rz(-old_vel*32) - sar8_rz(new_pos*32) + sar8_rz(drive*128)
         *           [0x004437DB-0x00443821] */
        int32_t new_vel = old_vel
                        + sar8_rz(-old_vel * 32)
                        - sar8_rz( new_pos * 32)
                        + sar8_rz( lateral * 128);
        actor->wheel_spring_dv[0] = new_vel;

        /* Two separate if-clamps (matches listing 0x00443827-0x00443859 exactly).
         * Algebraically equivalent to if/else-if. */
        if (new_pos > 0x2000) {
            actor->wheel_suspension_pos[0] = 0x2000;
            actor->wheel_spring_dv[0] = 0;
        }
        if (actor->wheel_suspension_pos[0] < -0x2000) {
            actor->wheel_suspension_pos[0] = -0x2000;
            actor->wheel_spring_dv[0] = 0;
        }
    }

    /* === Axis 1 (longitudinal-driven): pos @ +0x2E0, vel @ +0x2F0, clamp ±0x4000 ===
     * [CONFIRMED @ 0x00443859-0x004438EA] */
    {
        int32_t old_pos = actor->wheel_suspension_pos[1];
        int32_t old_vel = actor->wheel_spring_dv[1];

        int32_t new_pos = old_pos + old_vel;
        actor->wheel_suspension_pos[1] = new_pos;

        int32_t new_vel = old_vel
                        + sar8_rz(-old_vel * 32)
                        - sar8_rz( new_pos * 32)
                        + sar8_rz( longitudinal * 128);
        actor->wheel_spring_dv[1] = new_vel;

        if (new_pos > 0x4000) {
            actor->wheel_suspension_pos[1] = 0x4000;
            actor->wheel_spring_dv[1] = 0;
        }
        if (actor->wheel_suspension_pos[1] < -0x4000) {
            actor->wheel_suspension_pos[1] = -0x4000;
            actor->wheel_spring_dv[1] = 0;
        }
    }

    /* Original does NOT feed suspension into angular_velocity.
     * [CONFIRMED @ 0x4437C0-0x4438EC: writes only to +0x2DC/+0x2EC/+0x2E0/+0x2F0,
     *  never to angular_velocity_roll/pitch.]
     * Roll/pitch display angles are computed from surface normal + suspension
     * correction in UpdateTrafficVehiclePose, not from euler accumulators. */

}

/* ========================================================================
 * Suspension: IntegrateWheelSuspensionTravel (0x00403A20)
 *
 * Per-wheel spring-damper. Projects the world-frame net acceleration
 * applied this frame (accel_x, accel_z — the fx/fz written into
 * linear_velocity just before this call) onto the lever arm from the
 * chassis centre to each wheel's high-res world contact position. That
 * scalar projection is the external excitation for the spring.
 *
 *   arm.x = wheel_world_positions_hires[i].x - (world_pos.x >> 8)
 *   arm.z = wheel_world_positions_hires[i].z - (world_pos.z >> 8)
 *   input = arm.x * accel_x + arm.z * accel_z        (world-frame lever torque)
 *   spring_term = ((input >> 8) * cardef+0x62) >> 8
 *   load_term   = (wheel_load_accum[i] * cardef+0x66) >> 8
 *   new_vel = wheel_spring_dv[i] + spring_term + load_term
 *   new_vel -= (wheel_suspension_pos[i] * cardef+0x5E) >> 8   // pos-damping (restoring)
 *   new_vel -= (new_vel                * cardef+0x60) >> 8    // vel-damping
 *   if |new_vel| < 0x10: new_vel = 0                          // deadzone
 *   wheel_spring_dv[i] = new_vel
 *   wheel_suspension_pos[i] += new_vel
 *   clamp to +/- cardef+0x64   (zeros vel on hit)
 *
 * The post-loop central block repeats the same formula once with the
 * front-axle contact midpoint for center_suspension_pos/vel
 * (+0x2CC/+0x2D0), using 2x the per-wheel clamp and NO load-accum term
 * and NO deadzone.
 *
 * NOTE ON FIELD NAMES (from re/include/td5_actor_struct.h — confirmed):
 *   +0x2DC wheel_suspension_pos[4]  -- spring position (state)
 *   +0x2EC wheel_spring_dv[4]       -- spring velocity (state)  (misnomer)
 *   +0x2FC wheel_load_accum[4]      -- external force feed      (misnomer)
 *
 * The pre-2026-04-13 port of this function read/wrote the last two
 * swapped AND ignored the world-frame acceleration input entirely, so
 * it produced no dynamic bounce response. Fixed here.
 * ======================================================================== */

/* pool5 / 0x00403A20 pilot trace hooks — header included separately to keep
 * the function signature unchanged. */

/* Round-to-zero divide by 2 for signed int32. Mirrors the central-pass
 * `CDQ; SUB EAX,EDX; SAR EAX,1` idiom at 0x00403B78..B95.
 *   CDQ            ; EDX = sign(EAX) = -1 if neg, 0 if non-neg
 *   SUB EAX, EDX   ; subtract -1 (=add 1) if negative, no-op if non-neg
 *   SAR EAX, 1
 */
static inline int32_t sar1_rz(int32_t x)
{
    int32_t bias = (x < 0) ? -1 : 0;
    return (x - bias) >> 1;     /* (x - (-1)) = x + 1 when negative */
}

void td5_physics_integrate_suspension(TD5_Actor *actor, int32_t accel_x, int32_t accel_z)
{
    int16_t *phys = get_phys(actor);
    if (!phys) return;


    /* cardef constants -- see function header.
     * Names chosen to match the original's semantic use. */
    const int32_t k_pos_damp   = (int32_t)PHYS_S(actor, PHYS_SUSP_POS_DAMP);  /* position-proportional damping (restoring) */
    const int32_t k_vel_damp   = (int32_t)PHYS_S(actor, PHYS_SUSP_VEL_DAMP);  /* velocity-proportional damping */
    const int32_t k_spring     = (int32_t)PHYS_S(actor, PHYS_SUSP_SPRING);  /* spring coefficient (multiplies lever proj) */
    const int32_t k_travel_lim = (int32_t)PHYS_S(actor, PHYS_SUSP_TRAVEL_LIM);  /* per-wheel +/- travel clamp */
    const int32_t k_load_scale = (int32_t)PHYS_S(actor, PHYS_SUSP_LOAD_SCALE);  /* multiplier on wheel_load_accum */

    /* world_pos >> 8 with the original's round-to-zero bias (0x00403A7D-A85
     * and the symmetric one at 0x00403A9C-AA0). Plain SAR rounds toward -∞
     * for negatives. */
    const int32_t wpx_scaled = sar8_rz(actor->world_pos.x);
    const int32_t wpz_scaled = sar8_rz(actor->world_pos.z);

    /* [SUSPCONST 2026-05-27] Force-cascade root-cause: capture the spring
     * constants, lever arm, and accel so the suspension equilibrium can be
     * compared to the orig (0x00403A20). The port suspension saturates while
     * the orig stays ~500; fx is only 2x, so the extra factor is in the
     * spring response (constants or arm). Slot 0, every 60 ticks. */
    if (actor->slot_index == 0 && (actor->frame_counter % 60u) == 0u) {
        int32_t arm0x = actor->wheel_world_positions_hires[0].x - wpx_scaled;
        int32_t arm0z = actor->wheel_world_positions_hires[0].z - wpz_scaled;
        TD5_LOG_I(LOG_TAG,
            "SUSPCONST k_spring=%d k_pos_damp=%d k_vel_damp=%d k_travel=%d "
            "k_load=%d | arm0=(%d,%d) accel=(%d,%d) hires0=(%d,%d) wp8=(%d,%d)",
            k_spring, k_pos_damp, k_vel_damp, k_travel_lim, k_load_scale,
            arm0x, arm0z, accel_x, accel_z,
            actor->wheel_world_positions_hires[0].x,
            actor->wheel_world_positions_hires[0].z,
            wpx_scaled, wpz_scaled);
        TD5_LOG_I(LOG_TAG,
            "TUNING LUT[8..11]=%d,%d,%d,%d torque_mult=%d gear_ratio[2]=%d "
            "redline=%d | throttle(+0x33E)=%d rpm=%d gear=%d",
            (int)PHYS_S(actor, 16), (int)PHYS_S(actor, 18),
            (int)PHYS_S(actor, 20), (int)PHYS_S(actor, 22),
            (int)PHYS_S(actor, PHYS_DRIVE_TORQUE_MULT), (int)PHYS_S(actor, 0x32),
            (int)PHYS_S(actor, PHYS_REDLINE_RPM),
            (int)actor->encounter_steering_cmd,
            actor->engine_speed_accum, (int)actor->current_gear);
    }

    /* ---- Per-wheel pass (4 wheels) ---- */
    for (int i = 0; i < 4; i++) {
        /* Lever arm from chassis centre to this wheel's contact position.
         * Original (0x00403A7D-A88):
         *   arm = hires - sar8_rz(world_pos)
         * The original's hires is stored in raw world units (the 0x00403720
         * write path does NOT apply the `<<8` that wheel_contact_pos gets —
         * see decomp of 0x00403720: the shifts only touch piVar1=+0xF0).
         *
         * Port storage divergence (UPSTREAM, owned by pool1/0x00403720):
         * the port stores wheel_world_positions_hires in 24.8 FP (the write
         * at td5_physics.c:5580 applies `hub_x << 8`). vfx consumes that
         * 24.8 FP scale at td5_vfx.c:1367.
         *
         * To make the integrator byte-exact under the port's CURRENT hires
         * convention, we shift the port's 24.8 FP hires down to world units
         * with `sar8_rz`. If pool1 later changes hires storage to raw (matching
         * the original's 0x00403720 path), this `sar8_rz` becomes a no-op and
         * must be removed.
         *
         * Why sar8_rz and not plain `>>8`: the port writes hires as
         * `int32_t hub_x = lrintf(...); hires.x = hub_x << 8`. For positive
         * hub_x, `hires.x >> 8 == hub_x` exactly. For negative hub_x not
         * divisible by 256... actually `hub_x << 8` produces a value with the
         * low 8 bits zero, so `hires.x >> 8 == hub_x` for both signs. So plain
         * `>>8` would also recover hub_x exactly. BUT to match the original's
         * arithmetic semantics on whatever values the port writes (in case
         * the port's hires write later changes), use the round-to-zero shift
         * which matches the original's `(x + sign_bias) >> 8` convention. */
        /* Hires now stored in raw world units (the <<8 was removed in
         * 6595-6597, matching original 0x00403720 semantics). sar8_rz()
         * is no longer needed here -- consume the value directly. */
        const int32_t arm_x = actor->wheel_world_positions_hires[i].x - wpx_scaled;
        const int32_t arm_z = actor->wheel_world_positions_hires[i].z - wpz_scaled;

        /* proj = arm_x * accel_x + arm_z * accel_z (computed as
         * arm_z*accel_z then ADD arm_x*accel_x in the listing — order
         * doesn't matter for signed 32-bit add). */
        int32_t proj = arm_x * accel_x + arm_z * accel_z;

        /* spring_term = sar8_rz(proj) * k_spring
         * [0x00403AAC-BA: MOV EAX,ECX; CDQ; AND EDX,0xFF; ADD EAX,EDX;
         *                 SAR EAX,8; IMUL EAX, k_spring]
         * Note: the result is held as the multiplied (not yet shifted)
         * value; the next SAR happens later. */
        const int32_t spring_term_x256 = sar8_rz(proj) * k_spring;

        /* load_term = wheel_load_accum[i] * k_load_scale
         * [0x00403ACA-CD: MOV EAX,[ESI+0x20]; IMUL EAX, k_load_scale]
         * No pre-shift; the SAR comes after. */
        const int32_t load_term_x256 = actor->wheel_load_accum[i] * k_load_scale;

        /* new_vel = sar8_rz(load_term) + sar8_rz(spring_term) + wheel_spring_dv[i]
         * [0x00403AD2-E3: CDQ; AND EDX,0xFF; ADD; SAR EAX,8 (for load_term)
         *                 SAR ECX,8 (for spring_term — bias was added at 0x00403ABF)
         *                 ADD ECX,EAX; ADD ECX,[ESI+0x10]]
         *
         * Critical: the order of the SAR-8s in the listing applies the bias
         * to spring_term BEFORE the load_term computation overwrites the bias
         * register. To be byte-faithful, we sar8_rz each operand separately.
         * Algebraically equivalent because round-to-zero is per-operand. */
        int32_t new_vel = sar8_rz(load_term_x256)
                        + sar8_rz(spring_term_x256)
                        + actor->wheel_spring_dv[i];

        /* pos_damp = sar8_rz(wheel_suspension_pos[i] * k_pos_damp)
         * [0x00403AE6-F6: MOV EAX,[ESI]; IMUL EAX, k_pos_damp;
         *                 CDQ; AND EDX,0xFF; ADD EAX,EDX; (held in EBP)
         *                 ...; SAR EBP,8]
         *
         * vel_damp = sar8_rz(new_vel * k_vel_damp)
         * [0x00403AF8-08: MOV EAX,ECX; IMUL EAX, k_vel_damp;
         *                 CDQ; AND EDX,0xFF; ADD EAX,EDX; SAR EAX,8]
         *
         * new_vel -= vel_damp + pos_damp     [0x00403B0E-12: NEG EAX; SUB
         *                                     EAX,EBP; ADD ECX,EAX]
         */
        const int32_t pos_damp_x256 = actor->wheel_suspension_pos[i] * k_pos_damp;
        const int32_t vel_damp_x256 = new_vel * k_vel_damp;
        new_vel = new_vel - sar8_rz(pos_damp_x256) - sar8_rz(vel_damp_x256);

        /* Deadzone: |new_vel| < 0x10 → 0
         * [0x00403B14-1E: CMP -0x10; JLE; CMP 0x10; JGE; XOR ECX,ECX]
         * Strict inequality: -0x10 < new_vel && new_vel < 0x10. Both
         * jumps are signed-LE/GE; CMP -0x10 + JLE jumps when new_vel <= -0x10
         * (i.e. stays non-zero), CMP 0x10 + JGE jumps when new_vel >= 0x10. */
        if (new_vel > -0x10 && new_vel < 0x10)
            new_vel = 0;

        /* [SPRINGTRACE 2026-05-27] Per-tick spring dynamics for slot 0, wheels
         * 0(FL)/2(RL), every 30 ticks. Shows WHY pos runs to ±12288 when the
         * measured-accel equilibrium is only ~1044: logs proj, spring/load/damp
         * terms, old pos+dv, and new_vel. pos_in/dv_in are still pre-write. */
        if (actor->slot_index == 0 && i == 0 && actor->frame_counter < 80u) {
            TD5_LOG_I(LOG_TAG,
                "SPRINGTRACE w%d: proj=%d spT=%d(s%d) loadAcc=%d loadT=%d(s%d) "
                "dvIn=%d posIn=%d posdampT=%d(s%d) veldampT=%d(s%d) new_vel=%d",
                i, proj, spring_term_x256, sar8_rz(spring_term_x256),
                actor->wheel_load_accum[i], load_term_x256, sar8_rz(load_term_x256),
                actor->wheel_spring_dv[i], actor->wheel_suspension_pos[i],
                pos_damp_x256, sar8_rz(pos_damp_x256),
                vel_damp_x256, sar8_rz(vel_damp_x256), new_vel);
            TD5_LOG_I(LOG_TAG,
                "SPRINGARM w%d: arm=(%d,%d) accel=(%d,%d) hires=(%d,%d) wp8=(%d,%d)",
                i, arm_x, arm_z, accel_x, accel_z,
                actor->wheel_world_positions_hires[i].x,
                actor->wheel_world_positions_hires[i].z,
                wpx_scaled, wpz_scaled);
        }

        /* Write velocity then position; clamp at +k_travel_lim then -k_travel_lim.
         * [0x00403B24-54] */
        actor->wheel_spring_dv[i] = new_vel;
        int32_t new_pos = actor->wheel_suspension_pos[i] + new_vel;
        actor->wheel_suspension_pos[i] = new_pos;

        if (new_pos > k_travel_lim) {
            actor->wheel_suspension_pos[i] = k_travel_lim;
            actor->wheel_spring_dv[i] = 0;
        }
        /* Note: the original uses `if new_pos < -k_travel_lim` (not else-if),
         * but in any single tick the new_pos can't be both > k_travel_lim
         * and < -k_travel_lim, so else-if and a separate-if are equivalent.
         * Listing uses a separate IF (0x00403B3C-B53 after the upper-clamp
         * block at 0x00403B33-3B). */
        if (actor->wheel_suspension_pos[i] < -k_travel_lim) {
            actor->wheel_suspension_pos[i] = -k_travel_lim;
            actor->wheel_spring_dv[i] = 0;
        }
    }

    /* ---- Central chassis pass ----
     * The original averages the front-axle contact (wheels 0+1) x and z
     * to derive the lever arm for the body-level suspension. No load
     * term, no deadzone; same spring/damping constants; travel clamp is
     * the SAME cardef+0x64 as per-wheel (NOT doubled).
     *
     * Listing: 0x00403B6A-C70.
     * - (FL.z + FR.z) summed, then `CDQ; SUB EAX,EDX; SAR EAX,1` (round-to-zero /2)
     * - Subtract sar8_rz(world_pos.z)
     * - Multiply by accel_z
     * - Same for x axis
     * - Sum into proj, sar8_rz(proj) * k_spring → spring_term
     * - new_vel = sar8_rz(spring_term) + center_suspension_vel
     * - pos_damp = sar8_rz(center_suspension_pos * k_pos_damp)
     * - vel_damp = sar8_rz(new_vel * k_vel_damp)
     * - new_vel -= vel_damp + pos_damp
     * - new_pos = center_suspension_pos + new_vel
     * - Clamp +k_travel_lim then -k_travel_lim
     * - NO deadzone, NO load term.
     */
    {
        /* Original (0x00403B6A-99): front-midpoint = sar1_rz(FL + FR), then
         * arm = front_mid - sar8_rz(world_pos). Hires is treated as already-
         * in-world-units in the original. The port's hires is 24.8 FP, so we
         * apply sar8_rz to bring it to world units; the sum-then-/2 commutes
         * with the /256 scaling (both are linear), but to preserve byte-exact
         * matching to the original's instruction order we compute /2 on the
         * 24.8 FP sum first, then /256 of that.
         *
         * For port hires written as `(int32_t lrintf_result) << 8`, the low 8
         * bits are zero, so `(FL.x + FR.x) % 256 == 0` and `sar1_rz((FL.x +
         * FR.x))` has low 8 bits still zero (since sum/2 of two LSB-8-zero
         * values has low 7 bits zero). Then `>>8` is exact regardless of
         * sign. We still use the round-to-zero idiom to mirror the original. */
        /* Hires now stored in raw world units (see 6595-6597). The original's
         * mid = (FL + FR)/2 then arm = mid - (world_pos >> 8) -- both operands
         * already in world units; no sar8_rz pass needed. */
        const int32_t fl_x = actor->wheel_world_positions_hires[0].x;
        const int32_t fr_x = actor->wheel_world_positions_hires[1].x;
        const int32_t fl_z = actor->wheel_world_positions_hires[0].z;
        const int32_t fr_z = actor->wheel_world_positions_hires[1].z;

        const int32_t front_mid_x = sar1_rz(fl_x + fr_x);
        const int32_t front_mid_z = sar1_rz(fl_z + fr_z);

        const int32_t arm_x = front_mid_x - wpx_scaled;
        const int32_t arm_z = front_mid_z - wpz_scaled;

        const int32_t proj = arm_x * accel_x + arm_z * accel_z;
        const int32_t spring_term_x256 = sar8_rz(proj) * k_spring;

        int32_t new_vel = sar8_rz(spring_term_x256) + actor->center_suspension_vel;

        const int32_t pos_damp_x256 = actor->center_suspension_pos * k_pos_damp;
        const int32_t vel_damp_x256 = new_vel * k_vel_damp;
        new_vel = new_vel - sar8_rz(pos_damp_x256) - sar8_rz(vel_damp_x256);

        int32_t new_pos = actor->center_suspension_pos + new_vel;
        actor->center_suspension_pos = new_pos;
        actor->center_suspension_vel = new_vel;

        if (new_pos > k_travel_lim) {
            actor->center_suspension_pos = k_travel_lim;
            actor->center_suspension_vel = 0;
        }
        if (actor->center_suspension_pos < -k_travel_lim) {
            actor->center_suspension_pos = -k_travel_lim;
            actor->center_suspension_vel = 0;
        }
    }

}

/* ========================================================================
 * UpdateVehicleSuspensionResponse (0x4057F0)
 *
 * Aggregates per-wheel contact loads into chassis angular accelerations.
 *
 * Original decompilation (confirmed):
 *   bVar1 = damage_lockout (0x37C) = airborne/new bitmask
 *   bVar2 = wheel_contact_bitmask (0x37D) = previous-frame bitmask
 *   If bVar1 == 0xF (all airborne), skip entirely.
 *
 *   Per-wheel loop: for each wheel i NOT in bVar1 (grounded):
 *     sVar3 = wheel_display_angles[i][0] = body-space X arm (roll/pitch)
 *     sVar4 = wheel_display_angles[i][2] = body-space Z arm (roll)
 *     Rotate wheel_contact_velocities[i][0..2] by transposed rotation matrix.
 *     local_36 = Y-component of rotated velocity
 *     iVar8 = (local_36 * gravity + round) >> 12
 *     local_50 += iVar8 * sVar4   (roll accumulator)
 *     local_5c -= iVar8 * sVar3   (pitch accumulator)
 *     local_4c++                   (grounded wheel count)
 *
 *     If wheel was AIRBORNE previous frame (bVar2 & (1<<i), i.e. just
 *     landed this tick — bVar2 = [ESI+0x37D] = prev airborne mask):
 *       spring_dot   = gap_270[i][0]*wcv[0] + gap_270[i][1]*wcv[1] + gap_270[i][2]*wcv[2]
 *       spring_force = arith_round_shift(spring_dot, 0xFFF, 12)
 *       local_64    += spring_force * sVar4 * -0x100  (roll spring)
 *       local_60    += spring_force * sVar3 *  0x100  (pitch spring)
 *       bounce_accum+= spring_force / 2
 *       local_58++   (spring-grounded count)
 *
 *   angular_velocity_roll  += (local_64 + local_50 / local_4c) / 0x4B0
 *   angular_velocity_pitch += (local_60 + local_5c / local_4c) / 0x226
 *   linear_velocity_y      += iVar8 + gravity
 *
 *   Clamp roll/pitch to ±4000 per bitmask switch table.
 * ======================================================================== */

/* Y-component projection of a body-space vector into world space.
 * Uses ROW 1 of the body→world rotation matrix: {m[3], m[4], m[5]}.
 *
 * [CONFIRMED @ 0x0042E2E0] ConvertFloatVec3ToShortAngles Y output:
 *   Y = v[0]*M[+0xC] + v[1]*M[+0x10] + v[2]*M[+0x14]
 *     = v[0]*M[3]    + v[1]*M[4]     + v[2]*M[5]   (row-major float[9])
 *
 * Callers that pass `actor->rotation_matrix` directly (via
 * LoadRenderRotationMatrix without prior transpose) get body→world.
 * In port: td5_physics_integrate_pose ground-snap tail (line ~3618) —
 * src = per-wheel body offset, producing world-space Y for chassis
 * correction averaging. */
/* Full body→world short rotation matching ConvertFloatVec3ToShortAngles @
 * 0x0042E2E0 (row-major float[9] times short[3], FPU TRUNCATE rounding via
 * __ftol). Writes 3 shorts; saturates each component to int16.
 *
 * Used by the chassis-snap loop in IntegrateVehiclePoseAndContacts to
 * populate wheel_contact_normals at +0x230+i*8 (per-wheel rotated body
 * offset for the suspension-response consumer). The Y component matches
 * `rotate_body_to_world_y` above. */
static void rotate_body_to_world_vec3(const TD5_Actor *actor,
                                      const int16_t v[3],
                                      int16_t out[3])
{
    const float *m = actor->rotation_matrix.m;
    float rx = (float)v[0] * m[0] + (float)v[1] * m[1] + (float)v[2] * m[2];
    float ry = (float)v[0] * m[3] + (float)v[1] * m[4] + (float)v[2] * m[5];
    float rz = (float)v[0] * m[6] + (float)v[1] * m[7] + (float)v[2] * m[8];
    if (rx >  32767.0f) rx =  32767.0f;
    if (rx < -32768.0f) rx = -32768.0f;
    if (ry >  32767.0f) ry =  32767.0f;
    if (ry < -32768.0f) ry = -32768.0f;
    if (rz >  32767.0f) rz =  32767.0f;
    if (rz < -32768.0f) rz = -32768.0f;
    out[0] = (int16_t)(int32_t)rx;
    out[1] = (int16_t)(int32_t)ry;
    out[2] = (int16_t)(int32_t)rz;
}

static int16_t rotate_body_to_world_y(const TD5_Actor *actor, const int16_t v[3])
{
    float m3 = actor->rotation_matrix.m[3];
    float m4 = actor->rotation_matrix.m[4];
    float m5 = actor->rotation_matrix.m[5];
    float result = (float)v[0] * m3 + (float)v[1] * m4 + (float)v[2] * m5;
    if (result >  32767.0f) return  32767;
    if (result < -32768.0f) return -32768;
    /* Original ConvertFloatVec3ToShortAngles @ 0x0042E2E0 calls __ftol @
     * 0x0044817C which EXPLICITLY sets the FPU rounding mode to TRUNCATE
     * (`OR AH, 0xC` = RC=11 = chop) before FISTP. So the orig truncates
     * toward zero, not round-to-nearest-even. Earlier port comment claimed
     * "FISTP rounds-to-nearest-even by default" — that's only true when
     * RC=00 in the control word, but __ftol forces RC=11. C cast
     * `(int32_t)float` already truncates toward zero, matching orig.
     *
     * Sum/4 of per-wheel rotated body-Y, scaled by -0x100, accumulates
     * the ~±1 LSB rounding drift into the chassis world_y. The previous
     * lrintf path produced a +128 FP world_y offset at spawn (port=58112
     * vs orig=57984), which propagated to wheel_y at sim_tick=2 refresh
     * → rear wheel airborne detection → chassis launch upward → Honolulu
     * rollover root cause. [round 28: 2026-05-03] */
    return (int16_t)(int32_t)result;
}

/* Y-component projection of a world-space vector into body space.
 * Uses COLUMN 1 of body→world matrix = ROW 1 of body^T = {m[1], m[4], m[7]}.
 *
 * [CONFIRMED @ 0x004057FA] UpdateVehicleSuspensionResponse calls
 * `TransposeMatrix3x3(&actor->rotation_m00, local_30)` then
 * `LoadRenderRotationMatrix(local_30)`, so the subsequent
 * `ConvertFloatVec3ToShortAngles(wcv, ...)` reads ROW 1 of the TRANSPOSE =
 * COLUMN 1 of the original matrix.
 *
 * Semantically this is world→body: wcv is a world-space surface normal,
 * the output Y is the body-frame Y component of that normal — used to
 * compute the gravity projection onto body Y for the per-wheel torque
 * accumulator.
 *
 * Caller: td5_physics_update_suspension_response (line ~3022) with
 * src = actor->wheel_contact_velocities[i] (wcv, world-space normal).
 *
 * HISTORY: originally this helper WAS col-1 (correct for susp_response
 * via transpose semantics). A 2026-04-22 Ghidra re-pass against
 * ConvertFloatVec3ToShortAngles mistakenly flagged col-1 as wrong and
 * switched it to row-1 — that fixed the ground-snap callsite (which had
 * been silently broken) but broke this one. Now split: each call site
 * reads the matrix the way the original's transpose-or-not pairing
 * implies. */
static int16_t rotate_world_to_body_y(const TD5_Actor *actor, const int16_t v[3])
{
    float m1 = actor->rotation_matrix.m[1];
    float m4 = actor->rotation_matrix.m[4];
    float m7 = actor->rotation_matrix.m[7];
    float result = (float)v[0] * m1 + (float)v[1] * m4 + (float)v[2] * m7;
    if (result >  32767.0f) return  32767;
    if (result < -32768.0f) return -32768;
    return (int16_t)(int32_t)result;
}

/* Arithmetic rounding helper: (x + (x>>31 & mask)) >> shift
 * Matches original binary's signed-divide rounding idiom. */
static inline int32_t arith_round_shift(int32_t x, int32_t mask, int32_t shift)
{
    return (x + (int32_t)((uint32_t)(x >> 31) & (uint32_t)mask)) >> shift;
}


/* [CONFIRMED @ 0x004057F0] L5 promotion sweep audit (2026-05-18).
 *
 * UpdateVehicleSuspensionResponse — 783 bytes / 241 instructions / 120
 * decompiled lines. Body audited line-for-line vs disassembly:
 *
 *   - bVar1=wheel_contact_bitmask / bVar2=damage_lockout polarity:
 *     CONFIRMED at 0x004058E4 (spring-dot gate) + 0x00405A6A (pattern
 *     clamp) — port matches AIRBORNE-mask polarity end-to-end.
 *   - loop 4-wheel structure (psVar11 stride +0x254 / 4 shorts/wheel):
 *     CONFIRMED at 0x00405884/0x00405888.
 *   - g_view rotation_world_to_body_y reads row 1 of body^T:
 *     CONFIRMED at 0x004057FA-0x004058D8 (TransposeMatrix3x3 +
 *     LoadRenderRotationMatrix + ConvertFloatVec3ToShortAngles).
 *   - signed div-by-2 with C-style truncation (dot/2 vs dot>>1):
 *     SHIPPED fix in commit on precise-004057F0 branch.
 *   - axis assignment to ang_vel_roll/pitch:
 *     CONFIRMED at 0x00405A3D / 0x00405A43.
 *   - divisors /0x4B0 (roll) and /0x226 (pitch):
 *     CONFIRMED via IMUL constants 0x1B4E81B5 SAR 7 + 0x77280773 SAR 8.
 *   - pattern-clamp switch table on prev_air (bVar2): MATCHES.
 *
 * STATUS: Static audit COMPLETE — function is byte-faithful with one
 * SHIPPED fix (D1 dot>>1 → dot/2). Per the chassis-launch chain
 * investigation (agent #41) the residual runtime divergence is UPSTREAM
 * in ComputeActorTrackContactNormalExtended @ 0x00403720 (pool1) — wcv
 * sign / hires shift / gap_270 chain — and the side-channel cardef-
 * loading issue for PlayerIsAI=1 slot 0.
 *
 * KNOWN TODO CHAIN OWNERS (chassis-launch / suspension cascade):
 *   - todo_edinburgh_chassis_launch_proximate_cause.md (wcb=0 transient)
 *   - todo_edinburgh_span_433_residual_launch.md (second crest launch)
 *   - todo_chassis_snap_fix_2026-05-16.md (floorf vs FISTP-RNE rounding)
 *
 * Audit reference: re/analysis/pilot_004057F0_audit.md (pool6, 2026-05-14).
 * Effective level: L4 (byte-faithful per static audit; upstream-blocked).
 */

/* [FIX 2026-06-21] QoL damp for the documented "wcb=0 chassis launch" on steep
 * DESCENTS (user repro: Scotland ~span 2078, a real ~33° descent; cf. the TODO
 * notes above re: edinburgh_chassis_launch / wcb=0 transient). Driving DOWN a
 * steep grade, the contact/suspension path accrues spurious UPWARD chassis
 * velocity and the car takes off. descent_damp_apply() is called once per actor
 * from the SHARED td5_physics_integrate_pose (covering the 4-wheel PLAYER and the
 * 2-axle AI OPPONENTS alike — both populate wheel contacts there): when the
 * actor has ground contact AND the ground DROPPED this tick (descent) yet
 * linear_velocity_y is positive (rising), it replaces that velocity with the
 * ground's own drop rate so the chassis FOLLOWS the hill down instead of
 * launching. Scoped tight: it never fires while fully airborne (real jumps) or
 * on uphills (ground rising, dg > 0). Default ON; TD5RE_DESCENT_DAMP=0 reverts.
 * Per-slot state for netplay determinism (getenv is identical launch config on
 * every peer; the damp is a deterministic function of replicated sim state). */
static int descent_damp_enabled(void)
{
    static int s_en = -1;
    if (s_en < 0) {
        s_en = td5_env_flag_on("TD5RE_DESCENT_DAMP");
    }
    return s_en;
}
static int32_t s_descent_prev_ground[16];
static int     s_descent_prev_valid[16];
/* Min per-tick ground DROP (24.8 fp) counted as "descending". The steep Scotland
 * section drops ~76800 fp/tick at speed; 8000 ignores flat-ground noise while
 * catching any genuine descent. */
#define DESCENT_DAMP_MIN_DROP 8000
/* Max UPWARD chassis velocity (24.8 fp) tolerated for a GROUNDED car. ~16000 (≈63
 * world units/tick = a brisk vertical climb) clears real bumps/kerbs but clamps
 * the launch/snap spike (which reaches tens of thousands). Jumps go airborne first
 * (early return), so ramps/crests are unaffected. */
#define DESCENT_DAMP_MAX_UP 16000

/* [FIX 2026-06-21] Slope-roll damp. The wheel-derived attitude (attitude_from_wheels)
 * tips the car side-to-side on steep grades because the right-side wheel probes
 * over-walk a span and read asymmetric heights (the documented "rolls out of
 * control on steep slopes" residual — exposed once the descent-launch was fixed).
 * On a STEEP grade driven roughly STRAIGHT, the true roll should be ~0, so scale
 * the computed roll toward level. Cornering / banked turns keep their real roll
 * (high yaw rate gates the damp off). Default ON; TD5RE_ROLL_DAMP=0 reverts. */
static int roll_damp_enabled(void)
{
    static int s_en = -1;
    if (s_en < 0) {
        s_en = td5_env_flag_on("TD5RE_ROLL_DAMP");
    }
    return s_en;
}
/* |pitch| (12-bit angle, abs in [0,2048]) above which the grade counts as steep.
 * ~205 ≈ 18°. */
#define ROLL_DAMP_PITCH_MIN 205
/* |angular_velocity_yaw| below which the car counts as going straight (not
 * cornering). Conservative so only genuinely-straight descents are damped. */
#define ROLL_DAMP_YAW_MAX   1500
/* Fraction of the wheel-derived roll KEPT on a steep straight grade (Q8).
 * 0x50 ≈ 0.31 → most of the spurious slope-tilt removed, a little kept so the
 * transition isn't a hard snap. */
#define ROLL_DAMP_KEEP_Q8   0x50

static void descent_damp_apply(TD5_Actor *actor)
{
    if (!descent_damp_enabled()) return;
    int dslot = actor->slot_index & 0x0F;
    /* Average ONLY the grounded wheels (an airborne wheel_contact_pos.y is the
     * un-snapped transform Y and would pollute the ground reference). Any grounded
     * wheel gives a valid descent reference — on a bouncy descent the car often
     * has only partial contact while it pumps upward velocity. */
    int32_t gnd_sum = 0; int gnd_cnt = 0;
    for (int wi = 0; wi < 4; wi++) {
        if (((actor->wheel_contact_bitmask >> wi) & 1) == 0) {   /* bit clear = grounded */
            gnd_sum += actor->wheel_contact_pos[wi].y; gnd_cnt++;
        }
    }
    if (gnd_cnt == 0) { s_descent_prev_valid[dslot] = 0; return; }  /* airborne: no ref */
    int32_t gnd = gnd_sum / gnd_cnt;
#ifndef TD5RE_RELEASE
    int32_t before_vy = actor->linear_velocity_y;
#endif
    if (s_descent_prev_valid[dslot] && actor->linear_velocity_y > 0) {
        int32_t dg = gnd - s_descent_prev_ground[dslot];
        if (dg < -DESCENT_DAMP_MIN_DROP)     /* ground dropped -> descent */
            actor->linear_velocity_y = dg;   /* follow the hill down, don't launch */
    }
    /* Backstop: a GROUNDED car cannot plausibly have a large UPWARD velocity —
     * only a launch / ground-probe spike produces it. Clamp it even when the
     * descent test above didn't fire (catches the AI snap-spike and any partial-
     * contact moment the descent gate misses). Real jumps go through the airborne
     * early-return above, so ramps/crests are untouched. */
    if (actor->linear_velocity_y > DESCENT_DAMP_MAX_UP)
        actor->linear_velocity_y = DESCENT_DAMP_MAX_UP;
#ifndef TD5RE_RELEASE
    if (actor->slot_index <= 1 && actor->linear_velocity_y != before_vy) {
        static int s_dd_budget = 1500;
        if (s_dd_budget > 0) {
            s_dd_budget--;
            TD5_LOG_I(LOG_TAG, "descent-damp slot=%d vy %d->%d gnd=%d gnd_cnt=%d",
                      (int)actor->slot_index, before_vy,
                      actor->linear_velocity_y, gnd, gnd_cnt);
        }
    }
#endif
    s_descent_prev_ground[dslot] = gnd;
    s_descent_prev_valid[dslot]  = 1;
}

void td5_physics_update_suspension_response(TD5_Actor *actor)
{
    /* Faithful port of UpdateVehicleSuspensionResponse @ 0x004057F0.
     *
     * HISTORY: this is a restore of commit e97669e's literal port, which
     * had been replaced (between then and 2026-04-21) with a pragmatic
     * ground-probe P-controller + 1/16 decay. That replacement produced
     * an exaggerated uphill-roll bug: on positive pitch slopes the
     * P-controller's target and the decay-to-zero both pulled the same
     * direction (runaway), while downhill they opposed (self-resets).
     * Research agent 2026-04-21 confirmed:
     *   - No asymmetric term exists in the original 0x004057F0.
     *   - T2 block in IntegrateVehiclePoseAndContacts @ 0x00405E80 OVERWRITES
     *     angular_velocity_{roll,pitch} each tick; 0x004057F0 ADDS a small
     *     per-tick correction clamped ±4000. That pair is self-limiting.
     *   - Port T2 block already matches original exactly (±6000 clamp,
     *     wrap*0x100 delta). So restoring the faithful 0x004057F0 add-step
     *     reinstates the T2-dominates-with-corrective pattern.
     *
     * Two masks are needed:
     *   lock     = current-frame AIRBORNE mask (1=airborne). Original reads
     *              [ESI+0x37C]. Port live equivalent is
     *              actor->wheel_contact_bitmask after refresh.
     *   prev_air = previous-frame AIRBORNE mask (1=airborne). Original
     *              reads [ESI+0x37D] — refresh overwrites +0x37D ← old
     *              +0x37C at its top, so after refresh +0x37D holds last
     *              tick's airborne mask. Port reconstructs by inverting
     *              s_prev_grounded_mask[slot] (which was captured pre-
     *              refresh from the PREVIOUS frame's wheel_contact_bitmask).
     *
     * NOTE polarity: both masks are AIRBORNE (1=airborne). Prior port
     * mixed GROUNDED/AIRBORNE conventions and got BOTH the spring gate
     * and the pattern clamp switch-key wrong. Fixed 2026-04-24.
     *
     * Struct layout source: decomp @ 0x00405884/0x00405888 shows the loop
     * reads sVar3=psVar11[-0x22], sVar4=psVar11[-0x20] where psVar11 walks
     * +0x254-style stride of 4 shorts/wheel, giving actor+0x210/+0x214 for
     * wheel 0 — matches the `arms` pointer below. wcv at +0x250 stride 4
     * shorts; gap_270 at +0x270 same stride.
     */
    const uint8_t lock = actor->wheel_contact_bitmask;
    /* prev_air = previous-frame AIRBORNE mask (bVar2 in original @ +0x37D).
     * After refresh_wheel_contacts, the original's +0x37D holds the old
     * value of +0x37C (= last-frame airborne). Port snapshots
     * s_prev_grounded_mask[slot] = ~(last-frame airborne) BEFORE refresh,
     * so we invert to reconstruct prev_air.
     *
     * HISTORY: prior port named this `grnd` and treated it as previous-
     * frame GROUNDED. That polarity was wrong in TWO places:
     *   (1) spring-dot gate @ 0x004058E4 fires when wheel was AIRBORNE last
     *       frame (i.e. just landed this tick), not when grounded. Port
     *       was firing the spring impulse on every grounded frame, which
     *       continuously amplified tiny per-tick Y fluctuations into
     *       roll_spr/pitch_spr — matches user's "very sensitive of Y" bug.
     *   (2) pattern-clamp switch @ 0x00405A6A keys on +0x37D directly.
     *       Port's inverted key matches the complement pattern set.
     * 2026-04-24 research agent confirmed both polarities at the cited
     * disassembly addresses. */
    const uint8_t prev_air = (uint8_t)((~s_prev_grounded_mask[actor->slot_index & 0x0F]) & 0x0F);

    if (lock == 0x0F) {
        /* All four wheels airborne — early-return matches original
         * 0x00405809; gravity stays subtracted (not added back). */
        return;
    }

    if (actor->slot_index == 0 && (actor->frame_counter % 60u) == 0u) {
        TD5_LOG_I(LOG_TAG, "susp_resp_enter slot0: lock=0x%02x prev_air=0x%02x",
                  (int)lock, (int)prev_air);
    }

    /* Naming reflects WHAT the accumulator holds, NOT which actor field it
     * eventually feeds — orig and port disagree on that mapping (see write-
     * back block below).
     *
     * loni_grav / loni_spr accumulate (g_scaled * arm2)-derived terms, where
     * arm2 = wheel_display_angles[i][2] = body-Z (longitudinal) arm.
     * lat_grav  / lat_spr  accumulate (g_scaled * arm0)-derived terms, where
     * arm0 = wheel_display_angles[i][0] = body-X (lateral) arm.
     *
     * Original Ghidra locals: local_50=loni_grav, local_5c=lat_grav,
     * local_64=loni_spr, local_60=lat_spr, local_78=bounce, local_4c=cnt_active,
     * local_58=cnt_grounded. */
    int32_t loni_grav = 0;      /* local_50 — Σ g_scaled * arm2 */
    int32_t lat_grav  = 0;      /* local_5c — Σ -(g_scaled * arm0) */
    int32_t loni_spr  = 0;      /* local_64 — Σ (dot * arm2) * -0x100 */
    int32_t lat_spr   = 0;      /* local_60 — Σ (dot * arm0) * +0x100 */
    int32_t bounce     = 0;     /* local_78 */
    int32_t cnt_active   = 0;   /* local_4c — non-locked wheels */
    int32_t cnt_grounded = 0;   /* local_58 — wheels also grounded prev tick */

    const int16_t *arms = (const int16_t *)((const uint8_t *)actor + 0x210);
    const int16_t *wcv  = (const int16_t *)((const uint8_t *)actor + 0x250);
    const int16_t *wfdh = (const int16_t *)((const uint8_t *)actor + 0x270);

    for (int i = 0; i < 4; i++) {
        const uint8_t bit = (uint8_t)(1u << i);
        if (lock & bit) continue;            /* skip airborne wheel entirely */

        const int16_t lat  = arms[i * 4 + 0];   /* sVar3, lateral arm  */
        const int16_t loni = arms[i * 4 + 2];   /* sVar4, longitudinal */

        /* g_view = Y component of (transposed body matrix) · wcv[i] —
         * matches `ConvertFloatVec3ToShortAngles(wcv, &local_38)` reading
         * local_36. `rotate_world_to_body_y` reads {m[1],m[4],m[5]/m[7]} —
         * = row 1 of body^T = column 1 of body matrix — matching the
         * original's TransposeMatrix3x3 + LoadRenderRotationMatrix +
         * ConvertFloatVec3ToShortAngles sequence at 0x004057FA-0x004058D8. */
        int16_t cn[3] = { wcv[i * 4 + 0], wcv[i * 4 + 1], wcv[i * 4 + 2] };
        const int32_t y_view = (int32_t)rotate_world_to_body_y(actor, cn);

        /* g_scaled is (Y · gravity) >> 12; signed-divide rounding via
         * arith_round_shift matches original's SAR-with-bias idiom. */
        const int32_t g_scaled = arith_round_shift(y_view * g_gravity_constant, 0xFFF, 12);

        /* [CONFIRMED @ 0x004058B6-0x004058D5 disasm]:
         *   IMUL EDX, EBP   ; EDX = g_scaled * arm2 (loni)
         *   ADD  ECX, EDX   ; local_50 (loni_grav) += g_scaled * arm2
         *   IMUL EAX, EBX   ; EAX = g_scaled * arm0 (lat)
         *   SUB  ECX, EAX   ; local_5c (lat_grav)  -= g_scaled * arm0
         */
        loni_grav += g_scaled * (int32_t)loni;    /* += longitudinal arm */
        lat_grav  -= g_scaled * (int32_t)lat;     /* -= lateral arm (NOTE sign) */
        ++cnt_active;

        if (prev_air & bit) {
            /* Landing-impulse spring: fires ONLY for wheels that were
             * AIRBORNE last frame (just landed this tick). Literal port of
             * `if ((bVar2 & uVar7) != 0)` @ 0x004058E4 where bVar2 =
             * [ESI+0x37D] = previous-frame airborne mask. This is an
             * impulsive correction tied to touchdown, NOT a per-frame
             * spring damper.
             *
             * wfdh and wcv share +0x270 / +0x250 layout (4 shorts per
             * wheel). dot(wfdh[i], wcv[i]) scaled by per-wheel body arms
             * goes into pitch_spr/roll_spr/bounce. */
            int32_t dot =   (int32_t)wfdh[i * 4 + 0] * (int32_t)wcv[i * 4 + 0]
                          + (int32_t)wfdh[i * 4 + 1] * (int32_t)wcv[i * 4 + 1]
                          + (int32_t)wfdh[i * 4 + 2] * (int32_t)wcv[i * 4 + 2];
            dot = arith_round_shift(dot, 0xFFF, 12);   /* signed >>12 */

            /* Per orig 0x004058E4+ disasm (and decompile):
             *   local_64 (loni_spr) += (dot * arm2) * -0x100
             *   local_60 (lat_spr)  += (dot * arm0) * +0x100 */
            loni_spr += (dot * (int32_t)loni) * -0x100;    /* arm2 with -0x100 */
            lat_spr  += (dot * (int32_t)lat ) *  0x100;    /* arm0 with +0x100 */
            /* [CONFIRMED @ 0x00405938-0x0040593F] CDQ + SUB EAX,EDX + SAR EAX,1
             * implements C-style signed div-by-2 with truncation toward zero
             * (e.g. -5/2 = -2). Previous port used `dot >> 1` which is SAR
             * = truncation toward -inf (-5 >> 1 = -3). For negative odd dot
             * this would diverge by 1. */
            bounce    += dot / 2;                          /* signed div toward zero */
            ++cnt_grounded;

            if (actor->slot_index == 0) {
                TD5_LOG_I(LOG_TAG,
                    "susp_resp landing_impulse slot0: wheel=%d dot=%d "
                    "lat=%d loni=%d loni_spr+=%d lat_spr+=%d",
                    i, dot, (int)lat, (int)loni,
                    (dot * (int32_t)loni) * -0x100,
                    (dot * (int32_t)lat)  *  0x100);
            }
        }
    }

    if (cnt_grounded > 0) {
        loni_spr /= cnt_grounded;
        lat_spr  /= cnt_grounded;
        bounce   /= cnt_grounded;
    }

    /* [FIX 2026-05-31 falling-car-ground-impact-sound] Faithful port of the
     * normal-mode wheel-LANDING thud from UpdateVehicleSuspensionResponse
     * @0x004057F0. Orig: after averaging the per-just-landed-wheel impact into
     * `bounce` (iVar8), if bounce > 0x14 it plays sound 0x17 (Bottom*.wav,
     * 4 variants) at volume bounce*0x32, freq 0x5622, positioned at the actor
     * world_pos (+0x1FC). This is the "car falls off a slope and hits the
     * ground" thud the user reported missing — a SEPARATE emitter from the
     * collision/scripted-recovery thud at 0x004096B0 (which only runs in
     * vehicle_mode==1). RE basis: Ghidra agent (ab81bbcf) confirmed
     * iVar8>0x14 → PlayVehicleSoundAtPosition(0x17, iVar8*0x32, 0x5622,
     * actor+0x1FC, 4); the *0x32 multiplier is also corroborated by this
     * function's own line-by-line audit (the prior `bounce*50` stub comment).
     * Previously omitted ("sound side stubbed") → no landing thud on falls. */
    if (bounce > 0x14) {
        int32_t world_pos[3] = { actor->world_pos.x,
                                 actor->world_pos.y,
                                 actor->world_pos.z };
        td5_sound_play_at_position(0x17, bounce * 0x32, 0x5622, world_pos, 4);
    }

    /* Apply roll/pitch corrective torque + Y velocity restore.
     *
     * [CONFIRMED @ 0x004059B5-0x00405A49] Original writes ang_vel_roll,
     * ang_vel_pitch and lin_vel_y UNCONDITIONALLY once cnt_active > 0.
     * The cnt_grounded > 0 condition (`JLE 0x00405AFA` at 0x00405A50)
     * only gates the pattern-clamp switch that follows — the WRITES
     * have already happened.
     *
     * AXIS-ASSIGNMENT FIX [CONFIRMED via byte-level disasm 2026-05-04
     * round 23, addresses 0x00405A3D + 0x00405A43]:
     *
     *   ORIG:  av_roll(+0x1C0)  += (loni_spr + loni_grav/cnt) / 0x4B0
     *          av_pitch(+0x1C8) += (lat_spr  + lat_grav /cnt) / 0x226
     *
     * The longitudinal-arm (loni / arm2 / wheel_display_angles[i][2])
     * derived torque feeds AV_ROLL, and the lateral-arm (lat / arm0)
     * derived torque feeds AV_PITCH — opposite of the standard physics
     * convention. Ghidra disasm shows:
     *
     *   0x004058BB  IMUL EDX, EBP   ; EDX = g_scaled * arm2 (loni)
     *   0x004058BE  ADD  ECX, EDX   ; local_50 += loni-derived
     *   0x004058B8  IMUL EAX, EBX   ; EAX = g_scaled * arm0 (lat)
     *   0x004058CC  SUB  ECX, EAX   ; local_5c -= lat-derived
     *   0x00405A3D  MOV [EDI+0x1c0], ESI   ; av_roll  = ... + local_50/0x4B0
     *   0x00405A43  MOV [EDI+0x1c8], EAX   ; av_pitch = ... + local_5c/0x226
     *
     * HISTORY: prior port (since restoration commit eb36524 2026-04-21)
     * routed loni-derived torque into av_pitch and lat-derived torque
     * into av_roll, mirroring standard physics — but contradicting orig.
     * For Viper (lat values ±255 symmetric, sum=0; loni 435/-394, sum=82)
     * the consequence under PlayerIsAI=0 manual drive on a slope was:
     *   port:  av_pitch = -1900*82/4/0x226 ≈ -71/tick    (loni-derived)
     *          av_roll  = 0                              (lat sum = 0)
     *   orig:  av_pitch = 0                              (lat sum = 0)
     *          av_roll  = -1900*82/4/0x4B0 ≈ -32/tick    (loni-derived)
     * Frida probe confirmed orig av_pitch=0 throughout sim_tick=1..50,
     * av_roll oscillating around -288. Math + disasm both agree.
     *
     * Round 22 noted the writes are unconditional once cnt_active > 0,
     * which round 9 commit a4b91ac had over-gated on cnt_grounded > 0.
     * That gate is now removed (correct). The axis-swap fix below sits
     * on top: same divisors per orig, just the right inputs feeding the
     * right outputs. */
    if (cnt_active > 0) {
        int32_t roll_term  = (loni_spr + loni_grav / cnt_active) / 0x4B0;
        int32_t pitch_term = (lat_spr  + lat_grav  / cnt_active) / 0x226;
        actor->angular_velocity_roll  += roll_term;
        actor->angular_velocity_pitch += pitch_term;

        /* Y-velocity update: bounce + gravity restored. Original adds
         * gravity back here, cancelling the subtract at top of integrate_pose
         * for grounded cars.
         *
         * [CONFIRMED via Edinburgh wheel_contact_probe 2026-05-11]: this
         * write MUST be inside the `cnt_active > 0` gate. Comment at line
         * 3778 explicitly says "the original writes both ang_vel_pitch and
         * lin_vel_y UNCONDITIONALLY once cnt_active > 0" — meaning BOTH
         * are gated on cnt_active > 0, not "unconditional in absolute
         * terms". When all 4 wheels are airborne (cnt_active=0), the
         * original SKIPS this write, letting gravity from integrate_pose
         * accumulate. The port previously placed this write OUTSIDE the
         * gate, which cancelled gravity during airborne and produced the
         * Edinburgh "car drifts forward at constant altitude" symptom
         * after launching off a road bump. */
        actor->linear_velocity_y += bounce + g_gravity_constant;
    }

    /* [FIX 2026-06-21] Descent-launch damp for the PLAYER (this-tick wheel
     * contacts, applied after the faithful bounce write, before integrate_pose
     * integrates Y). AI opponents are damped from integrate_pose instead (they
     * never run this function). See descent_damp_apply(). */
    descent_damp_apply(actor);

    if (cnt_grounded > 0) {
        /* Per-pattern angular velocity clamps [CONFIRMED @ 0x00405a6a..
         * 0x00405af4]. Original switches on bVar2 = [ESI+0x37D] =
         * previous-frame AIRBORNE mask. Bitmasks 7,11,13,14,15 fall
         * through with NO clamp.
         *
         * HISTORY: prior port keyed on `grnd` (previous-frame GROUNDED),
         * which matched the COMPLEMENT pattern set (case 0 in port =
         * all airborne, case 0 in original = all grounded). Polarity
         * fixed 2026-04-24. */
        const int32_t LIM = 4000;   /* 0xFA0 */
        switch (prev_air) {
            case 0: case 1: case 2: case 4: case 6: case 8: case 9:
                if (actor->angular_velocity_roll  >  LIM) actor->angular_velocity_roll  =  LIM;
                if (actor->angular_velocity_roll  < -LIM) actor->angular_velocity_roll  = -LIM;
                if (actor->angular_velocity_pitch >  LIM) actor->angular_velocity_pitch =  LIM;
                if (actor->angular_velocity_pitch < -LIM) actor->angular_velocity_pitch = -LIM;
                break;
            case 3: case 12:
                if (actor->angular_velocity_roll  >  LIM) actor->angular_velocity_roll  =  LIM;
                if (actor->angular_velocity_roll  < -LIM) actor->angular_velocity_roll  = -LIM;
                break;
            case 5: case 10:
                if (actor->angular_velocity_pitch >  LIM) actor->angular_velocity_pitch =  LIM;
                if (actor->angular_velocity_pitch < -LIM) actor->angular_velocity_pitch = -LIM;
                break;
            default: /* 7,11,13,14,15 — no clamp */
                break;
        }
    }

    if (actor->slot_index == 0 && (actor->frame_counter % 60u) == 0u) {
        TD5_LOG_I(LOG_TAG,
                  "susp_resp slot0: lock=0x%02x prev_air=0x%02x cnt=%d/%d "
                  "loni_grav=%d lat_grav=%d loni_spr=%d lat_spr=%d bounce=%d "
                  "av_r=%d av_p=%d vy=%d",
                  (int)lock, (int)prev_air, cnt_active, cnt_grounded,
                  loni_grav, lat_grav, loni_spr, lat_spr, bounce,
                  actor->angular_velocity_roll, actor->angular_velocity_pitch,
                  actor->linear_velocity_y);
    }

}

/* ========================================================================
 * Traffic Edge Containment Helpers
 *
 * The original UpdateTrafficActorMotion (0x443ED0) calls, after
 * UpdateTrafficVehiclePose:
 *   ProcessActorRouteAdvance (0x407840)
 *   ProcessActorForwardCheckpointPass (0x4076C0)  -- route progression only
 *   ProcessActorSegmentTransition (0x407390)
 *
 * ProcessActorRouteAdvance and ProcessActorSegmentTransition both:
 *   1. Compute heading delta (from route table vs actor yaw).
 *   2. Build the boundary edge tangent from the first/last span vertex pair.
 *   3. Compute signed perpendicular distance from edge: pen.
 *   4. If pen < 0: call ApplySimpleTrackSurfaceForce (0x407270) to push
 *      the actor back and zero the outward velocity component.
 *   5. Call UpdateTrafficVehiclePose again to snap pose after the push.
 *
 * Without these calls traffic has NO wall containment, only steering-based
 * routing — on tight corners it drives straight through the wall.
 *
 * [CONFIRMED @ 0x443ED0: call sequence after IntegrateVehicleFrictionForces]
 * [CONFIRMED @ 0x407270: ApplySimpleTrackSurfaceForce modifies world_pos/vel]
 * [CONFIRMED @ 0x407390: ProcessActorSegmentTransition edge test for sub_lane boundaries]
 * [CONFIRMED @ 0x407840: ProcessActorRouteAdvance forward-limit edge test]
 * ======================================================================== */

/* Route state indices (mirrors RS_ constants from td5_ai.c).
 * Exposed here for cross-module route state access.
 * [CONFIRMED @ gActorRouteStateTable stride 0x47 dwords] */
#define RS_ROUTE_TABLE_PTR_PHYS    0x00   /* pointer to LEFT/RIGHT.TRK data */
#define RS_SLOT_INDEX_PHYS         0x35   /* slot index of reference actor */

/* Compute route heading delta for slot:
 *   heading_delta = -( ((euler_yaw>>8) - route_angle*0x102C/0x100) - 0x800 & 0xFFF ) - 0x800 & 0xFFF
 * [CONFIRMED @ ComputeActorRouteHeadingDelta 0x434040]
 *
 * The route table stores 3 bytes per span: [angle_lo, angle_hi, ???].
 * The angle at span 'span_normalized' is *(uint8_t*)(route_table + span_norm*3 + 1).
 * Multiplied by 0x102C gives the route heading in 24-bit fixed.
 */
static uint32_t traffic_route_heading_delta(int slot)
{
    int32_t *rs = td5_ai_get_route_state(slot);
    if (!rs) return 0;

    const uint8_t *route_table = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR_PHYS];
    if (!route_table) return 0;

    char *actor = (char *)((uint8_t *)td5_track_get_span(0) - 0x80);  /* not used directly */
    (void)actor;

    /* Get the reference actor slot — RS_SLOT_INDEX gives which actor's yaw to use */
    int ref_slot = rs[RS_SLOT_INDEX_PHYS];
    if (ref_slot < 0 || ref_slot >= TD5_MAX_TOTAL_ACTORS) return 0;

    char *ref_actor = (char *)(g_actor_table_base + (size_t)ref_slot * TD5_ACTOR_STRIDE);

    int16_t span_normalized = *(int16_t *)(ref_actor + 0x082);  /* track_span_normalized */
    if (span_normalized < 0) span_normalized = 0;
    uint8_t route_angle = route_table[(int)span_normalized * 3 + 1];
    int32_t yaw_accum   = *(int32_t *)(ref_actor + 0x1F4);      /* euler_accum.yaw */

    /* Formula from 0x434040:
     *   -(( ((yaw>>8) - route_angle*0x102C/0x100) - 0x800) & 0xFFF) - 0x800) & 0xFFF */
    uint32_t yaw12     = (uint32_t)(yaw_accum >> 8) & 0xFFF;
    uint32_t route12   = ((uint32_t)route_angle * 0x102C) >> 8;
    uint32_t raw_delta = (yaw12 - route12 - 0x800U) & 0xFFF;
    /* Negate to get heading_delta */
    return (-(int32_t)(((int32_t)raw_delta - 0x800) & 0xFFF)) & 0xFFF;
}

/* ApplySimpleTrackSurfaceForce — byte-faithful port of 0x00407270.
 *
 * Pushes actor out of wall penetration and reflects the outward velocity.
 *
 * Address note: scope file lists this as "TOI_substep at 0x0040728B", but
 * 0x0040728B is mid-function (the AND EDX,0xFFF sign-correction inside the
 * pen → depth computation). The actual function entry is 0x00407270.
 *
 * Register-level register/var mapping (from listing 0x00407270-0x0040738E):
 *   EDI = cos = CosFixed12bit(edge_angle)      [0x00407279 CALL 0x40A6E0]
 *   EBX = sin = SinFixed12bit(edge_angle)      [0x00407281 CALL 0x40A700]
 *   ESI (depth) = ((pen + sign_corr(0xFFF)) >> 12) - 4   [0x0040728C-72A4]
 *   EBP_pos (a/k/a iVar5_pos) = depth*sin sign-corrected then shifted by 4
 *           and *negated*; added to *(actor+0x1fc) (= world_pos.x)
 *                                                       [0x004072A9-72B7]
 *   EAX_pos = depth*cos sign-corrected then shifted by 4;
 *           added to *(actor+0x204) (= world_pos.z)     [0x004072BB-72D5]
 *
 *   ESI (vx) = *(actor+0x1cc) = linear_velocity_x       [0x004072BE]
 *   ...
 *   EBP_vel = (vx*cos + vz*sin + sign_corr(0xFFF))      [0x004072E1-72FD]
 *           — TANGENTIAL component (along edge)
 *   EAX_vel = (vz*cos - vx*sin + sign_corr(0xFFF))      [0x004072FF-407311]
 *           — NORMAL/OUTWARD component (perp to edge)
 *   After SAR by 12: EBP=tang_sh, EAX=normal_sh.
 *
 *   JS 0x40738A at 0x40731C → exits if NORMAL component is negative,
 *           i.e. when vel projects INTO the wall (no work to do).
 *   Otherwise (normal_sh >= 0):
 *     vel_perp_new = -(normal_sh >> 1)            [0x40731E-407325]
 *           — CDQ/SUB-EDX is a no-op here because the branch is gated
 *             on normal_sh >= 0, so sign-bit broadcast = 0.
 *     Dead-zone clamp on tang_sh (EBP):           [0x407327-407347]
 *       if tang_sh >  0x180: tang_clamped = tang_sh - 0x180
 *       elif tang_sh < -0x180: tang_clamped = tang_sh + 0x180
 *       else: tang_clamped = 0
 *     vx = (tang_clamped*cos - vel_perp_new*sin + sign_corr) >> 12
 *     vz = (tang_clamped*sin + vel_perp_new*cos + sign_corr) >> 12
 *     *(actor+0x37b) = 0  (track_contact_flag)    [0x407383]
 *
 * Naming reconciliation: Ghidra decompiler labels the components "perp"
 * and "para" but the geometry is reversed from intuition — what Ghidra
 * calls iVar4 (perp) is actually the EDGE-TANGENTIAL projection, and
 * what it calls iVar3 (para) is the OUTWARD-NORMAL projection. We use
 * tang_/normal_ here to match the listing semantics.
 * ======================================================================== */
static void apply_simple_track_surface_force(TD5_Actor *actor,
                                              uint32_t edge_angle,
                                              int32_t pen)
{
    /* 0x00407278-0x00407286 */
    int32_t cos_a = cos_fixed12((int32_t)(edge_angle & 0xFFF));
    int32_t sin_a = sin_fixed12((int32_t)(edge_angle & 0xFFF));

    /* 0x0040728C-0x004072A4: depth = ((pen + sign_corr) >> 12) - 4
     *   CDQ; AND EDX,0xFFF; ADD EAX,EDX; SAR ESI,0xC; SUB ESI,0x4   */
    int32_t depth = ((pen + ((int32_t)((uint32_t)(pen >> 31) & 0xFFFu))) >> 12) - 4;

    /* 0x004072A9-0x004072B7: actor->world_pos.x += -((depth*sin + sign_corr(0xF)) >> 4)
     *   IMUL EAX,EBX(sin); CDQ; AND EDX,0xF; ADD EAX,EDX; SAR EAX,0x4; NEG EAX;
     *   ADD EBP,EAX  (EBP was preloaded from [ECX+0x1fc])                 */
    {
        int32_t prod_sin = depth * sin_a;
        int32_t step_x   = (prod_sin + ((int32_t)((uint32_t)(prod_sin >> 31) & 0xFu))) >> 4;
        actor->world_pos.x += -step_x;
    }

    /* 0x004072B9-0x004072D5: actor->world_pos.z += ((depth*cos + sign_corr(0xF)) >> 4)
     *   IMUL EAX,EDI(cos); CDQ; AND EDX,0xF; ADD EAX,EDX; SAR EAX,0x4;
     *   ADD EDX(=[ECX+0x204]),EAX                                          */
    {
        int32_t prod_cos = depth * cos_a;
        int32_t step_z   = (prod_cos + ((int32_t)((uint32_t)(prod_cos >> 31) & 0xFu))) >> 4;
        actor->world_pos.z += step_z;
    }

    /* 0x004072BE / 0x004072DB-0x00407319: velocity decomposition.
     * Note ESI=vx is loaded at 0x4072BE BEFORE the x-position write, but
     * since world_pos.x is offset 0x1FC and linear_velocity_x is offset
     * 0x1CC they don't alias, and the order of reads/writes is observable
     * only through the actor struct (same outcome). */
    int32_t vx = actor->linear_velocity_x;
    int32_t vz = actor->linear_velocity_z;

    /* EBP = (vx*cos + vz*sin) + sign_corr(0xFFF) — TANGENTIAL raw       */
    int32_t tang_raw = vx * cos_a + vz * sin_a;
    int32_t tang_adj = tang_raw + ((int32_t)((uint32_t)(tang_raw >> 31) & 0xFFFu));

    /* EAX = (vz*cos - vx*sin) + sign_corr(0xFFF) — OUTWARD-NORMAL raw  */
    int32_t normal_raw = vz * cos_a - vx * sin_a;
    int32_t normal_adj = normal_raw + ((int32_t)((uint32_t)(normal_raw >> 31) & 0xFFFu));

    /* SAR by 12 [0x00407316, 0x00407319] */
    int32_t tang_sh   = tang_adj   >> 12;
    int32_t normal_sh = normal_adj >> 12;

    /* JS 0x0040738A at 0x40731C — exit when normal_sh < 0 [outward vel
     * projects toward wall: nothing to push back]                       */
    if (normal_sh < 0) {
        return;
    }

    /* 0x0040731E-0x00407325: vel_perp_new = -((normal_sh - 0) >> 1)
     *   CDQ; SUB EAX,EDX; SAR EAX,1; MOV ESI,EAX; NEG ESI
     * Since we just gated on normal_sh >= 0, EDX = sign-broadcast = 0,
     * so SUB EAX,EDX is a no-op. The arithmetic SAR happens BEFORE NEG,
     * which matters when normal_sh is odd: e.g. normal_sh=5 →
     *   asm: -(5 >> 1) = -2          (vs. (-5) >> 1 = -3)               */
    int32_t vel_perp_new = -(normal_sh >> 1);

    /* 0x00407327-0x00407347: dead-zone clamp on tang_sh (EBP)
     *   CMP EBP,0x180; JLE -> next test; ADD EBP,-0x180; JMP done
     *     [reached when EBP > 0x180, i.e. EBP >= 0x181]
     *   CMP EBP,-0x180; JGE 0x40347 (XOR EBP,EBP); ADD EBP,0x180
     *     [JGE means EBP >= -0x180 → zero;
     *      else (EBP < -0x180): EBP += 0x180]                            */
    int32_t tang_clamped;
    if (tang_sh > 0x180) {
        tang_clamped = tang_sh - 0x180;
    } else if (tang_sh < -0x180) {
        tang_clamped = tang_sh + 0x180;
    } else {
        tang_clamped = 0;
    }

    /* 0x00407349-0x0040737D: recompose velocity in world frame.
     *   nvx = tang_clamped*cos - vel_perp_new*sin
     *   nvz = tang_clamped*sin + vel_perp_new*cos
     * Each summand gets sign_corr(0xFFF) before SAR by 12.               */
    {
        int32_t nvx_raw = tang_clamped * cos_a - vel_perp_new * sin_a;
        int32_t nvx_adj = nvx_raw + ((int32_t)((uint32_t)(nvx_raw >> 31) & 0xFFFu));
        actor->linear_velocity_x = nvx_adj >> 12;
    }
    {
        int32_t nvz_raw = tang_clamped * sin_a + vel_perp_new * cos_a;
        int32_t nvz_adj = nvz_raw + ((int32_t)((uint32_t)(nvz_raw >> 31) & 0xFFFu));
        actor->linear_velocity_z = nvz_adj >> 12;
    }

    /* 0x00407383: *(actor+0x37b) = 0 — track_contact_flag */
    actor->track_contact_flag = 0;
}

/* [TRACE 2026-05-24 traffic-edge-pen-cluster] Per-call context so the
 * caller can tell traffic_edge_pen which call-site it is + which slot is
 * being tested. Gated by g_td5.ini.trace_traffic_edge_pen (INI key
 * [Trace] TrafficEdgePen=1). When disabled the trace path is a single
 * compare-and-bail at the top of traffic_edge_pen — no measurable cost.
 * Mirrors tools/_probes/traffic_edge_pen_probe.js capture for orig. */
static struct {
    const char *call_id;   /* "1a" inner / "1b" outer / "2" fwd-cp / "3" route-adv */
    int slot;
    int span_idx;
    int sub_lane;
    int span_type;
    int lane_count;
    int32_t rel_x;
    int32_t rel_z;
    int armed;             /* 1 = pen log is active for the next call */
} s_tep_trace;

static FILE *s_tep_trace_fp = NULL;

static void tep_trace_ensure_open(void)
{
    if (s_tep_trace_fp) return;
    s_tep_trace_fp = fopen("tools/frida_csv/traffic_edge_pen_port.csv", "w");
    if (s_tep_trace_fp) {
        fprintf(s_tep_trace_fp,
            "sim_tick,call_id,slot,span_idx,sub_lane,span_type,lane_count,"
            "A_x,A_z,B_x,B_z,rel_x,rel_z,pen_orig\n");
        fflush(s_tep_trace_fp);
    }
}

static void tep_trace_arm(const char *call_id, int slot,
                          int span_idx, int sub_lane,
                          int span_type, int lane_count,
                          int32_t rel_x, int32_t rel_z)
{
    if (!g_td5.ini.trace_traffic_edge_pen) { s_tep_trace.armed = 0; return; }
    s_tep_trace.call_id    = call_id;
    s_tep_trace.slot       = slot;
    s_tep_trace.span_idx   = span_idx;
    s_tep_trace.sub_lane   = sub_lane;
    s_tep_trace.span_type  = span_type;
    s_tep_trace.lane_count = lane_count;
    s_tep_trace.rel_x      = rel_x;
    s_tep_trace.rel_z      = rel_z;
    s_tep_trace.armed      = 1;
}

static void tep_trace_emit(int32_t v0_x, int32_t v0_z,
                           int32_t v1_x, int32_t v1_z, int32_t pen)
{
    if (!s_tep_trace.armed) return;
    s_tep_trace.armed = 0;
    tep_trace_ensure_open();
    if (!s_tep_trace_fp) return;
    /* "sim_tick" here is the port's simulation_tick_counter (mirrors
     * g_simTick @ 0x004AADA0 captured by the Frida orig probe). */
    fprintf(s_tep_trace_fp,
        "%u,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
        (unsigned)g_td5.simulation_tick_counter,
        s_tep_trace.call_id,
        s_tep_trace.slot,
        s_tep_trace.span_idx,
        s_tep_trace.sub_lane,
        s_tep_trace.span_type,
        s_tep_trace.lane_count,
        (int)v0_x, (int)v0_z, (int)v1_x, (int)v1_z,
        (int)s_tep_trace.rel_x, (int)s_tep_trace.rel_z,
        (int)pen);
    fflush(s_tep_trace_fp);
}

/* Compute signed perpendicular distance from span edge for traffic containment.
 *
 * The original (ProcessActorSegmentTransition, ProcessActorRouteAdvance) builds
 * a normalized tangent vector from two span vertices via ConvertFloatVec4ToShortAngles
 * (x87 FPU normalization), then computes:
 *   pen = (world_offset . tangent) - car_size_projection
 *
 * Port uses atan2_fixed12 to get the angle, then cos/sin for the dot product.
 * [CONFIRMED @ 0x407390 + 0x407840: same vertex pair logic, result sign test < 0]
 *
 * v0_x,v0_z = first vertex (world-relative, from span origin)
 * v1_x,v1_z = second vertex (world-relative, from span origin)
 * car_origin_x, car_origin_z = actor pos minus span origin
 * car_half_w = *(int16_t*)(car_def + 0x0C)
 * car_half_l = *(int16_t*)(car_def + 0x08)
 * cos_hd, sin_hd = cos/sin of heading delta from route
 *
 * Returns (pen, edge_angle).  pen < 0 means outside the edge.
 */
/* Signed edge penetration for traffic containment — REWRITTEN 2026-05-26 to
 * match orig ProcessActorSegmentTransition @ 0x00407390 (was a divergent
 * parallel-projection re-impl that fired spuriously every tick → traffic
 * locked, see reference_traffic_steer_saturation_2026-05-26).
 *
 * Edge runs A→B. Orig builds the rotated OUTWARD NORMAL
 *   normal = (B.z - A.z, A.x - B.x)   [edge tangent rotated -90°]
 * and normalizes it to length 4096 (ConvertFloatVec4ToShortAngles @ 0x0042CDB0
 * = FPU x²+z²→FSQRT→4096/len→FMUL→ftol). Penetration is the signed
 * perpendicular distance of the actor from the edge, minus the heading-folded
 * car half-extent:
 *   rel  = (actor_world>>8 - origin - A)
 *   pen  = rel·normal - (sin_hd*half_w + cos_hd*half_l)
 * pen < 0 ⇒ the car has crossed the edge → containment push.
 * out_edge_angle = AngleFromVector12(B.z-A.z, B.x-A.x) for the push direction.
 *
 * Scale: rel is world units, normal is ×4096, so rel·normal is the perp
 * distance ×4096; car_extent is (sin/cos ×4096)·half so also ×4096. No shift —
 * matches orig (which leaves both at ×4096 and tests sign only). */
static int32_t traffic_edge_pen(int32_t a_x, int32_t a_z,
                                 int32_t b_x, int32_t b_z,
                                 int32_t rel_x, int32_t rel_z,
                                 int32_t car_half_w, int32_t car_half_l,
                                 int32_t cos_hd, int32_t sin_hd,
                                 uint32_t *out_edge_angle)
{
    if (out_edge_angle)
        *out_edge_angle = (uint32_t)(atan2_fixed12(b_z - a_z, b_x - a_x) & 0xFFF);

    /* Outward edge normal, normalized to length 4096. */
    double dnx = (double)(b_z - a_z);
    double dnz = (double)(a_x - b_x);
    double len = sqrt(dnx * dnx + dnz * dnz);
    int32_t nx = 0, nz = 0;
    if (len >= 1.0) {
        nx = (int32_t)(dnx * 4096.0 / len);
        nz = (int32_t)(dnz * 4096.0 / len);
    }

    int32_t proj       = rel_z * nz + rel_x * nx;
    int32_t car_extent = sin_hd * car_half_w + cos_hd * car_half_l;
    int32_t pen        = proj - car_extent;

    /* [TRACE 2026-05-24 traffic-edge-pen-cluster] mirror Frida orig probe */
    tep_trace_emit(a_x, a_z, b_x, b_z, pen);

    return pen;
}

/* ProcessActorSegmentTransition — port of 0x00407390.
 *
 * Tests the inner (sub_lane < 2) and outer (sub_lane >= lane_count-2) edges
 * of the current span segment and applies containment if the car is outside.
 * [CONFIRMED @ 0x00407390: called from UpdateTrafficActorMotion 0x443ED0]
 *
 * The car_definition_ptr carries:
 *   *(int16_t*)(car_def + 0x08) = half-length equivalent  [CONFIRMED @ 0x407424]
 *   *(int16_t*)(car_def + 0x0C) = half-width equivalent   [CONFIRMED @ 0x407420]
 *
 * [L4 — Frida-pending] Multiple algorithmic divergences vs orig
 * 0x00407390. Documented here per L5 promotion audit 2026-05-18.
 * Resolution unblocks on Frida-traced traffic-wall contact comparison
 * (see todo_traffic_route_advance_lane_count_byte_2026-05-18.md for the
 * unblock criterion). NOT ARCH-DIVERGENCE — these are genuine math gaps:
 *
 *   1) DOT-PRODUCT SEMANTIC MISMATCH (the dominant divergence)
 *      Orig builds the *rotated outward normal* of edge A→B via
 *      `local_8 = (B.z - A.z, 0, A.x - B.x)` and normalizes it to length
 *      4096 via ConvertFloatVec4ToShortAngles @ 0x0042CDB0 (FPU
 *      x²+y²+z²→FSQRT→4096/len→FMUL→ftol). It then computes
 *      `pen = ((world - origin - A) . local_8) - car_size_proj` — a
 *      *signed perpendicular distance* (geometric cross-product component
 *      of (rel) and the unit edge direction).
 *
 *      Port computes `tan = (cos(atan2(tdz,tdx)), sin(atan2(tdz,tdx)))`
 *      and dots `(rel . tan)` — a *parallel projection along the edge*,
 *      NOT the perpendicular distance. These give different signs in
 *      general; only the angle passed to ApplySimpleTrackSurfaceForce
 *      remains the same.
 *
 *   2) REFERENCE-POINT MISMATCH
 *      Orig dots from psVar1 (the edge endpoint), port dots from the span
 *      origin. The car-size projection subtraction is identical but the
 *      pre-subtraction term is offset by `-psVar1`.
 *
 *   3) VERTEX-PAIR SWAP (OUTER TEST)
 *      Orig outer-test uses `psVar1 = vertex(strip[+0x04] + DAT_004631a0
 *      + nibble) = left_vertex_index+offset+nibble`, port uses
 *      `right_vertex_index+offset+outer_sub`. The port comment at line 5254
 *      claims "strip[+0x04] = right_vertex_index" but td5_types.h:383-384
 *      assigns strip[+0x04] = left_vertex_index. The swap is internally
 *      inconsistent and likely contributes a sign flip on outer-edge tests.
 *
 *   4) (PARTIAL CLOSE) lane_count byte source (+0x03 not +0x01) — fixed
 *      2026-05-18 commit ef0e862. Outer_sub mapping (= lane_count not
 *      lane_count-1) — fixed same commit. These two are no longer
 *      divergent.
 *
 * Why this still ships:
 *
 *   The traffic-bias-clamp slot gate shipped 2026-05-17 (commit 79023df F1)
 *   bounds the pre-loop to slots 0..5 (TD5_MAX_RACER_SLOTS), preventing
 *   traffic from cascading into the racer steering pipeline. The
 *   remaining edge-test errors fire only at traffic-wall contact ticks
 *   which are rare enough that no current Wave3 scenario isolates this
 *   path. San Francisco scenario (slot 6+ traffic, see
 *   reference_san_francisco_ai_completion_2026-05-16.md) is the closest
 *   isolated test bed but no traffic-wall contacts have been Frida-traced
 *   yet.
 *
 * Unblock criterion (per todo): Frida hook at orig 0x004073D8 + port
 * traffic_edge_pen entrypoint capturing (slot, span, sub_lane, type, A.x,
 * A.z, B.x, B.z, rel.x, rel.z, pen_orig, pen_port) at the first
 * traffic-wall contact tick. If `sign(pen_orig) == sign(pen_port)`
 * across all sampled ticks the divergence is benign and these notes
 * upgrade to ARCH-DIVERGENCE; otherwise file as a fix-required regression.
 *
 * NOTE: confirmed byte-faithful for: lane_count byte source (+0x03,
 * fixed 2026-05-18), outer_sub mapping (= lane_count, fixed 2026-05-18),
 * inner/outer trigger conditions (sub_lane < 2 / sub_lane >= lane_count-2),
 * DAT_004631a0 outer-vertex LUT (k_outer_left_offsets matches orig dump),
 * ApplySimpleTrackSurfaceForce wiring + DecayUltimateVariantTimer wiring.
 */
void process_traffic_segment_edge(TD5_Actor *actor, int slot)
{
    TD5_StripSpan *sp = td5_track_get_span((int)actor->track_span_raw);
    if (!sp) return;
    int32_t *car_def = (int32_t *)actor->car_definition_ptr;
    if (!car_def) return;

    int span_type    = (int)sp->span_type;
    if (span_type < 0 || span_type >= 12) return;

    int sub_lane  = (int)(int8_t)actor->track_sub_lane_index;
    /* [CONFIRMED @ 0x004073B0: `uVar10 = *(byte*)(strip+3) & 0xf`] — orig
     * reads byte +0x03 (geometry_metadata low nibble = lane_count), NOT
     * byte +0x01 (surface_attribute). Use the helper that mirrors orig. */
    int lane_count = (int)(((const uint8_t *)sp)[3] & 0x0F);

    /* Heading delta for car projection */
    uint32_t hd      = traffic_route_heading_delta(slot);
    hd &= 0x7FF;
    if (hd > 0x3FF) hd = 0x7FF - hd;
    int32_t cos_hd = cos_fixed12((int32_t)hd);
    int32_t sin_hd = sin_fixed12((int32_t)hd);

    int16_t car_half_w = (int16_t)((*(uint32_t *)((uint8_t *)car_def + 0x0C)) & 0xFFFF);
    int16_t car_half_l = (int16_t)((*(uint32_t *)((uint8_t *)car_def + 0x08)) & 0xFFFF);

    /* Span origin in world space (24.8 FP) */
    int32_t orig_x = sp->origin_x;
    int32_t orig_z = sp->origin_z;

    /* Actor position relative to span origin */
    int32_t rel_x = (actor->world_pos.x >> 8) - orig_x;
    int32_t rel_z = (actor->world_pos.z >> 8) - orig_z;

    /* Inner edge test: sub_lane < 2  [CONFIRMED @ 0x407394: if (iVar12 < 2)]
     * [FIX 2026-05-26] Orig 0x004073A4-0x004073B0: A=psVar1=right_vertex(+6),
     * B=psVar2=left_vertex(+4), NO sub_lane offset (sub_lane is only the gate).
     * Prior port added sub_lane to both indices (wrong). */
    if (sub_lane < 2) {
        TD5_StripVertex *A = td5_track_get_vertex((int)sp->right_vertex_index);
        TD5_StripVertex *B = td5_track_get_vertex((int)sp->left_vertex_index);
        if (!A || !B) goto outer_test;

        /* [TRACE 2026-05-24 traffic-edge-pen-cluster] arm call_id=1a (inner) */
        tep_trace_arm("1a", slot, (int)actor->track_span_raw, sub_lane,
                       span_type, lane_count, rel_x, rel_z);

        /* rel measured from edge endpoint A (orig dots from psVar1). */
        int32_t arel_x = rel_x - (int32_t)A->x;
        int32_t arel_z = rel_z - (int32_t)A->z;
        uint32_t edge_angle;
        int32_t pen = traffic_edge_pen(
            (int32_t)A->x, (int32_t)A->z,
            (int32_t)B->x, (int32_t)B->z,
            arel_x, arel_z,
            (int32_t)car_half_w, (int32_t)car_half_l,
            cos_hd, sin_hd,
            &edge_angle);

        if (pen < 0) {
            apply_simple_track_surface_force(actor, edge_angle, pen);
            /* DecayUltimateVariantTimer [CONFIRMED @ 0x0040A440]:
             * encounter mode 4 erodes the actor's clean_driving_score by 1
             * per wall-contact tick, but only while the actor is still racing. */
            if (g_td5.special_encounter_enabled == 4 && actor->finish_time == 0) {
                if (actor->clean_driving_score > 0) actor->clean_driving_score -= 1;
                if (actor->clean_driving_score < 0) actor->clean_driving_score  = 0;
            }
            return;
        }
    }

outer_test:
    /* Outer edge test: sub_lane >= lane_count - 2  [CONFIRMED @ 0x407462]
     * [FIX 2026-05-26] Orig outer 0x4074B4-0x4074F4:
     *   A = psVar1 = vtx[DAT_004631a0[type] + left_vertex_index(+4) + lane_count]
     *   B = psVar2 = vtx[DAT_004631a4[type](=0) + right_vertex_index(+6) + lane_count]
     * Prior port had LEFT/RIGHT swapped. DAT_004631a0 = k_outer_left_offsets. */
    if (sub_lane >= lane_count - 2) {
        static const int8_t k_outer_left_offsets[12] = {
            0, 0, -1, -1, -2, 0, -1, 0, -1, 0, -1, -2
        };
        int a_idx = k_outer_left_offsets[span_type] + (int)sp->left_vertex_index  + lane_count;
        int b_idx = (int)sp->right_vertex_index + lane_count;   /* DAT_004631a4 = 0 */
        TD5_StripVertex *A = td5_track_get_vertex(a_idx);
        TD5_StripVertex *B = td5_track_get_vertex(b_idx);
        if (!A || !B) return;

        /* [TRACE 2026-05-24 traffic-edge-pen-cluster] arm call_id=1b (outer) */
        tep_trace_arm("1b", slot, (int)actor->track_span_raw, sub_lane,
                       span_type, lane_count, rel_x, rel_z);

        int32_t arel_x = rel_x - (int32_t)A->x;
        int32_t arel_z = rel_z - (int32_t)A->z;
        uint32_t edge_angle;
        int32_t pen = traffic_edge_pen(
            (int32_t)A->x, (int32_t)A->z,
            (int32_t)B->x, (int32_t)B->z,
            arel_x, arel_z,
            (int32_t)car_half_w, (int32_t)car_half_l,
            cos_hd, sin_hd,
            &edge_angle);

        if (pen < 0) {
            apply_simple_track_surface_force(actor, edge_angle, pen);
            /* DecayUltimateVariantTimer [CONFIRMED @ 0x0040A440] — same as inner-edge */
            if (g_td5.special_encounter_enabled == 4 && actor->finish_time == 0) {
                if (actor->clean_driving_score > 0) actor->clean_driving_score -= 1;
                if (actor->clean_driving_score < 0) actor->clean_driving_score  = 0;
            }
        }
    }
}

/* ProcessActorRouteAdvance — port of 0x00407840.
 *
 * Tests the forward end of the current span (the actor is near the end of the
 * span ring) and applies containment if the car has gone past the forward edge.
 * [CONFIRMED @ 0x00407840: called from UpdateTrafficActorMotion 0x443ED0]
 *
 * Only fires when track_span_raw == g_trackTotalSpanCount - 1 (last span).
 * [CONFIRMED @ 0x407879: `if (actor->track_span == DAT_00483550 + -1)`]
 *
 * [CONFIRMED @ 0x00407840] Byte-faithful with orig ProcessActorRouteAdvance.
 *   - Wrap-span strip indexed at DAT_00483550 (total_spans), matching the
 *     port's `td5_track_get_span(0)` ring-wrap.
 *   - lane_count byte source = byte +0x03 (fixed 2026-05-18, was +0x01).
 *   - Vertex pair = single rail from base to base+lane_nibble (fixed
 *     2026-05-18, was both-rails+sub_lane).
 *
 * Algorithm:
 *   uVar6 = strip[wrap].left_vertex_index
 *   psVar2 = vertex_pool[uVar6 + (strip[wrap+3] & 0xF)]   ; left + lane_nibble
 *   psVar3 = vertex_pool[uVar6]                            ; left base
 *   pen    = ((world - psVar2) . (rotated_edge_normal)) - car_size
 */
void process_traffic_route_advance(TD5_Actor *actor, int slot)
{
    int total_spans = td5_track_get_span_count();
    if (total_spans <= 0) return;

    /* Only fires at the last span [CONFIRMED @ 0x407879] */
    int span_idx = (int)actor->track_span_raw;
    if (span_idx != total_spans - 1) return;

    /* Last span in the ring — test forward edge using vertices of span 0 wrap.
     * [CONFIRMED @ 0x40787E: uses span_at(DAT_00483550) i.e. last+1 = wrap to 0] */
    TD5_StripSpan *sp_last = td5_track_get_span(span_idx);
    if (!sp_last) return;
    TD5_StripSpan *sp_wrap = td5_track_get_span(0);  /* wraps to span 0 */
    if (!sp_wrap) return;

    int32_t *car_def = (int32_t *)actor->car_definition_ptr;
    if (!car_def) return;

    int16_t car_half_w = (int16_t)((*(uint32_t *)((uint8_t *)car_def + 0x0C)) & 0xFFFF);
    int16_t car_half_l = (int16_t)((*(uint32_t *)((uint8_t *)car_def + 0x08)) & 0xFFFF);

    uint32_t hd = traffic_route_heading_delta(slot);
    hd &= 0x7FF;
    if (hd > 0x3FF) hd = 0x7FF - hd;
    int32_t cos_hd = cos_fixed12((int32_t)hd);
    int32_t sin_hd = sin_fixed12((int32_t)hd);

    /* [CONFIRMED @ 0x004078B7] orig reads strip[wrap+3]&0xF = lane_nibble. */
    int wrap_lanes = (int)(((const uint8_t *)sp_wrap)[3] & 0x0F);

    /* [CONFIRMED @ 0x004078A4-0x004078BA] orig uses ONE rail of the wrap span:
     *   uVar6 = sp_wrap->left_vertex_index
     *   psVar2 = vertex(uVar6 + lane_nibble)    ; far endpoint
     *   psVar3 = vertex(uVar6)                  ; near endpoint
     * Port maps: vl = psVar2 (vertex at left+lane_nibble),
     *            vr = psVar3 (vertex at left base). traffic_edge_pen will
     * compute the tangent angle = atan2(vr.z - vl.z, vr.x - vl.x) =
     * angle of edge `(left+nibble) -> left`, equivalent to orig's
     * AngleFromVector12(psVar3.z - psVar2.z, psVar3.x - psVar2.x). */
    int li_idx = (int)sp_wrap->left_vertex_index + wrap_lanes;  /* psVar2 */
    int ri_idx = (int)sp_wrap->left_vertex_index;                /* psVar3 */
    TD5_StripVertex *vl = td5_track_get_vertex(li_idx);
    TD5_StripVertex *vr = td5_track_get_vertex(ri_idx);
    if (!vl || !vr) return;
    /* Note: actor->track_sub_lane_index is intentionally unread here —
     * orig uses single-rail-across-full-width, NOT sub_lane indexed. */

    /* Actor position relative to wrap span origin */
    int32_t rel_x = (actor->world_pos.x >> 8) - sp_wrap->origin_x;
    int32_t rel_z = (actor->world_pos.z >> 8) - sp_wrap->origin_z;

    /* [TRACE 2026-05-24 traffic-edge-pen-cluster] arm call_id=3 (route-adv) */
    tep_trace_arm("3", slot, (int)actor->track_span_raw,
                   (int)(int8_t)actor->track_sub_lane_index,
                   (int)sp_wrap->span_type, wrap_lanes, rel_x, rel_z);

    uint32_t edge_angle;
    int32_t pen = traffic_edge_pen(
        (int32_t)vl->x, (int32_t)vl->z,
        (int32_t)vr->x, (int32_t)vr->z,
        rel_x, rel_z,
        (int32_t)car_half_w, (int32_t)car_half_l,
        cos_hd, sin_hd,
        &edge_angle);

    if (pen < 0) {
        apply_simple_track_surface_force(actor, edge_angle, pen);
    }
}

/* ProcessActorForwardCheckpointPass — port of 0x004076C0.
 *
 * Forward-sentinel counterpart of process_traffic_route_advance.
 * Tests whether traffic has crossed the forward boundary span (near the
 * START of the ring, not the end) and applies a push impulse back into
 * the playable region.
 *
 * Key differences from ProcessActorRouteAdvance (0x407840):
 *   - Trigger:  span == fwd_sentinel + 1  (vs rev_sentinel - 1)
 *   - Strip:    strip[fwd_sentinel]        (vs strip[rev_sentinel])
 *   - Vertices: psVar2=left_base, psVar3=left_base+lane_count (NOT reversed)
 *
 * [CONFIRMED @ 0x4076C0] if (*(short*)(actor+0x80) == DAT_00483954 + 1)
 * [CONFIRMED @ 0x443ED0] called after ProcessActorRouteAdvance, before
 *                         ProcessActorSegmentTransition.
 */
void process_traffic_forward_checkpoint_pass(TD5_Actor *actor, int slot)
{
    int fwd_sentinel = td5_track_get_fwd_sentinel();
    if (fwd_sentinel < 0) return;  /* {-1} placeholder disables handler */

    /* Trigger: actor is one span PAST the forward sentinel */
    int span_idx = (int)actor->track_span_raw;
    if (span_idx != fwd_sentinel + 1) return;

    /* Use the strip at fwd_sentinel for the boundary edge
     * [CONFIRMED @ 0x4076E8: iVar1 = g_trackStripRecords + DAT_00483954 * 0x18] */
    TD5_StripSpan *sp = td5_track_get_span(fwd_sentinel);
    if (!sp) return;

    int32_t *car_def = (int32_t *)actor->car_definition_ptr;
    if (!car_def) return;

    int16_t car_half_w = (int16_t)((*(uint32_t *)((uint8_t *)car_def + 0x0C)) & 0xFFFF);
    int16_t car_half_l = (int16_t)((*(uint32_t *)((uint8_t *)car_def + 0x08)) & 0xFFFF);

    uint32_t hd = traffic_route_heading_delta(slot);
    hd &= 0x7FF;
    if (hd > 0x3FF) hd = 0x7FF - hd;
    int32_t cos_hd = cos_fixed12((int32_t)hd);
    int32_t sin_hd = sin_fixed12((int32_t)hd);

    /* Vertex ordering: NOT reversed (psVar2=left_base, psVar3=left_base+lane_count)
     * [CONFIRMED @ 0x407708-0x40771C in 0x4076C0]:
     *   uVar6 = left_vertex_index
     *   psVar2 = vertex[uVar6]                 (left base, NO sub_lane offset)
     *   psVar3 = vertex[uVar6 + lane_count]    (right end)
     * Compare ProcessActorRouteAdvance which swaps: psVar2=right, psVar3=left.
     *
     * [FIX 2026-06-01] li_idx previously added `+ sub` (the actor's sub_lane) to
     * the NEAR vertex — a divergence the original lacks. Independently re-confirmed
     * via Ghidra (0x407776-0x40777B): the near vertex is vertex[left_vertex_index],
     * and the actor sub_lane is NEVER read in 0x4076C0 (the only addend anywhere is
     * the lane-count nibble on the FAR vertex). The inner/outer
     * ProcessActorSegmentTransition path was already corrected 2026-05-26 (sub_lane
     * is only the edge-gate there); this forward-checkpoint path was the last site
     * still adding sub. Dropped — `sub` is retained only for the trace arg below. */
    int sub = (int)(int8_t)actor->track_sub_lane_index;
    if (sub < 0) sub = 0;
    int lane_count = (int)(sp->surface_attribute & 0xF);
    if (sub >= lane_count) sub = lane_count - 1;

    int li_idx = (int)sp->left_vertex_index;               /* psVar2 = left base (orig: NO sub) */
    int ri_idx = (int)sp->left_vertex_index + lane_count;  /* psVar3 = end vertex (NOT right_vertex_index) */

    TD5_StripVertex *vl = td5_track_get_vertex(li_idx);
    TD5_StripVertex *vr = td5_track_get_vertex(ri_idx);
    if (!vl || !vr) return;

    /* Actor position relative to sentinel span origin */
    int32_t rel_x = (actor->world_pos.x >> 8) - sp->origin_x;
    int32_t rel_z = (actor->world_pos.z >> 8) - sp->origin_z;

    /* [TRACE 2026-05-24 traffic-edge-pen-cluster] arm call_id=2 (fwd-cp) */
    tep_trace_arm("2", slot, (int)actor->track_span_raw, sub,
                   (int)sp->span_type, lane_count, rel_x, rel_z);

    uint32_t edge_angle;
    int32_t pen = traffic_edge_pen(
        (int32_t)vl->x, (int32_t)vl->z,
        (int32_t)vr->x, (int32_t)vr->z,
        rel_x, rel_z,
        (int32_t)car_half_w, (int32_t)car_half_l,
        cos_hd, sin_hd,
        &edge_angle);

    if (pen < 0) {
        apply_simple_track_surface_force(actor, edge_angle, pen);
        /* Original calls UpdateTrafficVehiclePose again after push;
         * port's integrate_traffic_pose runs at end of tick — skip redundant rebuild. */
    }
}

/* ========================================================================
 * Traffic Pose Integration: UpdateTrafficVehiclePose (0x443CF0)
 *
 * Traffic (slots 6-11) NEVER goes through IntegrateVehiclePoseAndContacts.
 * Instead the original:
 *   1. Integrates X/Z from velocity (NO gravity, NO Y velocity)
 *   2. Updates track position from new XZ
 *   3. Sets Y absolutely from barycentric track contact height
 *   4. Converts euler accumulators to display angles + rotation matrix
 *   5. Computes render position
 * [CONFIRMED @ 0x443ED0 — no call to 0x405E80 for traffic]
 * ======================================================================== */

void integrate_traffic_pose(TD5_Actor *actor)
{
    /* XZ + yaw integration is done by td5_physics_update_traffic
     * (inside the IntegrateVehicleFrictionForces port, matching original
     * @ 0x00443CBF/CCD/CD7). This function handles only Y ground-snap
     * and display-angle derivation, matching UpdateTrafficVehiclePose
     * @ 0x00443CF0. */

    /* Update chassis track position from new world XZ.
     * [CONFIRMED @ 0x443D40: UpdateTrafficVehiclePose calls UpdateActorTrackPosition] */
    {
        td5_track_update_actor_position(actor);

        int max_span = td5_track_get_span_count();
        if (max_span > 0 && actor->track_span_raw >= (uint16_t)max_span)
            actor->track_span_raw = (uint16_t)(max_span - 1);
    }

    /* Refresh heading_normal at +0x290 after the chassis-walker resolves
     * the new span. [CONFIRMED @ 0x00443d3d: UpdateTrafficVehiclePose
     * calls ComputeActorHeadingFromTrackSegment with LEA [esi+0x290].]
     * [precise-port pilot 00445B90, 2026-05-14] */
    td5_track_compute_runtime_heading_normal(actor);

    /* 4. Set Y from barycentric track contact height + car height offset.
     * [CONFIRMED @ 0x443D58 + 0x445A1C: ComputeActorTrackContactNormalExtended
     *  writes world_pos_y = barycentric_height + origin_y * 0x100]
     * [CONFIRMED @ 0x443D7C-0x443D8E: adds *(int16_t*)(car_definition + 0x86) << 8
     *  to world_pos_y after contact height — lifts car above track surface] */
    {
        int span  = (int)actor->track_span_raw;
        int lane  = (int)actor->track_sub_lane_index;
        int16_t surface_normal[3] = {0, 1, 0};  /* default: flat up */
        int32_t ground_y = td5_track_compute_contact_height_with_normal(
            span, lane, actor->world_pos.x, actor->world_pos.z, surface_normal);

        /* Car height offset: signed int16 at car_definition + 0x86, shifted left 8.
         * Original (Y-DOWN): `ADD [world_pos_y], ECX` with ECX = MOVSX cdef[0x86] << 8.
         * Port runs Y-UP so the equivalent lift is a subtract.
         *
         * compute_suspension_envelope @ 0x0042F8F4 writes cdef[0x86]:
         *   - Racer  (slot<6): negative ((cd[0x42]-cd[0x82]) * -0.7070...)
         *   - Traffic(slot>=6): positive (mesh max_y from vertex extents)
         *
         * This function is traffic-only, so cdef[0x86] is normally positive.
         * Subtracting a positive value would SINK the traffic vehicle into
         * the ground (user-reported "traffic clipping through ground"
         * 2026-05-26). Use the magnitude so the lift direction matches
         * racers regardless of the sign that compute_envelope chose. */
        if (actor->car_definition_ptr) {
            int32_t height_offset = (int32_t)CDEF_S(actor, CDEF_HEIGHT_OFFSET) << 8;
            if (height_offset > 0) height_offset = -height_offset;
            ground_y -= height_offset;
        }

        actor->world_pos.y = ground_y;

        /* 5. Compute roll/pitch display angles from surface normal + suspension.
         * [CONFIRMED @ 0x443E00-0x443EC4: UpdateTrafficVehiclePose computes
         *  roll/pitch from surface normal rotated by yaw, with suspension correction.
         *  Roll  = AngleFromVector12(-rotated_z, -normal_y) - (susp[1] >> 8)
         *  Pitch = AngleFromVector12( rotated_x,  mag_xz)  + (susp[0] >> 8)
         *  where susp[0] = wheel_suspension_pos[0] (lateral-driven axis)
         *        susp[1] = wheel_suspension_pos[1] (longitudinal-driven axis)] */
        int32_t nx = (int32_t)surface_normal[0];
        int32_t ny = (int32_t)surface_normal[1];
        int32_t nz = (int32_t)surface_normal[2];

        /* Rotate normal XZ by yaw */
        int32_t yaw12 = (actor->euler_accum.yaw >> 8) & 0xFFF;
        int32_t cy = cos_fixed12(yaw12);
        int32_t sy = sin_fixed12(yaw12);
        int32_t rotated_x = (nx * cy - nz * sy) >> 12;
        int32_t rotated_z = (nx * sy + nz * cy) >> 12;

        /* Roll from surface normal + longitudinal suspension correction.
         * Original formula: AngleFromVector12(-rotated_z, -normal_y).
         * triangle_height produces Y-down normals matching the original
         * (ny < 0 for "up"), so we negate ny to get roll=0 on flat ground. */
        int32_t roll_from_normal = atan2_fixed12(-rotated_z, -ny);
        int32_t susp_roll_corr = actor->wheel_suspension_pos[1] >> 8;
        actor->display_angles.roll = (int16_t)((roll_from_normal - susp_roll_corr) & 0xFFF);

        /* Pitch from surface normal + lateral suspension correction. mag_xz
         * is the sqrt of (rotated_x^2 + ny^2) regardless of ny's sign. */
        int32_t mag_xz = td5_isqrt(rotated_x * rotated_x + ny * ny);
        int32_t pitch_from_normal = atan2_fixed12(rotated_x, mag_xz);
        int32_t susp_pitch_corr = actor->wheel_suspension_pos[0] >> 8;
        actor->display_angles.pitch = (int16_t)((pitch_from_normal + susp_pitch_corr) & 0xFFF);
    }

    /* Yaw from euler accumulator (only axis using accumulators for traffic).
     * No 0xFFF mask -- original (0x4063AA-style) truncates via int16 store
     * only; mask flipped sign bit for negative accumulators. Matches the
     * unmasked fix at 5978 for the player/AI integrator path. */
    actor->display_angles.yaw = (int16_t)(actor->euler_accum.yaw >> 8);

    /* Keep vertical dynamics zeroed — traffic has no gravity or suspension */
    actor->linear_velocity_y = 0;

    /* 6. Build rotation matrix from display angles (YXZ order, same as racers) */
    {
        int32_t roll_a  = actor->display_angles.roll  & 0xFFF;
        int32_t yaw_a   = actor->display_angles.yaw   & 0xFFF;
        int32_t pitch_a = actor->display_angles.pitch  & 0xFFF;

        int32_t cr = cos_fixed12(roll_a);
        int32_t sr = sin_fixed12(roll_a);
        int32_t cyy = cos_fixed12(yaw_a);
        int32_t syy = sin_fixed12(yaw_a);
        int32_t cp = cos_fixed12(pitch_a);
        int32_t sp = sin_fixed12(pitch_a);

        float s = 1.0f / 4096.0f;

        actor->rotation_matrix.m[0] = (float)(((sp * syy >> 12) * sr >> 12) + ((cp * cyy) >> 12)) * s;
        actor->rotation_matrix.m[1] = (float)(((cp * syy >> 12) * sr >> 12) - ((sp * cyy) >> 12)) * s;
        actor->rotation_matrix.m[2] = (float)((syy * cr) >> 12) * s;

        actor->rotation_matrix.m[3] = (float)((sp * cr) >> 12) * s;
        actor->rotation_matrix.m[4] = (float)((cp * cr) >> 12) * s;
        actor->rotation_matrix.m[5] = (float)(-sr) * s;

        actor->rotation_matrix.m[6] = (float)(((sp * cyy >> 12) * sr >> 12) - ((cp * syy) >> 12)) * s;
        actor->rotation_matrix.m[7] = (float)(((cp * cyy >> 12) * sr >> 12) + ((sp * syy) >> 12)) * s;
        actor->rotation_matrix.m[8] = (float)((cyy * cr) >> 12) * s;
    }

    /* 7. Compute render position (world_pos / 256 as float) */
    actor->render_pos.x = (float)actor->world_pos.x * (1.0f / 256.0f);
    actor->render_pos.y = (float)actor->world_pos.y * (1.0f / 256.0f);
    actor->render_pos.z = (float)actor->world_pos.z * (1.0f / 256.0f);

    /* 8. Refresh the 4 wheel-contact probes (+0x90..0xbc) for the shadow.
     * [FIX 2026-05-26 r2 traffic-shadow-misaligned]
     * render_vehicle_shadow_quad (td5_render.c) reads probe_FL/FR/RL/RR as the
     * 4 ground corners of the shadow decal. The racer path computes these in
     * td5_physics_refresh_wheel_contacts (0x00403720) by transforming the
     * car-def BODY-CORNER offsets (4x int16[4] at the head of car_definition_
     * ptr) through the full render matrix, then OVERWRITING each corner Y with
     * the barycentric ground-contact height at that XZ. Traffic never runs
     * 0x00403720, so the r1 fix here hand-rolled a (±half_w,±half_l) box at
     * world_pos.y — which (a) used the wrong axis fields, rendering the decal
     * PERPENDICULAR to the car, and (b) sat at the body-CENTRE height, so the
     * shadow floated above the road and clipped through the body. Mirror the
     * proven racer body-corner pass exactly: real corners via render matrix +
     * ground-contact Y. */
    if (actor->car_definition_ptr) {
        TD5_Vec3_Fixed *body_pos[4] = {
            &actor->probe_FL, &actor->probe_FR, &actor->probe_RL, &actor->probe_RR
        };
        const int16_t *cardef_corners = (const int16_t *)actor->car_definition_ptr;

        float matrix[12];
        for (int j = 0; j < 9; j++) matrix[j] = actor->rotation_matrix.m[j];
        matrix[9]  = actor->render_pos.x;
        matrix[10] = actor->render_pos.y;
        matrix[11] = actor->render_pos.z;

        int max_sp = td5_track_get_span_count();
        for (int i = 0; i < 4; i++) {
            const int16_t corner_off[3] = {
                cardef_corners[i * 4 + 0],
                cardef_corners[i * 4 + 1],
                cardef_corners[i * 4 + 2],
            };
            int32_t world[3];
            td5_transform_short_vec3_by_render_matrix_rounded(corner_off, world, matrix);
            body_pos[i]->x = world[0] << 8;
            body_pos[i]->y = world[1] << 8;
            body_pos[i]->z = world[2] << 8;

            td5_track_update_probe_position(&actor->body_probes[i],
                                            body_pos[i]->x, body_pos[i]->z);
            if (max_sp > 0 && actor->body_probes[i].span_index >= (int16_t)max_sp)
                actor->body_probes[i].span_index = (int16_t)(max_sp - 1);
            td5_track_compute_probe_contact_vertices(&actor->body_probes[i]);

            int probe_span = actor->body_probes[i].span_index;
            int probe_lane = actor->body_probes[i].sub_lane_index;
            if (probe_span >= 0 && probe_span < max_sp) {
                body_pos[i]->y = td5_track_compute_contact_height_with_normal(
                    probe_span, probe_lane, body_pos[i]->x, body_pos[i]->z, NULL);
            }
        }
    }

    /* Log once per 60 frames per slot for diagnostics */
    if ((actor->frame_counter % 60u) == 0u) {
        TD5_LOG_I(LOG_TAG, "traffic_pose: slot=%d span=%d y=%d roll=%d pitch=%d",
                  actor->slot_index, (int)actor->track_span_raw,
                  actor->world_pos.y,
                  (int)actor->display_angles.roll, (int)actor->display_angles.pitch);
    }
}

/* ========================================================================
 * T2: TransformTrackVertexByMatrix equivalent (@ 0x00446030)
 *
 * Derives display_angle_roll and display_angle_pitch from the four per-wheel
 * ground-contact positions (actor+0xF0, written by refresh_wheel_contacts)
 * plus the four per-wheel suspension deflections (actor+0x2DC).
 *
 * Formula is a literal transcription of 0x00446037..0x0044612F x86
 * (two independent Ghidra passes, see T2 research log). All arithmetic is
 * 32-bit signed. SAR by 8 after each intermediate matches SAR instructions
 * at 0x446089/0x44608E/0x44608F/0x4460F4/0x4460F7/0x446103.
 *
 * The write to roll uses dz (front/rear-driven axis per original's wheel
 * ordering), write to pitch uses dx (left/right-driven axis). Semantic
 * labels "roll"/"pitch" match the original's field layout at +0x208/+0x20C;
 * whether this corresponds to the conventional pitch/roll axes depends on
 * the original binary's wheel index convention (not verified).
 *
 * [2026-05-17 OPERAND-SOURCE AUDIT — closes body-roll TODO Item 1]
 * The IntegrateVehiclePoseAndContacts caller at 0x00405FEC pushes:
 *     PUSH &actor->wheel_suspension_pos_FL   ; param_3 (sp[])
 *     PUSH &actor->field_0xf0                ; param_2 (wheel_contact_pos)
 *     PUSH &actor->display_angle_roll        ; param_1 (out)
 * Disassembly of 0x00446030 reads `[ESI+4/+0x10/+0x1C/+0x28]` from param_2
 * (stride 12, offset +4) which is the Y component of each of the 4
 * wheel_contact_pos entries. The port's `wcp[1]/wcp[4]/wcp[7]/wcp[10]`
 * indices (int32_t pointer) map 1-to-1 to those byte offsets. The X/Z
 * cross-spans (wcp[0]/3/6/9 and wcp[2]/5/8/11) match the listing too. */
/* ======================================================================== */
void td5_physics_attitude_from_wheels(const TD5_Actor *actor,
                                             int16_t *out_roll,
                                             int16_t *out_pitch)
{
    const int32_t *wcp = (const int32_t *)&actor->wheel_contact_pos[0];
    const int32_t *sp  = actor->wheel_suspension_pos;

    /* Numerators (height spreads + suspension deflection contributions) */
    int32_t dx = wcp[1] - wcp[4] - wcp[10] + wcp[7]
               + sp[0] - sp[1] + sp[2] - sp[3];       /* pitch numerator */
    int32_t dz = wcp[1] + wcp[4] - wcp[7] - wcp[10]
               + sp[0] + sp[1] - sp[2] - sp[3];       /* roll numerator */

    /* Cross spans (hypotenuse arguments — X/Z separations between wheel pairs) */
    int32_t crAp = wcp[0] - wcp[3] + wcp[6] - wcp[9];    /* pitch X span */
    int32_t crBp = wcp[2] - wcp[5] + wcp[8] - wcp[11];   /* pitch Z span */
    int32_t crAr = wcp[0] + wcp[3] - wcp[6] - wcp[9];    /* roll X span  */
    int32_t crBr = wcp[2] + wcp[5] - wcp[8] - wcp[11];   /* roll Z span  */

    dx   >>= 8;
    dz   >>= 8;
    crAp >>= 8;
    crBp >>= 8;
    crAr >>= 8;
    crBr >>= 8;

    int32_t hyp_p = td5_isqrt(crAp * crAp + crBp * crBp);
    int32_t hyp_r = td5_isqrt(crAr * crAr + crBr * crBr);

    *out_pitch = (int16_t)(AngleFromVector12(-dx, hyp_p) & 0xFFF);
    *out_roll  = (int16_t)(AngleFromVector12(-dz, hyp_r) & 0xFFF);

    /* Diagnostic for [[todo-car-tilted-right-flat-surface-2026-05-19]]:
     * on flat ground all 4 wheel Y values are equal and sp[0..3]==0 →
     * dx=dz=0 → roll=pitch=0. If user reports persistent tilt, this log
     * surfaces which input is biased. Slot-0 only, rate-limited. */
    if (actor->slot_index == 0) {
        static int s_attitude_log_div = 0;
        if ((++s_attitude_log_div % 120) == 0) {
            TD5_LOG_I(LOG_TAG,
                "attitude_from_wheels slot0: roll=%d pitch=%d "
                "dz_pre=%d dx_pre=%d sp=[%d,%d,%d,%d] "
                "wcp_y=[%d,%d,%d,%d]",
                (int)*out_roll, (int)*out_pitch,
                (int)dz, (int)dx,
                (int)sp[0], (int)sp[1], (int)sp[2], (int)sp[3],
                (int)wcp[1], (int)wcp[4], (int)wcp[7], (int)wcp[10]);
        }
    }
}

/* Helper: signed-wrap a 12-bit delta into [-2048, +2047], matching the
 * `(((new - old - 0x800) & 0xFFF) - 0x800)` pattern at 0x00405ED8-0x00405F10. */
static inline int32_t td5_physics_wrap_angle_delta(int32_t new_angle, int32_t old_angle) {
    return (int32_t)(((new_angle - old_angle - 0x800) & 0xFFF) - 0x800);
}

/* ========================================================================
 * Integration: IntegrateVehiclePoseAndContacts (0x405E80)
 *
 * Core integration step: gravity -> velocity -> position -> euler -> matrix.
 * Racers only (slots 0-5). Traffic uses integrate_traffic_pose above.
 * ======================================================================== */

void td5_physics_integrate_pose(TD5_Actor *actor)
{
    /* Pilot precise-port trace — snapshot inputs at entry (slot 0 only). */

    /* Save previous Y for suspension delta */
    actor->prev_frame_y_position = actor->world_pos.y;

    /* T2: Save pre-integration display roll/pitch for delta-based angular
     * velocity update. Matches IntegrateVehiclePoseAndContacts @ 0x00405E80
     * — angular_velocity_roll/pitch are recomputed from wrap(new - old)*256
     * each tick after TransformTrackVertexByMatrix. */
    int16_t t2_old_disp_roll  = actor->display_angles.roll;
    int16_t t2_old_disp_pitch = actor->display_angles.pitch;

    /* 1. Apply gravity to velocity Y — UNCONDITIONAL, matching
     * IntegrateVehiclePoseAndContacts @ 0x00405EDE. The suspension_response
     * tail call (0x004057F0) adds back `(g + Σ(wheel_vel·normal)/2/n)` at
     * 0x00405A49 — for stationary grounded cars the Σ term is 0 and the
     * two writes cancel cleanly. */
    actor->linear_velocity_y -= g_gravity_constant;

    /* 2. Integrate angular velocity into euler accumulators */
    actor->euler_accum.roll  += actor->angular_velocity_roll;
    actor->euler_accum.yaw   += actor->angular_velocity_yaw;
    actor->euler_accum.pitch += actor->angular_velocity_pitch;

    /* 3. Integrate linear velocity into position.
     * The original gates this on DAT_00483030 (ref_count=1, read-only), which
     * is never written, so effectively always runs. With dynamics skipped
     * during pause, vel_x/vel_z stay 0 and the integration is a no-op. */
    actor->world_pos.x += actor->linear_velocity_x;
    actor->world_pos.z += actor->linear_velocity_z;
    actor->world_pos.y += actor->linear_velocity_y;

    /* 3b. Update chassis track position from new world pos.
     * Original calls FUN_004440F0 here [CONFIRMED @ 0x405E80 callees].
     * Without this, track_span_raw stays at 0 and wall checks use the
     * wrong span — the primary reason collisions don't work. */
    {
        td5_track_update_actor_position(actor);

        /* Guard against span walker overflow. The walker can jump to out-of-
         * bounds or distant spans at track edges, causing terrain height
         * garbage (Y launch) and XZ teleportation.
         *
         * Use td5_track_get_span_count() which includes branch spans
         * (NOT ring_length which is main road only). Junction links
         * legitimately jump from e.g. span 499 to span 2790.
         *
         * The jump limit is DISABLED because junction transitions produce
         * large span deltas that are valid. The hard bounds clamp alone
         * prevents OOB access. */
        int max_span = td5_track_get_span_count();  /* includes branch spans */
        if (max_span > 0) {
            /* Hard bounds clamp */
            if (actor->track_span_raw >= (uint16_t)max_span)
                actor->track_span_raw = (uint16_t)(max_span - 1);

            /* Clamp per-wheel probes too */
            for (int wi = 0; wi < 4; wi++) {
                if (actor->wheel_probes[wi].span_index >= (int16_t)max_span)
                    actor->wheel_probes[wi].span_index = actor->track_span_raw;
                int wdelta = (int)actor->wheel_probes[wi].span_index - (int)actor->track_span_raw;
                if (wdelta < 0) wdelta = -wdelta;
                if (wdelta > 50)
                    actor->wheel_probes[wi].span_index = actor->track_span_raw;
            }
        }
    }

    /* Refresh heading_normal at +0x290 from the live span geometry.
     * [CONFIRMED @ 0x00405fb6: IntegrateVehiclePoseAndContacts calls
     * ComputeActorHeadingFromTrackSegment with LEA [esi+0x290].]
     * Mirrors the trailing call inside UpdateVehiclePoseFromPhysicsState;
     * was previously missing here, which kept heading_normal at its
     * stale spawn-pose value (with y=0 from the old td5_track_compute_heading
     * mapping) for the entire IntegrateVehiclePoseAndContacts code path.
     * [precise-port pilot 00445B90, 2026-05-14] */
    td5_track_compute_runtime_heading_normal(actor);

    /* 4. Convert accumulators to display angles. NO 0xFFF mask -- original
     * truncates via int16 store; explicit mask flipped sign bit. Matches
     * 5978 unmasked fix from the precise-port pilot 004063A0. */
    actor->display_angles.roll  = (int16_t)(actor->euler_accum.roll  >> 8);
    actor->display_angles.yaw   = (int16_t)(actor->euler_accum.yaw   >> 8);
    actor->display_angles.pitch = (int16_t)(actor->euler_accum.pitch >> 8);

    /* 5. Build rotation matrix from euler angles in FLOAT precision.
     *
     * Original (0x42E1E0): Ry(yaw) * Rx(roll) * Rz(pitch) — YXZ order, all
     * trig multiplications in 80-bit FPU. Earlier port used integer Q12
     * with `>> 12` truncations after each multiply — those LSB losses
     * propagated through `body_wy * m[4]` in chassis-snap and produced a
     * +128 FP averaged delta in world_pos.y at sim_tick=1.
     * [E1 / 2026-05-02 — physics_trace.csv ground-truth diff]
     *
     * 2026-05-15 PRECISION FIX: replaced `(float)cos_fixed12(a) * (1/4096)`
     * with direct `CosFloat12bit(a)`. The earlier path went int32-truncate →
     * float → divide, which clamps tiny `sin(small_angle)` outputs to 0
     * (cast to int32 truncates toward zero). Original 0x0042E1E0 reads
     * `g_sinCosFloatTable` directly as float32; for example at slot-0 tick-1
     * with near-zero roll/pitch, the original keeps sin(small) at its
     * float32 precision (~1e-3 .. 1e-6) while the port collapsed it to 0,
     * producing rotation_matrix.m[3] = port=0 vs orig=−7e−11 and cascading
     * into wheel_world_positions_hires.y off-by-1 LSB and probe_FR_x ±1
     * world unit. [whole_state_diff slot-0 tick-1 labelled cluster, 2026-05-15]
     *
     * 2026-05-22 FPU SPILL FIX (session_02): orig FSTPs four intermediate
     * products to float32 memory slots BEFORE the final FMUL/FADD chain:
     *   slot F = (float)(sp*sy)   slot G = (float)(cp*cy)
     *   slot H = (float)(cp*sy)   slot I = (float)(sp*cy)
     * Then m[0]/m[1]/m[6]/m[7] consume these float32 spills, so each entry
     * is `round_f32( round_f32(a*b) * c ± round_f32(d*e) )`, NOT the all-
     * 80-bit chain GCC produces by default. Use `volatile float` to force
     * MinGW (-m32, x87, no SSE) to emit FSTP-to-memory then FLD-from-memory
     * at exactly the orig spill points. This is the literal port of
     * 0x0042e244, 0x0042e250, 0x0042e26a, 0x0042e274. m[2]/m[3]/m[4]/m[5]/m[8]
     * have only one trig product each (no intermediate spill needed) and
     * already match orig at ULP level. */
    {
        int32_t roll_a  = actor->display_angles.roll  & 0xFFF;
        int32_t yaw_a   = actor->display_angles.yaw   & 0xFFF;
        int32_t pitch_a = actor->display_angles.pitch & 0xFFF;

        float cr = CosFloat12bit((unsigned int)roll_a);
        float sr = SinFloat12bit(roll_a);
        float cy = CosFloat12bit((unsigned int)yaw_a);
        float sy = SinFloat12bit(yaw_a);
        float cp = CosFloat12bit((unsigned int)pitch_a);
        float sp = SinFloat12bit(pitch_a);

        /* Float32 spills (slots F/G/H/I from orig 0x42E1E0). See
         * TD5_F32_SPILL at file top for rationale. */
        float sp_sy_f32 = TD5_F32_SPILL(sp * sy); /* slot F @ orig 0x42E244 */
        float cp_cy_f32 = TD5_F32_SPILL(cp * cy); /* slot G @ orig 0x42E250 */
        float cp_sy_f32 = TD5_F32_SPILL(cp * sy); /* slot H @ orig 0x42E26A */
        float sp_cy_f32 = TD5_F32_SPILL(sp * cy); /* slot I @ orig 0x42E274 */

        actor->rotation_matrix.m[0] = sp_sy_f32 * sr + cp_cy_f32;
        actor->rotation_matrix.m[1] = cp_sy_f32 * sr - sp_cy_f32;
        actor->rotation_matrix.m[2] = sy * cr;
        actor->rotation_matrix.m[3] = sp * cr;
        actor->rotation_matrix.m[4] = cp * cr;
        actor->rotation_matrix.m[5] = -sr;
        actor->rotation_matrix.m[6] = sp_cy_f32 * sr - cp_sy_f32;
        actor->rotation_matrix.m[7] = cp_cy_f32 * sr + sp_sy_f32;
        actor->rotation_matrix.m[8] = cy * cr;

        /* Per-frame rotation_matrix dump for cross-binary diff against orig
         * Frida probe of 0x0042E1E0 (tools/_probes/rotation_matrix_probe.js).
         * Gated on RaceTrace + slot 0 so it does not flood normal runs.
         * [session_02 follow-up, 2026-05-22] */
        if (actor->slot_index == 0 && g_td5.ini.race_trace_enabled) {
            const float *m = actor->rotation_matrix.m;
            TD5_LOG_I(LOG_TAG,
                "ROTMAT slot0 fc=%u roll=%d yaw=%d pitch=%d "
                "m=[%.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f] "
                "wp=(%d,%d,%d)",
                actor->frame_counter,
                (int)actor->display_angles.roll,
                (int)actor->display_angles.yaw,
                (int)actor->display_angles.pitch,
                m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8],
                actor->world_pos.x, actor->world_pos.y, actor->world_pos.z);
        }
    }

    /* 6. Compute render position (world_pos / 256 as float) */
    actor->render_pos.x = (float)actor->world_pos.x * (1.0f / 256.0f);
    actor->render_pos.y = (float)actor->world_pos.y * (1.0f / 256.0f);
    actor->render_pos.z = (float)actor->world_pos.z * (1.0f / 256.0f);

    /* 7. Save previous-frame grounded mask, then refresh wheel contacts.
     * suspension_response needs "was grounded last frame" for the spring
     * damping path. Refresh writes the live airborne mask into +0x37C
     * (wheel_contact_bitmask, 1=airborne) and snapshots the prior tick's
     * value into +0x37D (damage_lockout) at entry. This side-channel
     * remains for compatibility with consumers that already use it; +0x37D
     * also now carries the OLD mask in airborne polarity. */
    s_prev_grounded_mask[actor->slot_index & 0x0F] = (~actor->wheel_contact_bitmask) & 0x0F;
    td5_physics_refresh_wheel_contacts(actor);

    /* Inner-tick trace post_refresh: deprecated — td5_trace_write_physics
     * was dropped by merge 1acd3fb in favor of the modular MOD_PHYSICS API.
     * Stubbed out until rewritten against the new emission path. */

    /* Increment airborne_frame_counter (+0x360) when all 4 wheels airborne.
     * [CONFIRMED @ 0x0040634B]: original does INC word ptr [ESI+0x360]
     * inside the per-wheel averaging loop of IntegrateVehiclePoseAndContacts,
     * reached only when the grounded-wheel count is 0 — equivalently
     * damage_lockout == 0x0F. NO reset instruction exists in the binary
     * (exhaustive search): the counter monotonically grows during sustained
     * airborne, and simply stops growing when any wheel grounds (the INC
     * isn't reached). State0f_damping at td5_physics.c:547 fires when
     * afc >= 3 AND dlk == 0x0F (CMP at 0x00406835 + JL/JNZ at 0x0040683D). */
    if (actor->wheel_contact_bitmask == 0x0F && !g_game_paused) {
        /* afc++ gated behind !g_game_paused so countdown ticks don't
         * accumulate the all-airborne counter. Refresh runs every render
         * frame including the ~190-frame pause countdown; without this
         * gate, airborne_frame_counter would already be ≥5 by the first
         * un-paused dispatch on Honolulu, instantly triggering the
         * state0f damping gate at line 641 (wcb==0x0F && afc>=3) — that
         * gate replaces update_player, so longitudinal_speed (+0x314)
         * stays at zero through countdown into the active race. The
         * original almost certainly does NOT increment afc during the
         * countdown (race not active → tick loop suppressed). Verifying
         * exact gate location in original is a follow-up; this guard
         * preserves the desired post-countdown behavior either way. */
        actor->airborne_frame_counter++;
        TD5_LOG_I(LOG_TAG, "tumble_gate: slot=%d wcb=0x0F dlk=0x0F afc=%d",
                  actor->slot_index, actor->airborne_frame_counter);
    }

    /* T2: Wheel-contact attitude feedback — literal port of
     * TransformTrackVertexByMatrix @ 0x00446030 called from
     * IntegrateVehiclePoseAndContacts @ 0x00405E80.
     *
     * CASE GATING [CONFIRMED @ 0x00405FDE switch on [ESI+0x37C]]:
     * The original's switch skips T2 entirely for current-airborne masks
     * {7, 11, 13, 14, 15} (3+ wheels airborne) — with only one wheel on
     * the ground the 4-wheel height-spread solver produces garbage
     * (isqrt of a small hypotenuse vs a huge Y numerator → runaway
     * pitch/roll). Port previously ran this unconditionally, which matches
     * the user's "sometimes rolls out of control on steep slopes" symptom
     * (partial 2026-04-22 residual). 2026-04-24: added gate.
     *
     * Cases {0,1,2,4,6,8,9} call TransformTrackVertexByMatrix @ 0x00446030
     * (full 4-wheel solver — byte-faithful in attitude_from_wheels).
     * Cases {3,5,10,12} call reduced B/C solvers @ 0x00446140 / 0x004461C0
     * (not decompiled). Port falls back to the full solver for those;
     * likely over-corrects a single axle but less catastrophic than the
     * 3+-airborne garbage path. Decompile + split solver is a followup.
     *
     * 1. Compute new_roll, new_pitch from the freshly refreshed
     *    wheel_contact_pos[] heights + wheel_suspension_pos[] deflections.
     * 2. Derive angular_velocity_roll/pitch as signed-wrapped 12-bit delta,
     *    scaled by 256, clamped to ±6000 (matches 0x00405F0A-0x00405F2C).
     * 3. Override display_angles.roll/pitch with the wheel-derived values
     *    (overrides the euler_accum-derived values written at step 4 above).
     * 4. Update euler_accum.roll/pitch so the next tick's integration starts
     *    from the wheel-corrected attitude.
     * 5. Rebuild rotation_matrix from the corrected display_angles so
     *    downstream ground-snap and next-tick refresh use the right matrix. */
    uint8_t t2_lock = actor->wheel_contact_bitmask;

    /* Per-axis solver dispatch — [CONFIRMED @ 0x00405E80 switch(damage_lockout)]:
     *
     *   cases 0,1,2,4,6,8,9  → TransformTrackVertexByMatrix  @ 0x00446030 (full)
     *   cases 3, 12          → TransformTrackVertexByMatrixC @ 0x004461C0 (PITCH only)
     *   cases 5, 10          → TransformTrackVertexByMatrixB @ 0x00446140 (ROLL only)
     *   cases 7,11,13,14,15  → NOT IN SWITCH — T2 entirely skipped
     *
     * The B/C variants use IDENTICAL per-axis formulas to the full solver
     * (verified 2026-04-24 Ghidra byte-diff at listed addresses); the ONLY
     * difference is that they SKIP writing the inactive axis.
     *
     * Prior port ran the full solver on masks 3/5/10/12 and wrote BOTH
     * axes unconditionally. On steep slopes where one axle lifts (mask=12
     * rear-airborne or mask=5 diagonal), the port's unconditional roll/
     * pitch overwrite introduced noise the original leaves untouched —
     * matches the user's "sometimes rolls out of control on steep slopes"
     * residual after the 2026-04-22 6-fix chain. */
    int t2_full  = (t2_lock == 0 || t2_lock == 1 || t2_lock == 2 ||
                    t2_lock == 4 || t2_lock == 6 || t2_lock == 8 ||
                    t2_lock == 9);
    int t2_pitch_only = (t2_lock == 3  || t2_lock == 12);  /* C variant */
    int t2_roll_only  = (t2_lock == 5  || t2_lock == 10);  /* B variant */
    int t2_skip  = !(t2_full || t2_pitch_only || t2_roll_only);

    if (actor->slot_index == 0 && t2_skip) {
        TD5_LOG_I(LOG_TAG, "T2 skip slot0: lock=0x%02x (3+ airborne)",
                  (int)t2_lock);
    }
    if (!t2_skip)
    {
        int16_t new_roll  = 0;
        int16_t new_pitch = 0;
        td5_physics_attitude_from_wheels(actor, &new_roll, &new_pitch);

        uint8_t prev_airborne = (~s_prev_grounded_mask[actor->slot_index & 0x0F]) & 0x0F;
        int mask_stable = (actor->wheel_contact_bitmask == prev_airborne);

        /* ROLL axis: written only for full solver OR roll-only (B variant). */
        if (t2_full || t2_roll_only) {
            int32_t d_roll = td5_physics_wrap_angle_delta((int32_t)new_roll,
                                                          (int32_t)t2_old_disp_roll) * 0x100;
            if (d_roll >  6000) d_roll =  6000;
            if (d_roll < -6000) d_roll = -6000;
            /* [CONFIRMED @ 0x00405FF9] gate angular_velocity on mask stability. */
            if (mask_stable) {
                actor->angular_velocity_roll = d_roll;
            }

            /* [FIX 2026-06-21] Slope-roll damp: on a STEEP grade driven roughly
             * STRAIGHT, scale the wheel-derived roll toward level (the spurious
             * over-walk tilt). Cornering keeps its roll (yaw-rate gate). See
             * roll_damp_enabled(). */
            if (roll_damp_enabled()) {
                int32_t pitch_abs = (int32_t)new_pitch & 0xFFF;       /* 12-bit */
                if (pitch_abs > 0x800) pitch_abs = 0x1000 - pitch_abs; /* fold to [0,2048] */
                int32_t yaw_rate = actor->angular_velocity_yaw;
                if (yaw_rate < 0) yaw_rate = -yaw_rate;
                if (pitch_abs > ROLL_DAMP_PITCH_MIN && yaw_rate < ROLL_DAMP_YAW_MAX) {
                    int32_t roll_s = (int32_t)new_roll & 0xFFF;        /* 12-bit */
                    if (roll_s > 0x800) roll_s -= 0x1000;              /* -> signed */
                    roll_s = (roll_s * ROLL_DAMP_KEEP_Q8) >> 8;        /* scale toward 0 */
                    new_roll = (int16_t)(roll_s & 0xFFF);
                }
            }

            actor->display_angles.roll = new_roll;
            actor->euler_accum.roll    = (int32_t)new_roll << 8;
        }

        /* PITCH axis: written only for full solver OR pitch-only (C variant). */
        if (t2_full || t2_pitch_only) {
            int32_t d_pitch = td5_physics_wrap_angle_delta((int32_t)new_pitch,
                                                           (int32_t)t2_old_disp_pitch) * 0x100;
            if (d_pitch >  6000) d_pitch =  6000;
            if (d_pitch < -6000) d_pitch = -6000;
            if (mask_stable) {
                actor->angular_velocity_pitch = d_pitch;
            }
            actor->display_angles.pitch = new_pitch;
            actor->euler_accum.pitch    = (int32_t)new_pitch << 8;
        }

        /* Rebuild rotation_matrix from the (possibly partially updated)
         * display_angles in float precision (E1 fix — see line ~4361).
         * 2026-05-15: switched from int Q12 trig to LUT-style float trig
         * (CosFloat12bit / SinFloat12bit) to preserve sub-Q12 magnitudes
         * for near-zero pitch/roll. Mirrors the per-tick builder above. */
        int32_t roll_a  = actor->display_angles.roll  & 0xFFF;
        int32_t yaw_a   = actor->display_angles.yaw   & 0xFFF;
        int32_t pitch_a = actor->display_angles.pitch & 0xFFF;

        float cr = CosFloat12bit((unsigned int)roll_a);
        float sr = SinFloat12bit(roll_a);
        float cy = CosFloat12bit((unsigned int)yaw_a);
        float sy = SinFloat12bit(yaw_a);
        float cp = CosFloat12bit((unsigned int)pitch_a);
        float sp = SinFloat12bit(pitch_a);

        /* Float32 spills (slots F/G/H/I from orig 0x42E1E0). See
         * TD5_F32_SPILL at file top for rationale. */
        float sp_sy_f32 = TD5_F32_SPILL(sp * sy);
        float cp_cy_f32 = TD5_F32_SPILL(cp * cy);
        float cp_sy_f32 = TD5_F32_SPILL(cp * sy);
        float sp_cy_f32 = TD5_F32_SPILL(sp * cy);

        actor->rotation_matrix.m[0] = sp_sy_f32 * sr + cp_cy_f32;
        actor->rotation_matrix.m[1] = cp_sy_f32 * sr - sp_cy_f32;
        actor->rotation_matrix.m[2] = sy * cr;
        actor->rotation_matrix.m[3] = sp * cr;
        actor->rotation_matrix.m[4] = cp * cr;
        actor->rotation_matrix.m[5] = -sr;
        actor->rotation_matrix.m[6] = sp_cy_f32 * sr - cp_sy_f32;
        actor->rotation_matrix.m[7] = cp_cy_f32 * sr + sp_sy_f32;
        actor->rotation_matrix.m[8] = cy * cr;

        /* One-line per-tick diagnostic for the player car only */
        if (actor->slot_index == 0) {
            static int s_t2_log_frame = 0;
            if ((s_t2_log_frame++ % 120) == 0) {
                const char *variant = t2_full ? "FULL" :
                                      t2_pitch_only ? "C_pitch_only" :
                                      t2_roll_only  ? "B_roll_only"  : "?";
                TD5_LOG_I(LOG_TAG,
                    "T2 attitude slot0: lock=0x%02x variant=%s new_roll=%d new_pitch=%d "
                    "old_roll=%d old_pitch=%d",
                    (int)t2_lock, variant,
                    (int)new_roll, (int)new_pitch,
                    (int)t2_old_disp_roll, (int)t2_old_disp_pitch);
            }
        }
    }

    /* Inner-tick trace post_t2: deprecated — see post_refresh stub above. */

    /* DIAGNOSTIC: log player car (slot 0) physics state once per 30 frames */
    if (actor->slot_index == 0) {
        static int s_diag_frame = 0;
        if ((s_diag_frame++ % 30) == 0) {
            TD5_LOG_I(LOG_TAG,
                "DIAG slot0: pos_y=%d vel_y=%d prev_y=%d render_y=%.2f "
                "bitmask=0x%02X force=[%d,%d,%d,%d] susp_pos=[%d,%d,%d,%d] susp_vel=[%d,%d,%d,%d]",
                actor->world_pos.y, actor->linear_velocity_y, actor->prev_frame_y_position,
                actor->render_pos.y, actor->wheel_contact_bitmask,
                actor->wheel_spring_dv[0], actor->wheel_spring_dv[1],
                actor->wheel_spring_dv[2], actor->wheel_spring_dv[3],
                actor->wheel_suspension_pos[0], actor->wheel_suspension_pos[1],
                actor->wheel_suspension_pos[2], actor->wheel_suspension_pos[3],
                actor->wheel_load_accum[0], actor->wheel_load_accum[1],
                actor->wheel_load_accum[2], actor->wheel_load_accum[3]);
        }
    }

    /* 8. Ground-snap: correct world_pos.y to keep the car on the road.
     *
     * RefreshWheelContacts adds susp_href (cardef+0x82 * 0xB5/256) to the
     * wheel Y before rotation, raising the contact probe ~107 units toward
     * the chassis. Without compensation, ground-snap aligns this elevated
     * probe at ground level, leaving the visual model sunk by susp_href.
     *
     * Original (0x403720 / 0x406300): subtracts susp_href before rotation,
     * adds it back after — the add-back in world space effectively raises
     * the chassis by susp_href. We replicate by adding susp_href_world to
     * the per-wheel correction. */
    /* 8. Ground-snap: direct Y write from wheel contact average.
     *
     * Original (0x405E80): accumulates per-wheel Y from local_1c + suspension
     * spring output, averages over grounded wheels, and DIRECTLY WRITES the
     * result to world_pos.y. No delta clamp — the chassis position is set
     * absolutely to the contact surface each tick.
     *
     * Per-wheel Y contribution = wheel_world_y + spring_output * -0x100
     * where wheel_world_y is from RefreshWheelContacts (includes susp_href).
     */
    /* Ground-snap: direct Y write from wheel contact average.
     * [CONFIRMED via 2026-04-11 Ghidra pass on 0x00405E80 by research agent.]
     * Three bugs fixed vs prior port:
     *
     *   1. Per-wheel cardef stride is 8 bytes (not 16). Each wheel's body-
     *      space offset is `short[3]` at `cd + 0x40 + i*8`: X at +0x40+i*8,
     *      Y at +0x42+i*8, Z at +0x44+i*8. Prior port indexed at stride 16
     *      which made wheels 1..3 read garbage for every non-Viper car.
     *   2. No `+0x80` rounding bias. Original writes the plain `IDIV` result.
     *      Previous port added +0x80 to work around bug #1 on Viper.
     *   3. Full 3-vec rotation through the chassis rotation matrix. The
     *      ride_offset vector is (cwx, body_wy, cwz), rotated by the
     *      actor's rotation matrix, and ONLY the Y component of the rotated
     *      result is used. Prior port used `body_wy * -0x100` directly,
     *      ignoring the X/Z terms which contribute pitch/roll coupling.
     *
     * The `rotate_vec_world_y` helper (see ~line 2403) extracts exactly the
     * Y component via `m[1]*v[0] + m[4]*v[1] + m[7]*v[2]`, matching the
     * original's `TransformShortVec3ByRenderMatrixRounded @ 0x0042E2E0`
     * used inside 0x00405E80 (followed by reading out[1]). */
    {
        int64_t contact_y_sum = 0;
        int contact_count = 0;
        uint8_t gnd_mask = actor->wheel_contact_bitmask;

        int32_t href = (int32_t)CDEF_S(actor, CDEF_SUSP_REF_HEIGHT);
        int32_t href_x181 = href * 0xB5;
        int32_t susp_offset = (href_x181 + ((href_x181 >> 31) & 0xFF)) >> 8;

        uint8_t *cd = (uint8_t *)actor->car_definition_ptr;

        for (int i = 0; i < 4; i++) {
            if (gnd_mask & (1 << i)) continue;  /* airborne wheel — skip */
            if (!cd) break;

            /* [FIX #1] stride 8 per wheel, not 16. */
            int16_t cwx = *(int16_t *)(cd + 0x40 + i * 8);
            int16_t cwy = *(int16_t *)(cd + 0x42 + i * 8);
            int16_t cwz = *(int16_t *)(cd + 0x44 + i * 8);

            /* Truncate-toward-zero /256 on suspension pos (matches the
             * original's CDQ;AND 0xff;ADD;SAR 8 idiom at 0x00406273). */
            int32_t sp = actor->wheel_suspension_pos[i];
            int32_t sp_div = (sp + ((sp >> 31) & 0xFF)) >> 8;

            int16_t body_wy = (int16_t)((int32_t)cwy - sp_div - susp_offset);

            /* [FIX #3] rotate the full short[3] body-offset vector through
             * the actor's rotation matrix and take the Y component. Uses
             * body→world (row 1 of matrix) — matches original's direct
             * LoadRenderRotationMatrix (no transpose) at 0x00406135 /
             * 0x004061EA before ConvertFloatVec3ToShortAngles.
             *
             * Original 0x00405E80 (Ghidra decomp pool10 2026-05-15):
             *
             *   ConvertFloatVec3ToShortAngles(psVar10 + -1, puVar8);
             *
             * where psVar10-1 = wheel_disp_angles[i] = (cwx, body_wy, cwz)
             * and puVar8 = &actor->field_0x230 + i*8 = wheel_contact_normals[i].
             *
             * So the original writes all 3 rotated components to +0x230+i*8.
             * The chassis-snap accumulator uses only puVar8[1] (the Y),
             * but downstream suspension-response consumers read the full
             * vector. Without this write the wheel_contact_normals stay
             * zero (visible in whole_state_port.bin diff as the 16-byte
             * +0x230 blob mismatching). */
            int16_t src[3] = { cwx, body_wy, cwz };
            int16_t rot_v3[3];
            rotate_body_to_world_vec3(actor, src, rot_v3);
            int16_t rot_y = rot_v3[1];

            /* Write to wheel_contact_normals[i] at +0x230 + i*8 (4-short
             * stride, 4th short is padding / unused in the original). */
            {
                int16_t *wcn = (int16_t *)((uint8_t *)actor + 0x230 + i * 8);
                wcn[0] = rot_v3[0];
                wcn[1] = rot_v3[1];
                wcn[2] = rot_v3[2];
            }

            /* [FIX 2026-05-27 — wheels-lift-on-slopes root] Per-tick update of
             * the RENDER wheel-offset Y to the suspension-adjusted value
             * (cwy - sp_div), matching the orig integrate_pose loop @ 0x00405E80
             * (`*psVar10 = body_wy; ConvertFloatVec3ToShortAngles(...); *psVar10
             * += susp_offset` → wheel_display_angles[i][1] = cwy - sp_div).
             *
             * The port set wheel_display_angles ONCE at init (raw cardef Y, no
             * suspension — td5_physics.c ~9881), so the rendered wheel billboards
             * were RIGID and did NOT follow per-tick suspension travel. On slopes
             * the springs load differentially (and orientation-dependently), so
             * the static wheels floated/sank off the road — the "rear wheels lift
             * a lot uphill / side wheels lift sideways" symptom — even though the
             * 4-wheel attitude SOLVER (0x00446030) is correct. X,Z stay static
             * (cardef); only Y tracks the spring, grounded wheels only (the loop's
             * airborne `continue` mirrors the orig's damage_lockout gate). */
            /* [WHEEL OVERHAUL 2026-06-12] Render-only suspension clamp so the
             * wheel can't ride up INTO the chassis (or droop absurdly far) on
             * hard bumps. disp_y = cwy - sp_div is the wheel-centre Y arm; cwy
             * is its rest position, so (disp_y - cwy) = -sp_div is the visual
             * travel. Bound |travel| to WHEEL_VIS_TRAVEL body-units. This
             * touches ONLY the rendered wheel position — body_wy (which feeds
             * the faithful chassis ground-snap below) is left intact, so
             * gameplay/diff-race parity is unchanged. A/B: TD5RE_WHEEL_SUSP_CLAMP=0
             * disables; TD5RE_WHEEL_VIS_TRAVEL=N overrides the limit. */
            int32_t disp_y = (int32_t)body_wy + susp_offset;   /* = cwy - sp_div */
            {
                static int s_clamp_on = -1, s_travel = -1;
                if (s_clamp_on < 0) {
                    s_clamp_on = td5_env_flag_on("TD5RE_WHEEL_SUSP_CLAMP");
                    /* render-only travel limit; original had only a lower clamp (>=1) */
                    s_travel = td5_env_int("TD5RE_WHEEL_VIS_TRAVEL", 40, 1, 100000);   /* body-units */
                }
                if (s_clamp_on) {
                    int32_t lo = (int32_t)cwy - s_travel;
                    int32_t hi = (int32_t)cwy + s_travel;
                    if (disp_y < lo) disp_y = lo;
                    if (disp_y > hi) disp_y = hi;
                }
            }
            actor->wheel_display_angles[i][1] = (int16_t)disp_y;

            int32_t wheel_y = actor->wheel_contact_pos[i].y;
            contact_y_sum += (int64_t)(wheel_y + (int32_t)rot_y * -0x100);
            contact_count++;
        }

        if (contact_count > 0) {
            /* [FIX #2] NO +0x80 bias — the original writes the plain
             * signed-IDIV result (MOV [ESI+0x200], EAX at 0x00406307). */
            int32_t new_y = (int32_t)(contact_y_sum / contact_count);

            /* No snap-delta clamp. Original at 0x00406300 directly writes
             * world_pos.y from the contact-Y average without bounding the
             * delta vs prev_frame_y_position; the +0x37D OLD-mask gate
             * below is the original's only filter on this write. The prior
             * port's ±2M clamp was a workaround for the +0x37C/+0x37D
             * semantic reversal that has since been corrected. */

            /* Chassis Y-snap. No chassis-level airborne override here —
             * the per-wheel airborne bits written by refresh_wheel_contacts
             * (force >= 0x801 @ 0x00403720) now drive downstream airborne
             * behavior. Match original 0x00406300 which snaps
             * unconditionally once contact_count > 0.
             *
             * [CONFIRMED via Ghidra decomp of IntegrateVehiclePoseAndContacts
             * @ 0x00405E80, 2026-05-15]: the original writes render_pos_y
             * exactly ONCE per tick — at the post-gravity, pre-snap step
             * (matches our line 5468). The chassis-snap (this line) updates
             * world_pos.y ONLY, leaving render_pos.y at the gravity-dropped
             * value. The next frame's sub-tick interpolation pass propagates
             * the new world_pos.y into render_pos.y via
             * td5_physics_apply_render_interpolation. Removing the port's
             * post-snap render_pos.y write closes the 7.4u (g/256) gap
             * between port=226.5 and orig=219.078 on Honolulu sim_tick=1. */
            actor->world_pos.y = new_y;

            /* [WHEELGAP LOG 2026-05-27] Per-wheel RENDERED world-Y minus GROUND
             * contact-Y (world units, ÷256). Wheels render at world_pos +
             * body_matrix*wheel_display_angles[w] (+0x210); positive gap = that
             * wheel floats above the road. Self-contained capture: run with
             * AutoThrottle to drive up the hill, grep WHEELGAP at span ~160. */
            if (actor->slot_index == 0) {
                static int s_wg_div = 0;
                if ((s_wg_div++ % 15) == 0) {
                    uint8_t *ab = (uint8_t *)actor;
                    float wpy = (float)(*(int32_t *)(ab + 0x200)) / 256.0f;
                    /* body->world Y uses ROW 1 of the rotation matrix
                     * (m[3],m[4],m[5] @ +0x12C/+0x130/+0x134) — matching the
                     * actual wheel render (mat3x4 out[1]) and orig
                     * ConvertFloatVec3ToShortAngles (0x0042E2E0: Y = v·M[+0xC,+0x10,+0x14]).
                     * Previously read COL 1 (m[1],m[4],m[7]) → transposed,
                     * wrong gap magnitudes. [FIX 2026-05-27 PM-7] */
                    float m3f = *(float *)(ab + 0x12C);
                    float m4f = *(float *)(ab + 0x130);
                    float m5f = *(float *)(ab + 0x134);
                    int g[4];
                    for (int wq = 0; wq < 4; wq++) {
                        int16_t wxx = *(int16_t *)(ab + 0x210 + wq*8 + 0);
                        int16_t wyy = *(int16_t *)(ab + 0x210 + wq*8 + 2);
                        int16_t wzz = *(int16_t *)(ab + 0x210 + wq*8 + 4);
                        float roty = m3f*(float)wxx + m4f*(float)wyy + m5f*(float)wzz;
                        float ry   = wpy + roty;
                        float gy   = (float)(*(int32_t *)(ab + 0xF0 + wq*12 + 4)) / 256.0f;
                        g[wq] = (int)(ry - gy);
                    }
                    TD5_LOG_I(LOG_TAG,
                        "WHEELGAP span=%d wda=[%d,%d,%d,%d] gap(float+) FL=%d FR=%d RL=%d RR=%d",
                        (int)actor->track_span_raw,
                        (int)*(int16_t *)(ab+0x210+2), (int)*(int16_t *)(ab+0x210+8+2),
                        (int)*(int16_t *)(ab+0x210+16+2), (int)*(int16_t *)(ab+0x210+24+2),
                        g[0], g[1], g[2], g[3]);
                }
            }

          if (contact_count > 0) {
            /* Velocity-from-snap gate — literal port of original
             * IntegrateVehiclePoseAndContacts tail (0x0040630D–0x00406335):
             *
             *   if (DAT_0046318C[actor[0x37C]] != 0 &&
             *       (actor[0x37D] & (actor[0x37C] ^ 0xF)) == 0)
             *       linear_velocity_y = (new_y - prev_y) - gGravityConstant;
             *
             * Field roles (see td5_physics.c:2057-2058, 2113-2114):
             *   0x37C = NEW-frame wheel-contact bitmask (gate index)
             *   0x37D = PREVIOUS-frame wheel-contact bitmask (mask operand)
             *
             * The second condition detects "no wheel that was grounded last
             * frame is airborne this frame" — i.e. only apply the
             * snap-derived velocity when the wheel contact set did not lose
             * any wheels this tick. Otherwise we keep the integrated velocity
             * so a wheel lifting off doesn't kill vertical momentum.
             *
             * DAT_0046318C read via mcp memory_read at 0x0046318C:
             *   01 01 01 00 01 00 00 00 01 00 00 00 00 00 00 00
             * (indices 0,1,2,4,8 are gated ON; everything else OFF.)
             *
             * Previous port code had THREE bugs in this gate:
             *   (1) table bytes wrong (leaked into indices 6, 9, 12)
             *   (2) index read from 0x37D instead of 0x37C
             *   (3) XOR operand also read from 0x37D, making the mask
             *       check `(x & ~x) == 0` → always true → effectively
             *       no mask check at all.
             *
             * 2026-05-01 follow-up: bugs (2) + (3) returned in disguise.
             * The port's refresh_wheel_contacts mutates +0x37D in place to
             * hold the NEW airborne mask (per-wheel writes at 5204/5207),
             * and line 4409 copies that NEW value into +0x37C. So BOTH
             * actor+0x37C and actor+0x37D end the refresh holding NEW —
             * the gate `(prev_mask & (new_mask ^ 0x0F)) == 0` reduces to
             * `(NEW & ~NEW) == 0` → always TRUE. Velocity-snap fires every
             * tick, overriding integrated vy with a ±30000-clamped delta.
             * On slope onsets this manifests as the chassis NOT following
             * terrain (vertical momentum is killed, then clamped into a
             * step that doesn't match the ground gradient).
             *
             * The OLD mask in airborne polarity is reconstructed from
             * s_prev_grounded_mask[] (snapshotted pre-refresh at line 4398
             * in grounded polarity, so we re-invert here).
             *
             * [CONFIRMED @ 0x00403720, 0x004039E5: original keeps +0x37D
             * = OLD across the refresh]. */
            {
                static const uint8_t k_mode_gate[16] = {
                    1,1,1,0, 1,0,0,0, 1,0,0,0, 0,0,0,0
                };
                uint8_t new_mask  = actor->wheel_contact_bitmask;
                uint8_t prev_mask = (~s_prev_grounded_mask[actor->slot_index & 0x0F]) & 0x0F;
                if (k_mode_gate[new_mask & 0xF] &&
                    (prev_mask & (new_mask ^ 0x0F)) == 0 &&
                    actor->prev_frame_y_position != (int32_t)0xC0000000) {
                    int32_t snap_vy = new_y - actor->prev_frame_y_position
                                     - g_gravity_constant;
                    /* No clamp. Original at 0x0040630D-0x00406335 writes the
                     * raw delta-minus-gravity unconditionally once the gate
                     * passes; ±30000 was a port-only workaround for the
                     * +0x37C/+0x37D semantic reversal that gated the snap
                     * every tick. With the OLD-mask now correctly held in
                     * +0x37D, the gate fires only on stable wheel sets and
                     * the unclamped delta is faithful. */
                    if (actor->slot_index == 0 &&
                        (snap_vy > 100000 || snap_vy < -100000 ||
                         (actor->frame_counter % 60u) == 0u)) {
                        TD5_LOG_I(LOG_TAG, "y_snap: new=%u prev=%u vy=%d new_y=%d prev_y=%d",
                                  new_mask, prev_mask, snap_vy, new_y,
                                  actor->prev_frame_y_position);
                    }
                    actor->linear_velocity_y = snap_vy;
                }
            }

            /* [FIX 2026-06-21] Descent-launch damp for AI OPPONENTS, applied AFTER
             * the snap that injects their vertical velocity (the player is damped
             * in td5_physics_update_suspension_response and is skipped here, so it
             * is never double-processed). Same human-player test as the dispatch
             * (td5_physics.c:2717); traffic uses a separate integrator. Runs inside
             * `contact_count > 0`, i.e. only while grounded. See descent_damp_apply(). */
            if (!(actor->slot_index < g_traffic_slot_base &&
                  g_race_slot_state[actor->slot_index] == 1)) {
                descent_damp_apply(actor);
            }
          } /* end velocity snap guard */
        } else {
            /* No ground contact: increment airborne counter (original 0x40631A) */
            actor->frame_counter++;
        }
    }

    /* 8b. OOB recovery disabled — was teleporting cars due to suspension
     * instability rather than genuine out-of-bounds conditions. */

    /* 9. Clamp angular velocity deltas to +/- 6000 per frame.
     * [CONFIRMED @ 0x405F0A-0x405F2C, 0x406058-0x40607B]: original T2 block
     * clamps roll and pitch only — yaw is NOT clamped here. The yaw clamp
     * was a port addition that throttled spin recovery after V2V hits and
     * hard cornering; removing it restores faithful behavior. */
    if (actor->angular_velocity_roll > 6000) actor->angular_velocity_roll = 6000;
    if (actor->angular_velocity_roll < -6000) actor->angular_velocity_roll = -6000;
    if (actor->angular_velocity_pitch > 6000) actor->angular_velocity_pitch = 6000;
    if (actor->angular_velocity_pitch < -6000) actor->angular_velocity_pitch = -6000;

    /* Inner-tick trace post_snap: deprecated — see post_refresh stub above. */

    /* 10. Update suspension response.
     * UNCONDITIONAL, matching original 0x00405E80 which always calls
     * UpdateVehicleSuspensionResponse (0x004057F0) when damage_lockout == 0.
     * The target-angle spring-damper block inside is gated on !g_game_paused
     * internally so the parked car doesn't tilt during countdown. */
    td5_physics_update_suspension_response(actor);

    /* SPIKE TRIGGER: log full attitude state when av_roll or av_pitch
     * exceeds ±3000 in magnitude. Catches the "car goes nuts on wall +
     * uphill" scenario without flooding the log during normal driving. */
    if (actor->slot_index == 0) {
        int32_t ar = actor->angular_velocity_roll;
        int32_t ap = actor->angular_velocity_pitch;
        if (ar > 3000 || ar < -3000 || ap > 3000 || ap < -3000) {
            TD5_LOG_I(LOG_TAG,
                "spike slot0: t=%u bm=0x%02x pos_y=%d vy=%d "
                "av{r=%d p=%d y=%d} disp{r=%d p=%d y=%d} "
                "eacc{r=%d p=%d} susp_pos=[%d %d %d %d] contact_y=[%d %d %d %d]",
                (unsigned)actor->frame_counter,
                (int)actor->wheel_contact_bitmask,
                actor->world_pos.y, actor->linear_velocity_y,
                ar, ap, actor->angular_velocity_yaw,
                (int)actor->display_angles.roll,
                (int)actor->display_angles.pitch,
                (int)actor->display_angles.yaw,
                actor->euler_accum.roll, actor->euler_accum.pitch,
                actor->wheel_suspension_pos[0], actor->wheel_suspension_pos[1],
                actor->wheel_suspension_pos[2], actor->wheel_suspension_pos[3],
                actor->wheel_contact_pos[0].y, actor->wheel_contact_pos[1].y,
                actor->wheel_contact_pos[2].y, actor->wheel_contact_pos[3].y);
        }
    }

    if (actor->slot_index == 0 && (actor->frame_counter % 60u) == 0u) {
        TD5_LOG_D(LOG_TAG, "Integrate actor0: pos=(%d,%d,%d)",
                  actor->world_pos.x,
                  actor->world_pos.y,
                  actor->world_pos.z);
    }

    /* ---- CLIP TELEMETRY ----
     * Dedicated per-tick diagnostic for slot 0. Probes the road surface at
     * the current chassis XZ and compares against pos_y. Logs a compact
     * CSV-style line every tick so we can count clip events, correlate with
     * gear/throttle, and verify whether a "should-clip" scenario (jump over
     * a gap, genuine airborne) is misclassified as a ride-height-bug clip.
     *
     * Fields:
     *   clip_t: t=<frame_counter> pos_y=<FP> probe=<FP> d=<FP_diff> vy=<FP>
     *           bm=<contact_bitmask> gear=<n> thr=<0..256> brk=<0..1>
     *           lspd=<FP> span=<raw> CLIP/ok
     * Delta is (pos_y - probe_y): NEGATIVE means pos_y BELOW ground (clip).
     * Only slot 0 and paused=0 OR every 10 frames during paused to keep
     * volume manageable. */
    if (actor->slot_index == 0) {
        int emit = 1;
        if (g_game_paused && (actor->frame_counter % 10u) != 0u) emit = 0;
        if (emit) {
            int32_t probe_y = 0;
            int probe_surf = 0;
            int probe_span = actor->track_span_raw;
            int probe_ok = td5_track_probe_height(
                actor->world_pos.x, actor->world_pos.z,
                probe_span, &probe_y, &probe_surf);
            if (probe_ok) {
                int32_t delta = actor->world_pos.y - probe_y;
                const char *flag = (delta < 0) ? "CLIP" : "ok";
                TD5_LOG_I(LOG_TAG,
                    "clip_t: t=%u pos_y=%d probe=%d d=%d vy=%d bm=0x%02X "
                    "gear=%u thr=%d brk=%u lspd=%d span=%d paused=%d %s",
                    actor->frame_counter,
                    actor->world_pos.y, probe_y, delta,
                    actor->linear_velocity_y,
                    actor->wheel_contact_bitmask,
                    actor->current_gear,
                    actor->encounter_steering_cmd,
                    actor->brake_flag,
                    actor->longitudinal_speed,
                    (int)actor->track_span_raw,
                    g_game_paused,
                    flag);
            }
        }
    }

    /* Pilot precise-port trace — snapshot outputs at exit (slot 0 only). */
}

/* ========================================================================
 * UpdateVehiclePoseFromPhysicsState (0x4063A0)
 *
 * Lightweight pose refresh (no force integration). Used as a callback
 * during track segment contact resolution.
 * ======================================================================== */

void update_vehicle_pose_from_physics(TD5_Actor *actor)
{
    /* Pilot precise-port trace — snapshot inputs at entry. */

    /* Convert current angles to display.
     *
     * Listing 0x004063D5/3DE/3E1/3F8/401/408:
     *   SAR EAX, 8           (arithmetic signed shift)
     *   MOV [actor+0x208], AX (int16 truncate — NO 12-bit mask)
     *
     * Removed the `& 0xFFF` mask the port previously applied to all three
     * components: for negative euler_accum values that mask flipped the
     * sign bit into a 12-bit positive number, diverging from the original's
     * int16 truncation. The trig helpers (cos_fixed12 / sin_fixed12) mask
     * to 12 bits internally, so the rotation matrix is unaffected, but
     * any external consumer of display_angles (camera, HUD, network) was
     * reading wrong values for negative angles.
     * [D2 — precise-port pilot 004063A0, 2026-05-14] */
    actor->display_angles.roll  = (int16_t)(actor->euler_accum.roll  >> 8);
    actor->display_angles.yaw   = (int16_t)(actor->euler_accum.yaw   >> 8);
    actor->display_angles.pitch = (int16_t)(actor->euler_accum.pitch >> 8);

    /* Render position — matches FILD/FMUL/FSTP sequence at 0x004063AA-0x0040641B.
     * (Float scale = 1/256 from [0x0045D5E8].) */
    actor->render_pos.x = (float)actor->world_pos.x * (1.0f / 256.0f);
    actor->render_pos.y = (float)actor->world_pos.y * (1.0f / 256.0f);
    actor->render_pos.z = (float)actor->world_pos.z * (1.0f / 256.0f);

    /* Build rotation matrix #1 (first call to BuildRotationMatrixFromAngles
     * @ 0x0042E1E0 at 0x00406421). */
    {
        int32_t roll_a  = actor->display_angles.roll  & 0xFFF;
        int32_t yaw_a   = actor->display_angles.yaw   & 0xFFF;
        int32_t pitch_a = actor->display_angles.pitch  & 0xFFF;

        /* Float-precision matrix build (E1 fix — see line ~4361).
         * 2026-05-15: switched to LUT-style float trig to preserve
         * sub-Q12 magnitudes for near-zero angles (see line ~4625). */
        float cr = CosFloat12bit((unsigned int)roll_a);
        float sr = SinFloat12bit(roll_a);
        float cy = CosFloat12bit((unsigned int)yaw_a);
        float sy = SinFloat12bit(yaw_a);
        float cp = CosFloat12bit((unsigned int)pitch_a);
        float sp = SinFloat12bit(pitch_a);

        /* Float32 spills (slots F/G/H/I from orig 0x42E1E0). See
         * TD5_F32_SPILL at file top for rationale. */
        float sp_sy_f32 = TD5_F32_SPILL(sp * sy);
        float cp_cy_f32 = TD5_F32_SPILL(cp * cy);
        float cp_sy_f32 = TD5_F32_SPILL(cp * sy);
        float sp_cy_f32 = TD5_F32_SPILL(sp * cy);

        actor->rotation_matrix.m[0] = sp_sy_f32 * sr + cp_cy_f32;
        actor->rotation_matrix.m[1] = cp_sy_f32 * sr - sp_cy_f32;
        actor->rotation_matrix.m[2] = sy * cr;
        actor->rotation_matrix.m[3] = sp * cr;
        actor->rotation_matrix.m[4] = cp * cr;
        actor->rotation_matrix.m[5] = -sr;
        actor->rotation_matrix.m[6] = sp_cy_f32 * sr - cp_sy_f32;
        actor->rotation_matrix.m[7] = cp_cy_f32 * sr + sp_sy_f32;
        actor->rotation_matrix.m[8] = cy * cr;
    }

    /* Refresh chassis track position from updated world coords.
     * Original 0x00406432: CALL UpdateActorTrackPosition (0x004440F0)
     *   args: (&actor->track_span_raw @+0x80, &actor->world_pos @+0x1FC).
     *
     * Without this, after a V2V/wall impulse the chassis span (+0x80) stays at
     * the pre-impulse value and downstream per-wheel probes in
     * td5_physics_refresh_wheel_contacts copy the stale span into the wheel
     * probes (the per-wheel walker then re-syncs but starts from the wrong
     * span — measurable as a 1-tick lag in lateral-wall pen tests).
     * [D1 — precise-port pilot 004063A0, 2026-05-14] */
    td5_track_update_actor_position(actor);

    /* Recompute heading-relative-to-segment short at +0x290.
     * Original 0x00406447: CALL ComputeActorHeadingFromTrackSegment (0x00445B90)
     *   args: (&actor->track_span_raw @+0x80, &actor->world_pos @+0x1FC,
     *          &actor->heading_normal @+0x290).
     *
     * Re-routed 2026-05-14 (precise-00445B90) to the byte-faithful
     * runtime variant td5_track_compute_runtime_heading_normal — it
     * writes the normalized surface normal of the picked triangle
     * (heading_normal.y ≈ 4096 on flat track, dropping on slopes),
     * unblocking ApplyMissingWheelVelocityCorrection which multiplies
     * by heading_normal.y. The previous mapping to compute_heading
     * (InitializeActorTrackPose, 0x00434350) was a SPAWN-only writer
     * that hard-coded heading_normal[1]=0, neutering the correction.
     * [precise-port pilot 00445B90, 2026-05-14] */
    td5_track_compute_runtime_heading_normal(actor);

    /* Refresh wheel contacts (CALL RefreshVehicleWheelContactFrames @ 0x00403720
     * at 0x00406453). The wheel walker, contact-frame transforms, gap_270,
     * and the OLD-bitmask snapshot at +0x37D all happen inside this call. */
    td5_physics_refresh_wheel_contacts(actor);

    /* TODO (D3+D4 — wheel-attitude switch + 2nd matrix rebuild):
     * Original 0x00406459-0x004064E1:
     *   switch (actor->wheel_contact_bitmask @ +0x37C) {   // NEW mask, NOT +0x37D
     *     case 0,1,2,4,6,8,9: TransformTrackVertexByMatrix  (0x00446030); writes back roll + pitch
     *     case 5,10:          TransformTrackVertexByMatrixB (0x00446140); writes back roll only
     *     case 3,12:          TransformTrackVertexByMatrixC (0x004461C0); writes back pitch only
     *     case 7,11,default:  no-op
     *   }
     *   euler_accum_roll  = display_angle_roll  << 8   (if A or B branch)
     *   euler_accum_pitch = display_angle_pitch << 8   (if A or C branch)
     *   BuildRotationMatrixFromAngles(rotation_matrix, display_angles)   // 2nd build
     *
     * NOTE 2026-05-25 audit: original switches on +0x37C (wheel_contact_bitmask,
     * the NEW airborne mask just written by refresh_wheel_contacts), NOT on
     * +0x37D (damage_lockout/OLD mask). Verified via Ghidra listing at
     * 0x0040645B: `MOV AL, byte ptr [ESI + 0x37c]`. The "damage_lockout"
     * name in earlier TODOs was a misread. Case values map to the airborne
     * bitmask: case 5 = FL+RL airborne (LEFT-side off) → roll-only solver;
     * case 3 = FL+FR airborne (FRONT axle off) → pitch-only solver; etc.
     *
     * The Transform* helpers derive wheel-implied roll/pitch from the post-
     * impulse wheel_contact_pos + wheel_suspension_pos, allowing the next
     * physics tick to start integration at the wheel-attitude. Without this,
     * after a collision the chassis pose remains at the impulse-applied
     * attitude even when the wheels disagree with it.
     *
     * IMPORTANT: orig 0x004063A0 (this function) is ONLY called from
     * FUN_004079c0 (V2V vehicle-to-vehicle collision response, callers
     * verified 2026-05-25 via ghidra function_callers). It is NOT per-tick.
     * The per-tick attitude rebuild lives in IntegrateVehiclePoseAndContacts
     * (orig 0x00405E80 → port td5_physics_integrate_pose at td5_physics.c
     * line ~6128) and that path's switch dispatch IS already implemented at
     * port lines 6402-6469 with the SAME case-set + helpers. So this missing
     * dispatch only affects POST-V2V-COLLISION attitude rebuild — without it,
     * after two cars touch, both chassis snap to the impulse-applied yaw
     * but retain their pre-impulse roll/pitch until the next 0x00405E80 tick
     * rebuilds them.
     *
     * Helper status (2026-05-25): A (full solver, 0x00446030) is shipped as
     * td5_physics_attitude_from_wheels (td5_physics.c near line 6054). B
     * (0x00446140) and C (0x004461C0) are ported as pure infrastructure at
     * the bottom of this file — see td5_physics_transform_track_vertex_by_matrix_b
     * and td5_physics_transform_track_vertex_by_matrix_c. AngleFromVector12
     * (0x0040A720) was already ported in the precise-trig worktree; FSQRT-
     * rounding mirrors orig FILD/FSQRT/__ftol via (int32_t)sqrtf((float)...).
     *
     * The switch dispatch + 2nd-rebuild wire-up remains DEFERRED pending
     * pool13 dynamic-diff validation. RULED OUT 2026-05-25 as candidate
     * for Jarash bouncing / not-touching-ground / yaw-rotation symptoms on
     * curved terrain — those occur during normal driving (no V2V impulse),
     * so this V2V-only dispatch is inert in that scenario. See companion
     * Frida probe tools/frida_jarash_pose_probe.js for paired-diff capture
     * to localize the actual culprit (candidate: DA-T2 case 1 always-advance
     * walker bug at td5_track.c:2619, or wheel-walker single_step on slope
     * onsets). HIGH-risk per-tick attitude change still applies — to be
     * applied in a follow-up precise-port worktree (precise-00446030). */
    /* The helpers themselves are defined at the bottom of this file with
     * __attribute__((unused)) to suppress the unused-function warning until
     * the switch dispatch lands. [PORT 2026-05-25 damage-lockout-helpers] */

    /* Second BuildRotationMatrixFromAngles call (0x004064E1).
     * Even without the switch writeback, the original UNCONDITIONALLY rebuilds
     * the rotation matrix here after the switch path completes. For cases
     * where the switch is a no-op (case 7, 11, or out-of-range), this rebuild
     * is functionally a redundant identical recomputation; for the active
     * cases (0..6, 8..10, 12) it picks up the new wheel-derived roll/pitch.
     *
     * Since the port currently SKIPS the switch (D3), this 2nd rebuild
     * regenerates the SAME matrix already built above. It is included anyway
     * so the byte-faithful sequence is preserved for the day D3 lands.
     * [D4 — precise-port pilot 004063A0, 2026-05-14]
     *
     * 2026-05-22: switched from cos_fixed12*(1/4096) to CosFloat12bit + the
     * TD5_F32_SPILL float32 intermediate spill so both BuildRotationMatrix-
     * FromAngles call sites in 0x4063A0 produce identical byte-faithful
     * matrices. Previously this 2nd rebuild OVERWROTE the first with the
     * older Q12-truncating trig path, partially undoing the precision fix
     * in the first rebuild. [session_02 follow-up, 2026-05-22] */
    {
        int32_t roll_a  = actor->display_angles.roll  & 0xFFF;
        int32_t yaw_a   = actor->display_angles.yaw   & 0xFFF;
        int32_t pitch_a = actor->display_angles.pitch  & 0xFFF;

        float cr = CosFloat12bit((unsigned int)roll_a);
        float sr = SinFloat12bit(roll_a);
        float cy = CosFloat12bit((unsigned int)yaw_a);
        float sy = SinFloat12bit(yaw_a);
        float cp = CosFloat12bit((unsigned int)pitch_a);
        float sp = SinFloat12bit(pitch_a);

        float sp_sy_f32 = TD5_F32_SPILL(sp * sy);
        float cp_cy_f32 = TD5_F32_SPILL(cp * cy);
        float cp_sy_f32 = TD5_F32_SPILL(cp * sy);
        float sp_cy_f32 = TD5_F32_SPILL(sp * cy);

        actor->rotation_matrix.m[0] = sp_sy_f32 * sr + cp_cy_f32;
        actor->rotation_matrix.m[1] = cp_sy_f32 * sr - sp_cy_f32;
        actor->rotation_matrix.m[2] = sy * cr;
        actor->rotation_matrix.m[3] = sp * cr;
        actor->rotation_matrix.m[4] = cp * cr;
        actor->rotation_matrix.m[5] = -sr;
        actor->rotation_matrix.m[6] = sp_cy_f32 * sr - cp_sy_f32;
        actor->rotation_matrix.m[7] = cp_cy_f32 * sr + sp_sy_f32;
        actor->rotation_matrix.m[8] = cy * cr;
    }

    /* Load the rebuilt matrix into the global render matrix
     * (LoadRenderRotationMatrix @ 0x0043DA80 at 0x0040650B). Chassis-snap's
     * ConvertFloatVec3ToShortAngles (0x0042E2E0) reads from this global. The
     * port's chassis-snap below uses a cached s_wheel_offset_y_world from
     * refresh — but loading the matrix maintains parity for any other
     * consumer that pumps body-space vectors through the global transform
     * before the next render tick.
     * [D5 — precise-port pilot 004063A0, 2026-05-14] */
    LoadRenderRotationMatrix(actor->rotation_matrix.m);

    /* Chassis ground snap — faithful port of the tail of
     *   UpdateVehiclePoseFromPhysicsState @ 0x004063A0
     *
     *     local_10 = sum_over_grounded(wheel_probe_y - rotated_wheel_y_offset)
     *              / count;
     *     world_pos.y = local_10;          // absolute assignment, NOT delta
     *
     * Inputs (post-refresh):
     *   wheel_contact_pos[i].y == ground_y           (refresh snapped it for
     *                                                 grounded wheels @ td5_physics.c:5210)
     *   s_wheel_offset_y_world[slot][i]               (rotated body→world Y offset,
     *                                                 pre-snap, captured in refresh)
     *
     * Replaces a port-invented `td5_track_probe_height(x, z, chassis_span)`
     * re-probe that used the chassis span for every wheel and a heuristic
     * lane picker disjoint from the per-wheel walker. With the per-wheel
     * walker fixed to single-step (2026-05-01), the wheel_contact_pos
     * values are themselves the correct ground heights at each wheel's own
     * span/lane, and the original's straight `sum/count` form lands a
     * stable chassis on slope onsets without the spurious +896 FP per-tick
     * launch the re-probe produced at Moscow span 196 (Frida-localized).
     *
     * "Grounded" = wheel_contact_bitmask bit clear (1=airborne). On total
     * airborne (count==0) world_pos.y is left alone. */
    {
        int slot = actor->slot_index;
        if (slot < 0 || slot >= 16) slot = 0;
        uint8_t lock = actor->wheel_contact_bitmask;
        int64_t target_sum = 0;
        int target_count = 0;
        for (int i = 0; i < 4; i++) {
            if (lock & (1 << i)) continue;        /* airborne — skip */
            target_sum += (int64_t)actor->wheel_contact_pos[i].y
                        - (int64_t)s_wheel_offset_y_world[slot][i];
            target_count++;
        }
        if (target_count > 0) {
            actor->world_pos.y = (int32_t)(target_sum / target_count);
            actor->render_pos.y = (float)actor->world_pos.y * (1.0f / 256.0f);
        }
    }

    /* Pilot precise-port trace — snapshot outputs at exit. */
}

/* ========================================================================
 * TransformShortVec3ByRenderMatrixRounded (0x0042EB10) -- byte-faithful port
 *
 * Original layout (from listing 0x0042EB10..0x0042EBE3):
 *   out[0] = round(p1*M[1] + p2*M[2] + p0*M[0] + M[9])
 *   out[1] = round(p1*M[4] + p0*M[3] + p2*M[5] + M[10])
 *   out[2] = round(p1*M[7] + p0*M[6] + p2*M[8] + M[11])
 *
 * The x87 sequence per output is:
 *   FILD p1; FMUL M[col_y]
 *   FILD p2; FMUL M[col_z]; FADDP
 *   FILD p0; FMUL M[col_x]; FADDP
 *   FADD M[trans]
 *   FSTP [tmp]   ; collapse 80→32 bit
 *   FLD  [tmp]   ; reload as 32-bit
 *   FISTP [out]  ; round to int32 USING THE CURRENT FPU CONTROL WORD
 *
 * [PILOT FINDING 2026-05-14] — empirical FPU control-word audit via the
 * trace at log/orig/pool4_0042EB10.csv shows 99.82% of rounded outputs
 * match `floorf` (round toward -infinity, RC=01). The 0.18% residue is
 * 80-bit vs 64-bit accumulator divergence. The original's runtime startup
 * therefore sets the x87 RC bits to 01 — not the Windows default 00
 * (round-to-nearest-even).
 *
 * Comparison vs alternative rounding modes (6228 outputs):
 *   round-to-nearest:        49.05%
 *   round-down (toward -inf): 99.82%   ← matches original
 *   round-toward-zero:       66.20%
 *   round-up (toward +inf):   0.11%
 *
 * Smoking-gun example (input p=(-255,-227,435), matrix at slot-0 wcp wheel 0):
 *   FP accumulator = 204.5853, original out2 = 204
 *   round-to-nearest → 205 (off by 1), floor → 204 (match).
 *
 * NOTE on row order: outputs 1 and 2 use a DIFFERENT operand permutation
 * than output 0 (the assembler emits "p1, p0, p2"). We match that exactly.
 *
 * NOTE on order-of-operations: the x87 chain accumulates at 80-bit until
 * the final FSTP. The single `volatile float tmp` below would force a
 * 32-bit collapse at every intermediate step. Original keeps 80-bit until
 * the last FSTP only. Since GCC -m32 -O2 typically uses x87 too, removing
 * the volatile lets the compiler emit the same FADDP chain.
 *
 * Pilot trace pool4_0042EB10.csv hooks at every call site.
 *
 * [CONFIRMED @ 0x0042EB10 by precise-port pilot 2026-05-14]
 * ======================================================================== */

/* Match orig's FISTP rounding behavior in TransformShortVec3ByRenderMatrixRounded
 * @ 0x0042EB10. Orig uses raw FISTP with the default x87 control word
 * (RC=00 = round-to-nearest-even), NOT round-toward-negative-infinity.
 *
 * Pre-2026-05-16: this used `floorf` to be "rounding-mode independent" but
 * that's actively wrong -- floorf is RTNI while orig is RNE. For values
 * near half-integers (e.g. 0.5, -0.5, 1.5) the two differ by 1 LSB, which
 * after the post-transform `<< 8` pre-scale shows up as ±256 in the
 * resulting fp8 coords. Agent T3 (Round 3 Wave 2 audit) confirmed via
 * Ghidra static disasm of 0x0042EB10 that orig uses raw FISTP, not any
 * RC-clamping prologue.
 *
 * lrintf uses the current FPU rounding mode, which on x87 defaults to
 * round-to-nearest-even. TD5_d3d.exe never changes the FPU control word,
 * so RNE is always in effect for FISTP -- and the port's lrintf inherits
 * that same RNE by default. */
static inline int32_t td5_round_toward_neg_inf_to_int32(float x) {
    return (int32_t)lrintf(x);
}

void td5_transform_short_vec3_by_render_matrix_rounded(
    const int16_t param_1[3], int32_t param_2[3], const float matrix[12])
{
    /* Promote int16 → int32 → float exactly (FILD-equivalent). */
    int p0 = (int)param_1[0];
    int p1 = (int)param_1[1];
    int p2 = (int)param_1[2];
    float fp0 = (float)p0;
    float fp1 = (float)p1;
    float fp2 = (float)p2;

    /* Output 0 — operand order from listing 0x0042EB28..0x0042EB4A
     *   ST = p1*M1 + p2*M2 + p0*M0 + M9 */
    {
        /* Keep accumulator at compiler-natural precision through the chain.
         * Only collapse once via the float assignment below before FISTP. */
        float tmp = fp1 * matrix[1] + fp2 * matrix[2] + fp0 * matrix[0] + matrix[9];
        /* `tmp` is `float` — the assignment forces 32-bit collapse the same way
         * the original's FSTP/FLD pair does. */
        param_2[0] = td5_round_toward_neg_inf_to_int32(tmp);
    }
    /* Output 1 — operand order from listing 0x0042EB6B..0x0042EB94
     *   ST = p1*M4 + p0*M3 + p2*M5 + M10 */
    {
        float tmp = fp1 * matrix[4] + fp0 * matrix[3] + fp2 * matrix[5] + matrix[10];
        param_2[1] = td5_round_toward_neg_inf_to_int32(tmp);
    }
    /* Output 2 — operand order from listing 0x0042EBB1..0x0042EBD6
     *   ST = p1*M7 + p0*M6 + p2*M8 + M11 */
    {
        float tmp = fp1 * matrix[7] + fp0 * matrix[6] + fp2 * matrix[8] + matrix[11];
        param_2[2] = td5_round_toward_neg_inf_to_int32(tmp);
    }
}

/* ========================================================================
 * RefreshVehicleWheelContactFrames (0x403720)
 *
 * Builds per-wheel contact frames for suspension/collision system.
 * Transforms wheel offsets by body rotation matrix -> world coordinates.
 * Computes wheel vertical force and airborne bitmask.
 * ======================================================================== */

void td5_physics_refresh_wheel_contacts(TD5_Actor *actor)
{
    /* Pilot precise-port trace — snapshot inputs at entry. */

    float *rot = actor->rotation_matrix.m;
    int resolved_surface = actor->surface_type_chassis;
    int resolved_surface_valid = 0;

    /* [DIAGNOSTIC 2026-05-27 — lateral-leak unit test, slot 0] On a purely
     * LONGITUDINAL slope, two ground samples displaced purely LATERALLY
     * (perpendicular to heading) must have EQUAL height. If the right sample
     * reads higher, the per-lane height interpolation is leaking the slope
     * into a sideways roll (the confirmed right-side-lift bug). We also sample
     * fore/aft so the lateral gradient (R-L) can be compared to the real
     * slope (A-B). Driven by the AI harness; read LATLEAK lines in race.log. */
    if (actor->slot_index == 0) {
        static int s_latleak_div = 0;
        if ((s_latleak_div++ % 30) == 0) {
            int yaw = (int)actor->display_angles.yaw & 0xFFF;
            float fwx = SinFloat12bit(yaw), fwz = CosFloat12bit(yaw); /* forward XZ */
            float ltx = fwz, ltz = -fwx;                              /* lateral XZ */
            int cx = actor->world_pos.x, cz = actor->world_pos.z;
            int sp = actor->track_span_raw;
            int d  = 50000;
            int yC = 0, yL = 0, yR = 0, yA = 0, yB = 0, st = 0;
            td5_track_probe_height(cx, cz, sp, &yC, &st);
            td5_track_probe_height(cx + (int)(d * ltx), cz + (int)(d * ltz), sp, &yR, &st);
            td5_track_probe_height(cx - (int)(d * ltx), cz - (int)(d * ltz), sp, &yL, &st);
            td5_track_probe_height(cx + (int)(d * fwx), cz + (int)(d * fwz), sp, &yA, &st);
            td5_track_probe_height(cx - (int)(d * fwx), cz - (int)(d * fwz), sp, &yB, &st);
            /* dRoll = display_angles.roll FIELD = front-rear-derived = the
             * car's PITCH (should track the surface slope A-B). dPitch =
             * display_angles.pitch FIELD = left-right-derived = the car's
             * ROLL (should track the lateral R-L). up.x/up.z = world up-axis
             * tilt. If slope is big but dRoll≈0 → car not following slope. */
            TD5_LOG_I(LOG_TAG,
                "LATLEAK span=%d yaw=%d slope=%d latRL=%d | car pitch(dRoll)=%d roll(dPitch)=%d | up.x=%.3f up.y=%.3f up.z=%.3f | FRwy=%d %d %d %d",
                sp, yaw, yA - yB, yR - yL,
                (int)actor->display_angles.roll, (int)actor->display_angles.pitch,
                actor->rotation_matrix.m[1], actor->rotation_matrix.m[4], actor->rotation_matrix.m[7],
                actor->wheel_contact_pos[0].y, actor->wheel_contact_pos[1].y,
                actor->wheel_contact_pos[2].y, actor->wheel_contact_pos[3].y);
        }
    }

    /* Entry snapshot: copy prior-tick airborne mask (+0x37C) into the OLD
     * slot (+0x37D). Matches original 0x00403793/0x004037D5:
     *   MOV CL, byte ptr [ESI+0x37C]
     *   MOV byte ptr [ESI+0x37D], CL
     * The per-wheel loop below will overwrite +0x37C with the freshly-
     * computed mask; +0x37D retains the OLD value for downstream gates
     * (velocity-snap @ 0x004060CE/D4, tumble-recovery transition logic). */
    actor->damage_lockout = actor->wheel_contact_bitmask;

    /* Step 1 (original 0x403720): seed each probe with the actor's
     * CURRENT span_index and sub_lane_index only. The walker (called
     * after the position transform below) updates span_index further
     * if the probe's world position crosses span boundaries, and also
     * increments span_accumulated / span_high_water on forward steps.
     *
     * span_normalized, span_accumulated, span_high_water, contact_vertex_A,
     * contact_vertex_B are NOT seeded here -- they carry over from the
     * previous tick and are updated by the walker + contactNormal
     * computation. At race start they are zero-initialized by the actor
     * memset, matching the original's tick-1 state. Whole-state diff
     * 2026-05-15 showed the prior init-time copies of actor->track_span_*
     * polluted span_norm to 102 on tick 1 while the original had 0. */
    for (int i = 0; i < 4; i++) {
        actor->wheel_probes[i].span_index     = actor->track_span_raw;
        actor->wheel_probes[i].sub_lane_index = (int8_t)actor->track_sub_lane_index;
    }

    /* Step 1b: same for body_probes (corners). The original runs TWO
     * symmetric loops -- the second writes to actor+0x00 (body_probes).
     * Without this, UpdateRaceOrder and scripted-recovery mode read
     * empty body-corner probe state. */
    for (int i = 0; i < 4; i++) {
        actor->body_probes[i].span_index     = actor->track_span_raw;
        actor->body_probes[i].sub_lane_index = (int8_t)actor->track_sub_lane_index;
    }

    /* gap_270 uses the pre-snap transform results, not post-snap positions.
     * Read old values from the persistent per-slot array (set at end of this
     * function from this frame's transform output, before Y-snap). */
    int slot = actor->slot_index;
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) slot = 0;

    /* Per-wheel contact frame computation */
    for (int i = 0; i < 4; i++) {
        /* Get wheel display angle data.
         * [2026-05-27] body-Y is read from the CARDEF directly, NOT from
         * wheel_display_angles[i][1]. That field is now rewritten every tick to
         * the suspension-adjusted RENDER value (cwy - sp_div) in
         * integrate_pose's chassis loop (matching orig 0x00405E80's psVar10
         * write), so reading it back here and subtracting sp_div again would
         * DOUBLE-count the spring and run away. The orig refresh (0x00403720)
         * reads the raw cardef wheel Y. X,Z stay static cardef (= the init
         * wheel_display_angles[0]/[2], unchanged). */
        int32_t wx = actor->wheel_display_angles[i][0];
        int32_t wy = (int32_t)CDEF_S(actor, CDEF_WHEEL_Y_BASE + i * 8);
        int32_t wz = actor->wheel_display_angles[i][2];

        /* Body-space Y of the wheel = cwy − (susp_pos>>8) − (href*0xB5>>8).
         *
         * Literal port of the instruction sequence at 0x0040384D–0x00403869
         * inside RefreshVehicleWheelContactFrames @ 0x00403720:
         *   CDQ; AND EDX,0xFF; ADD EAX,EDX; SAR EAX,8      ; sp_div  = sp / 256 (signed toward zero)
         *   SUB CX, AX                                     ; body_wy = cwy - sp_div
         *   SUB ECX, EDX                                   ; body_wy -= href_preload (preloaded once, signed /256 of href*0xB5)
         *   MOV [EBX], CX                                  ; body_wy is written as int16
         *
         * Two pre-existing port bugs corrected here:
         *   1. susp_pos (`sp`) was used RAW — susp_pos is stored in 24.8 FP
         *      (see 0x00403A20 consumers), so it must be >>8 before subtract.
         *   2. Both correction terms were ADDED. Original SUBTRACTS. This sign
         *      inversion alone produced the ~55k FP (~217 world unit)
         *      equilibrium offset above ground (memory:
         *      reference_port_ride_height_offset.md).
         * [CONFIRMED @ 0x00403720 by research agent — ride-height refactor] */
        int32_t sp = actor->wheel_suspension_pos[i];
        int32_t sp_div = (sp + (int32_t)((uint32_t)(sp >> 31) & 0xFFu)) >> 8;
        int32_t href = (int32_t)CDEF_S(actor, CDEF_SUSP_REF_HEIGHT);
        int32_t href_x181 = href * 0xB5;
        int32_t href_preload =
            (href_x181 + (int32_t)((uint32_t)(href_x181 >> 31) & 0xFFu)) >> 8;
        wy = (int32_t)(int16_t)(wy - sp_div - href_preload);

        /* Faithful port of TransformShortVec3ByRenderMatrixRounded @ 0x0042EB10
         * followed by `<<8` in 0x00403720. The original transform consumes
         * float render_pos (= world_pos/256.0f) and FISTPs the integer result;
         * the `<<8` then forces wheel_contact_pos to a multiple of 256 in
         * 24.8 FP. Earlier port path truncated `(rot*body)` then added
         * world_pos directly (preserving its low 8 bits), producing wheel
         * positions that aren't multiples of 256 — every chassis-snap then
         * inherited those leaked low bits and produced the +128 FP world_y
         * delta + ~263..1017 FP wheel XZ deltas observed in physics_trace.csv.
         * [E2 / 2026-05-02 ground-truth diff finding]
         *
         * [PRECISE-PORT PILOT 2026-05-14] — replaced the previous in-line
         * `rot*body + render_pos` expression with the byte-faithful helper
         * `td5_transform_short_vec3_by_render_matrix_rounded`, which mirrors
         * the original's operand order and 80→32-bit precision collapse.
         */
        {
            const int16_t body_off[3] = {
                (int16_t)wx, (int16_t)wy, (int16_t)wz
            };
            float matrix[12];
            for (int j = 0; j < 9; j++) matrix[j] = rot[j];
            matrix[9]  = actor->render_pos.x;
            matrix[10] = actor->render_pos.y;
            matrix[11] = actor->render_pos.z;
            int32_t world[3];
            td5_transform_short_vec3_by_render_matrix_rounded(body_off, world, matrix);
            actor->wheel_contact_pos[i].x = world[0] << 8;
            actor->wheel_contact_pos[i].y = world[1] << 8;
            actor->wheel_contact_pos[i].z = world[2] << 8;

        }

        /* Step C: stash int16-truncated body-rotated Y for chassis-snap.
         * Original `ConvertFloatVec3ToShortAngles @ 0x0042E2E0` writes
         * `(short)__ftol(by)` to actor+0x232+8*i; chassis-snap then reads
         * MOVSX EAX, [EBP+2]; SHL EAX, 8 → effectively `(int16)round(by) * 256`.
         * Port stashes the equivalent in s_wheel_offset_y_world for the
         * integrator's chassis-snap loop in update_vehicle_pose_from_physics.
         *
         * Recompute body-only Y (no render_pos add) in the same FADD order as
         * 0x0042EB10 output 1: p1*M4 + p0*M3 + p2*M5. No translation add.
         * Uses RNE (round-to-nearest-even) to match orig FISTP — see
         * td5_round_toward_neg_inf_to_int32 helper at line ~6740 and
         * memory/todo_chassis_snap_fix_2026-05-16.md. */
        {
            float by_tmp = (float)(int16_t)wy * rot[4]
                         + (float)(int16_t)wx * rot[3]
                         + (float)(int16_t)wz * rot[5];
            s_wheel_offset_y_world[slot & 0x0F][i] =
                (int32_t)(int16_t)lrintf(by_tmp) * 256;
        }

        /* Per-probe track position update [CONFIRMED @ 0x403720].
         * Original calls FUN_004440F0 per probe with probe's own world pos.
         * This gives each wheel its own span for accurate edge testing. */
        td5_track_update_probe_position(&actor->wheel_probes[i],
                                        actor->wheel_contact_pos[i].x,
                                        actor->wheel_contact_pos[i].z);

        /* Clamp per-wheel span after the probe walker (it can overflow
         * independently of the chassis span walker). */
        {
            int max_sp = td5_track_get_span_count();  /* includes branch spans */
            if (max_sp > 0 && actor->wheel_probes[i].span_index >= (int16_t)max_sp)
                actor->wheel_probes[i].span_index = (int16_t)(max_sp - 1);
        }

        /* [FIX 2026-05-27 — slope-induced false ROLL] On slopes the RIGHT
         * wheels (FR=probe1, RR=probe3) consistently over-walk one span
         * FORWARD of their LEFT partners (FL=probe0, RL=probe2). Because the
         * right wheels then sample the ground a span further along the slope,
         * an UPHILL reads right-side-high (car lifts on the right) and a
         * DOWNHILL reads right-side-low (car lifts on the left). At a dead
         * stop on a slope this is a static ~9° roll (player overlay: SPD 0,
         * ROLL 9.1). The original keeps each axle's left/right wheels on the
         * SAME span, so the slope only ever produces PITCH, never a fake roll
         * (orig L-R wheel delta is balanced; port is right-biased by ~2400fp).
         *
         * Snap each axle's right wheel onto its left partner's span. The
         * wheel keeps its own lateral sub_lane, so genuine road camber is
         * still sampled; the front-vs-rear span gap (true pitch) is untouched.
         * Guard ±2 leaves real junction/branch transitions alone. Replaces an
         * earlier chassis-clamp that only halved the split and risked
         * flattening pitch. Processed in wheel order 0,1,2,3 so the left
         * partner (i-1) is already walked. */
        /* [FIX 2026-05-31 sub-lane-shift bounce] Axle right-wheel span-snap
         * DISABLED. It papered over the slope-roll by forcing FR/RR onto the
         * left partner's span, but at sub-lane-count transitions it toggles the
         * right wheel's reference span on/off, producing the user-reported
         * vertical bounce. The faithful 0x00403720 path walks each wheel
         * independently and reads height from the walker's carried
         * span+sub_lane (see the contact-height call below). */

        /* Mirror the original's ComputeActorTrackContactNormalExtended
         * prefix: write contact_vertex_A/B to the probe based on its
         * (post-walker) span_index + sub_lane_index. */
        td5_track_compute_probe_contact_vertices(&actor->wheel_probes[i]);

        /* Compute wheel vertical force from the probed span surface. */
        int32_t wheel_y = actor->wheel_contact_pos[i].y;
        int32_t ground_y = 0;
        int surface_type = actor->surface_type_chassis;
        int probe_span = actor->wheel_probes[i].span_index;

        /* Per-wheel probe span bounds check.
         *
         * Use the FULL physical span count (td5_track_get_span_count) — branch
         * spans are indices [ring_length, s_span_count) and must be probed
         * directly, not rejected. The earlier `probe_span >= ring_length`
         * fallback mis-classified branch spans as invalid and forced every
         * wheel to read ground_y from span 0, which on Newcastle made all
         * wheels report force > 0x800 → airborne bitmask 0x0F → chassis
         * Y-snap skipped → car freefell through the branch road. */
        {
            int max_sp = td5_track_get_span_count();
            if (probe_span < 0 || probe_span >= max_sp)
                probe_span = actor->track_span_raw;
            if (probe_span < 0 || probe_span >= max_sp)
                probe_span = 0;  /* absolute fallback */
        }

        /* Ground height + surface normal at the wheel, using the BEST-FIT
         * (geometrically containing) lane within the wheel's span — NOT the
         * walker's stored sub_lane.
         *
         * [FIX 2026-05-27 — right-side-lift root cause] The per-wheel walker's
         * stored sub_lane lands on the WRONG lane on slopes (the right wheels
         * systematically), so the wheel reads a height a lane over — leaking
         * the longitudinal slope into a sideways roll (uphill→right lifts,
         * downhill→left lifts, ~9° even at a dead stop). Proven by a lateral-
         * leak unit test: sampling the surface at the containing lane is
         * balanced (R-L≈0 vs slope up to 21000), but the walked sub_lane is
         * right-biased ~6000fp (96% of rows). The original's walker resolves
         * the containing lane, so best-fit matches its result. The wheel keeps
         * its own world XZ, so genuine road camber is still sampled; only the
         * lane SELECTION is corrected.
         *
         * Also retrieves the span surface normal (orig FUN_00445A70 → actor+
         * 0x250+i*8, the "wcv" consumed by UpdateVehicleSuspensionResponse). */
        /* [FIX 2026-05-31] The BEST-FIT-lane rationale in the comment above is
         * SUPERSEDED. Per-tick lane re-selection (bestlane) made best_lane jump
         * discontinuously across sub-lane-count transitions, popping ground_y
         * (the user-reported bounce). We now use the walker's carried sub_lane,
         * faithful to 0x00403720 -> 0x00445450. If the slope-roll (2026-05-27)
         * returns, the real bug is the walker's sub_lane carry in
         * resolve_neighbor / update_position_recursive, not the height call. */
        int16_t span_normal[3] = {0, 4096, 0};  /* default: flat upward normal (magnitude 4096) */
        {
            ground_y = td5_track_compute_contact_height_bounded(
                probe_span,
                (int)actor->wheel_probes[i].sub_lane_index,
                actor->wheel_contact_pos[i].x,
                actor->wheel_contact_pos[i].z,
                span_normal);
            /* Resolve surface_type from the probe's span+lane attributes
             * instead of echoing the stale actor->surface_type_chassis
             * (which is zero on tick 1 from the actor memset). Use the
             * first probe-index 4..7 (= wheel_probes[i]) on the first
             * valid wheel, matching the original's pattern of picking the
             * surface from the lead grounded wheel. */
            if (!resolved_surface_valid) {
                resolved_surface = td5_track_get_surface_type(actor, 4 + i);
                resolved_surface_valid = 1;
            }
        }

        /* [FIX 2026-05-28 — fast-tilt out-of-bounds launch] Did the height probe
         * just cap an upward out-of-quad extrapolation? If so this wheel is over
         * a fictional plane extrapolated above its (mis-assigned) span — it must
         * NOT be allowed to read "grounded", or the chassis Y-snap ratchets the
         * car upward off-track. Force it airborne in the contact test below so
         * the car falls under gravity instead, matching the original. */
        int wheel_capped = td5_track_last_contact_was_capped();

        /* [#20 2026-06-17] The old "S18" chassis-span ground reject lived here —
         * a TD6-only block that, on the false "displaced branch" premise, discarded
         * the chassis-span ground when a wheel probed a different span (and pulled
         * the car branch->main = the "teleport to the left track"). Branch geometry
         * is actually CONNECTED (verified; see re/tools/connect_branches.py),
         * so TD6 uses the native path with no special wheel-ground override — byte-
         * identical to faithful TD5. Removed once the faithful path was confirmed. */

        /* Write surface normal to wheel_contact_velocities[i][0..2] (actor+0x250+i*8).
         * Original: FUN_00445A70 computes cross-product of span edge vectors >> 12,
         * then FUN_0042CD40 normalizes to magnitude 4096. For flat ground: (0, 4096, 0).
         * Normalize the raw cross-product normal to magnitude 4096 before writing. */
        {
            int32_t snx = span_normal[0], sny = span_normal[1], snz = span_normal[2];
            int32_t mag_sq = snx * snx + sny * sny + snz * snz;
            if (mag_sq > 0) {
                int32_t mag = td5_isqrt(mag_sq);
                if (mag > 0) {
                    snx = (snx * 4096) / mag;
                    sny = (sny * 4096) / mag;
                    snz = (snz * 4096) / mag;
                }
            } else {
                snx = 0; sny = 4096; snz = 0;
            }
            actor->wheel_contact_velocities[i][0] = (int16_t)snx;
            actor->wheel_contact_velocities[i][1] = (int16_t)sny;
            actor->wheel_contact_velocities[i][2] = (int16_t)snz;
        }

        /* Original order in FUN_00403720:
         *   1. force = (wheel_y - ground_y) + gravity_offset
         *   2. gap_270 = (new_pos - old_pos) >> 8   ← BEFORE Y-snap
         *   3. dead zone on force
         *   4. Y-snap if grounded
         * The gap_270 Y component uses the TRANSFORM-computed Y, not ground-snapped Y.
         * This ensures the velocity reflects the body's actual motion, not the snap. */

        /* Compute per-wheel contact force — literal port of 0x00403720.
         *
         *   *local_4c = (*piVar8 - local_30) + gGravityConstant;
         *   dead-zone |force| < 0x200 -> 0
         *   if (force < 0x801) grounded: snap wheel_y = ground_y
         *   else                airborne: set bit in new mask, clamp force = 12000
         *
         * The force is written to wheel_load_accum (+0x2FC) where the
         * spring-damper (IntegrateWheelSuspensionTravel @ 0x00403A20)
         * reads it next tick. This drives the convergence that keeps
         * wheel_y tracking ground_y at equilibrium and lets the per-wheel
         * airborne bit drive downstream suspension response.
         *
         * Raw 24.8 FP units — NO >>8 shift. Thresholds 0x200/0x801/12000
         * are all in the same raw scale.
         * [CONFIRMED @ 0x00403720] */
        int32_t force = (wheel_y - ground_y) + g_gravity_constant;

        /* gap_270[i] = frame-to-frame wheel contact position delta >> 8.
         * MUST compare pre-snap transform results from two consecutive frames.
         * Using post-snap (ground_y) as old causes huge Y deltas (~90k).
         *
         * 2026-05-22: the pragmatic `g_td5.paused -> zero gap_270` gate that
         * used to live here was removed once the rotation_matrix FPU spill
         * fix landed (orig 0x42E1E0 byte-faithful). If pause-frame gap_270
         * starts producing phantom impulses again, the gate's history is in
         * git (search for "countdown guard" in `td5_physics.c`). */
        {
            int16_t *g270 = (int16_t *)((uint8_t *)actor + 0x270 + i * 8);
            if (s_prev_wheel_valid[slot]) {
                int32_t dx = actor->wheel_contact_pos[i].x - s_prev_wheel_tx[slot][i];
                int32_t dy = actor->wheel_contact_pos[i].y - s_prev_wheel_ty[slot][i];
                int32_t dz = actor->wheel_contact_pos[i].z - s_prev_wheel_tz[slot][i];
                /* No clamp on the delta. Original 0x00403720 writes
                 * `(new - old) >> 8` with sign-extending round, NO bounds
                 * test (decomp: `*local_28 = (short)((*piVar1 - local_c) +
                 * (*piVar1 - local_c >> 0x1f & 0xffU) >> 8);`). Pilot Frida
                 * capture 2026-05-13 confirms orig writes large deltas
                 * verbatim. Prior `CLAMP_DELTA(±20000)` was an approximation
                 * for "teleport hides launch"; removing it per the
                 * byte-exact precise-port workflow. */
                g270[0] = (int16_t)arith_round_shift(dx, 0xFF, 8);
                g270[1] = (int16_t)arith_round_shift(dy, 0xFF, 8);
                g270[2] = (int16_t)arith_round_shift(dz, 0xFF, 8);
                if (actor->slot_index == 0 && i == 0 && (actor->frame_counter % 60u) == 0u) {
                    TD5_LOG_I(LOG_TAG, "gap270[0]: dx=%d dy=%d dz=%d g270=(%d,%d,%d) "
                              "wcv=(%d,%d,%d)",
                              dx, dy, dz, g270[0], g270[1], g270[2],
                              actor->wheel_contact_velocities[0][0],
                              actor->wheel_contact_velocities[0][1],
                              actor->wheel_contact_velocities[0][2]);
                }
            } else {
                g270[0] = 0; g270[1] = 0; g270[2] = 0;
            }
            /* Save this frame's pre-snap transform result for next frame */
            s_prev_wheel_tx[slot][i] = actor->wheel_contact_pos[i].x;
            s_prev_wheel_ty[slot][i] = actor->wheel_contact_pos[i].y;
            s_prev_wheel_tz[slot][i] = actor->wheel_contact_pos[i].z;
        }

        /* Dead zone */
        if (force > -0x200 && force < 0x200)
            force = 0;

        /* Airborne detection + ground snap.
         * Original (0x403720): when force < 0x801 (grounded), DIRECTLY
         * overwrites wheel_contact_pos[i].y = ground_y. Without this,
         * wheel_y stays at the rotation-computed value (often 0) and the
         * ground snap in integrate_pose has no valid baseline.
         * [CONFIRMED @ 0x403720 — piVar8 = local_30]
         *
         * `wheel_capped` (fast-tilt OOB fix) forces airborne regardless of force:
         * the probed ground was a capped upward extrapolation (fictional), so the
         * wheel is genuinely off-track even if its capped height happens to sit
         * near the chassis. */
        if (force > 0x800 || wheel_capped) {
            actor->wheel_contact_bitmask |= (1 << i);
            force = 12000;
        } else {
            actor->wheel_contact_bitmask &= ~(1 << i);
            /* Snap wheel Y to ground surface when grounded.
             * Original (0x403720) unconditionally snaps without checking
             * probe success — the track update above ensures a valid span. */
            actor->wheel_contact_pos[i].y = ground_y;
        }

        /* Publish the per-wheel force to wheel_load_accum (+0x2FC).
         * IntegrateWheelSuspensionTravel (0x00403A20) reads this next tick
         * as the external excitation for the spring-damper.
         * [CONFIRMED @ 0x00403720: *local_4c = ...] */
        actor->wheel_load_accum[i] = force;

        if (actor->slot_index == 0 && i == 0 && (actor->frame_counter % 60u) == 0u) {
            TD5_LOG_I(LOG_TAG, "wheel_force: slot=%d wheel=%d wy=%d gy=%d force=%d bitmask=0x%02x",
                      actor->slot_index, i, wheel_y, ground_y, force,
                      actor->wheel_contact_bitmask);
        }

        /* Store high-res wheel world position.
         *
         * [FIXED 2026-05-12 via Ghidra audit of 0x00403720]: the original
         * runs TransformShortVec3ByRenderMatrixRounded TWICE — once with
         * body_y = (cwy - sp/256 - preload) writing wheel_contact_pos
         * (the ground-contact point), then ADDS BACK preload to body_y
         * (giving cwy - sp/256) and transforms AGAIN, writing that to
         * field_0x298 = wheel_world_positions_hires (the wheel hub).
         *
         * The two transforms produce DIFFERENT world X and Z because
         * rot[1] * preload contributes to the rotated X and Z when the
         * chassis is pitched/rolled. The port previously aliased
         * wheel_world_positions_hires = wheel_contact_pos, missing this
         * rot[1]*preload offset (~5-15 fp units at typical pitch angles
         * for Viper's preload, accumulating into wrong suspension lever
         * arms in td5_physics_integrate_suspension).
         *
         * Second transform: input body_y_no_preload = wy + href_preload
         * (where wy already had the preload subtracted at line 5295).
         *
         * NO `<<8` on the hires output. Original asm 0x40388A-0x40388E only
         * shifts wheel_contact_pos (piVar8.x/y/z), NOT the hires field at
         * actor+0x298. Confirmed via pilot Frida capture 2026-05-13:
         * orig hires_x at tick 0 = -461 (raw world unit), port was writing
         * -65280 = -255<<8 — i.e. shifted body offset, not unshifted world. */
        {
            const int16_t body_off[3] = {
                (int16_t)wx, (int16_t)(wy + href_preload), (int16_t)wz
            };
            float matrix[12];
            for (int j = 0; j < 9; j++) matrix[j] = rot[j];
            matrix[9]  = actor->render_pos.x;
            matrix[10] = actor->render_pos.y;
            matrix[11] = actor->render_pos.z;
            int32_t hub[3];
            td5_transform_short_vec3_by_render_matrix_rounded(body_off, hub, matrix);
            /* NO <<8 -- the original (0x40388A-0x40388E) writes the rounded
             * world-unit value straight into actor+0x298. Comment above
             * documents this; previous version applied the shift in error,
             * producing values 256x too large. Verified via whole-state diff
             * 2026-05-15: port hub<<8 = -17829120 vs original = -69644 = hub. */
            actor->wheel_world_positions_hires[i].x = hub[0];
            actor->wheel_world_positions_hires[i].y = hub[1];
            actor->wheel_world_positions_hires[i].z = hub[2];

        }

    }

    /* Mark this slot's pre-snap transform as valid for next frame */
    s_prev_wheel_valid[slot] = 1;

    /* Original (0x00403720) runs a SECOND loop that walks each body
     * corner probe (body_probes[0..3]) through the per-probe walker and
     * ComputeActorTrackContactNormal, mirroring the wheel-probe loop.
     *
     * Body corner offsets are stored at the head of car_definition_ptr as
     * 4 x short[4] (8-byte stride, last short ignored). Each is the body
     * corner position in chassis-local space; transforming via the
     * (rot, render_pos) matrix and shifting <<8 yields the corner's world
     * position in 24.8 FP, written to actor->probe_FL/FR/RL/RR.
     *
     * Whole-state diff 2026-05-15: replaced the prior wheel_contact alias
     * (which used wheel offsets instead of body corners) with a faithful
     * port of the original's TransformShortVec3ByRenderMatrixRounded
     * pass. The walker + ComputeActorTrackContactNormal helper write
     * span_acc/hw and contact_vertex_A/B per body probe. */
    if (actor->car_definition_ptr) {
        TD5_Vec3_Fixed *body_pos[4] = {
            &actor->probe_FL,
            &actor->probe_FR,
            &actor->probe_RL,
            &actor->probe_RR,
        };
        const int16_t *cardef_corners = (const int16_t *)actor->car_definition_ptr;

        float matrix[12];
        for (int j = 0; j < 9; j++) matrix[j] = rot[j];
        matrix[9]  = actor->render_pos.x;
        matrix[10] = actor->render_pos.y;
        matrix[11] = actor->render_pos.z;

        for (int i = 0; i < 4; i++) {
            const int16_t corner_off[3] = {
                cardef_corners[i * 4 + 0],
                cardef_corners[i * 4 + 1],
                cardef_corners[i * 4 + 2],
            };
            int32_t world[3];
            td5_transform_short_vec3_by_render_matrix_rounded(corner_off, world, matrix);
            body_pos[i]->x = world[0] << 8;
            body_pos[i]->y = world[1] << 8;
            body_pos[i]->z = world[2] << 8;

            td5_track_update_probe_position(&actor->body_probes[i],
                                            body_pos[i]->x,
                                            body_pos[i]->z);
            int max_sp = td5_track_get_span_count();
            if (max_sp > 0 && actor->body_probes[i].span_index >= (int16_t)max_sp)
                actor->body_probes[i].span_index = (int16_t)(max_sp - 1);
            td5_track_compute_probe_contact_vertices(&actor->body_probes[i]);

            /* Original ComputeActorTrackContactNormal (0x00445450) overwrites
             * piVar8[1] (= probe_FL/FR/RL/RR .y) with the barycentric ground
             * height at the body corner's XZ. Use the same compute_contact_
             * height path the wheel loop uses; pass NULL for the normal
             * out-pointer since the body probe doesn't store one. */
            int probe_span = actor->body_probes[i].span_index;
            int probe_lane = actor->body_probes[i].sub_lane_index;
            if (probe_span >= 0 && probe_span < max_sp) {
                body_pos[i]->y = td5_track_compute_contact_height_with_normal(
                    probe_span, probe_lane,
                    body_pos[i]->x, body_pos[i]->z, NULL);

                /* [S18 FIX — TD6 branch-fork probe teleport, shadow corners]
                 * Same root as the wheel-contact guard above, applied to the
                 * SHADOW body corners (probe_FL/FR/RL/RR). A body-corner probe
                 * can follow a branch sentinel's link_prev to a distant span and
                 * read a wild-high ground; that stretches the shadow decal right
                 * at the fork (user report: "the shadow distorts where the car
                 * used to jump"). Re-probe the ground from the CHASSIS span at
                 * the corner's XZ and, if the corner-span ground is a large
                 * outlier (> 256 world units) and the reference did not cap, use
                 * the chassis-span ground instead. TD6-only; faithful TD5 tracks
                 * are byte-identical. */
                if (g_active_td6_level > 0) {
                    int chassis_span = (int)actor->track_span_raw;
                    if (chassis_span >= 0 && chassis_span < max_sp &&
                        chassis_span != probe_span) {
                        int32_t ground_ref = td5_track_compute_contact_height_with_normal(
                            chassis_span, probe_lane,
                            body_pos[i]->x, body_pos[i]->z, NULL);
                        int ref_capped = td5_track_last_contact_was_capped();
                        int32_t diff = body_pos[i]->y - ground_ref;
                        if (!ref_capped && (diff > 0x10000 || diff < -0x10000))
                            body_pos[i]->y = ground_ref;
                    }
                }
            }
        }
    }

    if (resolved_surface_valid)
        actor->surface_type_chassis = (uint8_t)resolved_surface;

    /* surface_contact_flags update moved to the tail of integrate_pose
     * so the spawn-time init call chain (td5_physics_init_vehicle_runtime
     * -> refresh_wheel_contacts at line 3707) does NOT seed the flag. The
     * original leaves the flag at 0 from the slot memset; it is only
     * written inside UpdatePlayerVehicleDynamics. Running it at the end of
     * integrate_pose (after refresh_wheel_contacts has updated
     * wheel_contact_bitmask) keeps the field in sync with the port's
     * existing "bits = grounded axles" interpretation while matching the
     * original's tick-1 behavior (flag=0 → airborne branch → no drive
     * torque). See the gate block in td5_physics_integrate_pose. */

    /* Pilot precise-port trace — emit row(s) at exit. */
}

/* ========================================================================
 * ClampVehicleAttitudeLimits (0x405B40) — precise-port (pool4 / session 20)
 *
 * Listing: 0x00405B40..0x00405D65 (Ghidra TD5_pool4, 2026-05-14)
 * Audit:   re/analysis/pilot_00405B40_audit.md
 *
 * Reads two 12-bit display angles (roll, pitch) and dispatches on the
 * global at 0x00463188 (Ghidra "g_cameraMode" -- actually the user's
 * 3D-collisions option):
 *   *0x00463188 == 0 -> MODE-0 (matrix recovery latch)
 *   *0x00463188 != 0 -> MODE-1 (soft nudge + hard clamp)
 *
 * The port mirrors g_collisions_enabled to this dword. NOTE: the SETTER
 * `td5_physics_set_collisions(int enabled)` writes the value INVERTED vs
 * the original ("0=on, 1=off" comment in line ~6983); that is a separate
 * upstream binding bug. This function ports the LISTING faithfully -- it
 * reads g_collisions_enabled with the same semantics as the original reads
 * *0x00463188 (so the branch routing matches the LISTING, given that the
 * port's value at this address means the same as the original's value).
 * ======================================================================== */

void td5_physics_clamp_attitude(TD5_Actor *actor)
{
    /* === Read 12-bit display angles + center-shift unwrap [0x00405B40-B85] ===
     *
     * Original: EAX = (uint16) actor->display_angle_roll  [+0x208]
     *           ECX = (uint16) actor->display_angle_pitch [+0x20C]
     *           iVar1 = ((EAX - 0x800) & 0xFFF) - 0x800   ; signed[-0x800..+0x7FF]
     *           iVar2 = ((ECX - 0x800) & 0xFFF) - 0x800
     *
     * The center-shift unwrap maps the raw uint16 to signed [-0x800,+0x7FF].
     * Critically, val == 0x800 maps to -0x800 (signed wrap), not +0x800.
     * The earlier port used (val >> 8) & 0xFFF with "if (v > 0x800) v -= 0x1000"
     * which off-by-ones at v == 0x800. */
    int32_t raw_roll  = (uint16_t)actor->display_angles.roll;
    int32_t raw_pitch = (uint16_t)actor->display_angles.pitch;
    int32_t iVar1 = td5_angle12_signed(raw_roll);   /* signed roll  */
    int32_t iVar2 = td5_angle12_signed(raw_pitch);  /* signed pitch */

    /* === Dispatch on collisions flag [0x00405B86-B88] === */
    if (g_collisions_enabled != 0) {
        /* MODE-1: soft nudge then hard clamp [0x00405B8E-C3D].
         *
         * Original layout is 8 straight-line independent `if`s with no
         * combined early-out. The signed comparisons match the listing
         * directly. Each hard-clamp branch writes BOTH the display_angles
         * field AND the euler_accum field (the earlier port omitted the
         * display_angles write). */

        /* Soft nudge: pull angular velocity back toward 0 with a fixed
         * +/-0x200 step when |angle| > nudge threshold [0x00405B8E-CE].
         * Original constants: ESI = +0x200, EDX = -0x200.
         * Roll nudge bound: 0x27F (listing CMP EAX, 0xfffffd81 / 0x27f).
         * Pitch nudge bound: 0x2BB (listing CMP ECX, 0xfffffd45 / 0x2bb). */
        if (iVar1 < -0x27F) actor->angular_velocity_roll  += 0x200;
        if (iVar1 >  0x27F) actor->angular_velocity_roll  += -0x200;
        if (iVar2 < -0x2BB) actor->angular_velocity_pitch += 0x200;
        if (iVar2 >  0x2BB) actor->angular_velocity_pitch += -0x200;

        /* Hard clamp: zero omega and pin angle at +/-limit [0x00405BCE-C3D].
         * Roll  limit: 0x355  (listing CMP EAX, 0xfffffcab / 0x355).
         * Pitch limit: 0x3A4  (listing CMP ECX, 0xfffffc5c / 0x3a4).
         *
         * Both display_angle (+0x208 / +0x20C) AND euler_accum (+0x1F0 / +0x1F8)
         * are written. euler_accum value is the display value shifted left by 8. */
        if (iVar1 < -0x355) {
            actor->angular_velocity_roll = 0;
            actor->display_angles.roll   = (int16_t)0xFCAB;  /* (uint16)-0x355 */
            actor->euler_accum.roll      = (int32_t)0xFFFCAB00; /* -0x35500 */
        }
        if (iVar1 >  0x355) {
            actor->angular_velocity_roll = 0;
            actor->display_angles.roll   = (int16_t)0x355;
            actor->euler_accum.roll      = 0x35500;
        }
        if (iVar2 < -0x3A4) {
            actor->angular_velocity_pitch = 0;
            actor->display_angles.pitch   = (int16_t)0xFC5C;  /* (uint16)-0x3A4 */
            actor->euler_accum.pitch      = (int32_t)0xFFFC5C00; /* -0x3A400 */
        }
        if (iVar2 >  0x3A4) {
            actor->angular_velocity_pitch = 0;
            actor->display_angles.pitch   = (int16_t)0x3A4;
            actor->euler_accum.pitch      = 0x3A400;
            /* original early-returns here as a compiler artifact (POP chain
             * split); functionally equivalent to falling through to RET. */
            return;
        }
        return;
    }

    /* === MODE-0: matrix recovery latch [0x00405C5C-D65] ===
     *
     * If |roll| <= 0x355 AND |pitch| <= 0x3A4, no-op early return.
     * Otherwise: build a delta-rotation matrix from (eul - omega) >> 8,
     * post-multiply by actor->rotation_matrix, store the product in
     * collision_spin_matrix (+0x180), snapshot rotation_matrix into
     * saved_orientation (+0x150), set vehicle_mode=1, zero frame_counter.
     *
     * NOTE: the earlier port had this branch disabled with a comment
     * about suspension equilibrium drift triggering spurious latches.
     * Per the precise-port mandate, this function is now ported
     * faithfully; the upstream suspension drift fix is out of scope. */

    /* Inside-limits early-out [0x00405C5C-78] */
    int within_limits = (iVar1 >= -0x355 && iVar1 <= 0x355 &&
                         iVar2 >= -0x3A4 && iVar2 <= 0x3A4);

    /* [S18 FIX — TD6-scoped recovery-latch debounce]
     * Bug: on the migrated TD6 tracks the car "goes vertical and instantly
     * triggers recovery" on down slopes. Root analysis (S18):
     *   - The TD6 collision geometry is NOT anomalously steep: measured road
     *     slopes peak ~21deg (Rome/London), with banking/curvature comparable
     *     to stock Newcastle. A 21deg slope is pitch ~0xE3, far below the 0x3A4
     *     (82deg) recovery threshold — so the limit is only reached by a
     *     TRANSIENT overshoot, not by the terrain itself.
     *   - The MODE-0 recovery latch above is byte-faithful, but the port carries
     *     a known suspension equilibrium drift (see the note above: this branch
     *     was historically DISABLED in the port for exactly this reason, then
     *     re-enabled for faithfulness with the drift fix left "out of scope").
     *     On a slope that drift transiently spikes pitch/roll past the limit for
     *     a frame or two and the immediate latch yanks the car into recovery.
     * Mitigation: on TD6 tracks ONLY (g_active_td6_level > 0), require the
     * over-limit condition to PERSIST for a few consecutive ticks before
     * latching recovery. A genuinely flipped car stays over-limit and still
     * recovers a few ticks later (~0.27s, imperceptible); a transient drift
     * spike clears before the debounce elapses, so the car keeps driving.
     * Faithful TD5 tracks (g_active_td6_level == 0) are byte-IDENTICAL: the
     * debounce block is skipped entirely and the immediate latch is preserved.
     *
     * NOTE: this addresses the SYMPTOM (the spurious instant yank). The deeper
     * root is the port suspension drift; the over-limit log below records each
     * suppressed spike so the exact spiral can be captured on a real drive and
     * the drift fixed at source in a follow-up. */
    if (g_active_td6_level > 0) {
        int dbi = (int)actor->slot_index;
        if (dbi < 0 || dbi >= TD5_MAX_TOTAL_ACTORS) dbi = 0;
        if (!within_limits) {
            if (s_td6_recovery_debounce[dbi] < 255)
                s_td6_recovery_debounce[dbi]++;
            TD5_LOG_I(LOG_TAG,
                "S18 TD6 attitude over-limit slot=%d roll=%d pitch=%d ticks=%d "
                "span_raw=%d wcb=0x%02x (latch when ticks>=%d)",
                (int)actor->slot_index, (int)iVar1, (int)iVar2,
                (int)s_td6_recovery_debounce[dbi],
                (int)*(int16_t *)((uint8_t *)actor + 0x80),
                (int)actor->wheel_contact_bitmask,
                S18_TD6_RECOVERY_DEBOUNCE_TICKS);
            if (s_td6_recovery_debounce[dbi] < S18_TD6_RECOVERY_DEBOUNCE_TICKS)
                within_limits = 1;   /* not yet persistent — suppress recovery */
        } else {
            s_td6_recovery_debounce[dbi] = 0;  /* settled — reset */
        }
    }

    if (within_limits) {
        return;
    }

    /* Build delta-rotation angles: (eul - omega) >> 8 [0x00405C7E-CB].
     * Original SAR is signed arithmetic shift right; signed int32 >> 8 in C
     * is arithmetic for the platform we target (i686). The result is
     * truncated to int16 via the `MOV word ptr [ESP+offset], reg` stores. */
    int16_t delta_roll  = (int16_t)(((int32_t)(actor->euler_accum.roll  - actor->angular_velocity_roll))  >> 8);
    int16_t delta_yaw   = (int16_t)(((int32_t)(actor->euler_accum.yaw   - actor->angular_velocity_yaw))   >> 8);
    int16_t delta_pitch = (int16_t)(((int32_t)(actor->euler_accum.pitch - actor->angular_velocity_pitch)) >> 8);

    /* BuildRotationMatrixFromAngles(&local_30, {delta_roll, delta_yaw, delta_pitch})
     * [CALL 0x0042E1E0 at 0x00405CC8].  Original takes a short* with three
     * angles laid out as roll/yaw/pitch. */
    int16_t delta_angles[3] = { delta_roll, delta_yaw, delta_pitch };
    float build_mat[9];
    BuildRotationMatrixFromAngles(build_mat, delta_angles);

    /* The original then shuffles the build output through `local_30..local_10`
     * back into `local_60[0..8]`. The shuffle is identity for the port-side
     * helper (which writes its 9-float row-major output directly into the
     * destination buffer). Skip the shuffle.
     *
     * MultiplyRotationMatrices3x3(&actor->rotation_matrix, local_60, local_60)
     * [CALL 0x0042DA10 at 0x00405D26]. */
    float product[9];
    MultiplyRotationMatrices3x3((float *)&actor->rotation_matrix,
                                build_mat, product);

    /* MOVSD.REP 12-dword copies [0x00405D2B-4C].
     *
     * Original copies 48 bytes (12 dwords) to BOTH destinations:
     *   - +0x180 collision_spin_matrix <- product (9 floats) + 3 trailing
     *     dwords from stack (originally the first row of the BuildRotation
     *     output before the shuffle). The trailing 3 dwords land in
     *     gap_1A4[12] which the actor struct marks "unused/reserved".
     *   - +0x150 saved_orientation <- rotation_matrix (9 floats) + 3
     *     trailing floats which are actor->render_pos at +0x144..+0x14F.
     *     The trailing 3 dwords land in gap_174[4] + gap_178[8] which
     *     the actor struct also marks "unused/reserved".
     *
     * Per static-port mandate we replicate the 48-byte copy semantics
     * exactly via memcpy. The trailing 12 bytes are stack/render_pos
     * residue but the original writes them, so we do too. */
    memcpy(&actor->collision_spin_matrix, product, 9 * sizeof(float));
    /* gap_1A4 trailing 12 bytes: original copies stack residue (the first
     * row of the pre-shuffle BuildRotation output, which the shuffle then
     * read into local_60). The exact values are stack garbage from the
     * compiler's perspective, but bit-exact matching means writing the
     * post-shuffle values that remain at the source addresses. Given the
     * port doesn't perform the shuffle (it writes Build output directly to
     * build_mat[0..8]), the equivalent residue is build_mat[0..2]. */
    memcpy(((uint8_t *)&actor->collision_spin_matrix) + 9 * sizeof(float),
           build_mat, 3 * sizeof(float));

    memcpy(&actor->saved_orientation, &actor->rotation_matrix, 9 * sizeof(float));
    /* gap_174/178 trailing 12 bytes: original copies the 12 bytes following
     * rotation_matrix, which is render_pos (+0x144..+0x14F). */
    memcpy(((uint8_t *)&actor->saved_orientation) + 9 * sizeof(float),
           &actor->render_pos, 3 * sizeof(float));

    /* Set recovery state flags [0x00405D4E-5D].
     *   MOV byte ptr [EBX+0x379], 0x1   -> vehicle_mode = 1
     *   MOV word ptr [EBX+0x338], 0x0   -> frame_counter = 0 (int16 write) */
    /* [S18] Event log at the recovery latch: records the attitude (roll/pitch)
     * that tripped it and the span/collision context. Fires only when recovery
     * actually engages (a rare, session-level event, NOT a per-tick path), so
     * the cost is negligible. This is the capture the user needs when driving a
     * down slope: it pins down whether pitch/roll genuinely reach the 0x3A4
     * (~82deg) / 0x355 limit and on which span, so the residual port suspension
     * drift behind the TD6 down-slope blow-up can be fixed at source. */
    if (actor->vehicle_mode != 1) {
        TD5_LOG_I(LOG_TAG,
            "S18 RECOVERY LATCH: slot=%d roll=%d pitch=%d (lim roll=0x355 pitch=0x3A4) "
            "span_raw=%d span_norm=%d collflag=%d td6=%d",
            (int)actor->slot_index, (int)iVar1, (int)iVar2,
            (int)*(int16_t *)((uint8_t *)actor + 0x80),
            (int)*(int16_t *)((uint8_t *)actor + 0x82),
            (int)g_collisions_enabled, (int)g_active_td6_level);

        /* [task #12] Damp the RETAINED linear velocity on entering roll
         * recovery. The scripted recovery integrator (integrate_scripted_motion)
         * only sheds ~1/256 of linear velocity per tick, so a car that was
         * travelling fast when it rolled past ~90deg keeps almost all of that
         * momentum through the whole ~59-frame animation and then "accelerates
         * violently in the direction it was travelling at the moment it rolled".
         * Shed most of it here, ONCE, at the 0->1 latch transition (this branch
         * runs exactly when vehicle_mode flips into recovery).
         *
         * Knob TD5RE_ROLL_RECOVER_DAMP = retained fraction in [0,1] (default
         * 0.15 = strong damp, keep 15%). "1" = retain all (old byte-faithful
         * carry-through); "0" = kill all retained linear velocity. Y is left
         * to the integrator's gravity so the car still settles onto the road. */
        {
            static int   s_rrd_init = 0;
            static float s_rrd_keep = 0.15f;
            if (!s_rrd_init) {
                s_rrd_keep = td5_env_float("TD5RE_ROLL_RECOVER_DAMP", 0.15f, 0.0f, 1.0f);
                s_rrd_init = 1;
                TD5_LOG_I(LOG_TAG, "roll_recover_damp: TD5RE_ROLL_RECOVER_DAMP=%.3f (retained linvel fraction)",
                          s_rrd_keep);
            }
            if (s_rrd_keep < 1.0f && !recovery_gentle_for_actor(actor)) {
                /* [GENTLE FLIP-RECOVERY 2026-06-21] When the gentle coast owns
                 * this player's recovery it needs the car's REAL momentum to
                 * "continue the motion" (and eases it down itself), so skip the
                 * latch-time slash here. The faithful path still slashes. */
                actor->linear_velocity_x = (int32_t)((float)actor->linear_velocity_x * s_rrd_keep);
                actor->linear_velocity_z = (int32_t)((float)actor->linear_velocity_z * s_rrd_keep);
            }
        }
    }
    actor->vehicle_mode = 1;
    actor->frame_counter = 0;

    /* Pilot precise-port trace — emit row at exit. */
}

/* Race-init hook (called from InitializeRaceVehicleRuntime in the core):
 * invalidate the pre-snap wheel-transform snapshots -- init-path transforms
 * differ from in-race transforms and cause huge gap_270 transients. */
void td5_physics_suspension_race_reset(void)
{
    memset(s_prev_wheel_valid, 0, sizeof(s_prev_wheel_valid));
}
