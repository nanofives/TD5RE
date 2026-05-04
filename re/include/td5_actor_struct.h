/**
 * td5_actor_struct.h -- RuntimeSlotActor (vehicle actor) struct for Test Drive 5
 *
 * The actor struct is the central per-vehicle data structure used during races.
 * Each slot is 0x388 (904) bytes. Up to 6 racer slots are allocated at a fixed
 * base address; when traffic is enabled, 6 additional traffic slots (6-11) follow.
 *
 * All offsets are BYTE offsets from the start of the actor.
 *
 * IMPORTANT: Many Ghidra decompilations pass the actor as `short *param_1`,
 * so param_1[N] accesses byte offset N*2. Others pass `int param_1` (raw address)
 * where param_1+N is byte offset N directly. This header uses true byte offsets.
 *
 * Coordinate system: 24.8 fixed-point for world positions and velocities.
 * Angles: 20-bit accumulators; >>8 gives 12-bit display angles (0-0xFFF = 0-360).
 *
 * Derived from reverse engineering of TD5_d3d.exe (864 named functions).
 * See re/analysis/ for per-system documentation.
 */

#ifndef TD5_ACTOR_STRUCT_H
#define TD5_ACTOR_STRUCT_H

#include <stddef.h>
#include <stdint.h>

#pragma pack(push, 1)   /* exact layout required -- no compiler padding */

/* ======================================================================
 * Constants
 * ====================================================================== */

#define TD5_ACTOR_STRIDE            0x388       /* 904 bytes per actor */
#define TD5_ACTOR_MAX_RACER_SLOTS   6
#define TD5_ACTOR_MAX_TRAFFIC_SLOTS 6
#define TD5_ACTOR_MAX_TOTAL_SLOTS   12          /* 6 racers + 6 traffic */
#define TD5_ACTOR_BASE_ADDR         0x004AB108  /* gRuntimeSlotActorTable */

#define TD5_FIXED_POINT_SHIFT       8           /* 24.8 fixed-point */
#define TD5_ANGLE_SHIFT             8           /* accum >> 8 = 12-bit display angle */
#define TD5_ANGLE_FULL_CIRCLE       0x1000      /* 4096 = 360 degrees in 12-bit */

/* Gear indices */
#define TD5_GEAR_REVERSE            0
#define TD5_GEAR_NEUTRAL            1
#define TD5_GEAR_FIRST              2           /* first forward gear */

/* Vehicle mode (vehicle_mode field) */
#define TD5_VMODE_NORMAL            0           /* normal physics simulation */
#define TD5_VMODE_SCRIPTED          1           /* scripted/collision recovery */

/* ======================================================================
 * Helper Macros
 * ====================================================================== */

/** Get pointer to actor slot N (as uint8_t*) */
#define TD5_ACTOR(slot) \
    ((uint8_t*)(TD5_ACTOR_BASE_ADDR) + (slot) * TD5_ACTOR_STRIDE)

/** Get pointer to actor slot N (as TD5_Actor*) */
#define TD5_ACTOR_PTR(slot) \
    ((TD5_Actor*)(TD5_ACTOR_BASE_ADDR + (slot) * TD5_ACTOR_STRIDE))

/** Read a field from actor slot at given byte offset */
#define TD5_ACTOR_FIELD(slot, offset, type) \
    (*(type*)(TD5_ACTOR(slot) + (offset)))

/* ======================================================================
 * Sub-structures
 * ====================================================================== */

/**
 * Vec3_Fixed -- 3-component vector in 24.8 fixed-point
 *
 * Used for world positions and velocities.
 * To convert to float: (float)v * (1.0f / 256.0f)
 */
typedef struct TD5_Vec3_Fixed {
    int32_t x;                          /* +0x00 */
    int32_t y;                          /* +0x04: vertical axis */
    int32_t z;                          /* +0x08 */
} TD5_Vec3_Fixed;

/**
 * Vec3_Float -- 3-component float vector
 *
 * Used for render positions (= world_pos * 1/256).
 */
typedef struct TD5_Vec3_Float {
    float x;                            /* +0x00 */
    float y;                            /* +0x04 */
    float z;                            /* +0x08 */
} TD5_Vec3_Float;

/**
 * Vec3_Int16 -- 3-component int16 vector
 *
 * Used for heading normals and other compact directional data.
 */
typedef struct TD5_Vec3_Int16 {
    int16_t x;                          /* +0x00 */
    int16_t y;                          /* +0x02 */
    int16_t z;                          /* +0x04 */
} TD5_Vec3_Int16;

/**
 * Mat3x3 -- 3x3 rotation matrix (row-major float)
 *
 * Built from euler angles by BuildRotationMatrixFromAngles (0x447230).
 */
#ifndef TD5_TYPES_H  /* TD5_Mat3x3 already defined in td5_types.h */
typedef struct TD5_Mat3x3 {
    float m[9];                         /* +0x00: row-major [r0c0,r0c1,r0c2, r1c0,...] */
} TD5_Mat3x3;
#endif

/**
 * EulerAccum -- 20-bit precision euler angle accumulators
 *
 * Integration: accum += angular_velocity each tick.
 * Display value = accum >> 8 (written to display angles at 0x208).
 */
typedef struct TD5_EulerAccum {
    int32_t roll;                       /* +0x00: X-axis rotation */
    int32_t yaw;                        /* +0x04: Y-axis rotation (heading) */
    int32_t pitch;                      /* +0x08: Z-axis rotation */
} TD5_EulerAccum;

/**
 * DisplayAngles -- 12-bit display-resolution euler angles
 *
 * Derived from accumulators: display = (accum >> 8) & 0xFFF.
 */
typedef struct TD5_DisplayAngles {
    int16_t roll;                       /* +0x00 */
    int16_t yaw;                        /* +0x02 */
    int16_t pitch;                      /* +0x04 */
} TD5_DisplayAngles;

/**
 * WheelSuspensionState -- per-wheel spring-damper state
 *
 * 4 instances (FL, FR, RL, RR), stored in separate arrays
 * within the actor struct rather than as a contiguous sub-struct.
 */
typedef struct TD5_WheelSuspensionState {
    int32_t position;                   /* spring deflection */
    int32_t velocity;                   /* spring-damper velocity */
    int32_t force;                      /* accumulated drive/steer force */
} TD5_WheelSuspensionState;

/**
 * TrackProbeState -- per-probe track segment position state (16 bytes)
 *
 * Each probe (wheel or body corner) maintains its own copy of the
 * track position state. Before each contact frame, the main span_index
 * (0x080) and sub_lane_index (0x08C) are copied in, then each entry
 * is independently updated by UpdateActorTrackPosition based on that
 * probe's world position.
 *
 * Written by: RenderVehicleActorModel, RefreshVehicleWheelContactFrames
 * Read by: UpdateActorTrackSegmentContacts[Forward/Reverse]
 */
typedef struct TD5_TrackProbeState {
    int16_t  span_index;                /* +0x00: current STRIP.DAT span index */
    int16_t  span_normalized;           /* +0x02: span modulo ring length */
    int16_t  span_accumulated;          /* +0x04: monotonic forward span counter */
    int16_t  span_high_water;           /* +0x06: high-water mark for ordering */
    int16_t  contact_vertex_A;          /* +0x08: left-edge track vertex index */
    int16_t  contact_vertex_B;          /* +0x0A: right-edge track vertex index */
    int8_t   sub_lane_index;            /* +0x0C: sub-lane within span */
    uint8_t  _pad_0D;                   /* +0x0D: padding */
    int16_t  _pad_0E;                   /* +0x0E: padding/unused */
} TD5_TrackProbeState;  /* 16 bytes */

/* ======================================================================
 * TD5_Actor -- Main vehicle actor struct (0x388 bytes)
 *
 * Fields marked [CONFIRMED] have been verified against Ghidra
 * decompilation of multiple functions.
 * Fields marked [CONFIRMED wave3] are from the wave-3 analysis pass.
 * Fields marked [INFERRED] are derived from analysis docs and
 * cross-referenced patterns but not individually verified in code.
 * ====================================================================== */

typedef struct TD5_Actor {

    /* === CONTACT PROBE TRACK STATE (0x000-0x07F) =====================
     *
     * 8 per-probe track position entries, each 16 bytes.
     * Entries 0-3: wheel probes (FL, FR, RL, RR)
     * Entries 4-7: body corner probes (FL, FR, RL, RR bumper)
     *
     * Before each contact frame, main span_index (0x080) and
     * sub_lane_index (0x08C) are copied into each entry. Each
     * probe is then independently updated by UpdateActorTrackPosition
     * based on that probe's world position (0x090-0x0BF for wheels,
     * 0x0C0-0x0EF for body corners).
     *
     * Normal physics mode checks only wheel probes (index table 0x467384).
     * Scripted recovery mode checks all 8 probes (index table 0x46738c).
     *
     * Written by: RenderVehicleActorModel, RefreshVehicleWheelContactFrames
     * Read by: UpdateActorTrackSegmentContacts[Forward/Reverse]
     */
    /* 2026-05-02: storage swapped to match original binary memory layout.
     * Per Ghidra of RefreshVehicleWheelContactFrames @ 0x00403720:
     *   - First loop (per-wheel suspension/contact) writes span/lane to
     *     `(actor + 0x40 + 0x10*i)` — that storage is the WHEEL probe data.
     *   - Second loop (body corners) writes to `(actor + 0x00 + 0x10*i)`.
     * Earlier port had these inverted so the per-tick physics_trace.csv
     * read all w?_span = 0 at the +0x40 offset. Field NAMES are preserved
     * so call sites (`actor->wheel_probes[i]`, `actor->body_probes[i]`)
     * remain semantically correct. Only the storage location changes. */
    TD5_TrackProbeState body_probes[4];  /* +0x000: body corners (FL,FR,RL,RR) */
    TD5_TrackProbeState wheel_probes[4]; /* +0x040: per-wheel suspension/contact */

    /* === TRACK POSITION STATE (0x080-0x08F) ==========================
     *
     * This block is passed as a pointer to UpdateActorTrackPosition
     * via (actor + 0x80). Contains the span-based position tracking
     * used for race ordering, lap counting, and AI routing.
     */
    int16_t  track_span_raw;            /* +0x080: current STRIP.DAT span index (raw) [CONFIRMED] */
    int16_t  track_span_normalized;     /* +0x082: span index modulo ring length (circuits) [CONFIRMED] */
    int16_t  track_span_accumulated;    /* +0x084: monotonic forward span counter [CONFIRMED] */
    int16_t  track_span_high_water;     /* +0x086: high-water mark for race ordering [CONFIRMED]
                                         *         UpdateRaceOrder reads this for position sort */
    uint8_t  gap_088[4];               /* +0x088: unknown auxiliary track state */
    uint8_t  track_sub_lane_index;      /* +0x08C: sub-lane within current span [CONFIRMED]
                                         *         used by FindNearestRoutePeer for same-lane match */
    uint8_t  gap_08D[3];               /* +0x08D: unknown */

    /* === CONTACT PROBE POSITIONS (0x090-0x0BF) =======================
     *
     * 4 wheel/contact probe world positions in 24.8 fixed-point.
     * Written by RefreshVehicleWheelContactFrames.
     * Read by UpdateActorTrackSegmentContacts for boundary tests.
     * RenderRaceActorForView reads these at gap_0000[idx*0xC + 0x90].
     */
    TD5_Vec3_Fixed probe_FL;            /* +0x090: front-left wheel probe [CONFIRMED] */
    TD5_Vec3_Fixed probe_FR;            /* +0x09C: front-right wheel probe [CONFIRMED] */
    TD5_Vec3_Fixed probe_RL;            /* +0x0A8: rear-left wheel probe [CONFIRMED] */
    TD5_Vec3_Fixed probe_RR;            /* +0x0B4: rear-right wheel probe [CONFIRMED] */

    /* === UPPER BOUNDING BOX VERTICES (0x0C0-0x0EF) ===================
     *
     * 4 upper bounding box corner positions (Vec3_Fixed x4, 48 bytes).
     * Written by RefreshVehicleWheelContactFrames (second loop, stride 12,
     * starting at (int*)(param_1 + 0x60) = byte 0xC0).
     * Used for body-corner probe world positions.
     *
     * Previously misidentified as collision_spin_matrix (that is at 0x180).
     */
    TD5_Vec3_Fixed bbox_vertices_upper[4]; /* +0x0C0: body corner world positions [CONFIRMED wave3] */

    /* === WHEEL CONTACT WORLD POSITIONS (0x0F0-0x11F) =================
     *
     * Per-wheel ground contact world positions in 24.8 fixed-point.
     * 4 wheels x Vec3_Fixed (12 bytes each) = 48 bytes.
     * Written by RefreshVehicleWheelContactFrames.
     * Read by IntegrateVehiclePoseAndContacts, suspension response.
     *
     * Previously misidentified as int16 track indices -- these are int32
     * world positions.
     */
    TD5_Vec3_Fixed wheel_contact_pos[4]; /* +0x0F0: FL, FR, RL, RR contact positions [CONFIRMED wave3] */

    /* === MAIN ROTATION MATRIX (0x120-0x143) ==========================
     *
     * Primary 3x3 orientation matrix (row-major float[9]).
     * Rebuilt from euler angles each integration tick by
     * BuildRotationMatrixFromAngles.
     * Accessed as (float*)(param_1 + 0x90) with short* param_1.
     */
    TD5_Mat3x3 rotation_matrix;         /* +0x120: primary orientation [CONFIRMED]
                                         *         IntegrateVehiclePoseAndContacts rebuilds;
                                         *         RenderRaceActorForView uses for transform */

    /* === RENDER POSITION (0x144-0x14F) ===============================
     *
     * Float world position for 3D rendering pipeline.
     * Computed as world_pos * (1/256) each integration tick.
     * Accessed as *(float*)(param_1 + 0xA2) with short* param_1.
     */
    TD5_Vec3_Float render_pos;          /* +0x144: float render position [CONFIRMED] */

    /* === SAVED ORIENTATION (0x150-0x173) ==============================
     *
     * Saved rotation matrix used for attitude recovery and scripted
     * mode interpolation. Written by ClampVehicleAttitudeLimits and
     * read by RefreshScriptedVehicleTransforms.
     */
    TD5_Mat3x3 saved_orientation;       /* +0x150: saved orientation for recovery [CONFIRMED] */

    /* === ORIENTATION PADDING (0x174-0x17F) ============================ */
    uint8_t  gap_174[4];               /* +0x174: unused/reserved */
    uint8_t  gap_178[8];               /* +0x178: unused/reserved */

    /* === COLLISION SPIN MATRIX (0x180-0x1A3) =========================
     *
     * 3x3 rotation matrix for scripted/collision recovery mode.
     * Written by ApplyVehicleCollisionImpulse (0x4079C0) on heavy
     * impacts (>90000). Applied as incremental rotation each frame by
     * IntegrateScriptedVehicleMotion (0x409D20) via
     * MultiplyRotationMatrices3x3.
     *
     * Accessed as (float*)(short_ptr + 0xC0) = byte 0x180.
     * Previously misplaced at 0x0C0 -- that region is bbox_vertices_upper.
     */
    TD5_Mat3x3 collision_spin_matrix;   /* +0x180: recovery rotation [CONFIRMED wave3] */

    /* === MATRIX BLOCK PADDING (0x1A4-0x1AF) ========================== */
    uint8_t  gap_1A4[12];              /* +0x1A4: unused/reserved */

    /* === CAR DEFINITION & TUNING POINTERS (0x1B0-0x1BF) ==============
     *
     * Pointers to the loaded car configuration and physics tuning
     * parameter blocks. Set during actor initialization.
     */
    void*    car_config_ptr;            /* +0x1B0: car visual config (bounding box, hardpoints) [INFERRED] */
    uint8_t  gap_1B4[4];               /* +0x1B4: unknown (zeroed on init, possibly car_visual_config_ptr) */
    void*    car_definition_ptr;        /* +0x1B8: car definition struct [CONFIRMED]
                                         *         half-width(+0x04), half-length(+0x08),
                                         *         bounding_radius(+0x80), mass(+0x88) */
    void*    tuning_data_ptr;           /* +0x1BC: tuning/physics params [CONFIRMED]
                                         *         gear ratios, torque curves, suspension,
                                         *         drivetrain type at +0x76 */

    /* === VELOCITIES & ANGULAR RATES (0x1C0-0x1D7) ====================
     *
     * Angular and linear velocities in 24.8 / 12-bit fixed-point.
     * Integrated into euler angles and world position each tick.
     * Zeroed by ResetVehicleActorState.
     *
     * Ghidra short* access: param_1[0xE0..0xEB] = bytes 0x1C0..0x1D6.
     */
    int32_t  angular_velocity_roll;     /* +0x1C0: roll angular velocity [CONFIRMED] */
    int32_t  angular_velocity_yaw;      /* +0x1C4: yaw angular velocity (heading rate) [CONFIRMED] */
    int32_t  angular_velocity_pitch;    /* +0x1C8: pitch angular velocity [CONFIRMED] */
    int32_t  linear_velocity_x;         /* +0x1CC: world-space linear velocity X [CONFIRMED]
                                         *         Ghidra names this "path_vec_x";
                                         *         used for sub-tick render interpolation */
    int32_t  linear_velocity_y;         /* +0x1D0: world-space linear velocity Y (vertical) [CONFIRMED]
                                         *         gravity subtracted here each tick */
    int32_t  linear_velocity_z;         /* +0x1D4: world-space linear velocity Z [CONFIRMED]
                                         *         Ghidra names this "path_vec_y" */

    /* === UNUSED PADDING (0x1D8-0x1EF) ================================
     *
     * Not zeroed by ResetVehicleActorState. Not accessed by any
     * identified dynamics function. Unused padding between velocity
     * and angle accumulator blocks.
     */
    uint8_t  gap_1D8[24];              /* +0x1D8: unused padding [CONFIRMED wave3] */

    /* === EULER ANGLE ACCUMULATORS (0x1F0-0x1FB) ======================
     *
     * High-precision (20-bit) angle accumulators.
     * Integration: accum += angular_velocity each tick.
     * Display value = accum >> 8 (written to display angles at 0x208).
     *
     * Ghidra short* access: param_1[0xF8..0xFE] = bytes 0x1F0..0x1FC.
     */
    TD5_EulerAccum euler_accum;         /* +0x1F0: euler angle accumulators [CONFIRMED]
                                         *         .roll=0x1F0, .yaw=0x1F4 (heading_12bit),
                                         *         .pitch=0x1F8 */

    /* === WORLD POSITION (0x1FC-0x207) ================================
     *
     * World coordinates in 24.8 fixed-point.
     * Integration: pos += velocity each tick.
     * Render position = pos * (1/256).
     *
     * Ghidra short* access: param_1[0xFE..0x102] = bytes 0x1FC..0x204.
     */
    TD5_Vec3_Fixed world_pos;           /* +0x1FC: world position [CONFIRMED]
                                         *         .x=0x1FC, .y=0x200, .z=0x204 */

    /* === DISPLAY EULER ANGLES (0x208-0x20D) ==========================
     *
     * 12-bit display-resolution angles derived from accumulators.
     * Written as (uint16_t)(euler_accum >> 8) each integration tick.
     * Used as input to BuildRotationMatrixFromAngles.
     *
     * Ghidra short* access: param_1[0x104..0x106] = bytes 0x208..0x20C.
     */
    TD5_DisplayAngles display_angles;   /* +0x208: display euler angles [CONFIRMED] */

    /* === WHEEL CONTACT FRAMES & SUSPENSION (0x20E-0x2CB) =============
     *
     * Per-wheel display angles, track contact normals, contact velocity
     * vectors, and wheel world positions. Written by
     * RefreshVehicleWheelContactFrames. Sub-field layout partially resolved.
     */
    uint8_t  gap_20E[2];               /* +0x20E: padding after display angles */
    int16_t  wheel_display_angles[4][4]; /* +0x210: per-wheel angle data (4 wheels x 4 shorts) [INFERRED] */
    int16_t  wheel_contact_normals[4][4]; /* +0x230: per-wheel track contact normals [INFERRED] */
    int16_t  wheel_contact_velocities[4][4]; /* +0x250: per-wheel contact velocity vectors [INFERRED] */
    uint8_t  gap_270[32];              /* +0x270: wheel contact velocity hires / mixed data [INFERRED] */

    /* === HEADING NORMAL (0x290-0x295) ================================
     *
     * 3-component int16 heading normal vector.
     * Written by ComputeActorHeadingFromTrackSegment.
     * Accessed as param_1+0x148 (short*) = byte 0x290.
     */
    TD5_Vec3_Int16 heading_normal;      /* +0x290: heading direction normal [CONFIRMED wave3] */

    uint8_t  gap_296[2];               /* +0x296: padding */

    /* === HIGH-RES WHEEL WORLD POSITIONS (0x298-0x2C7) ================
     *
     * Per-wheel world positions at full int32 precision.
     * Written by UpdateWheelSuspension (0x403A20), stride 12 bytes.
     * Read for suspension response and contact frame updates.
     */
    TD5_Vec3_Fixed wheel_world_positions_hires[4]; /* +0x298: FL, FR, RL, RR [CONFIRMED wave3] */

    /* === CLEAN DRIVING SCORE (0x2C8-0x2CB) =========================== */
    int32_t  clean_driving_score;       /* +0x2C8: accumulated clean driving metric [CONFIRMED wave3]
                                         *         += abs(speed)>>8 - grip_reduction/2 per frame;
                                         *         -= collision_severity on impact.
                                         *         Only accumulates while finish_time == 0 */

    /* === CENTER SUSPENSION (0x2CC-0x2DB) ============================= */
    int32_t  center_suspension_pos;     /* +0x2CC: chassis roll/pitch suspension position [CONFIRMED] */
    int32_t  center_suspension_vel;     /* +0x2D0: chassis suspension velocity [CONFIRMED] */
    uint8_t  gap_2D4[4];               /* +0x2D4: unknown */
    int32_t  prev_frame_y_position;     /* +0x2D8: previous frame world_pos.y [CONFIRMED]
                                         *         saved before integration for suspension delta */

    /* === PER-WHEEL SUSPENSION (0x2DC-0x30B) ==========================
     *
     * 4 wheels: FL(0), FR(1), RL(2), RR(3).
     * Zeroed by ResetVehicleActorState (loop at param_1+0x16E, short* = byte 0x2DC).
     */
    int32_t  wheel_suspension_pos[4];   /* +0x2DC: per-wheel spring deflection [CONFIRMED] */
    int32_t  wheel_spring_dv[4];        /* +0x2EC: per-wheel spring delta-v buffer [CONFIRMED]
                                         *         (was: wheel_force_accum — misnomer) */
    int32_t  wheel_load_accum[4];      /* +0x2FC: per-wheel load accumulator [CONFIRMED]
                                         *         (was: wheel_suspension_vel — misnomer) */

    /* === STEERING & ENGINE (0x30C-0x323) =============================
     *
     * Player/AI control outputs and drivetrain state.
     */
    int32_t  steering_command;          /* +0x30C: steering angle [-0x18000..+0x18000] [CONFIRMED]
                                         *         consumed by dynamics and friction integrators */
    int32_t  engine_speed_accum;        /* +0x310: engine RPM accumulator (idle=400) [CONFIRMED]
                                         *         UpdateEngineSpeedAccumulator writes */
    int32_t  longitudinal_speed;        /* +0x314: body-frame forward speed (signed, 8.8) [CONFIRMED]
                                         *         used for gear selection, speedometer, wheel spin */
    int32_t  lateral_speed;             /* +0x318: body-frame lateral speed [CONFIRMED]
                                         *         used for slip detection */
    int32_t  front_axle_slip_excess;    /* +0x31C: front axle slip beyond grip [CONFIRMED]
                                         *         drives tire squeal SFX */
    int32_t  rear_axle_slip_excess;     /* +0x320: rear axle slip beyond grip [CONFIRMED] */

    /* === SCORING & METRICS (0x324-0x337) =============================
     *
     * Race timing, distance, and speed tracking.
     */
    int32_t  cached_car_suspension_travel; /* +0x324: cached from car_definition+0x1E at init [CONFIRMED wave3]
                                            *         used as brake force multiplier */
    int32_t  finish_time;               /* +0x328: cumulative timer at finish [CONFIRMED]
                                         *         0 while racing, nonzero after finish.
                                         *         Ghidra: post_finish_metric_base */
    int32_t  accumulated_distance;      /* +0x32C: odometer metric (speed*frames >> 8) [CONFIRMED]
                                         *         read by BuildRaceHudMetricDigits */
    int16_t  peak_speed;                /* +0x330: maximum speed achieved [CONFIRMED] */
    int16_t  average_speed_metric;      /* +0x332: running average speed [CONFIRMED] */
    int16_t  finish_time_aux;           /* +0x334: post-finish timing auxiliary [CONFIRMED] */
    int16_t  finish_time_subtick;       /* +0x336: sub-tick precision for tiebreaking [CONFIRMED]
                                         *         used by UpdateRaceOrder in time-trial mode.
                                         *         NOTE: overloaded as checkpoint_gate_mask in
                                         *         circuit mode (bitmask 0/1/3/7/0xF for 4 gates) */

    /* === FRAME COUNTER & TIMERS (0x338-0x345) ======================== */
    int16_t  frame_counter;             /* +0x338: frames since spawn/reset [CONFIRMED]
                                         *         incremented each tick in UpdateVehicleActor;
                                         *         scripted mode timeout: reset if > 59 */
    int16_t  steering_ramp_accumulator; /* +0x33A: steering sensitivity ramp [CONFIRMED wave3]
                                         *         incremented by 0x40/frame up to 0x100;
                                         *         multiplier for steering force */
    int16_t  current_slip_metric;       /* +0x33C: instantaneous tire slip magnitude [CONFIRMED wave3]
                                         *         = abs(tire_slip) >> 8; used for SFX/feedback */
    int16_t  encounter_steering_cmd;    /* +0x33E: traffic/encounter speed command [CONFIRMED]
                                         *         0x3C=drive, 0=stopped, -0x100=brake.
                                         *         consumed as *(short*)(actor+0x33E) * 4 */
    int16_t  accumulated_tire_slip_x;   /* +0x340: tire slip X / yaw for wheel billboard [CONFIRMED] */
    int16_t  accumulated_tire_slip_z;   /* +0x342: tire slip Z / steering for wheel billboard [CONFIRMED] */
    uint16_t pending_finish_timer;      /* +0x344: checkpoint countdown timer [CONFIRMED]
                                         *         decremented per tick; 0 = DNF */

    /* === CHECKPOINT & RACE STATE (0x346-0x36A) ======================= */
    int16_t  gap_346;                   /* +0x346: unknown */
    uint8_t  gap_348[4];               /* +0x348: unknown */
    int16_t  timing_frame_counter;      /* +0x34C: timing frame counter [CONFIRMED]
                                         *         init -1, incremented per tick while racing;
                                         *         used for average speed calculation */
    int16_t  checkpoint_split_times[9]; /* +0x34E: per-checkpoint timing values [CONFIRMED wave3]
                                         *         indexed by checkpoint_count (at 0x37E);
                                         *         stores timing_frame_counter at each crossing.
                                         *         Maximum 9 checkpoints (18 bytes) */
    int16_t  airborne_frame_counter;    /* +0x360: frames with no wheel ground contact [CONFIRMED wave3]
                                         *         incremented each frame when all wheels airborne;
                                         *         >= 3 frames + wheel_contact_bitmask==0x0F
                                         *         (i.e. all wheels airborne THIS tick at +0x37C)
                                         *         triggers damping recovery (FUN_00403d90) */
    uint8_t  gap_362[9];               /* +0x362: unknown input/state block */

    /* === GEAR & CONTROL FLAGS (0x36B-0x374) ========================== */
    uint8_t  current_gear;              /* +0x36B: gear index [CONFIRMED]
                                         *         0=reverse, 1=neutral, 2..8=forward */
    uint8_t  max_gear_index;            /* +0x36C: maximum gear the car can reach [CONFIRMED wave3]
                                         *         initialized from car definition at race setup;
                                         *         gear shift up clamped to max_gear_index - 1 */
    uint8_t  brake_flag;                /* +0x36D: nonzero = braking active [CONFIRMED]
                                         *         read by RenderVehicleTaillightQuads */
    uint8_t  handbrake_flag;            /* +0x36E: nonzero = handbrake active [CONFIRMED]
                                         *         modifies rear wheel grip via tuning+0x7A */
    uint8_t  throttle_state;            /* +0x36F: coasting/neutral throttle state [CONFIRMED wave3]
                                         *         1=throttle released in neutral, 0=brake at low speed */
    uint8_t  surface_type_chassis;      /* +0x370: ground surface type under chassis [CONFIRMED]
                                         *         1=dry asphalt, 2=wet, 3=dirt, 4=gravel */
    uint8_t  tire_track_emitter_FL;     /* +0x371: tire track emitter ID (0xFF=none) [CONFIRMED] */
    uint8_t  tire_track_emitter_FR;     /* +0x372: tire track emitter ID [CONFIRMED] */
    uint8_t  tire_track_emitter_RL;     /* +0x373: tire track emitter ID [CONFIRMED] */
    uint8_t  tire_track_emitter_RR;     /* +0x374: tire track emitter ID [CONFIRMED] */

    /* === SLOT INDEX & STATE FLAGS (0x375-0x387) ======================
     *
     * The slot_index identifies the actor in the global table.
     * The flags array (0x376-0x383) contains per-actor mode and
     * contact state packed into individual bytes.
     */
    uint8_t  slot_index;                /* +0x375: actor slot number [CONFIRMED]
                                         *         0-5=racer, 6-11=traffic */
    uint8_t  surface_contact_flags;     /* +0x376: surface contact bitmask [CONFIRMED]
                                         *         bit0=rear contact, bit1=front contact */
    uint8_t  gap_377;                   /* +0x377: unknown */
    uint8_t  throttle_input_active;     /* +0x378: player throttle input [CONFIRMED wave3]
                                         *         = ~(input_flags >> 0x1C) & 1;
                                         *         1 = accelerator pressed */
    uint8_t  vehicle_mode;              /* +0x379: physics mode [CONFIRMED]
                                         *         0=normal, 1=scripted/recovery.
                                         *         controls dispatch in UpdateVehicleActor */
    uint8_t  gap_37A;                   /* +0x37A: unknown */
    uint8_t  track_contact_flag;        /* +0x37B: V2W contact flag [CONFIRMED]
                                         *         cleared each frame, set on wall collision.
                                         *         0=none, 1=left/right, 2=inner edge */
    uint8_t  wheel_contact_bitmask;     /* +0x37C: per-wheel ground contact bits THIS tick (NEW).
                                         *         bit0=FL, bit1=FR, bit2=RL, bit3=RR; 1=airborne.
                                         *         Written at refresh exit (0x004039B9 MOV
                                         *         [ESI+0x37C], AL); read by every downstream
                                         *         consumer of "live wheel airborne mask". */
    uint8_t  damage_lockout;            /* +0x37D: per-wheel ground contact bits PREV tick (OLD).
                                         *         Snapshotted at refresh entry (0x004037D5
                                         *         MOV [ESI+0x37D], CL ← [ESI+0x37C]) before the
                                         *         per-wheel loop accumulates the new mask into
                                         *         +0x37C. Original Ghidra label was
                                         *         "damage_lockout"; semantic role is the OLD
                                         *         snapshot used by the velocity-snap gate at
                                         *         0x004060CE/D4 and tumble-recovery checks. */
    uint8_t  ghost_flag;                /* +0x37E: time trial ghost [CONFIRMED]
                                         *         nonzero = force zero throttle + max brake.
                                         *         NOTE: overloaded as checkpoint_count in
                                         *         point-to-point mode -- indexes into
                                         *         checkpoint_split_times[9] at 0x34E */
    uint8_t  gap_37F;                   /* +0x37F: unknown */
    uint8_t  grip_reduction;            /* +0x380: grip reduction override [CONFIRMED]
                                         *         effective grip = min(this, race_position) */
    uint8_t  prev_race_position;        /* +0x381: previous frame's race position [CONFIRMED wave3]
                                         *         copied from race_position each frame;
                                         *         used for grip_reduction clamping */
    uint8_t  gap_382;                   /* +0x382: unknown */
    uint8_t  race_position;             /* +0x383: display race position [CONFIRMED]
                                         *         0=1st, 1=2nd, ..., 5=6th.
                                         *         written by UpdateRaceOrder */
    int32_t  special_encounter_state;   /* +0x384: encounter completion counter [CONFIRMED]
                                         *         incremented on each encounter teardown */

    /* === END OF STRUCT at 0x388 ====================================== */

} TD5_Actor;

/* ======================================================================
 * Compile-time size verification
 * ====================================================================== */

_Static_assert(sizeof(TD5_Vec3_Fixed) == 0x0C, "TD5_Vec3_Fixed must stay 12 bytes");
_Static_assert(sizeof(TD5_DisplayAngles) == 0x06, "TD5_DisplayAngles must stay 6 bytes");
_Static_assert(sizeof(TD5_EulerAccum) == 0x0C, "TD5_EulerAccum must stay 12 bytes");
_Static_assert(sizeof(TD5_TrackProbeState) == 0x10, "TD5_TrackProbeState must stay 16 bytes");
_Static_assert(sizeof(TD5_Actor) == TD5_ACTOR_STRIDE, "TD5_Actor size drifted from 0x388");
_Static_assert(offsetof(TD5_Actor, body_probes) == 0x000, "TD5_Actor.body_probes offset drifted");
_Static_assert(offsetof(TD5_Actor, wheel_probes) == 0x040, "TD5_Actor.wheel_probes offset drifted");
_Static_assert(offsetof(TD5_Actor, track_span_raw) == 0x080, "TD5_Actor.track_span_raw offset drifted");
_Static_assert(offsetof(TD5_Actor, track_sub_lane_index) == 0x08C, "TD5_Actor.track_sub_lane_index offset drifted");
_Static_assert(offsetof(TD5_Actor, probe_FL) == 0x090, "TD5_Actor.probe_FL offset drifted");
_Static_assert(offsetof(TD5_Actor, rotation_matrix) == 0x120, "TD5_Actor.rotation_matrix offset drifted");
_Static_assert(offsetof(TD5_Actor, collision_spin_matrix) == 0x180, "TD5_Actor.collision_spin_matrix offset drifted");
_Static_assert(offsetof(TD5_Actor, world_pos) == 0x1FC, "TD5_Actor.world_pos offset drifted");
_Static_assert(offsetof(TD5_Actor, display_angles) == 0x208, "TD5_Actor.display_angles offset drifted");
_Static_assert(offsetof(TD5_Actor, steering_command) == 0x30C, "TD5_Actor.steering_command offset drifted");
_Static_assert(offsetof(TD5_Actor, engine_speed_accum) == 0x310, "TD5_Actor.engine_speed_accum offset drifted");
_Static_assert(offsetof(TD5_Actor, finish_time) == 0x328, "TD5_Actor.finish_time offset drifted");
_Static_assert(offsetof(TD5_Actor, pending_finish_timer) == 0x344, "TD5_Actor.pending_finish_timer offset drifted");
_Static_assert(offsetof(TD5_Actor, current_gear) == 0x36B, "TD5_Actor.current_gear offset drifted");
_Static_assert(offsetof(TD5_Actor, slot_index) == 0x375, "TD5_Actor.slot_index offset drifted");
_Static_assert(offsetof(TD5_Actor, vehicle_mode) == 0x379, "TD5_Actor.vehicle_mode offset drifted");
_Static_assert(offsetof(TD5_Actor, track_contact_flag) == 0x37B, "TD5_Actor.track_contact_flag offset drifted");
_Static_assert(offsetof(TD5_Actor, wheel_contact_bitmask) == 0x37C, "TD5_Actor.wheel_contact_bitmask offset drifted");
_Static_assert(offsetof(TD5_Actor, damage_lockout) == 0x37D, "TD5_Actor.damage_lockout offset drifted");
_Static_assert(offsetof(TD5_Actor, race_position) == 0x383, "TD5_Actor.race_position offset drifted");

/* ======================================================================
 * Race Slot State Table
 *
 * Separate 1-byte-per-slot table that indicates human(1) vs AI(0).
 * Located at 0x4AE272, stride 1, 6 entries.
 * ====================================================================== */

#define TD5_RACE_SLOT_STATE_BASE    0x004AE272
#define TD5_RACE_SLOT_STATE(slot)   (*(uint8_t*)(TD5_RACE_SLOT_STATE_BASE + (slot)))

/* Race order table: sorted slot indices by position. 6 bytes at 0x4AE278. */
#define TD5_RACE_ORDER_BASE         0x004AE278
#define TD5_RACE_ORDER(pos)         (*(uint8_t*)(TD5_RACE_ORDER_BASE + (pos)))

/* ======================================================================
 * Legacy offset defines (backward-compatible with td5_sdk.h)
 *
 * These match the ACTOR_OFF_* defines used by the ASI mod framework.
 * ====================================================================== */

#define ACTOR_OFF_SPAN_INDEX        0x082   /* int16: track_span_normalized */
#define ACTOR_OFF_SPAN_COUNTER      0x084   /* int16: track_span_accumulated */
#define ACTOR_OFF_WORLD_POS_X       0x1FC   /* int32: world_pos.x (24.8 fixed) */
#define ACTOR_OFF_WORLD_POS_Y       0x200   /* int32: world_pos.y */
#define ACTOR_OFF_WORLD_POS_Z       0x204   /* int32: world_pos.z */
#define ACTOR_OFF_FORWARD_SPEED     0x314   /* int32: longitudinal_speed */
#define ACTOR_OFF_LATERAL_SPEED     0x318   /* int32: lateral_speed */
#define ACTOR_OFF_ENGINE_SPEED      0x310   /* int32: engine_speed_accum */
#define ACTOR_OFF_STEERING_CMD      0x30C   /* int32: steering_command */
#define ACTOR_OFF_GEAR_CURRENT      0x36B   /* int8:  current_gear */
#define ACTOR_OFF_RACE_FINISHED     0x328   /* int32: finish_time (0=racing) */
#define ACTOR_OFF_PHYSICS_PTR       0x1BC   /* ptr:   tuning_data_ptr */
#define ACTOR_OFF_TUNING_PTR        0x1B8   /* ptr:   car_definition_ptr */
#define ACTOR_OFF_VEHICLE_MODE      0x379   /* int8:  vehicle_mode (0=normal, 1=scripted) */
#define ACTOR_OFF_SLOT_INDEX        0x375   /* int8:  slot_index */
#define ACTOR_OFF_RACE_POSITION     0x383   /* int8:  race_position (0=1st) */
#define ACTOR_OFF_ROTATION_MATRIX   0x120   /* float[9]: rotation_matrix */
#define ACTOR_OFF_RENDER_POS        0x144   /* float[3]: render_pos */
#define ACTOR_OFF_EULER_ACCUM       0x1F0   /* int32[3]: euler angle accumulators */
#define ACTOR_OFF_DISPLAY_ANGLES    0x208   /* int16[3]: display euler angles */
#define ACTOR_OFF_ANG_VEL_ROLL      0x1C0   /* int32: angular_velocity_roll */
#define ACTOR_OFF_ANG_VEL_YAW       0x1C4   /* int32: angular_velocity_yaw */
#define ACTOR_OFF_ANG_VEL_PITCH     0x1C8   /* int32: angular_velocity_pitch */
#define ACTOR_OFF_LIN_VEL_X         0x1CC   /* int32: linear_velocity_x */
#define ACTOR_OFF_LIN_VEL_Y         0x1D0   /* int32: linear_velocity_y */
#define ACTOR_OFF_LIN_VEL_Z         0x1D4   /* int32: linear_velocity_z */

#pragma pack(pop)

#endif /* TD5_ACTOR_STRUCT_H */
