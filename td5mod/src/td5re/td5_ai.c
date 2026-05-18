/**
 * td5_ai.c -- AI routing, rubber-banding, traffic, script VM
 *
 * Reimplementation of:
 *   0x434FE0  UpdateActorTrackBehavior
 *   0x4340C0  UpdateActorSteeringBias
 *   0x4370A0  AdvanceActorTrackScript (12-opcode bytecode VM)
 *   0x434900  UpdateActorTrackOffsetBias
 *   0x4337E0  FindActorTrackOffsetPeer
 *   0x434AA0  UpdateActorRouteThresholdState
 *   0x432D60  ComputeAIRubberBandThrottle
 *   0x432E60  InitializeRaceActorRuntime
 *   0x434DA0  UpdateSpecialTrafficEncounter
 *   0x434BA0  UpdateSpecialEncounterControl
 *   0x4353B0  RecycleTrafficActorFromQueue
 *   0x435940  InitializeTrafficActorsFromQueue
 *   0x435E80  UpdateTrafficRoutePlan
 *   0x433680  FindNearestRoutePeer
 *   0x436A70  UpdateRaceActors (master dispatcher)
 */

#include "td5_ai.h"
#include "td5_track.h"
#include "td5_physics.h"
#include "td5_platform.h"
#include "td5_sound.h"
#include "td5_trace.h"
#include "td5re.h"
#include <string.h>
#include <math.h>

#define LOG_TAG "ai"

/* ========================================================================
 * Route State access macros
 *
 * Route state array: base 0x4AFB60, stride 0x47 dwords (0x11C bytes).
 * Each field index is a dword offset within the entry.
 * ======================================================================== */

#define RS_STRIDE_DWORDS  0x47
#define RS_STRIDE_BYTES   0x11C

#define RS_ROUTE_TABLE_PTR        0x00
#define RS_ROUTE_TABLE_SELECTOR   0x03
#define RS_DEFAULT_THROTTLE       0x04
/* RS dword 0x09 (byte 0x24) per Ghidra: DAT_004afb84 (0x004afb84) is the
 * per-slot lateral_bias array. With gActorRouteStateTable base = 0x004afb60,
 * `0x4afb84 - 0x4afb60 = 0x24` bytes = dword index 9.
 * [CONFIRMED via Ghidra symbol audit pass 2026-05-11 cross-checking stride
 * against DAT_004afbb0 (+0x50 = index 20 = RS_ACTIVE_LOWER_BOUND) and
 * gActorTrackSpanProgress (+0x64 = index 25 = RS_TRACK_PROGRESS).]
 *
 * A prior port revision used 0x0B with a comment that computed the offset
 * as 0x2C — that arithmetic is wrong (0x84 - 0x60 = 0x24, not 0x2C). */
#define RS_TRACK_OFFSET_BIAS      0x09
#define RS_LEFT_BOUNDARY_A        0x0E
#define RS_LEFT_BOUNDARY_B        0x0F
#define RS_RIGHT_BOUNDARY_A       0x10
#define RS_RIGHT_BOUNDARY_B       0x11
#define RS_RIGHT_EXTENT_A         0x12
#define RS_RIGHT_EXTENT_B         0x13
#define RS_ACTIVE_LOWER_BOUND     0x14
#define RS_ACTIVE_UPPER_BOUND     0x15
#define RS_LEFT_DEVIATION         0x16
#define RS_RIGHT_DEVIATION        0x17
#define RS_FORWARD_TRACK_COMP     0x18
#define RS_TRACK_PROGRESS         0x19
#define RS_SCRIPT_OFFSET_PARAM    0x1B
#define RS_ENCOUNTER_HANDLE       0x1F
#define RS_RECOVERY_STAGE         0x22
/* RS_DIRECTION_POLARITY_LEGACY = 0x25 (byte 0x94): this offset is NOT a real
 * field — the original game listing has zero references to base+0x94. The
 * macro was port-only and reads/writes here had no effect. Kept as a
 * compile-time alias so any leftover code paths can be audited if found; do
 * not introduce new uses. The canonical field is RS_ROUTE_DIRECTION_POLARITY
 * at dword index 0x3F (byte 0xFC), which corresponds to the original symbol
 * `gActorRouteDirectionPolarity` at 0x004afc5c.
 */
#define RS_DIRECTION_POLARITY_LEGACY 0x25  /* DEPRECATED — do not use in new code */
#define RS_ROUTE_DIRECTION_POLARITY  0x3F  /* gActorRouteDirectionPolarity @ 0x4afc5c */
#define RS_SLOT_INDEX             0x35
#define RS_SCRIPT_BASE_PTR        0x3A
#define RS_SCRIPT_IP              0x3B
#define RS_SCRIPT_SPEED_PARAM     0x3C
#define RS_SCRIPT_FLAGS           0x3D
#define RS_SCRIPT_FIELD_3E        0x3E
#define RS_SCRIPT_FIELD_43        0x43
#define RS_SCRIPT_COUNTDOWN       0x45

/* ========================================================================
 * Actor runtime access macros (stride 0xE2 dwords = 0x388 bytes)
 *
 * Byte offsets from actor base.
 * ======================================================================== */

#define ACTOR_STRIDE              0x388

#define ACTOR_SPAN_RAW            0x080   /* int16: raw span index — JUMPS at junction remap */
#define ACTOR_SPAN_NORMALIZED     0x082   /* int16: wrapped-but-not-remapped span for route-table indexing */
#define ACTOR_SPAN_ACCUM          0x084   /* int16: monotonically-accumulated span; immune to remap */
#define ACTOR_SUB_LANE_INDEX      0x08C   /* byte  */
#define ACTOR_CAR_DEF_PTR         0x1B8   /* void*: car definition pointer (bounding box, mass) */
#define ACTOR_YAW_ACCUM           0x1F4   /* int32 */
#define ACTOR_STEERING_CMD        0x30C   /* int32 */
#define ACTOR_LONGITUDINAL_SPEED  0x314   /* int32 */
#define ACTOR_REAR_AXLE_SLIP      0x320   /* int32: rear_axle_slip_excess */
#define ACTOR_STEERING_RAMP_ACCUM 0x33A   /* int16: steering_ramp_accumulator */
#define ACTOR_LIN_VEL_X           0x1CC   /* int32: world-space velocity X */
#define ACTOR_LIN_VEL_Z           0x1D4   /* int32: world-space velocity Z */
#define ACTOR_WORLD_POS_X         0x1FC   /* int32: 24.8 fixed */
#define ACTOR_WORLD_POS_Z         0x204   /* int32: 24.8 fixed */
#define ACTOR_ENCOUNTER_STEER     0x33E   /* int16 */
#define ACTOR_BRAKE_FLAG          0x36D   /* byte  */
#define ACTOR_THROTTLE_STATE      0x36F   /* byte  */
#define ACTOR_SLOT_INDEX          0x375   /* byte  */
#define ACTOR_VEHICLE_MODE        0x379   /* byte  */
#define ACTOR_TRACK_CONTACT_FLAG  0x37B   /* byte: V2W contact flag (1=wall, 2=boundary) */
#define ACTOR_ENCOUNTER_STATE     0x384   /* int32 */

/* Wheel contact probe positions (4 × Vec3_Fixed @ +0x90, stride 0x0C).
 * Used by UpdateActorTrackBounds [CONFIRMED @ 0x004366E0-0x00436891]. */
#define ACTOR_PROBE_FL_BASE       0x090   /* int32 x,y,z = probe_FL */
#define ACTOR_PROBE_FR_BASE       0x09C   /* int32 x,y,z = probe_FR */
#define ACTOR_PROBE_RL_BASE       0x0A8   /* int32 x,y,z = probe_RL */
#define ACTOR_PROBE_RR_BASE       0x0B4   /* int32 x,y,z = probe_RR */

/* ========================================================================
 * Helper accessors (cast through char* arithmetic)
 * ======================================================================== */

static int32_t  g_route_state_storage[TD5_MAX_TOTAL_ACTORS * RS_STRIDE_DWORDS];
static int32_t *g_route_state_base;     /* 0x4AFB60 */
static char    *g_actor_base;           /* 0x4AB108 */
static const uint8_t *g_route_tables[2];
static size_t         g_route_table_sizes[2];

static inline int32_t *route_state(int slot) {
    return g_route_state_base + slot * RS_STRIDE_DWORDS;
}

/* Public accessor used by td5_physics.c */
int32_t *td5_ai_get_route_state(int slot) {
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS || !g_route_state_base)
        return NULL;
    return route_state(slot);
}

/* Pilot trace accessor (pool9_00434FE0): used by td5_pilot_trace_00434FE0.c
 * to compute the is_canonical_route field. Returns the LEFT.TRK table ptr
 * (selector 0). */
void *td5_ai_get_left_route_ptr(void) {
    return (void *)g_route_tables[0];
}

static inline char *actor_ptr(int slot) {
    return g_actor_base + slot * ACTOR_STRIDE;
}

#define ACTOR_I16(base, off)  (*(int16_t *)((base) + (off)))
#define ACTOR_I32(base, off)  (*(int32_t *)((base) + (off)))
#define ACTOR_U8(base, off)   (*(uint8_t *)((base) + (off)))
#define ACTOR_I8(base, off)   (*(int8_t  *)((base) + (off)))

/* ========================================================================
 * Globals
 * ======================================================================== */

/* Rubber-band parameters (4 globals at 0x473D9C-0x473DA8) */
/* Pool11 pilot: exposed (non-static) so td5_pilot_trace_00432D60.c can
 * snapshot inputs without struct drift. */
int32_t g_rb_behind_scale;       /* 0x473D9C */
int32_t g_rb_ahead_scale;        /* 0x473DA0 */
int32_t g_rb_behind_range;       /* 0x473DA4 */
int32_t g_rb_ahead_range;        /* 0x473DA8 */

/* Default throttle table (0x473D64, 6+padding entries) */
int32_t g_default_throttle[14] = {
    0x0100, 0x0100, 0x0140, 0x0118, 0x0122, 0x0140,
    0, 0, 0, 0, 0, 0, 0, 0
};

/* Live throttle table (0x473D2C) -- copied from default each tick,
 * then rubber-band-modified per slot */
int32_t g_live_throttle[14];

/* Per-actor throttle bias output consumed by route threshold */
/* Pool11 pilot: exposed for td5_pilot_trace_00432D60.c. */
int32_t g_actor_route_steer_bias[TD5_MAX_TOTAL_ACTORS];

/* Per-actor forward track component */
static int32_t g_actor_forward_track_component[TD5_MAX_TOTAL_ACTORS];

/* Slot state: 0x00=AI, 0x01=player, 0x02=finished/dead */
static int32_t g_slot_state[TD5_MAX_TOTAL_ACTORS];

/* Total active actor count (6=no traffic, 12=with traffic, 2=time trial) */
static int32_t g_active_actor_count;

/* Traffic recovery stage per slot (0=normal, 1-7=recovering) */
static int32_t g_traffic_recovery_stage[TD5_MAX_TOTAL_ACTORS];

/* Lateral avoidance direction toggle (0x4B08B0): 0=push negative, 1=push positive.
 * Persistent; flips only when peer reaches a lane boundary (sub_lane==0 or ==lane_count). */
static int32_t g_lateral_avoidance_direction;

/* Track script program banks (hardcoded .rdata at 0x473CD4-0x473D18) */

/* Program A @ 0x473CD4: stop, set offset -32, steer-left, stop, terminate */
static const int32_t g_script_program_a[] = {
    8, 2, (int32_t)0xFFFFFFE0, 5, 8, 0
};

/* Program B @ 0x473CEC: stop, set offset +64, steer-right, terminate */
static const int32_t g_script_program_b[] = {
    8, 2, 0x40, 6, 0
};

/* Program C @ 0x473D00: stop, set offset -32, steer-right, stop, terminate */
static const int32_t g_script_program_c[] = {
    8, 2, (int32_t)0xFFFFFFE0, 6, 8, 0
};

/* Program D @ 0x473D18: stop, set offset +64, steer-left, terminate */
static const int32_t g_script_program_d[] = {
    8, 2, 0x40, 5, 0
};

/* Array of 4 program bank pointers for round-robin cycling */
static const int32_t *g_script_banks[4] = {
    g_script_program_a,
    g_script_program_b,
    g_script_program_c,
    g_script_program_d
};

/* Initial recovery script (0x473CC8): stop, auto-select, terminate */
static const int32_t g_script_init_recovery[] = { 8, 9, 0 };

/* Current script bank index per-actor for round-robin */
static int g_script_bank_index[TD5_MAX_TOTAL_ACTORS];
static int32_t g_last_logged_opcode[TD5_MAX_TOTAL_ACTORS];

/* Special encounter globals */
static int32_t g_encounter_tracked_handle = -1;    /* -1 = none */
static int32_t g_encounter_enabled;                 /* master gate */
static int32_t g_encounter_cooldown;                /* counts down from 300 */
static int32_t g_encounter_active[TD5_MAX_TOTAL_ACTORS];
static int32_t g_encounter_control_active_latch;
static int32_t g_encounter_steer_bias_latch;
static int32_t g_encounter_route_table_selector;
static int32_t g_encounter_phase_flag;              /* 0=acquisition, !0=tracking */
static int32_t g_encounter_ref_slot;                /* 0x4B0630 */
/* gSpecialEncounterMinForwardTrackComponentThreshold @ 0x4B05BC — gates the
 * brake band in UpdateSpecialEncounterControl. Defaults to 0 in static memory. */
static int32_t g_special_encounter_min_fwd_track_threshold;

/* Per-car torque triplets table (9 cars x 3 dwords at 0x466F90) */
static int32_t g_car_torque_triplets[9 * 3];

/* AI physics template (128 bytes at 0x473DB0 in TD5_d3d.exe) — verbatim
 * copy of the original binary's global AI tuning data. Used by the
 * UpdateAIVehicleDynamics bicycle solve for all AI slots; the port
 * previously pointed AI slots' tuning_data_ptr at per-slot carparam.dat,
 * which gave DIFFERENT Wf/Wr/I values and flipped the sign of
 * D = (Wf*Wr - I) >> 10 in the bicycle solve. That flipped the sign of
 * front_lat at v≈0 with saturated steer, which is what drove the
 * positive-feedback yaw spin symptom. */
static uint8_t g_ai_physics_template[128] = {
    0xA0, 0x00, 0xC0, 0x00, 0xD8, 0x00, 0xE0, 0x00,  /* +0x00 torque curve */
    0xE4, 0x00, 0xE8, 0x00, 0xEC, 0x00, 0xF0, 0x00,  /* +0x08 */
    0xF4, 0x00, 0xF8, 0x00, 0xFC, 0x00, 0x00, 0x01,  /* +0x10 */
    0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01,  /* +0x18 */
    0x20, 0xBF, 0x02, 0x00, 0xC0, 0x5D, 0x00, 0x00,  /* +0x20 I=180000 half_wb=24000 */
    0x90, 0x01, 0x90, 0x01, 0xAC, 0x0D, 0xF0, 0x0A,  /* +0x28 Wf=400 Wr=400 grip=3500 */
    0x00, 0x00, 0xC4, 0x09, 0x3A, 0x07, 0x5A, 0x05,  /* +0x30 gear ratios */
    0xF2, 0x03, 0xEE, 0x02, 0x26, 0x02, 0x0F, 0x27,  /* +0x38 */
    0x00, 0x00, 0x64, 0x19, 0x64, 0x19, 0x64, 0x19,  /* +0x40 */
    0x64, 0x19, 0x64, 0x19, 0x0F, 0x27, 0x0F, 0x27,  /* +0x48 */
    0x00, 0x00, 0x00, 0x00, 0xCC, 0x10, 0xCC, 0x10,  /* +0x50 */
    0xCC, 0x10, 0xCC, 0x10, 0xCC, 0x10, 0x32, 0x00,  /* +0x58 */
    0x28, 0x00, 0x1E, 0x00, 0x00, 0x20, 0x18, 0x00,  /* +0x60 */
    0x64, 0x00, 0x2C, 0x01, 0xB8, 0x0B, 0x58, 0x02,  /* +0x68 steer=0x100 brake=0x2C1? speedlim=0x0BB8 */
    0x58, 0x02, 0xE8, 0x1C, 0x2A, 0x04, 0x03, 0x00,  /* +0x70 */
    0x50, 0x00, 0xA0, 0x00, 0xA0, 0x00, 0x00, 0x00,  /* +0x78 */
};

/* Difficulty tier (0/1/2) mirrors gRaceDifficultyTier @ 0x00463210.
 * Written by ConfigureGameTypeFlags @ 0x00410CA0 (port: td5_frontend.c:2675)
 * from game_type: 1/2→0, 3/4/5→1, 6/7→2.
 * Pre-2026-05-14: this file-static was *never written* in the port — every
 * race used tier=0 regardless of game type, which produced systematic AI
 * under-performance on tier-1/2 races (pool11 0x00432D60 audit blocker #1).
 * The fix routes through g_td5.difficulty_tier instead — keep this static
 * removed and rely on the global TD5 state.
 *
 * static int32_t g_race_difficulty_tier;  -- REMOVED 2026-05-14 (precise-00432E60)
 */

/* Traffic queue pointer and base */
static const uint8_t *g_traffic_queue_base;
static const uint8_t *g_traffic_queue_ptr;

/* Frame counter for misc timing */
static uint32_t g_ai_frame_counter;

/* Wanted-mode damage state table (mirrors gWantedDamageStateTable @ 0x4BEAD4).
 * int16[6], one per racer slot. Initialized to 0x1000 per slot.
 * Decremented on player<->cop collision by 0x200 (impact<=20000) or 0x400 (impact>20000)
 * [CONFIRMED @ AwardWantedDamageScore 0x43D690]. When slot reaches 0, cop is arrested
 * and its AI is frozen [CONFIRMED @ UpdateRaceActors 0x436E1D gate]. */
static int16_t g_wanted_damage_state[TD5_MAX_RACER_SLOTS];

/* ========================================================================
 * Forward declarations (internal helpers)
 * ======================================================================== */

static void ai_update_single_racer(int slot);
static void ai_update_single_traffic(int slot);

uint8_t *td5_ai_get_physics_template(void) {
    return g_ai_physics_template;
}

static int32_t ai_cos_fixed12(int32_t angle);
static int32_t ai_sin_fixed12(int32_t angle);
static void td5_ai_refresh_route_state_slot(int slot);
static int32_t ai_angle_from_vector(int32_t dx, int32_t dz);

/* Pre-loop helpers for the per-slot bias-clamp/boundary chain ported from
 * UpdateRaceActors @ 0x00436A70. Gated behind g_td5.ini.experimental_bias_clamp. */
static void td5_ai_update_actor_track_bounds(int slot);
static int  td5_ai_classify_track_offset_clamp_v2(int slot, int track_offset_bias);
static int  td5_ai_remap_for_classify(int span_normalized);

static int32_t ai_cos_fixed12(int32_t angle) {
    /* Use standard math — the quadratic approximation was catastrophically
     * wrong at quadrant boundaries (sin(0) returned -4096 instead of 0). */
    double rad = (double)(angle & 0xFFF) * (2.0 * 3.14159265358979323846 / 4096.0);
    return (int32_t)(cos(rad) * 4096.0);
}

static int32_t ai_sin_fixed12(int32_t angle) {
    double rad = (double)(angle & 0xFFF) * (2.0 * 3.14159265358979323846 / 4096.0);
    return (int32_t)(sin(rad) * 4096.0);
}

/* ========================================================================
 * AngleFromVector12 -- atan2-based angle computation (0x40A720 equivalent)
 *
 * Returns angle in 0..4095 (12-bit), matching original's table-based LUT.
 * Original uses 1024-entry atan LUT at 0x463214; we use atan2f for precision.
 *
 * Coordinate system: 0=north(+Z), 0x400=east(+X), 0x800=south(-Z), 0xC00=west(-X)
 * ======================================================================== */

static int32_t ai_angle_from_vector(int32_t dx, int32_t dz) {
    if (dx == 0 && dz == 0) return 0;
    /* atan2(dx, dz) gives angle from +Z axis, clockwise = positive */
    double angle_rad = atan2((double)dx, (double)dz);
    /* Convert from [-pi, pi] to [0, 4096) */
    int32_t angle = (int32_t)(angle_rad * (4096.0 / (2.0 * 3.14159265358979323846)));
    return angle & 0xFFF;
}

/* Compute route heading from route table for a given actor span.
 * Matches inline computation at 0x43503A in the original, which reads
 * span_normalized (+0x82), NOT span_raw (+0x80). Post-junction-remap the
 * two diverge by thousands (e.g. 499→2790 on Moscow); indexing the route
 * table with raw returns heading bytes for the wrong ring position and
 * makes the recovery-misalignment check at 0x00434FE0 fire at every
 * branch entry — the visible symptom is the AI braking into the turn. */
static int32_t ai_route_heading_for_actor(const int32_t *rs, const char *actor) {
    const uint8_t *rb = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
    int16_t sp = ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED);
    if (rb && sp >= 0) {
        return (((int)rb[(size_t)(unsigned)sp * 3u + 1u] * 0x102C) >> 8) & 0xFFF;
    }
    return 0;
}

/* ========================================================================
 * Module lifecycle
 * ======================================================================== */

int td5_ai_init(void) {
    memset(g_live_throttle, 0, sizeof(g_live_throttle));
    memset(g_route_state_storage, 0, sizeof(g_route_state_storage));
    memset(g_actor_route_steer_bias, 0, sizeof(g_actor_route_steer_bias));
    memset(g_actor_forward_track_component, 0, sizeof(g_actor_forward_track_component));
    memset(g_slot_state, 0, sizeof(g_slot_state));
    memset(g_traffic_recovery_stage, 0, sizeof(g_traffic_recovery_stage));
    memset(g_encounter_active, 0, sizeof(g_encounter_active));
    memset(g_script_bank_index, 0, sizeof(g_script_bank_index));
    memset(g_last_logged_opcode, 0xFF, sizeof(g_last_logged_opcode));
    g_encounter_tracked_handle = -1;
    g_encounter_cooldown = 0;
    g_encounter_enabled = 0;
    g_encounter_control_active_latch = 0;
    g_encounter_steer_bias_latch = 0;
    g_encounter_phase_flag = 0;
    g_active_actor_count = 6;
    g_lateral_avoidance_direction = 0;
    g_ai_frame_counter = 0;
    g_rb_behind_scale = 0x80;
    g_rb_ahead_scale = 0x80;
    g_rb_behind_range = 0x80;
    g_rb_ahead_range = 0x80;
    g_route_state_base = g_route_state_storage;
    g_actor_base = NULL;
    g_route_tables[0] = NULL;
    g_route_tables[1] = NULL;
    g_route_table_sizes[0] = 0;
    g_route_table_sizes[1] = 0;
    return 1;
}

void td5_ai_shutdown(void) {
    /* nothing to free -- all static */
}

/* Per-slot encounter latch accessor [CONFIRMED @ 0x00403180].
 * Original UpdatePlayerVehicleControlState gates the encounter-control branch
 * on a per-slot field at gActorSpecialEncounterActive[slot * 0x11c], NOT on
 * the global g_wantedModeEnabled. Without this distinction, the port routed
 * every Cop Chase frame into td5_ai_update_encounter_control, blocking the
 * player's throttle write to actor+0x33E. */
int td5_ai_is_encounter_active(int slot) {
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return 0;
    return g_encounter_active[slot] != 0;
}

/* Award damage to a cop slot on player<->cop collision.
 * Mirrors AwardWantedDamageScore @ 0x43D690 [CONFIRMED]:
 *   decrement = 0x400 if impact_mag > 20000, else 0x200.
 *   Clamp to [0, 0x1000].
 *   When state reaches 0, cop AI is frozen (arrested).
 * Called from td5_physics.c when player (slot 0) collides with a cop. */
void td5_ai_wanted_cop_hit(int cop_slot, int32_t impact_mag) {
    int16_t dec, cur;
    char *actor;

    if (cop_slot < 1 || cop_slot >= TD5_MAX_RACER_SLOTS) return;

    dec = (impact_mag > 20000) ? (int16_t)0x400 : (int16_t)0x200;
    cur = g_wanted_damage_state[cop_slot] - dec;
    if (cur < 0) cur = 0;
    g_wanted_damage_state[cop_slot] = cur;

    TD5_LOG_I(LOG_TAG,
              "wanted_damage: cop_slot=%d new_state=%d dec=%d impact=%d",
              cop_slot, (int)cur, (int)dec, (int)impact_mag);

    if (cur == 0) {
        /* Cop arrested: freeze AI — matches original gate at 0x436E1D */
        actor = actor_ptr(cop_slot);
        if (actor) {
            ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 1;
            ACTOR_I32(actor, ACTOR_STEERING_CMD) = 0;
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0;
        }
        TD5_LOG_I(LOG_TAG, "wanted_arrest: cop slot=%d arrested", cop_slot);
    }
}

void td5_ai_bind_actor_table(void *actor_base) {
    extern void *g_route_data;

    g_actor_base = (char *)actor_base;
    g_route_state_base = g_route_state_storage;
    g_route_data = g_route_state_base;
}

void td5_ai_set_route_tables(const uint8_t *left_route, size_t left_size,
                             const uint8_t *right_route, size_t right_size) {
    g_route_tables[0] = left_route;
    g_route_table_sizes[0] = left_size;
    g_route_tables[1] = right_route;
    g_route_table_sizes[1] = right_size;
    td5_ai_refresh_route_state();
}

/**
 * Correct an AI actor's spawn heading to match the LEFT.TRK route byte.
 *
 * Problem: td5_track_compute_heading writes a geometry-derived heading that
 * is ~1050 angle units off from what the LEFT.TRK route byte expects.  This
 * causes td5_ai_update_track_behavior to enter the recovery-script loop on
 * every tick from the very first frame because hdelta > 0x320 (800 decimal)
 * is immediately true.  With v=0 the bicycle model cannot generate yaw
 * torque, so the script never successfully turns the car, and the recovery
 * fires forever → throttle stays 0 → cop never moves.
 *
 * Fix: after compute_heading seeds the yaw accumulator from geometry, re-seed
 * it from the route-byte heading instead.  route_bytes[span*3+1] encodes the
 * expected 12-bit heading as route_heading = (rb * 0x102C) >> 8.  Writing
 * that value to ACTOR_YAW_ACCUM (shift left 8 to match the 20-bit accum
 * convention) places the car at exactly the heading the route cascade expects,
 * suppressing the recovery check.
 *
 * Called from td5_game.c immediately after td5_track_compute_heading() for
 * AI racer slots (state == 0).  Only acts when the route table is available.
 *
 * [CONFIRMED need @ race.log "recovery: slot=1 hdelta=0x41A heading=0xB90
 *  route=0xFAA" — same delta for every tick of the race, car never moves]
 */
void td5_ai_correct_spawn_heading(int slot) {
    int32_t *rs;
    const uint8_t *route_bytes;
    int16_t span;
    int32_t rb, route_heading;
    char *actor;

    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS || !g_route_state_base)
        return;

    rs = route_state(slot);
    if (!rs) return;

    route_bytes = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
    if (!route_bytes) return;

    actor = actor_ptr(slot);
    span = ACTOR_I16(actor, ACTOR_SPAN_RAW);
    if (span < 0) return;

    rb = (int32_t)route_bytes[(size_t)(unsigned)span * 3u + 1u];
    if (rb < 4) return; /* junction-zone sentinel: skip */

    /* Same formula as ai_route_heading_for_actor [CONFIRMED @ td5_ai.c:305]:
     * route_heading = (rb * 0x102C) >> 8 */
    route_heading = ((rb * 0x102C) >> 8) & 0xFFF;

    TD5_LOG_I(LOG_TAG,
              "correct_spawn_heading: slot=%d span=%d rb=%d geom_yaw=%d route_yaw=%d",
              slot, (int)span, (int)rb,
              ACTOR_I32(actor, ACTOR_YAW_ACCUM) >> 8,
              route_heading);

    /* Write route heading to the 20-bit yaw accumulator (<<8 = 12→20 bit) */
    ACTOR_I32(actor, ACTOR_YAW_ACCUM) = route_heading << 8;
}


void td5_ai_refresh_route_state(void) {
    int slot_count = g_active_actor_count;

    if (!g_route_state_base || !g_actor_base) {
        return;
    }

    if (slot_count < 0) {
        slot_count = 0;
    }
    if (slot_count > TD5_MAX_TOTAL_ACTORS) {
        slot_count = TD5_MAX_TOTAL_ACTORS;
    }

    for (int slot = 0; slot < slot_count; ++slot) {
        td5_ai_refresh_route_state_slot(slot);
    }
}

/* ========================================================================
 * UpdateActorTrackBounds @ 0x004366E0 (port byte-faithful)
 *
 * Samples the actor's four wheel-contact probe positions (+0x90 .. +0xB7,
 * stride 0x0C) against the actor's current span_raw, stores the four progress
 * dwords into route_state scratch slots 10..13, computes min/max into RS[5]
 * and RS[6], copies RS[7]=RS[11] / RS[8]=RS[10], and caches signed offsets
 * (computed against both LEFT and RIGHT route tables at the min/max progress)
 * into RS_RIGHT_BOUNDARY_A/B (0x10/0x11) and RS_RIGHT_EXTENT_A/B (0x12/0x13).
 *
 * The min/max + four signed-offset writes are consumed by the per-slot
 * bias-clamp chain at 0x00436BFE-0x00436E50 and ultimately drive the four
 * boundary writebacks (RS_LEFT/RIGHT_BOUNDARY + RS_ACTIVE_LOWER/UPPER_BOUND).
 * ======================================================================== */
static void td5_ai_update_actor_track_bounds(int slot) {
    int32_t *rs;
    char *actor;
    int span_raw;
    int span_norm;
    int32_t progress[4];
    int32_t min_p, max_p;
    const uint8_t *left_table  = g_route_tables[0];
    const uint8_t *right_table = g_route_tables[1];

    if (!g_route_state_base || !g_actor_base ||
        slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) {
        return;
    }

    rs    = route_state(slot);
    actor = actor_ptr(slot);
    span_raw  = (int)(int16_t)ACTOR_I16(actor, ACTOR_SPAN_RAW);
    span_norm = (int)(int16_t)ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED);

    /* Four wheel-contact probe ComputeTrackSpanProgress calls.
     * [CONFIRMED @ 0x004366E0-0x00436749] — actor+0x90, +0x9C, +0xA8, +0xB4. */
    {
        const int probe_offsets[4] = {
            ACTOR_PROBE_FL_BASE, ACTOR_PROBE_FR_BASE,
            ACTOR_PROBE_RL_BASE, ACTOR_PROBE_RR_BASE
        };
        for (int i = 0; i < 4; ++i) {
            int32_t *probe = (int32_t *)(actor + probe_offsets[i]);
            int64_t prog64 = td5_track_compute_span_progress(span_raw, probe);
            progress[i] = (int32_t)(uint32_t)(prog64 & 0xFFFFFFFFu);
            rs[10 + i] = progress[i];  /* RS scratch 10..13 = DAT_004afb88..94 */
        }
    }

    /* min/max scan + copy [CONFIRMED @ 0x00436798-0x004367DD]:
     *   min  = clamp init 0x10000, max = init -0x10000
     *   RS[5] = min, RS[6] = max
     *   RS[7] = RS[11] (probe_FR progress)
     *   RS[8] = RS[10] (probe_FL progress) */
    min_p =  0x10000;
    max_p = -0x10000;
    for (int i = 0; i < 4; ++i) {
        if (progress[i] < min_p) min_p = progress[i];
        if (progress[i] > max_p) max_p = progress[i];
    }
    rs[5] = min_p;
    rs[6] = max_p;
    rs[7] = progress[1];
    rs[8] = progress[0];

    /* Four signed-offset writes [CONFIRMED @ 0x004367E0-0x00436878]:
     *   RS[0x10] = signed_offset(span_raw, min, LEFT[span_norm*3])
     *   RS[0x11] = signed_offset(span_raw, max, LEFT[span_norm*3])
     *   RS[0x12] = signed_offset(span_raw, min, RIGHT[span_norm*3])
     *   RS[0x13] = signed_offset(span_raw, max, RIGHT[span_norm*3])
     *
     * NB: the route_byte read is keyed on span_normalized (not span_raw)
     * — the original reads actor->field_0x82, never +0x80, in this block. */
    {
        int route_byte_left  = 0;
        int route_byte_right = 0;
        if (left_table && span_norm >= 0) {
            route_byte_left  = (int)left_table[(size_t)(unsigned)span_norm * 3u];
        }
        if (right_table && span_norm >= 0) {
            route_byte_right = (int)right_table[(size_t)(unsigned)span_norm * 3u];
        }
        rs[RS_RIGHT_BOUNDARY_A] =
            td5_track_compute_signed_offset(span_raw, min_p, route_byte_left);
        rs[RS_RIGHT_BOUNDARY_B] =
            td5_track_compute_signed_offset(span_raw, max_p, route_byte_left);
        rs[RS_RIGHT_EXTENT_A] =
            td5_track_compute_signed_offset(span_raw, min_p, route_byte_right);
        rs[RS_RIGHT_EXTENT_B] =
            td5_track_compute_signed_offset(span_raw, max_p, route_byte_right);
    }
}

/* ========================================================================
 * ClassifyTrackOffsetClamp @ 0x004368A0 (port byte-faithful)
 *
 * Samples the planned look-ahead position 4 spans forward (with
 * junction-table remapping when self is on the RIGHT route) and projects it
 * onto the next span. Returns:
 *   0 = both samples in-range (no clamp)
 *   1 = first sample (cardef+8 offset) projects past the span end → clamp
 *   2 = second sample (cardef+0 offset) projects backwards → clamp
 * Used to pick which side of the bounding box should re-seed the bias on
 * the next sim_tick.
 *
 * NB: this is a probe — it does NOT mutate route state. The caller
 * (td5_ai_refresh_route_state_slot) then rewrites RS_TRACK_OFFSET_BIAS
 * accordingly.
 * ======================================================================== */
static int td5_ai_remap_for_classify(int span_normalized) {
    /* Replicates the inline junction walk at 0x004368C8-0x00436954.
     *   iVar5 = span_normalized + 4
     *   if (g_trackTotalSpanCount <= iVar5):
     *     iVar5 = (span_normalized - g_trackTotalSpanCount) + 4
     *   walk DAT_004c3da0 jump table for a matching entry; remap inline.
     * The port's td5_track_branch_to_junction implements the same lookup
     * (post-wrap) for span indices >= ring_length. */
    int ring = td5_track_get_ring_length();
    int remapped = span_normalized + 4;
    if (ring > 0 && ring <= remapped) {
        remapped = (span_normalized - ring) + 4;
    }
    /* If post-wrap span is inside the branch range, fold back into main road
     * (same as the original's loop body at 0x004368F0-0x00436950). */
    if (ring > 0 && remapped >= ring) {
        int folded = td5_track_branch_to_junction(remapped);
        if (folded >= 0) {
            remapped = folded;
        }
    }
    return remapped;
}

static int td5_ai_classify_track_offset_clamp(int slot, int track_offset_bias) {
    int32_t *rs;
    char *actor;
    int span_norm;
    int sample_span;      /* iVar5: junction-remapped OR simple-wrap */
    int route_byte_idx;   /* iVar4: ALWAYS simple-wrap span_norm+4 */
    int route_byte;
    int target_x = 0;
    int target_z = 0;
    int sample_progress;
    int64_t prog64;
    int16_t *cardef;
    int cardef0, cardef8;

    if (!g_route_state_base || !g_actor_base ||
        slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) {
        return 0;
    }

    rs    = route_state(slot);
    actor = actor_ptr(slot);
    span_norm = (int)(int16_t)ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED);
    cardef = (int16_t *)(intptr_t)ACTOR_I32(actor, ACTOR_CAR_DEF_PTR);
    if (!cardef) {
        return 0;
    }
    cardef0 = (int)cardef[0];   /* bounds_lo (LEFT-side extent) */
    cardef8 = (int)cardef[4];   /* bounds_hi (RIGHT-side extent), at byte +0x08 */

    /* Two parallel span computations. Match the original's iVar5/iVar4 split:
     *   iVar4 = ALWAYS simple wrap of span_norm + 4
     *   iVar5 = junction-remapped iff RIGHT-selector AND inside branch range,
     *           else simple wrap (same as iVar4)
     * [CONFIRMED @ 0x004368B6-0x004368FB + LAB_0043699B] */
    {
        int ring = td5_track_get_ring_length();
        route_byte_idx = span_norm + 4;
        if (ring > 0 && ring <= route_byte_idx) {
            route_byte_idx = (span_norm - ring) + 4;
        }
        if (route_byte_idx < 0) route_byte_idx = 0;
    }
    {
        const uint8_t *self_table = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
        if (self_table == g_route_tables[0]) {
            sample_span = route_byte_idx;  /* LEFT route — use simple-wrap */
        } else {
            sample_span = td5_ai_remap_for_classify(span_norm);
            if (sample_span < 0) sample_span = route_byte_idx;
        }
    }
    if (sample_span < 0) sample_span = 0;

    /* First sample: cardef[4] (bounds_hi) + track_offset_bias offset
     * [CONFIRMED @ 0x004369C4-0x004369F2]
     *   route_byte = *piVar1[iVar4 * 3]    ← simple-wrap idx, NOT sample_span
     *   SampleTrackTargetPoint(sample_span=iVar5, route_byte, ...) */
    {
        const uint8_t *table = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
        if (!table) return 0;
        route_byte = (int)table[(size_t)(unsigned)route_byte_idx * 3u];
        if (!td5_track_sample_target_point(sample_span, route_byte,
                                           &target_x, &target_z,
                                           cardef8 + track_offset_bias)) {
            return 0;
        }
    }
    {
        int32_t pos_vec[3];
        pos_vec[0] = target_x;
        pos_vec[1] = 0;
        pos_vec[2] = target_z;
        prog64 = td5_track_compute_span_progress(sample_span, pos_vec);
        sample_progress = (int)(int32_t)(uint32_t)(prog64 & 0xFFFFFFFFu);
    }
    /* (int)lVar7 < 0xff → return 1 [CONFIRMED @ 0x00436A0C] */
    if (sample_progress < 0xFF) {
        return 1;
    }

    /* Second sample: cardef[0] (bounds_lo) + track_offset_bias offset
     * [CONFIRMED @ 0x00436A12-0x00436A38]
     * Note original recomputes iVar3 (span_norm) here and rebuilds iVar4 from
     * scratch — equivalent to our cached route_byte_idx since span_norm didn't
     * change in between. */
    {
        const uint8_t *table = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
        route_byte = (int)table[(size_t)(unsigned)route_byte_idx * 3u];
        if (!td5_track_sample_target_point(sample_span, route_byte,
                                           &target_x, &target_z,
                                           cardef0 + track_offset_bias)) {
            return 0;
        }
    }
    {
        int32_t pos_vec[3];
        pos_vec[0] = target_x;
        pos_vec[1] = 0;
        pos_vec[2] = target_z;
        prog64 = td5_track_compute_span_progress(sample_span, pos_vec);
        sample_progress = (int)(int32_t)(uint32_t)(prog64 & 0xFFFFFFFFu);
    }
    /* `(0 < (int)lVar7) - 1) & 0x200000002` → 2 if positive, 0 otherwise
     * [CONFIRMED @ 0x00436A4D] */
    if (sample_progress > 0) {
        return 2;
    }
    return 0;
}

static void td5_ai_refresh_route_state_slot(int slot) {
    int32_t *rs;
    char *actor;
    int selector;
    const uint8_t *route_table;
    size_t route_count;
    int16_t span;
    int32_t route_heading;
    int32_t forward_heading;

    if (!g_route_state_base || !g_actor_base ||
        slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) {
        return;
    }

    rs = route_state(slot);
    actor = actor_ptr(slot);

    /* Per-tick branch-vs-main selector update [CONFIRMED @ 0x00436AAD..0x00436C9B].
     *
     * Orig UpdateRaceActors has THREE paths for the selector/ptr write:
     *
     *   PATH 1  (span_raw > total_span_count):
     *     selector := 1, ptr := RIGHT.TRK
     *
     *   PATH 2a (junction match — span_norm in entry main-road range AND
     *            branch_lo - main_target + span_norm != -1):
     *     selector := 0, ptr := LEFT.TRK
     *
     *   PATH 2b (default — no junction match):
     *     selector := 0, ptr UNCHANGED   ← KEY DIVERGENCE FROM PRIOR PORT
     *
     * Prior port wrote rs[RS_ROUTE_TABLE_PTR] = g_route_tables[selector]
     * UNCONDITIONALLY. That meant slot 0 (initialized to RIGHT.TRK by
     * td5_ai_initialize_runtime via `selector = (slot & 1) ? 0 : 1`) had
     * its ptr force-rewritten to LEFT.TRK on every tick where span_raw <=
     * total. In orig, slot 0 keeps RIGHT.TRK in PATH 2b → route_byte read
     * via that table → hdelta computed against RIGHT-relative heading →
     * recovery-script trigger fires when hdelta in (0x320, 0xCE0).
     *
     * On Moscow at sub_tick=1, this is what drove RS[0x3D] (script_flags)
     * and RS[0x45] (script_countdown) to stay at 0 in port: with LEFT.TRK
     * substituted, hdelta misses the trigger window, script_base_ptr stays
     * 0, AdvanceActorTrackScript never runs, and the two RS fields never
     * pick up their script-driven values (0x10 / 0x96).
     *
     * Fix (this commit): only write rs[RS_ROUTE_TABLE_PTR] when the orig
     * writes it (PATH 1 or PATH 2a). PATH 2b leaves the ptr alone — it
     * keeps whatever value was set during td5_ai_initialize_runtime (LEFT
     * for odd slots, RIGHT for even slots). */
    {
        int ring_len = td5_track_get_ring_length();
        int span_raw_val = (int)(int16_t)ACTOR_I16(actor, ACTOR_SPAN_RAW);
        int span_norm_val = (int)(int16_t)ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED);

        if (ring_len > 0 && span_raw_val > ring_len) {
            /* PATH 1: span_raw > total → selector=1, ptr=RIGHT
             * [orig 0x436ab4 CMP EAX,ECX / JLE 0x436adb — fall-through on >]. */
            rs[RS_ROUTE_TABLE_SELECTOR] = 1;
            rs[RS_ROUTE_TABLE_PTR] = (int32_t)(intptr_t)g_route_tables[1];
        } else if (td5_track_route_junction_path2a_match(span_norm_val)) {
            /* PATH 2a: junction match → selector=0, ptr=LEFT */
            rs[RS_ROUTE_TABLE_SELECTOR] = 0;
            rs[RS_ROUTE_TABLE_PTR] = (int32_t)(intptr_t)g_route_tables[0];
        } else {
            /* PATH 2b: default → selector=0, ptr UNCHANGED */
            rs[RS_ROUTE_TABLE_SELECTOR] = 0;
        }
    }

    selector = rs[RS_ROUTE_TABLE_SELECTOR] & 1;
    /* route_table now derived from rs[RS_ROUTE_TABLE_PTR] which may have been
     * left UNCHANGED in PATH 2b, OR forcibly set in PATH 1/2a. The selector
     * variable continues to drive downstream branches; the lookup against
     * g_route_tables[selector] is only used to derive route_count for the
     * empty-table fallback and route_byte indexing. To keep downstream code
     * byte-equivalent we still cache route_table = the resolved ptr, but
     * we read from rs (not g_route_tables) so PATH 2b's preserved ptr wins. */
    route_table = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
    /* For route_count, prefer the size that matches the actual ptr. If the
     * stored ptr equals g_route_tables[1] use selector=1's size; otherwise
     * use selector=0's size (covers PATH 2b where ptr keeps initial value). */
    route_count = (route_table == g_route_tables[1])
                  ? (g_route_table_sizes[1] / 3u)
                  : (g_route_table_sizes[0] / 3u);

    /* rs[RS_ROUTE_TABLE_PTR] write is now path-gated above; no fall-through
     * unconditional write here. */
    if (!route_table || route_count == 0u) {
        rs[RS_FORWARD_TRACK_COMP] = 0;
        g_actor_forward_track_component[slot] = 0;
        /* Even without route data, run the bias-clamp Step-12 boundary
         * writebacks (lines 1017-1018 / 1044-1045) using whatever values
         * RS_RIGHT_BOUNDARY_A/B + RS_RIGHT_EXTENT_A/B currently hold (e.g.
         * from replay-injected orig state). Orig's per-tick AI runs this
         * writeback unconditionally; absent it, port's RS_ACTIVE_*_BOUND
         * fields are stale relative to the injected RS[0x10..0x13] values
         * that the replay harness puts in place. This is a port-only fallback
         * for the missing-route-tables scenario (Honolulu level zip is
         * sometimes built without LEFT/RIGHT.TRK in the assets bundle); when
         * route data IS loaded, the gated block below handles the writeback
         * via the same lines, so behaviour is preserved. */
        {
            int slot_is_racer = (slot < TD5_MAX_RACER_SLOTS);
            int slot_is_encounter_9 =
                (slot == 9 && g_encounter_tracked_handle != -1);
            int racer_or_encounter = slot_is_racer || slot_is_encounter_9;
            /* Selector-driven branch: SELECTOR==0 → LEFT/main route (matches
             * line 1017-1018 in the gated block); SELECTOR==1 → RIGHT route
             * (matches line 1044-1045). */
            if ((rs[RS_ROUTE_TABLE_SELECTOR] & 1) == 0) {
                /* LEFT route: ACTIVE_UPPER=BOUND_A, ACTIVE_LOWER=BOUND_B.
                 * Racer LEFT-side writeback: RS_LEFT_BOUNDARY_A <- bias. */
                if (racer_or_encounter) {
                    rs[RS_LEFT_BOUNDARY_A] = rs[RS_TRACK_OFFSET_BIAS];
                }
                rs[RS_ACTIVE_UPPER_BOUND] = rs[RS_RIGHT_BOUNDARY_A];
                rs[RS_ACTIVE_LOWER_BOUND] = rs[RS_RIGHT_BOUNDARY_B];
            } else {
                /* RIGHT route: ACTIVE_UPPER=EXTENT_A, ACTIVE_LOWER=EXTENT_B.
                 * Racer RIGHT-side writeback: RS_LEFT_BOUNDARY_B <- bias. */
                if (racer_or_encounter) {
                    rs[RS_LEFT_BOUNDARY_B] = rs[RS_TRACK_OFFSET_BIAS];
                }
                rs[RS_ACTIVE_UPPER_BOUND] = rs[RS_RIGHT_EXTENT_A];
                rs[RS_ACTIVE_LOWER_BOUND] = rs[RS_RIGHT_EXTENT_B];
            }
        }
        return;
    }

    /* Original UpdateActorTrackBehavior @ 0x00435022/0x00435039 reads
     * actor->span_normalized (+0x82), NOT span_raw (+0x80). span_norm is the
     * wrapped-but-not-branch-remapped index consistent with the route-table
     * layout; using span_raw picks up the post-remap jump (e.g. 499→2790 at
     * Moscow's first branch), which clamps to the tail of the route table and
     * reads a heading ~180° off the car's motion — flipping fwd_comp sign and
     * triggering the emergency-brake path post-branch. */
    span = ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED);
    if (span < 0) {
        span = 0;
    } else if ((size_t)span >= route_count) {
        span = (int16_t)(route_count - 1u);
    }

    /* Route-byte → 12-bit angle: (byte * 0x102C) >> 8 [CONFIRMED @ 0x434FFB] */
    route_heading = (((int32_t)route_table[(size_t)span * 3u + 1u] * 0x102C) >> 8) & 0xFFF;

    forward_heading = route_heading;
    /* Polarity at dword 0x3F = gActorRouteDirectionPolarity (0x004afc5c) per
     * UpdateTrafficRoutePlan @ 0x00435ef4 / UpdateSpecialEncounterControl
     * teardown @ 0x00434d40. Prior port read dword 0x25 (byte 0x94) which has
     * NO references in the original — the mirror branch was effectively dead. */
    if (rs[RS_ROUTE_DIRECTION_POLARITY] != 0) {
        forward_heading = (forward_heading + 0x800) & 0xFFF;
    }

    /* Project world-frame velocity onto route heading direction.
     * [CONFIRMED @ 0x436B43-0x436B90: RS[0x18] = velocity dot product]
     * Original stores the velocity projection here, NOT the route heading.
     * The recovery check at 0x434FE0 computes route heading inline. */
    {
        int32_t vx = ACTOR_I32(actor, ACTOR_LIN_VEL_X) >> 8;
        int32_t vz = ACTOR_I32(actor, ACTOR_LIN_VEL_Z) >> 8;
        int32_t cos_r = ai_cos_fixed12(forward_heading);
        int32_t sin_r = ai_sin_fixed12(forward_heading);
        int32_t fwd_comp = (vx * sin_r + vz * cos_r) >> 12;
        rs[RS_FORWARD_TRACK_COMP] = fwd_comp;
        g_actor_forward_track_component[slot] = fwd_comp;
    }

    /* ===================================================================
     * Pre-loop ClassifyTrackOffsetClamp chain (feature-gated)
     *
     * Port of the per-slot body at 0x00436B43-0x00436E50 in
     * UpdateRaceActors. Without this block, RS_TRACK_OFFSET_BIAS follows a
     * different trajectory across the 160 countdown sub-ticks, leaving the
     * port with ~-1280 more bias at sim_tick=1. That delta cascades into
     * the steering-2x family (steering_command, angular_velocity_yaw,
     * linear_velocity_x/z, euler_accum_yaw, world_pos.x/z) plus the
     * wheel/center suspension state.
     *
     * Gated behind [Trace] ExperimentalBiasClamp=1 so the change is opt-in
     * per session (it can be reverted instantly by flipping the flag).
     *
     * 2026-05-15: unconditional enable for the replay-diff measurement —
     * snapshot-replay isolates per-tick error so the prior trajectory-diff
     * regression (21 → 30 labeled) doesn't reproduce. Per-tick the
     * boundary/bias-clamp writes track orig within ~30-200 units instead of
     * compounding for 160 sub-ticks.
     *
     * 2026-05-16 (Round 3 Wave 1, F1 follow-up): gated to slots 0..5 only.
     * Traffic actors (slots 6..11) use a DIFFERENT init flow
     * (RecycleTrafficActorFromQueue @ 0x004353B0 — see memory
     * reference_arch_recycle_heading_collapse.md) and don't expect these
     * RS_ACTIVE_LOWER/UPPER_BOUND / RS_TRACK_OFFSET_BIAS writes. Running
     * the clamp for traffic slots was corrupting their spawn pose, route-
     * table indexing, and V2V eligibility -- causing the regression in
     * memory todo_traffic_through_ground_no_motion_no_collision_2026-05-16.md
     * (traffic vehicles spawning underground, idle, no collision). */
    if (slot < TD5_MAX_RACER_SLOTS) {
        int span_raw_i = (int)(int16_t)ACTOR_I16(actor, ACTOR_SPAN_RAW);
        int span_norm_i = (int)(int16_t)ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED);

        /* Step 3-4: span_progress + cache in RS_TRACK_PROGRESS
         * [CONFIRMED @ 0x00436B80-0x00436BCC] */
        {
            int32_t pos_vec[3];
            pos_vec[0] = ACTOR_I32(actor, ACTOR_WORLD_POS_X);
            pos_vec[1] = 0;
            pos_vec[2] = ACTOR_I32(actor, ACTOR_WORLD_POS_Z);
            int64_t prog64 = td5_track_compute_span_progress(span_raw_i, pos_vec);
            rs[RS_TRACK_PROGRESS] = (int32_t)(uint32_t)(prog64 & 0xFFFFFFFFu);
        }

        /* Step 5: RenderTrackSegmentNearActor @ 0x00433CE0 writes RS[0x37]
         * and RS[0x38], neither of which is consumed elsewhere in the port.
         * Skipping it (no state effect on lateral_bias cascade).
         *
         * [L5 audit 2026-05-18, TD5_pool0 read-only]
         *
         * Despite the name, this is SIM math, NOT render. It is called from
         * UpdateRaceActors @ 0x00436A70 (callers={UpdateRaceActors}, the
         * SIM-side per-actor update loop, NOT the render pipeline). It takes
         * the per-actor RS-table pointer (gActorRouteStateTable + slot*0x47)
         * and writes ONLY two int32s at +0xdc/+0xe0 of that table — i.e.
         * RS[0x37] (dword index 0xdc/4 = 55) and RS[0x38] (224/4 = 56).
         *
         * The body (decompiled @ 0x00433CE0):
         *   switch on strip-type (1/2/5 vs 3/4 vs 6/7) computes
         *     a 2x2 linear system [iVar9*iVar6 - iVar7*iVar8] = det,
         *     RS[0x37] = (cross_num * 0x100) / det,
         *     RS[0x38] = (cross_num2 * 0x100) / det.
         *   The math is span-local barycentric coords ("actor's x/z relative
         *   to the span's vertex parallelogram", scaled <<8). Per Wave 5
         *   confidence-map notes: "computes actor's field_0xdc/0xe0 — local-
         *   space x/z relative to span". The actor->field_0xdc and 0xe0 are
         *   NOT actor-struct fields — they are RS_TABLE[+0xdc/+0xe0].
         *
         * Original-program consumer audit: zero. A program-wide instruction
         * scan for `MOV ESI,[EBX+0xdc]` style reads returns hits only inside
         * UpdateTrafficRoutePlan @ 0x00435E80, where EBX holds an ACTOR base
         * (stride 0x388), NOT the RS-table — reading actor field +0xdc which
         * is a separate, unrelated location. No instruction in the binary
         * reads gActorRouteStateTable[+0xdc] or +0xe0.
         *
         * Conclusion: byte-faithful skipping is the correct port — these RS
         * slots are dead writes in the original. Promoting to L5. If a future
         * Frida sweep ever turns up a hidden reader (e.g. unanalyzed DLL or
         * fastcall thunk), the math is trivial to bring online; the helper
         * `td5_track_compute_span_progress()` already gives us the parallel
         * primitive. Confidence map (re/analysis/ghidra_confidence_map_*.csv)
         * should be updated to L5 + UNUSED-IN-PORT tag. */

        /* Step 6: UpdateActorTrackBounds — caches min/max progress + signed
         * offsets that feed the boundary writebacks below. */
        td5_ai_update_actor_track_bounds(slot);

        /* Step 7-9: ClassifyTrackOffsetClamp + bias rewrite
         * [CONFIRMED @ 0x00436BFE-0x00436CFE] */
        {
            int classify = td5_ai_classify_track_offset_clamp_v2(
                slot, rs[RS_TRACK_OFFSET_BIAS]);
            int16_t *cardef = (int16_t *)(intptr_t)ACTOR_I32(actor, ACTOR_CAR_DEF_PTR);
            const uint8_t *table = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
            int route_byte_now = 0;
            if (table && span_norm_i >= 0) {
                route_byte_now = (int)table[(size_t)(unsigned)span_norm_i * 3u];
            }
            if (classify == 1 && cardef) {
                int32_t off = td5_track_compute_signed_offset(
                    span_raw_i, 0x100, route_byte_now);
                rs[RS_TRACK_OFFSET_BIAS] = (int)cardef[0] + off - 0x20;
            } else if (classify == 2 && cardef) {
                int32_t off = td5_track_compute_signed_offset(
                    span_raw_i, 0, route_byte_now);
                rs[RS_TRACK_OFFSET_BIAS] = (int)cardef[4] + off + 0x20;
            }
        }

        /* Step 10: surface_type > 0x0F → zero bias
         * [CONFIRMED @ 0x00436D29-0x00436D67]
         *
         * Inline GetTrackSegmentSurfaceType @ 0x0042F100 against the actor's
         * own span_raw + sub_lane_index (NOT body_probes[0] which port's
         * td5_track_get_surface_type uses — those are bounding-box corners
         * which can lag span_raw by a frame). High nibble + 0x10 sentinel
         * → original returns >= 0x10, so the >0x0F check fires whenever the
         * sub_lane bit is set in the strip's lane_bitmask. */
        {
            TD5_StripSpan *sp = td5_track_get_span(span_raw_i);
            if (sp) {
                uint8_t sub_lane = ACTOR_U8(actor, ACTOR_SUB_LANE_INDEX);
                uint8_t lane_mask = sp->pad_02[0]; /* lane_bitmask at +0x02 */
                int surface;
                if ((lane_mask & (1u << (sub_lane & 0x1F))) != 0u) {
                    surface = (sp->surface_attribute >> 4) | 0x10;
                } else {
                    surface = sp->surface_attribute & 0x0F;
                }
                if (surface > 0x0F) {
                    rs[RS_TRACK_OFFSET_BIAS] = 0;
                }
            }
        }

        /* Step 11: track_contact_flag in {1, 2} → zero bias
         * [CONFIRMED @ 0x00436D77-0x00436D9C] */
        {
            uint8_t tcf = ACTOR_U8(actor, ACTOR_TRACK_CONTACT_FLAG);
            if (tcf == 1 || tcf == 2) {
                rs[RS_TRACK_OFFSET_BIAS] = 0;
            }
        }

        /* Step 12: boundary writebacks — selector and slot-class dependent.
         * [CONFIRMED @ 0x00436DA9-0x00436E50] */
        {
            const uint8_t *self_table =
                (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
            int slot_is_racer = (slot < TD5_MAX_RACER_SLOTS);
            int slot_is_encounter_9 =
                (slot == 9 && g_encounter_tracked_handle != -1);
            int racer_or_encounter = slot_is_racer || slot_is_encounter_9;

            if (self_table == g_route_tables[0]) {
                /* Selector == LEFT (DAT_004afb58):
                 *   racer    : RS_LEFT_BOUNDARY_A   = RS_TRACK_OFFSET_BIAS
                 *   traffic  : RS_TRACK_OFFSET_BIAS = signed_offset(LEFT)
                 *              RS_LEFT_BOUNDARY_A   = same
                 *   both     : RS_LEFT_BOUNDARY_B   = signed_offset(RIGHT)
                 *              RS_ACTIVE_UPPER      = RS_RIGHT_BOUNDARY_A
                 *              RS_ACTIVE_LOWER      = RS_RIGHT_BOUNDARY_B
                 * NB: original's `&DAT_004afbb4` / `&DAT_004afbb0` writes are
                 *     RS[0x15] / RS[0x14] in port-index land:
                 *     0x4afbb4 - 0x4afb60 = 0x54 → dword 0x15 = ACTIVE_UPPER_BOUND
                 *     0x4afbb0 - 0x4afb60 = 0x50 → dword 0x14 = ACTIVE_LOWER_BOUND */
                if (racer_or_encounter) {
                    rs[RS_LEFT_BOUNDARY_A] = rs[RS_TRACK_OFFSET_BIAS];
                } else if (g_route_tables[0] && span_norm_i >= 0) {
                    int route_byte_left = (int)g_route_tables[0][
                        (size_t)(unsigned)span_norm_i * 3u];
                    int32_t offL = td5_track_compute_signed_offset(
                        span_raw_i, rs[RS_TRACK_PROGRESS], route_byte_left);
                    rs[RS_TRACK_OFFSET_BIAS] = offL;
                    rs[RS_LEFT_BOUNDARY_A]   = offL;
                }
                if (g_route_tables[1] && span_norm_i >= 0) {
                    int route_byte_right = (int)g_route_tables[1][
                        (size_t)(unsigned)span_norm_i * 3u];
                    rs[RS_LEFT_BOUNDARY_B] = td5_track_compute_signed_offset(
                        span_raw_i, rs[RS_TRACK_PROGRESS], route_byte_right);
                }
                rs[RS_ACTIVE_UPPER_BOUND] = rs[RS_RIGHT_BOUNDARY_A];
                rs[RS_ACTIVE_LOWER_BOUND] = rs[RS_RIGHT_BOUNDARY_B];
            } else {
                /* Selector == RIGHT (DAT_004b08b4):
                 *   racer    : RS_LEFT_BOUNDARY_B   = RS_TRACK_OFFSET_BIAS
                 *              [CONFIRMED @ 0x00436DC4: DAT_004afb9c write]
                 *   traffic  : RS_TRACK_OFFSET_BIAS = signed_offset(RIGHT)
                 *              RS_LEFT_BOUNDARY_B   = same
                 *   both     : RS_LEFT_BOUNDARY_A   = signed_offset(LEFT)
                 *              RS_ACTIVE_UPPER      = RS_RIGHT_EXTENT_A
                 *              RS_ACTIVE_LOWER      = RS_RIGHT_EXTENT_B */
                if (racer_or_encounter) {
                    rs[RS_LEFT_BOUNDARY_B] = rs[RS_TRACK_OFFSET_BIAS];
                } else if (g_route_tables[1] && span_norm_i >= 0) {
                    int route_byte_right = (int)g_route_tables[1][
                        (size_t)(unsigned)span_norm_i * 3u];
                    int32_t offR = td5_track_compute_signed_offset(
                        span_raw_i, rs[RS_TRACK_PROGRESS], route_byte_right);
                    rs[RS_TRACK_OFFSET_BIAS] = offR;
                    rs[RS_LEFT_BOUNDARY_B]   = offR;
                }
                if (g_route_tables[0] && span_norm_i >= 0) {
                    int route_byte_left = (int)g_route_tables[0][
                        (size_t)(unsigned)span_norm_i * 3u];
                    rs[RS_LEFT_BOUNDARY_A] = td5_track_compute_signed_offset(
                        span_raw_i, rs[RS_TRACK_PROGRESS], route_byte_left);
                }
                rs[RS_ACTIVE_UPPER_BOUND] = rs[RS_RIGHT_EXTENT_A];
                rs[RS_ACTIVE_LOWER_BOUND] = rs[RS_RIGHT_EXTENT_B];
            }
        }
    }
}

void td5_ai_tick(void) {
    td5_ai_compute_rubber_band();
    if ((g_ai_frame_counter % 60u) == 0u) {
        int racer_count = TD5_MAX_RACER_SLOTS;
        if (g_active_actor_count < racer_count)
            racer_count = g_active_actor_count;
        for (int i = 0; i < racer_count; i++) {
            if (g_slot_state[i] == 0) {
                TD5_LOG_D(LOG_TAG,
                          "Rubber band: slot=%d throttle=%d steer_bias=%d",
                          i, g_live_throttle[i], g_actor_route_steer_bias[i]);
            }
        }
    }
    td5_ai_update_race_actors();
    g_ai_frame_counter++;
}

/* ========================================================================
 * Rubber-Band System  (0x432D60  ComputeAIRubberBandThrottle)
 *
 * Per-tick: copies default throttle -> live throttle, then for each AI
 * slot computes: modifier = (scale * delta) / range
 *                live_bias[slot] = 0x100 - modifier
 *
 * When AI is behind player: delta < 0, range > 0 ->
 *   modifier = (scale * negative) / positive = negative
 *   bias = 0x100 - negative > 0x100 (catch-up boost)
 * When AI is ahead: delta > 0, range > 0 ->
 *   modifier = (scale * positive) / positive = positive
 *   bias = 0x100 - positive < 0x100 (slow down)
 *
 * NOTE: the denominator is always the POSITIVE range value for both branches.
 * An earlier port error used (-g_rb_behind_range) in the behind branch, which
 * made the modifier positive and throttled down AI that should be boosted.
 * ======================================================================== */

/* ========================================================================
 * Pool11 pilot trace integration (0x00432D60).
 *
 * The pilot snapshot mirrors the function inputs read by Frida from the
 * original. Collector lives here so it can read the static slot-state
 * array + the public actor pointer. */
#include "td5_pilot_trace_00432D60.h"

void td5_pilot_00432D60_collect(PilotSnapshot_00432D60 *snap) {
    snap->network_active = g_td5.network_active ? 1 : 0;
    snap->racer_count    = g_active_actor_count;
    snap->player0_span_accum = (int16_t)ACTOR_I16(actor_ptr(0), ACTOR_SPAN_ACCUM);
    for (int i = 0; i < 6; i++) {
        snap->slot_state[i]    = (int)g_slot_state[i];
        snap->ai_span_accum[i] = (int16_t)ACTOR_I16(actor_ptr(i), ACTOR_SPAN_ACCUM);
    }
}

void td5_ai_compute_rubber_band(void) {
    int i, racer_count;
    int32_t player0_span, ai_span, delta, modifier;

    td5_pilot_emit_00432D60_enter();

    /* [0x00432D6B-7A] MOVSD.REP ECX=14: unconditional default→live copy */
    memcpy(g_live_throttle, g_default_throttle, 14 * sizeof(int32_t));

    /* [0x00432D60-7C] TEST g_networkRaceActive; JZ → rubber-band loop.
     * Fall-through here = network ACTIVE → write 0x100 bias for AI slots. */
    if (g_td5.network_active) {
        /* [0x00432D7E-DC2] network-active branch.
         * Original re-reads g_racerCount each iter (CMP ESI,0x6 with ESI cached);
         * effective cap = min(g_racerCount, 6). */
        racer_count = TD5_MAX_RACER_SLOTS;
        if (g_active_actor_count < racer_count)
            racer_count = g_active_actor_count;
        for (i = 0; i < racer_count; i++) {
            /* [0x00432D9F-A8] state byte at gRaceSlotStateTable.slot[i].state */
            if (g_slot_state[i] == 0) { /* AI slot */
                /* [0x00432DAA] DAT_00473d2c[i] = 0x100 (live throttle override) */
                g_live_throttle[i] = 0x100;
                /* [0x00432DB5] gActorDefaultRouteSteerBias[i*0x47] = 0x100 */
                g_actor_route_steer_bias[i] = 0x100;
            }
            /* [0x00432DBC] ADD EDX,0x11C unconditional (handled by [i] indexing) */
        }
        td5_pilot_emit_00432D60_leave();
        return;
    }

    /* [0x00432DC4-E4B] rubber-band branch.
     *
     * Per pilot_00432D60_audit.md: original reads actor+0x84 (span_accum) as
     * signed 16-bit, computes delta = ai - player0. Behind branch has a DEAD
     * upper-clamp at +behind_range (never fires for negative delta); there is
     * NO lower-clamp at -behind_range — earlier port over-approximated this.
     * Ahead branch has an ACTIVE upper-clamp at +ahead_range.
     *
     * IDIV truncates toward zero (signed); C's `/` operator since C99 matches. */

    /* [0x00432DF1] MOVSX EAX, word ptr [0x4ab18c] — actor[0].+0x84 read once
     * (constant address in original; player0 span_accum). */
    player0_span = (int16_t)ACTOR_I16(actor_ptr(0), ACTOR_SPAN_ACCUM);

    /* [0x00432DD5-E4B] g_racerCount re-read at loop top each iteration in
     * original. We cache here — the function is single-threaded with no
     * callees, so g_racerCount cannot change within this call. Equivalent. */
    racer_count = TD5_MAX_RACER_SLOTS;
    if (g_active_actor_count < racer_count)
        racer_count = g_active_actor_count;

    for (i = 0; i < racer_count; i++) {
        /* [0x00432DE8-EC] CMP byte [EBP],0x0; JNZ skip — non-AI slots leave
         * gActorDefaultRouteSteerBias[i] untouched. */
        if (g_slot_state[i] != 0)
            continue;

        /* [0x00432DEE] MOVSX ECX, word ptr [ESI] — actor[i].+0x84 */
        ai_span = (int16_t)ACTOR_I16(actor_ptr(i), ACTOR_SPAN_ACCUM);
        /* [0x00432DF8] SUB ECX,EAX — signed 32-bit delta */
        delta = ai_span - player0_span;

        /* [0x00432DFA] JNS — sign of delta selects branch */
        if (delta < 0) {
            /* [0x00432DFC-16] AI BEHIND player: catch-up boost.
             *   MOV  EAX, [0x473da4]    ; behind_range (positive)
             *   CMP  ECX, EAX           ; delta vs +range
             *   JLE  skip_clamp         ; always true (delta<0 < range>0)
             *   MOV  ECX, EAX           ; DEAD — never executes
             * skip_clamp:
             *   MOV  EAX, [0x473d9c]    ; behind_scale
             *   IMUL EAX, ECX
             *   CDQ
             *   IDIV [0x473da4]         ; / behind_range, trunc-to-zero
             * Faithful port: keep the dead upper-clamp for byte parity with
             * the listing's branch shape; do NOT add any lower-clamp. */
            if (delta > g_rb_behind_range)
                delta = g_rb_behind_range;  /* dead path; mirrors 0x00432E01-05 */
            modifier = (g_rb_behind_scale * delta) / g_rb_behind_range;
        } else {
            /* [0x00432E18-32] AI AHEAD or tied: throttle reduction.
             *   MOV  EAX, [0x473da8]    ; ahead_range
             *   CMP  ECX, EAX
             *   JLE  skip_clamp
             *   MOV  ECX, EAX           ; ACTIVE — clamp delta to +ahead_range
             *   MOV  EAX, [0x473da0]    ; ahead_scale
             *   IMUL EAX, ECX
             *   CDQ
             *   IDIV [0x473da8] */
            if (delta > g_rb_ahead_range)
                delta = g_rb_ahead_range;
            modifier = (g_rb_ahead_scale * delta) / g_rb_ahead_range;
        }

        /* [0x00432E32-39] MOV ECX,0x100; SUB ECX,EAX; MOV [EDI],ECX */
        g_actor_route_steer_bias[i] = 0x100 - modifier;
    }
    td5_pilot_emit_00432D60_leave();
}

/* ========================================================================
 * Race Actor Runtime Initialization  (0x432E60)
 *
 * Configures rubber-band parameters via a decision tree over:
 *   difficulty (easy/normal/hard)
 *   circuit vs point-to-point
 *   traffic enabled
 *   difficulty tier (0/1/2)
 *   game mode (pitbull, time-trial, etc.)
 *
 * Also applies difficulty scaling to the AI physics template and sets
 * the active actor count (6/12/2).
 * ======================================================================== */

void td5_ai_init_race_actor_runtime(void) {
    /* [CONFIRMED @ InitializeRaceActorRuntime 0x00432E60 reads gRaceDifficultyTier
     * @ 0x00463210 throughout the Layer-2 decision tree.] The port previously
     * read a file-static `g_race_difficulty_tier` that was never written —
     * route through g_td5.difficulty_tier (which ConfigureGameTypeFlags writes
     * at td5_frontend.c:2675 mirroring the original's switch at 0x00410CA0). */
    int tier = g_td5.difficulty_tier;
    int is_circuit = (g_td5.track_type == TD5_TRACK_CIRCUIT);
    int has_traffic = g_td5.traffic_enabled;
    /* [@ 0x00432FF0] Cop Chase ("wanted mode") branch — original gates on
     *   [0x00466e94] gTrackIsCircuit==0 && [0x004aaf74] g_raceOverlayPresetMode==4.
     * g_raceOverlayPresetMode=4 is set ONLY by ConfigureGameTypeFlags case 8
     * (Cop Chase). The port's previous `is_pitbull = race_rule_variant==4`
     * was misnamed AND wrong: race_rule_variant=4 is set by case 6 (Ultimate),
     * not Cop Chase. */
    int is_cop_chase = g_td5.wanted_mode_enabled;
    int is_time_trial = g_td5.time_trial_enabled;
    int racer_count;

    /* Pilot trace probe (precise-00432E60): capture inputs at entry. */
    {
        extern void td5_pilot_emit_00432E60_enter(int, int, int, int, int, int,
                                                  int16_t, int16_t, int16_t, int16_t, int16_t);
        const int16_t *t_steer = (const int16_t *)(g_ai_physics_template + 0x68);
        const int16_t *t_grip  = (const int16_t *)(g_ai_physics_template + 0x2C);
        const int16_t *t_brake = (const int16_t *)(g_ai_physics_template + 0x6E);
        const int16_t *t_lspdb = (const int16_t *)(g_ai_physics_template + 0x70);
        const int16_t *t_spd   = (const int16_t *)(g_ai_physics_template + 0x74);
        td5_pilot_emit_00432E60_enter(tier, is_circuit, has_traffic, is_cop_chase,
                                      (int)g_td5.difficulty, is_time_trial,
                                      *t_steer, *t_grip, *t_brake, *t_lspdb, *t_spd);
    }

    if (!g_route_state_base) {
        g_route_state_base = g_route_state_storage;
    }

    /* --- Active actor count --- */
    if (is_time_trial) {
        g_active_actor_count = (g_td5.split_screen_mode > 0) ? 2 : 1;
    } else if (has_traffic) {
        g_active_actor_count = 12;
    } else {
        g_active_actor_count = 6;
    }
    g_td5.total_actor_count = g_active_actor_count;
    racer_count = is_time_trial
                  ? ((g_td5.split_screen_mode > 0) ? 2 : 1)
                  : TD5_MAX_RACER_SLOTS;

    memset(g_route_state_storage, 0, sizeof(g_route_state_storage));
    memset(g_actor_forward_track_component, 0, sizeof(g_actor_forward_track_component));
    memset(g_actor_route_steer_bias, 0, sizeof(g_actor_route_steer_bias));
    memset(g_slot_state, 0, sizeof(g_slot_state));

    for (int i = 0; i < TD5_MAX_TOTAL_ACTORS; ++i) {
        int32_t *rs = route_state(i);
        int selector = 0;

        rs[RS_SLOT_INDEX] = i;
        rs[RS_ENCOUNTER_HANDLE] = -1;
        rs[RS_DEFAULT_THROTTLE] = g_default_throttle[i];
        rs[RS_RECOVERY_STAGE] = 0;
        rs[RS_ROUTE_DIRECTION_POLARITY] = 0;

        /* [CONFIRMED via InitializeRaceActorRuntime @ 0x00432e60 decomp]
         * Route-table selection is by slot parity, not sub_lane:
         *   (slot & 1) == 1 (odd)  → LEFT.TRK  (selector 0, canonical)
         *   (slot & 1) == 0 (even) → RIGHT.TRK (selector 1, junction remap active)
         * Prior port used `sub_lane >= 2` which left ALL slots on LEFT and
         * silently disabled the AI junction-remap walker (td5_ai.c:~1440). */
        selector = (i & 1) ? 0 : 1;
        rs[RS_ROUTE_TABLE_SELECTOR] = selector;
        rs[RS_ROUTE_TABLE_PTR] = (int32_t)(intptr_t)g_route_tables[selector];
        g_last_logged_opcode[i] = -1;
    }

    /* Player-as-AI: leave slot 0 at 0 so td5_ai_update_race_actors drives
     * it and td5_ai_compute_rubber_band treats it as an AI racer. Mirrors
     * the original attract-mode slot-state write at 0x0042ACCF. */
    if (!g_td5.ini.player_is_ai) {
        g_slot_state[0] = 1;
    } else {
        TD5_LOG_I(LOG_TAG,
                  "player_is_ai=1 -> AI slot_state[0]=0 (autopilot active)");
    }
    if (g_td5.split_screen_mode > 0 && racer_count > 1) {
        g_slot_state[1] = 1;
    }
    /* Wanted mode (cop chase): slots 2-5 are inactive (no AI, no physics dispatch).
     * Mirrors gRaceSlotStateTable init at 0x42ABF8 for non-zero game types. */
    if (g_td5.wanted_mode_enabled) {
        for (int k = 2; k < TD5_MAX_RACER_SLOTS; k++)
            g_slot_state[k] = 3;
        TD5_LOG_I(LOG_TAG, "wanted_mode: g_slot_state[2..5] = 3 (inactive)");
    }
    /* Drag race: slots 2..5 are decoration (no AI dispatch). Mirrors the
     * same override in td5_game.c:849-857 so the two parallel slot-state
     * tables stay consistent. Without this, the synthetic drag driver in
     * ai_update_single_racer case 0x00 fires for slots 2..5 too — and
     * those slots have no spawned actor, so the drive command lands on
     * uninitialized memory. */
    if (g_td5.drag_race_enabled) {
        for (int k = 2; k < TD5_MAX_RACER_SLOTS; k++)
            g_slot_state[k] = 3;
        TD5_LOG_I(LOG_TAG, "drag_race: g_slot_state[2..5] = 3 (inactive)");
    }

    /* --- First layer: global difficulty scaling on AI physics template ---
     * Mirrors [@ 0x00432F2F..0x00432FEC] verbatim.
     *
     * Template field offsets:
     *   +0x2C: grip / base lateral coefficient (short)
     *   +0x68: steering factor (short)
     *   +0x6E: brake force (short)
     *   +0x70: low-speed brake coefficient (short)
     *   +0x74: top speed / rev limiter (short)
     */
    {
        int16_t *steer  = (int16_t *)(g_ai_physics_template + 0x68);
        int16_t *grip   = (int16_t *)(g_ai_physics_template + 0x2C);
        int16_t *brake  = (int16_t *)(g_ai_physics_template + 0x6E);
        int16_t *lspd_brake = (int16_t *)(g_ai_physics_template + 0x70);

        switch (g_td5.difficulty) {
        case TD5_DIFFICULTY_EASY:
            /* [@ 0x00432FB4 if (gDifficultyEasy != 0)] No change (Easy). */
            break;

        case TD5_DIFFICULTY_NORMAL:
            /* [@ 0x00432FBC..0x00432FEC] NORMAL: steer*0x168/256, grip*300/256 only.
             * No brake / lspd_brake / top_spd writes here. */
            *steer = (int16_t)(((int32_t)*steer * 0x168) >> 8);
            *grip  = (int16_t)(((int32_t)*grip  * 0x12C) >> 8); /* 0x12C = 300 */
            break;

        case TD5_DIFFICULTY_HARD:
            /* [@ 0x00432F37..0x00432FB2] HARD: 4 scaled fields. */
            *steer      = (int16_t)(((int32_t)*steer      * 0x28A) >> 8); /* [@ 0x00432F37-57]  */
            *grip       = (int16_t)(((int32_t)*grip       * 0x17C) >> 8); /* [@ 0x00432F5B-72]  */
            *brake      = (int16_t)(((int32_t)*brake      * 0x1C2) >> 8); /* [@ 0x00432F76-91]  */
            *lspd_brake = (int16_t)(((int32_t)*lspd_brake * 0x190) >> 8); /* [@ 0x00432F95-AE] 0x190=400 */
            break;
        }
    }

    /* --- Second layer: mode/circuit/traffic/tier decision tree ---
     *
     * Mirrors [@ 0x00432FF0..0x004334E1] verbatim.
     *
     * Every leaf writes: steer (+0x68), grip (+0x2C), brake (+0x6E)=1000,
     *                    lspd_brake (+0x70)=1000, top_speed (+0x74)=branch_const
     * plus the rubber-band tuples: rb_behind_scale, rb_behind_range,
     *                              rb_ahead_scale, rb_ahead_range.
     */
    {
        int16_t *steer = (int16_t *)(g_ai_physics_template + 0x68);
        int16_t *grp   = (int16_t *)(g_ai_physics_template + 0x2C);
        int16_t *brake = (int16_t *)(g_ai_physics_template + 0x6E);
        int16_t *lspdb = (int16_t *)(g_ai_physics_template + 0x70);
        int16_t *spd   = (int16_t *)(g_ai_physics_template + 0x74);

        if (is_time_trial) {
            /* [@ 0x00432F01-1F] Time-Trial path — skip all template scaling,
             * write rb_*=0/0x40/0/0x40 only. No brake/spd writes. */
            g_rb_behind_scale = 0;
            g_rb_behind_range = 0x40;
            g_rb_ahead_scale  = 0;
            g_rb_ahead_range  = 0x40;
        } else if (!is_circuit) {
            /* [@ 0x00432FF7 JZ 0x00433178] !circuit branch */
            if (is_cop_chase) {
                /* [@ 0x00433181..0x004331F0] g_raceOverlayPresetMode == 4 (Cop Chase) */
                *steer = (int16_t)(((int32_t)*steer * 0x91) >> 8);   /* [@ 0x00433185-99] */
                *brake = 1000;                                       /* [@ 0x004331B3] */
                *lspdb = 1000;                                       /* [@ 0x004331B7] */
                *spd   = 0x3A1;                                      /* [@ 0x004331BB] */
                *grp   = (int16_t)(((int32_t)*grp   * 0xB9) >> 8);   /* [@ 0x004331A1-C4] */
                g_rb_behind_scale = 0x8C;                            /* [@ 0x004331C8] */
                g_rb_behind_range = 0x64;                            /* [@ 0x004331D2] */
                g_rb_ahead_scale  = 0xC0;                            /* [@ 0x004331DC] */
                g_rb_ahead_range  = 0x40;                            /* [@ 0x004331E6] */
            } else if (!has_traffic) {
                /* [@ 0x004331F5-FA reads gTrafficActorsEnabled; 0x004331FC
                 * loads gRaceDifficultyTier; 0x00433201 JNZ → traffic-on path.
                 * Fall through: P2P no-traffic tier dispatch. */
                switch (tier) {
                case 0:
                    /* [@ 0x00433300..0x00433370 tier-0] */
                    *steer = (int16_t)(((int32_t)*steer * 0xAA) >> 8);
                    *brake = 1000;
                    *lspdb = 1000;
                    *spd   = 0x3A1;
                    *grp   = (int16_t)(((int32_t)*grp * 0x100) >> 8); /* [@ 0x00433324] */
                    g_rb_behind_scale = 0xA0;                          /* [@ 0x00433345] */
                    g_rb_behind_range = 0x64;                          /* [@ 0x0043334F] */
                    g_rb_ahead_scale  = 0x96;                          /* [@ 0x00433359] */
                    g_rb_ahead_range  = 0x50;                          /* [@ 0x00433363] */
                    break;
                case 1:
                    /* [@ 0x00433299..0x004332FB] */
                    *steer = (int16_t)(((int32_t)*steer * 0xB4) >> 8);
                    *brake = 1000;
                    *lspdb = 1000;
                    *spd   = 0x3A1;                                   /* [@ 0x004332D2] */
                    *grp   = (int16_t)(((int32_t)*grp * 0x100) >> 8); /* [@ 0x004332B7] */
                    g_rb_behind_scale = 0xC8;                          /* [@ 0x004332DD] */
                    g_rb_behind_range = 0x4B;                          /* [@ 0x004332E7] */
                    g_rb_ahead_scale  = 0xC0;                          /* [@ 0x004332EC] */
                    g_rb_ahead_range  = 0x4B;                          /* [@ 0x004332F6] */
                    break;
                default: /* tier 2 */
                    /* [@ 0x0043321E..0x00433294] */
                    *steer = (int16_t)(((int32_t)*steer * 0xDC) >> 8);
                    *brake = 1000;
                    *lspdb = 1000;
                    *spd   = 0x433;                                   /* [@ 0x0043325F] */
                    *grp   = (int16_t)(((int32_t)*grp * 0x10E) >> 8);
                    g_rb_behind_scale = 0x10E;                         /* [@ 0x0043326C] */
                    g_rb_behind_range = 0x41;                          /* [@ 0x00433276] */
                    g_rb_ahead_scale  = 0x96;                          /* [@ 0x00433280] */
                    g_rb_ahead_range  = 0x50;                          /* [@ 0x0043328A] */
                    break;
                }
            } else {
                /* [@ 0x00433372..0x004334DF] P2P WITH traffic */
                switch (tier) {
                case 0:
                    /* [@ 0x00433478..0x004334DF] */
                    *steer = (int16_t)(((int32_t)*steer * 0xB4) >> 8);
                    *brake = 1000;
                    *lspdb = 1000;
                    *spd   = 0x3A1;
                    *grp   = (int16_t)(((int32_t)*grp * 0x100) >> 8);
                    g_rb_behind_scale = 0xB4;
                    g_rb_behind_range = 0x4B;
                    g_rb_ahead_scale  = 0xBE;
                    g_rb_ahead_range  = 0x64;
                    break;
                case 1:
                    /* [@ 0x00433402..0x00433476] note unique top_spd=0x3B9 */
                    *steer = (int16_t)(((int32_t)*steer * 0xBE) >> 8);
                    *brake = 1000;
                    *lspdb = 1000;
                    *spd   = 0x3B9;                                   /* [@ 0x00433441] */
                    *grp   = (int16_t)(((int32_t)*grp * 0x10E) >> 8);
                    g_rb_behind_scale = 0xC8;
                    g_rb_behind_range = 0x3C;
                    g_rb_ahead_scale  = 0xBE;
                    g_rb_ahead_range  = 0x64;
                    break;
                default: /* tier 2 */
                    /* [@ 0x00433389..0x004333FD] */
                    *steer = (int16_t)(((int32_t)*steer * 0xDC) >> 8);
                    *brake = 1000;
                    *lspdb = 1000;
                    *spd   = 0x433;
                    *grp   = (int16_t)(((int32_t)*grp * 0x122) >> 8);
                    g_rb_behind_scale = 0xDC;
                    g_rb_behind_range = 0x3C;
                    g_rb_ahead_scale  = 0x64;
                    g_rb_ahead_range  = 0x40;
                    break;
                }
            }
        } else {
            /* [@ 0x00432FFD..0x00433173] CIRCUIT branch — tier dispatch */
            switch (tier) {
            case 0:
                /* [@ 0x00433104..0x00433173] */
                *steer = (int16_t)(((int32_t)*steer * 0x91) >> 8);
                *brake = 1000;
                *lspdb = 1000;
                *spd   = 0x3A1;                                       /* [@ 0x0043313E] */
                *grp   = (int16_t)(((int32_t)*grp * 0xC8) >> 8);
                g_rb_behind_scale = 0x8C;                              /* [@ 0x0043314B] */
                g_rb_behind_range = 0x64;                              /* [@ 0x00433155] */
                g_rb_ahead_scale  = 0xC8;                              /* [@ 0x0043315F] */
                g_rb_ahead_range  = 0x37;                              /* [@ 0x00433169] */
                break;
            case 1:
                /* [@ 0x0043308C..0x004330FF] */
                *steer = (int16_t)(((int32_t)*steer * 0xA0) >> 8);
                *brake = 1000;
                *lspdb = 1000;
                *spd   = 0x3A1;                                       /* [@ 0x004330CA] */
                *grp   = (int16_t)(((int32_t)*grp * 0xEC) >> 8);
                g_rb_behind_scale = 0x96;                              /* [@ 0x004330D7] */
                g_rb_behind_range = 0x64;                              /* [@ 0x004330E1] */
                g_rb_ahead_scale  = 0xC0;                              /* [@ 0x004330EB] */
                g_rb_ahead_range  = 0x40;                              /* [@ 0x004330F5] */
                break;
            default: /* tier 2 */
                /* [@ 0x00433015..0x00433087] circuit tier-2 leaf */
                *steer = (int16_t)(((int32_t)*steer * 0xC3) >> 8);
                *brake = 1000;
                *lspdb = 1000;
                *spd   = 0x433;                                       /* [@ 0x00433052] */
                *grp   = (int16_t)(((int32_t)*grp * 0x104) >> 8);
                g_rb_behind_scale = 0xC8;                              /* [@ 0x0043305F] */
                g_rb_behind_range = 0x64;                              /* [@ 0x00433069] */
                g_rb_ahead_scale  = 0x78;                              /* [@ 0x00433073] */
                g_rb_ahead_range  = 0x40;                              /* [@ 0x0043307D] */
                break;
            }
        }
    }

    /* Cop Chase (wanted_mode): initialize gWantedDamageStateTable proxy.
     * Original: InitializeWantedHudOverlays @ 0x43D2FC sets all 6 int16 entries
     * to 0x1000 [CONFIRMED]. Gate in UpdateRaceActors @ 0x436E1D runs the cop's
     * UpdateActorTrackBehavior only when damage_state[slot] != 0. */
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++)
        g_wanted_damage_state[i] = 0x1000;
    if (g_td5.wanted_mode_enabled) {
        TD5_LOG_I(LOG_TAG,
                  "Cop Chase: g_wanted_damage_state[0..5]=0x1000 "
                  "(mirrors gWantedDamageStateTable init @ 0x0043D2FC)");
    }

    /* Initialize encounter globals */
    g_encounter_tracked_handle = -1;
    g_encounter_cooldown = 0;
    g_encounter_enabled = g_td5.special_encounter_enabled;

    /* Initialize traffic actors from queue if traffic is enabled */
    if (g_active_actor_count > TD5_MAX_RACER_SLOTS) {
        td5_ai_init_traffic_actors();
    }

    td5_ai_refresh_route_state();

    for (int i = 0; i < g_active_actor_count; ++i) {
        TD5_LOG_I(LOG_TAG,
                  "Init AI runtime: slot=%d tier=%d mode=%d rb_behind=(scale=%d range=%d) rb_ahead=(scale=%d range=%d)",
                  i,
                  tier,
                  g_slot_state[i],
                  g_rb_behind_scale,
                  g_rb_behind_range,
                  g_rb_ahead_scale,
                  g_rb_ahead_range);
    }

    /* Pilot trace probe (precise-00432E60): capture outputs at exit. */
    {
        extern void td5_pilot_emit_00432E60_leave(int32_t, int32_t, int32_t, int32_t,
                                                  int16_t, int16_t, int16_t, int16_t, int16_t, int);
        const int16_t *t_steer = (const int16_t *)(g_ai_physics_template + 0x68);
        const int16_t *t_grip  = (const int16_t *)(g_ai_physics_template + 0x2C);
        const int16_t *t_brake = (const int16_t *)(g_ai_physics_template + 0x6E);
        const int16_t *t_lspdb = (const int16_t *)(g_ai_physics_template + 0x70);
        const int16_t *t_spd   = (const int16_t *)(g_ai_physics_template + 0x74);
        td5_pilot_emit_00432E60_leave(g_rb_behind_scale, g_rb_behind_range,
                                      g_rb_ahead_scale,  g_rb_ahead_range,
                                      *t_steer, *t_grip, *t_brake, *t_lspdb, *t_spd,
                                      g_active_actor_count);
    }
}

/* ========================================================================
 * AI Steering Bias  (0x4340C0  UpdateActorSteeringBias)
 *
 * 4-layer steering pipeline:
 *   Layer 1: Route heading (computed externally, passed as heading delta)
 *   Layer 2: Offset bias (lateral avoidance from UpdateActorTrackOffsetBias)
 *   Layer 3: Threshold state (from UpdateActorRouteThresholdState)
 *   Layer 4: Steering bias (this function)
 *
 * Speed-dependent with 3 deviation bands:
 *   < 0x400: direct steering delta (sin-scaled if < 0x100)
 *   0x400-0x7FF: ramp-up with accumulator (0x40/tick, max 0x100)
 *   >= 0x800: emergency snap (0x4000/tick)
 *
 * param route_state: pointer to this actor's route state dword array
 * param steer_weight: 0x10000 (braking), 0x20000 (coasting), 0x4000 (script)
 *
 * [CONFIRMED @ 0x004340C0] Byte-faithful with orig UpdateActorSteeringBias.
 * L5 audit 2026-05-18 (TD5_pool0 read-only):
 *   - Field offsets match: longitudinal_speed +0x314, rear_axle_slip +0x320,
 *     steering_cmd +0x30C, steering_ramp_accum +0x33A, slot_index +0xD4 (RS[0x35]).
 *   - RS[0x16]/[0x17] = LEFT/RIGHT_DEVIATION (param_1+0x58/+0x5C).
 *   - abs(longitudinal_speed >> 8) via XOR-sign idiom matches.
 *   - rate cap iVar2 = 0xC0000 / ((|v|*0x400)/(slip²+0x400) + 0x40) match.
 *   - cap iVar3 = 0x1800000 / ((|v|*0x10000)/(slip²+0x10000) + 0x100) match.
 *   - NESTED cascade structure (LEFT<0x800 → LEFT<0x401 → param_2==0 OR
 *     LEFT<0x100 OR 0x100<=LEFT<0x401; ELSE 0x401<=LEFT<0x800 +0x4000;
 *     ELSE RIGHT<0x401 mirror; ELSE -0x4000) all branches walked.
 *   - Script-ramp branch: ramp += 0x40 if <0x100; bias = curr + sar_rz(iVar2*ramp, 8);
 *     write bias FIRST, then conditional overwrite to iVar3 (LEFT) or -iVar3 (RIGHT).
 *     The RIGHT mirror uses `iVar4 <= iVar7` (skip overwrite) vs port
 *     `param_2 <= bias` — semantically identical.
 *   - Sin-fine branch uses sar_rz with 0xFFF mask (n=12). Match.
 *   - Final clamp ±0x18000 match (orig 0xfffe8000 == -0x18000).
 *   - ai_sin_fixed12 uses libm sin*4096 truncate vs original's int LUT; both
 *     yield int32 (same integer values within FPU precision). Pre-existing
 *     known class — see float-LUT trig commit 4e71a88.
 * ======================================================================== */

/* Pilot tracing — per-function entry/exit emitter for pool10 / 0x004340C0. */
extern void td5_pilot_emit_004340C0_enter(int slot, const int32_t *rs,
                                          const void *actor,
                                          int32_t steer_weight,
                                          const char *call_site);
extern void td5_pilot_emit_004340C0_leave(int slot, const int32_t *rs,
                                          const void *actor);

/* Thread-local-style call-site hint. Set by each port caller immediately
 * before invoking td5_ai_update_steering_bias() so the trace probe can
 * attribute rows to "track_behavior", "traffic_plan", or "script_advance"
 * (mirroring the Frida probe's return-address classification). */
static const char *s_pilot_004340C0_callsite = "unknown";

void td5_ai_pilot_004340C0_set_callsite(const char *site) {
    s_pilot_004340C0_callsite = site ? site : "unknown";
}

void td5_ai_update_steering_bias(int *route_state, int32_t steer_weight) {
    /* Literal translation of UpdateActorSteeringBias @ 0x4340C0.
     *
     * NESTED cascade (NOT parallel left/right):
     *   if (LEFT < 0x800) left-side branches
     *   else if (RIGHT < 0x401) right-side small branches
     *   else -0x4000
     *
     * This prevents the aligned case (LEFT=0xFFF, RIGHT=0) from
     * incorrectly firing the LEFT-emergency path and saturating steer_cmd.
     *
     * param_2 == 0 path is SCRIPT mode (uses actor ramp accumulator +0x33A
     * and iVar2-rate-limited update with clamp to iVar3).
     * param_2 != 0 path is NORMAL mode (uses SinFixed12bit for fine bands
     * and direct param_2 addition for mid bands).
     * Clamp at end is always ±0x18000.
     */
    int   slot       = route_state[RS_SLOT_INDEX];
    char *actor      = actor_ptr(slot);
    const char *site = s_pilot_004340C0_callsite;
    s_pilot_004340C0_callsite = "unknown"; /* consumed; reset for next call */
    td5_pilot_emit_004340C0_enter(slot, route_state, actor, steer_weight, site);
    int32_t iVar6    = ACTOR_I32(actor, ACTOR_LONGITUDINAL_SPEED);
    int32_t sign_mask = iVar6 >> 31;
    int32_t iVar4    = ACTOR_I32(actor, ACTOR_REAR_AXLE_SLIP) >> 8;
    int32_t iVar2, iVar3;
    int32_t left_dev, right_dev;
    int32_t bias;
    int32_t param_2 = steer_weight;

    iVar4 = iVar4 * iVar4;                           /* rear_slip_excess_shifted² */
    iVar6 = ((iVar6 >> 8) ^ sign_mask) - sign_mask;  /* abs(longitudinal_speed >> 8) */

    /* iVar2 = per-tick rate cap (small-band script path) */
    iVar2 = 0xC0000 / (((iVar6 * 0x400) / (iVar4 + 0x400)) + 0x40);

    /* iVar3 = absolute steering cap (small-band script path) */
    iVar3 = 0x1800000 / (((iVar6 * 0x10000) / (iVar4 + 0x10000)) + 0x100);

    left_dev  = route_state[RS_LEFT_DEVIATION];
    right_dev = route_state[RS_RIGHT_DEVIATION];

    if (left_dev < 0x800) {
        if (left_dev < 0x401) {
            if (param_2 == 0) {
                /* Script mode: ramp-accumulated delta, clamp to +iVar3 */
                int16_t ramp = ACTOR_I16(actor, ACTOR_STEERING_RAMP_ACCUM);
                if (ramp < 0x100) {
                    ramp += 0x40;
                    ACTOR_I16(actor, ACTOR_STEERING_RAMP_ACCUM) = ramp;
                }
                iVar2 = (int32_t)ramp * iVar2;
                bias = ACTOR_I32(actor, ACTOR_STEERING_CMD)
                     + ((iVar2 + ((iVar2 >> 31) & 0xFF)) >> 8);
                ACTOR_I32(actor, ACTOR_STEERING_CMD) = bias;
                if (iVar3 < bias) {
                    ACTOR_I32(actor, ACTOR_STEERING_CMD) = iVar3;
                }
                goto clamp_end;
            }
            if (left_dev < 0x100) {
                /* Normal mode, fine deviation: proportional correction.
                 * [CONFIRMED @ 0x4340C0 via 0x0040a700, and LUT init
                 * BuildSinCosLookupTables @ 0x0040a650]
                 * Original calls FUN_0040a700(raw_deviation) = SinFixed12bit.
                 * DAT_00483984 is a COSINE table (init stores cos(i*step)*4096),
                 * so LUT[(arg - 0x400) & 0xFFF] = cos(arg - π/2) = +sin(arg).
                 * For small deviation the return is ≈ arg, producing a
                 * proportional correction. sin(0) = 0 → aligned car (left=0xFFF
                 * right=0 fires this branch via the right mirror) stays put. */
                int32_t t   = ai_sin_fixed12(left_dev);
                int32_t mul = t * param_2;
                ACTOR_I32(actor, ACTOR_STEERING_CMD) =
                    ACTOR_I32(actor, ACTOR_STEERING_CMD)
                    + ((mul + ((mul >> 31) & 0xFFF)) >> 12);
                TD5_LOG_I(LOG_TAG, "cascade_left_fine: slot=%d L=%d w=%d t=%d",
                          slot, left_dev, param_2, t);
                goto clamp_end;
            }
            /* 0x100 <= left_dev < 0x401: direct add param_2 */
            param_2 = ACTOR_I32(actor, ACTOR_STEERING_CMD) + param_2;
        } else {
            /* 0x401 <= left_dev < 0x800: fixed +0x4000 */
            param_2 = ACTOR_I32(actor, ACTOR_STEERING_CMD) + 0x4000;
        }
    } else if (right_dev < 0x401) {
        if (param_2 == 0) {
            /* Script mode mirror: subtract, clamp to -iVar3 */
            int16_t ramp = ACTOR_I16(actor, ACTOR_STEERING_RAMP_ACCUM);
            if (ramp < 0x100) {
                ramp += 0x40;
                ACTOR_I16(actor, ACTOR_STEERING_RAMP_ACCUM) = ramp;
            }
            iVar2 = (int32_t)ramp * iVar2;
            bias = ACTOR_I32(actor, ACTOR_STEERING_CMD)
                 - ((iVar2 + ((iVar2 >> 31) & 0xFF)) >> 8);
            param_2 = -iVar3;
            ACTOR_I32(actor, ACTOR_STEERING_CMD) = bias;
            if (param_2 <= bias) {
                goto clamp_end;
            }
            /* fall through: write -iVar3 as new bias */
        } else {
            if (right_dev < 0x100) {
                /* +sin via FUN_0040a700 mirror — see left fine-band comment.
                 * Aligned car (delta=0) -> LEFT=0xFFF, RIGHT=0: this path
                 * fires with sin(0)=0 → no correction → car stays aligned. */
                int32_t t   = ai_sin_fixed12(right_dev);
                int32_t mul = t * param_2;
                ACTOR_I32(actor, ACTOR_STEERING_CMD) =
                    ACTOR_I32(actor, ACTOR_STEERING_CMD)
                    - ((mul + ((mul >> 31) & 0xFFF)) >> 12);
                TD5_LOG_I(LOG_TAG, "cascade_right_fine: slot=%d R=%d w=%d t=%d",
                          slot, right_dev, param_2, t);
                goto clamp_end;
            }
            /* 0x100 <= right_dev < 0x401: direct sub param_2 */
            param_2 = ACTOR_I32(actor, ACTOR_STEERING_CMD) - param_2;
        }
    } else {
        /* LEFT >= 0x800 AND RIGHT >= 0x401: emergency snap -0x4000 */
        param_2 = ACTOR_I32(actor, ACTOR_STEERING_CMD) + -0x4000;
    }
    ACTOR_I32(actor, ACTOR_STEERING_CMD) = param_2;

clamp_end:
    if (ACTOR_I32(actor, ACTOR_STEERING_CMD) > 0x18000) {
        ACTOR_I32(actor, ACTOR_STEERING_CMD) = 0x18000;
    } else if (ACTOR_I32(actor, ACTOR_STEERING_CMD) < -0x18000) {
        ACTOR_I32(actor, ACTOR_STEERING_CMD) = -0x18000;
    }

    TD5_LOG_I(LOG_TAG, "steer_bias: slot=%d L=%d R=%d w=%d out=%d",
              slot, left_dev, right_dev, steer_weight,
              ACTOR_I32(actor, ACTOR_STEERING_CMD));

    td5_pilot_emit_004340C0_leave(slot, route_state, actor);
}

/* ========================================================================
 * Route Threshold State  (0x434AA0  UpdateActorRouteThresholdState)
 *
 * Byte-faithful port of TD5_d3d.exe @ 0x00434AA0..0x00434B95
 * (TD5_pool6, Ghidra read_only=true, 2026-05-14).
 *
 * Throttle/brake controller operating on the route speed-threshold byte.
 * Consumes the rubber-band-modified throttle bias.
 *
 * Returns: 1 only on the emergency-brake branch (threshold==0 AND
 *          fwd_comp>0x80 AND speed<0x10000); 0 on all other paths
 *          including the slot-9 special-encounter early-exit.
 *
 * Listing structure (post-prologue at 0x00434ACF):
 *
 *   [SPECIAL-ENCOUNTER EARLY-EXIT @ 0x00434AAD-0x00434ACE]
 *     if (slot == 9
 *         && gSpecialEncounterTrackedActorHandle != -1
 *         && gActorSpecialEncounterActive[tracked * 0x47] != 0)
 *       goto early_out;            ; JNZ 0x00434B8F → ret 0, NO writes
 *
 *   [MAIN BODY @ 0x00434ACF-0x00434B7B]
 *     span      = (int16_t)actor[+0x82]    ; SPAN_NORMALIZED, MOVSX
 *     rt        = RS[0]                    ; ROUTE_TABLE_PTR
 *     bias      = RS[4]                    ; DEFAULT_THROTTLE / steer bias
 *     threshold = (uint8_t)rt[span*3 + 2]
 *     if (threshold == 0) goto T0;
 *     ; --- scaled-threshold branch ---
 *     scaled = ((threshold << 10) * 0x80808081_signed) added/SAR(7)
 *            = (threshold * 0x400) / 255          ; signed magic div
 *     if (fwd_comp >= scaled) {            ; JL fallthrough
 *       actor[+0x33E] = 0;
 *       actor[+0x36D] = 0;
 *       actor[+0x36F] = 0;
 *       return 0;
 *     }
 *     goto fallback;
 *   T0:
 *     if (fwd_comp <= 0x80) goto fallback; ; JLE
 *     if (speed   >= 0x10000) goto fallback; ; JGE
 *     actor[+0x33E] = 0xFF00;
 *     actor[+0x36D] = 1;
 *     actor[+0x36F] = 1;
 *     return 1;
 *   fallback:                              ; 0x00434B7C
 *     actor[+0x33E] = (int16_t)bias;
 *     actor[+0x36D] = 0;
 *     actor[+0x36F] = 0;
 *   early_out:                             ; 0x00434B8F
 *     return 0;
 *
 * Span source: MOVSX EAX, word [ECX + 0x4ab18a] at 0x00434AE8 with
 * ECX = slot * 0x388. Base 0x4ab18a = g_actorRuntimeState + 0x82, so the
 * reader uses actor[+0x82] = ACTOR_SPAN_NORMALIZED, NOT the raw +0x80.
 * On junction-remap spans (Moscow ~498, Newcastle wrap) the wrong field
 * triggered spurious emergency-brake / scaled-throttle gates — the
 * normalized field is the only one safe for route-table lookups.
 *
 * Special-encounter early-exit: the original gates the whole writer
 * behind the slot-9 race/traffic hijack flag so a hijacked slot 9 does
 * not stomp the racer-AI encounter_steer / brake outputs while
 * UpdateSpecialEncounterControl is also writing them. Port mirrors this
 * gate exactly using g_encounter_tracked_handle / g_encounter_active[]
 * which are this module's analogs of gSpecialEncounterTrackedActorHandle
 * / gActorSpecialEncounterActive[slot * 0x11c]. */

int td5_ai_update_route_threshold(int slot) {
    /* --- Special-encounter early-exit (slot 9 only) --------------------
     * Listing 0x00434AAD-0x00434ACE: if (slot==9 && tracked!=-1 &&
     * gActorSpecialEncounterActive[tracked*0x47]!=0) ret 0 without writing.
     * FIXME: port indexes g_encounter_active[] per-slot; original indexes
     * gActorSpecialEncounterActive at stride 0x11c. Both are "is slot N
     * currently in a special encounter?" with the same non-zero semantics. */
    if (slot == 9
        && g_encounter_tracked_handle != -1
        && g_encounter_active[g_encounter_tracked_handle] != 0) {
        return 0;
    }

    int32_t *rs    = route_state(slot);
    char    *actor = actor_ptr(slot);

    int32_t fwd_comp = g_actor_forward_track_component[slot]; /* RS[24] mirror */
    int32_t speed    = ACTOR_I32(actor, ACTOR_LONGITUDINAL_SPEED); /* +0x31C */
    int32_t bias     = g_actor_route_steer_bias[slot];        /* RS[4] mirror */

    /* MOVSX EAX, [actor+0x82] @ 0x00434AE8 — sign-extended int16. */
    int32_t span = (int32_t)ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED);

    /* MOV AL, [EDI + EDX + 2] with EDI = span*3, EDX = rt; threshold is
     * a zero-extended byte (XOR EAX,EAX before MOV AL).
     *
     * PORT-ONLY NULL GUARD (KEEP): original 0x00434AFB dereferences
     * RS[0]=route_table unconditionally. Upstream invariant in original is
     * "RS[0] is bound to a LEFT.TRK / RIGHT.TRK throttle table during
     * BindActorRouteState at race-start" (race-init path 0x00435F60). Port
     * runs this AI tick from td5_game.c's menu-state benchmark path with
     * unbound RS slots, so a NULL would fault. Behaving as "no limit"
     * (threshold=0xFF) maps to the bias-fallback exit in original. */
    int32_t threshold;
    {
        const uint8_t *route_table = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
        if (route_table) {
            threshold = (int32_t)route_table[span * 3 + 2];
        } else {
            threshold = 0xFF; /* port-only fallback; see KEEP comment above */
        }
    }

    TD5_LOG_D(LOG_TAG, "route_threshold: slot=%d span_norm=%d thr=0x%02X",
              slot, (int)span, (unsigned)threshold);

    if (threshold == 0) {
        /* T0 branch @ 0x00434B45 */
        if (fwd_comp > 0x80 && speed < 0x10000) {
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0xFF00;
            ACTOR_U8(actor, ACTOR_BRAKE_FLAG)       = 1;
            ACTOR_U8(actor, ACTOR_THROTTLE_STATE)   = 1;
            return 1;
        }
        /* fall through to fallback */
    } else {
        /* Scaled-threshold branch @ 0x00434B0B-0x00434B29.
         * Magic-divide of (threshold << 10) by 0xFF:
         *   IMUL EDI by 0x80808081 (signed) → high 32 bits → ADD EDX,EDI → SAR 7
         *   → ADD signed-bit correction. Equivalent to signed (a/255).
         * Since threshold ∈ [1,255] and (threshold<<10) ∈ [0x400,0x3FC00],
         * (threshold * 0x400) / 0xFF reproduces the result on any modern compiler.
         * At threshold==0xFF this yields 0x400 (not skipped). */
        int32_t scaled = (threshold * 0x400) / 0xFF;

        if (fwd_comp >= scaled) {
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0;
            ACTOR_U8(actor, ACTOR_BRAKE_FLAG)       = 0;
            ACTOR_U8(actor, ACTOR_THROTTLE_STATE)   = 0;
            return 0;
        }
        /* fall through to fallback */
    }

    /* fallback @ 0x00434B7C: MOV word [...], BP — write low 16 bits of bias. */
    ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)bias;
    ACTOR_U8(actor, ACTOR_BRAKE_FLAG)       = 0;
    ACTOR_U8(actor, ACTOR_THROTTLE_STATE)   = 0;
    return 0;
}

/* ========================================================================
 * Track Offset Bias / Peer Avoidance
 *   0x4337E0  FindActorTrackOffsetPeer
 *   0x434900  UpdateActorTrackOffsetBias
 * ======================================================================== */

/**
 * FindActorTrackOffsetPeer (precise-004337E0): byte-faithful port of the
 * original at 0x004337E0. Replaces the prior simplified single-pass scan.
 *
 * TWO-PASS structure [CONFIRMED @ disassembly 0x004337E0-0x00433CDE]:
 *
 * Pass 1 (TRAFFIC slots): gated on `g_racerCount > 6`. Iterates slots 6..N-1.
 *   - Compares self vs each traffic slot. Uses peer.field_0x82 (span_normalized)
 *     for the proximity test (self.field_0x82 <= peer.field_0x82).
 *   - Cross-route is NOT a separate branch in this pass — same body executes
 *     for both. The route-table swap happens inline (rs[0]==DAT_004afb58 chain).
 *   - Range gate: target_offset_at_clamp must lie within peer's RS[0x14],RS[0x15].
 *   - dist gate (final): `0 < local_c && local_c < 0x28` and best != self.
 *
 * Pass 2 (RACER slots): always runs. Iterates slots 0..min(g_racerCount,6)-1,
 *   skipping i==self.
 *   - Extra gate: peer.RS[0x18] - 0x10 <= self.RS[0x18]
 *     (RS_FORWARD_TRACK_COMP forward-track-component proximity).
 *   - Cross-route inline swap (same code as pass 1).
 *   - dist gate (final): `-1 < local_c && local_c <= 0x28` and best != self.
 *
 * Both passes use ClassifyTrackOffsetClamp + ComputeSignedTrackOffset (port:
 * td5_ai_classify_track_offset_clamp, module-level helper) to produce the
 * lateral target offset used in the range-gate against peer's RS[0x14]/RS[0x15].
 *
 * Direction formula [CONFIRMED @ 0x00433A11-0x00433A4D and 0x00433C84-0x00433CBE]:
 *   mid = (DAT_004afbb4[best*0x47] + DAT_004afbb0[best*0x47]) / 2
 *     -- these are RS[0x14]/RS[0x15] of the BEST peer (signed CDQ-divide /2,
 *        which equals C99 signed div for nonneg arg). Pass uses SAR 1 after
 *        CDQ — the original adds the sign bit before SAR, so >>1 with signed
 *        is equivalent to /2 with truncation toward -infinity for negatives
 *        (different from C /2 which truncates toward zero). We match SAR 1.
 *   abs_r = abs((peer_cardef_hi - mid) + 0x20 + offset_at_clamp)
 *   abs_l = abs((peer_cardef_lo - mid) - 0x20 + offset_at_clamp)
 *   DAT_004b08b0 = (abs_r >= abs_l)  -- note: SETGE means right >= left,
 *     which differs from prior port's "abs_r <= abs_l" by sign convention.
 *   ** Bounds source [CONFIRMED @ 0x00433C92]: cardef pointer comes from
 *     g_actorRuntimeState.slot[self_slot].field_0x1b8 (SELF's cardef), NOT
 *     the peer's. The EAX*0x8 arithmetic resolves to self_slot*0x388+0x1b8.
 *
 * Returns: best peer slot (int), or self_slot when nothing in range. Also
 * writes DAT_004b08b0 (g_lateral_avoidance_direction) when a peer is found
 * AND the direction formula executed.
 *
 * [CONFIRMED @ disassembly 0x004337E0-0x00433CDE — full pool14 audit].
 */

/* td5_ai_classify_track_offset_clamp_v2: byte-faithful port of
 * ClassifyTrackOffsetClamp @ 0x004368A0. Returns 0,1,2 to select which side's
 * cardef offset to apply in the target-offset formula.
 *   0 = in range (both samples produce span_progress >= 0xFF on first try
 *       OR second sample's progress > 0).
 *   1 = clamp against the "hi-bound" path (first sample produced progress
 *       >= 0xFF — early-exit with low32 = 1).
 *   2 = clamp against the "lo-bound" path (second sample's progress <= 0).
 *
 * Original control flow [CONFIRMED @ 0x004368A0-0x00436A65]:
 *   if (rs[0] == DAT_004afb58)             -- primary route
 *       iVar3 = span_norm                  -- NO walker; fall through
 *   else                                   -- secondary route
 *       iVar3 = span_norm
 *       iVar5 = (iVar3 + 4) wrapped
 *       walk DAT_004c3da0 jump table:
 *           if a row matches iVar5: iVar5 = remap; goto LAB_0043699b
 *   fall-through (primary OR secondary that did NOT match walker):
 *   iVar5 = (iVar3 + 4) wrapped            -- unconditional default
 *   LAB_0043699b:
 *     sample using iVar5 and iVar4 (= iVar3+4 wrapped)
 *
 * Prior port double-applied the walker remap (called
 * td5_track_apply_target_span_remap twice in succession on the secondary
 * route), which could produce the wrong iVar5 if the helper is not
 * idempotent on already-remapped spans. The original applies it ONCE.
 */
static int td5_ai_classify_track_offset_clamp_v2(int param_1, int param_2) {
    int32_t *rs = route_state(param_1);
    char *self = actor_ptr(param_1);
    int iVar3 = (int)ACTOR_I16(self, ACTOR_SPAN_NORMALIZED);
    int iVar5;
    int iVar4;
    int local_c[3] = {0, 0, 0};
    int64_t lVar7;
    const uint8_t *route_bytes = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
    int16_t *self_cd;
    int walker_matched = 0;

    /* Secondary-route walker remap: ONLY on secondary, ONLY once.
     * On primary (rs[0] == g_route_tables[0]) skip the walker entirely. */
    if (rs[RS_ROUTE_TABLE_PTR] != (int32_t)(intptr_t)g_route_tables[0]) {
        int iVar5_pre = iVar3 + 4;
        if (g_strip_span_count > 0 && g_strip_span_count <= iVar5_pre) {
            iVar5_pre = (iVar3 - g_strip_span_count) + 4;
        }
        {
            int remapped = td5_track_apply_target_span_remap(iVar5_pre, /*is_canonical=*/0);
            if (remapped != iVar5_pre && remapped != -1) {
                iVar5 = remapped;
                walker_matched = 1;
            } else {
                iVar5 = iVar5_pre; /* fall through to LAB_0043699b default */
            }
        }
    } else {
        iVar5 = 0; /* placeholder; set below in fall-through */
    }

    /* LAB_0043699b fall-through: when walker did not match (or on primary
     * route), iVar5 = (iVar3 + 4) wrapped. */
    if (!walker_matched) {
        iVar5 = iVar3 + 4;
        if (g_strip_span_count > 0 && g_strip_span_count <= iVar5) {
            iVar5 = (iVar3 - g_strip_span_count) + 4;
        }
    }

    iVar4 = iVar3 + 4;
    if (g_strip_span_count > 0 && g_strip_span_count <= iVar4) {
        iVar4 = (iVar3 - g_strip_span_count) + 4;
    }

    self_cd = (int16_t *)(intptr_t)ACTOR_I32(self, ACTOR_CAR_DEF_PTR);
    if (!route_bytes || !self_cd) return 0;

    /* SampleTrackTargetPoint(iVar5, route_bytes[iVar4*3], local_c,
     *                         cardef[+0x08] + param_2) */
    {
        int route_byte_a = (int)route_bytes[(size_t)iVar4 * 3u];
        int bias_arg = (int)self_cd[4] + param_2; /* cardef+0x08 = hi bound */
        if (!td5_track_sample_target_point(iVar5, route_byte_a,
                                            &local_c[0], &local_c[2], bias_arg)) {
            return 0;
        }
        lVar7 = td5_track_compute_span_progress(iVar5, local_c);
    }
    if ((int)lVar7 < 0xff) {
        /* Second sample uses cardef[+0x00] (lo bound) + param_2. */
        int span_norm = (int)ACTOR_I16(self, ACTOR_SPAN_NORMALIZED);
        int iVar3c = span_norm + 4;
        int route_byte_b;
        int bias_arg2;
        if (g_strip_span_count > 0 && g_strip_span_count <= iVar3c) {
            iVar3c = (span_norm - g_strip_span_count) + 4;
        }
        route_byte_b = (int)route_bytes[(size_t)iVar3c * 3u];
        bias_arg2 = (int)self_cd[0] + param_2;
        if (!td5_track_sample_target_point(iVar5, route_byte_b,
                                            &local_c[0], &local_c[2], bias_arg2)) {
            return 0;
        }
        lVar7 = td5_track_compute_span_progress(iVar5, local_c);
        /* (0 < lVar7) - 1 → 0 when lVar7>0, -1 (=0xFFFFFFFF) when lVar7<=0.
         * Mask & 0x200000002 keeps only bit 1: net low32 = (lVar7 > 0) ? 0 : 2. */
        return (((int)lVar7 > 0) ? 0 : 2);
    }
    /* CONCAT44((int)((ulonglong)lVar7 >> 0x20), 1) -> low32 = 1 */
    return 1;
}

int td5_ai_find_offset_peer(int *route_state_ptr) {
    int slot = route_state_ptr[RS_SLOT_INDEX];
    char *self = actor_ptr(slot);
    int self_slot = slot;
    int32_t self_field82 = (int32_t)ACTOR_I16(self, ACTOR_SPAN_NORMALIZED);
    int32_t self_field80 = (int32_t)ACTOR_I16(self, ACTOR_SPAN_RAW);
    int32_t self_fwd_track = route_state_ptr[RS_FORWARD_TRACK_COMP];
    int racer_count = g_active_actor_count;
    int32_t best_slot;
    int32_t best_dist;
    int i;
    int32_t offset_at_clamp = route_state_ptr[RS_TRACK_OFFSET_BIAS];

    (void)self_field80; /* read below per-peer */

    /* ----------------------------------------------------------------
     * Pass 1: TRAFFIC slots [6 .. racer_count-1].
     * Gated on racer_count > 6 (i.e., traffic mode active).
     * [CONFIRMED @ 0x004337E8/0x00433807/0x00433815: dual cmp,jl/jle].
     * ---------------------------------------------------------------- */
    best_slot = self_slot;
    best_dist = 0x2ee00;
    if (racer_count > 6) {
        for (i = 6; i < racer_count; i++) {
            int32_t *peer_rs = route_state(i);
            char *peer_actor = actor_ptr(i);
            int32_t peer_field82 = (int32_t)ACTOR_I16(peer_actor, ACTOR_SPAN_NORMALIZED);
            int32_t peer_field80 = (int32_t)ACTOR_I16(peer_actor, ACTOR_SPAN_RAW);
            int32_t classify, offset_a, offset_b;
            int classify_result;
            int16_t *peer_cd;
            int32_t bound_lo_q, bound_hi_q;
            int32_t dist;

            /* Route-id match: rs[3] [CONFIRMED @ 0x00433832] */
            if (route_state_ptr[3] != peer_rs[3]) continue;
            /* self.field_0x82 <= peer.field_0x82 [CONFIRMED @ 0x00433851] */
            if (self_field82 > peer_field82) continue;

            /* Cross-route swap [CONFIRMED @ 0x0043385C-0x0043388A] */
            if (route_state_ptr[RS_ROUTE_TABLE_PTR] != peer_rs[RS_ROUTE_TABLE_PTR]) {
                if (route_state_ptr[RS_ROUTE_TABLE_PTR] == (int32_t)(intptr_t)g_route_tables[0]) {
                    /* On primary: rs[0x0F]→rs[9], ptr ← DAT_004b08b4
                     *   [DAT_004b08b4 = g_route_tables[1] / RIGHT route] */
                    route_state_ptr[RS_TRACK_OFFSET_BIAS] = route_state_ptr[RS_LEFT_BOUNDARY_B];
                    route_state_ptr[RS_ROUTE_TABLE_PTR]   = (int32_t)(intptr_t)g_route_tables[1];
                } else {
                    /* On secondary (or other): rs[0x0E]→rs[9], ptr ← DAT_004afb58 */
                    route_state_ptr[RS_TRACK_OFFSET_BIAS] = route_state_ptr[RS_LEFT_BOUNDARY_A];
                    route_state_ptr[RS_ROUTE_TABLE_PTR]   = (int32_t)(intptr_t)g_route_tables[0];
                }
            }
            /* Bounds writeback [original 0x00433887-0x004338A8].
             *
             * Direction inverted vs the prior port comment: empirical replay
             * data (Honolulu sub_tick=0 → sub_tick=1) shows orig writes
             *   RS_ACTIVE_LOWER (RS[0x14]) = RS_RIGHT_BOUNDARY_B (RS[0x11])
             *   RS_ACTIVE_UPPER (RS[0x15]) = RS_RIGHT_BOUNDARY_A (RS[0x10])
             * across all 6 racer slots — matching the bias-clamp Step-12
             * writeback at lines 1017-1018 / 1044-1045. The previous port
             * comment ("rs[0x14]←rs[0x10]") was wrong; Ghidra mislabeled the
             * dst indices. Aligning find_offset_peer with the bias-clamp
             * direction keeps both writers consistent. */
            if (route_state_ptr[RS_ROUTE_TABLE_PTR] == (int32_t)(intptr_t)g_route_tables[1]) {
                route_state_ptr[RS_ACTIVE_UPPER_BOUND] = route_state_ptr[RS_RIGHT_EXTENT_A];
                route_state_ptr[RS_ACTIVE_LOWER_BOUND] = route_state_ptr[RS_RIGHT_EXTENT_B];
            } else {
                route_state_ptr[RS_ACTIVE_UPPER_BOUND] = route_state_ptr[RS_RIGHT_BOUNDARY_A];
                route_state_ptr[RS_ACTIVE_LOWER_BOUND] = route_state_ptr[RS_RIGHT_BOUNDARY_B];
            }

            classify_result = td5_ai_classify_track_offset_clamp(i, route_state_ptr[RS_TRACK_OFFSET_BIAS]);

            peer_cd = (int16_t *)(intptr_t)ACTOR_I32(self, ACTOR_CAR_DEF_PTR);
            if (!peer_cd) continue;
            offset_a = (int32_t)peer_cd[0];      /* cardef+0x00 */
            offset_b = (int32_t)peer_cd[4];      /* cardef+0x08 */

            if (classify_result == 1) {
                int32_t signed_off = td5_track_compute_signed_offset(
                    (int)peer_field80, 0x100, (int)(uint8_t)((const uint8_t *)(intptr_t)peer_rs[RS_ROUTE_TABLE_PTR])[(size_t)peer_field82 * 3u]);
                classify = (int32_t)peer_cd[0] + signed_off - 0x20;
            } else if (classify_result == 2) {
                int32_t signed_off = td5_track_compute_signed_offset(
                    (int)peer_field80, 0, (int)(uint8_t)((const uint8_t *)(intptr_t)peer_rs[RS_ROUTE_TABLE_PTR])[(size_t)peer_field82 * 3u]);
                classify = (int32_t)peer_cd[4] + signed_off - 0x20;
            } else {
                classify = route_state_ptr[RS_TRACK_OFFSET_BIAS];
            }

            /* Self cardef for the range gate at 0x00433968-0x00433988:
             *   bound_lo_q = cardef[0] + classify - 0x20
             *   bound_hi_q = cardef[8] + classify + 0x20
             *   guards: bound_lo_q <= peer.RS[0x14] AND peer.RS[0x15] <= bound_hi_q */
            bound_lo_q = offset_a + classify - 0x20;
            bound_hi_q = offset_b + classify + 0x20;
            if (bound_lo_q > peer_rs[RS_ACTIVE_LOWER_BOUND]) continue;
            if (peer_rs[RS_ACTIVE_UPPER_BOUND] > bound_hi_q) continue;

            dist = peer_field82 - self_field82;
            if (dist < best_dist) {
                best_dist = dist;
                best_slot = i;
                offset_at_clamp = classify; /* piVar5 carried forward */
            }
            (void)peer_field80; /* used inside classify branches */
        }

        /* Pass 1 finalisation [CONFIRMED @ 0x004339D8-0x00433A5D]:
         *   0 < local_c && local_c < 0x28 && piVar11 != piVar3[0x35] */
        if (best_dist > 0 && best_dist < 0x28 && best_slot != self_slot) {
            int32_t *bp_rs = route_state(best_slot);
            int32_t mid;
            int32_t mid_lo = bp_rs[RS_ACTIVE_LOWER_BOUND]; /* DAT_004afbb0[best*0x47] = RS[0x14] */
            int32_t mid_hi = bp_rs[RS_ACTIVE_UPPER_BOUND]; /* DAT_004afbb4[best*0x47] = RS[0x15] */
            int32_t mid_sum = mid_hi + mid_lo;
            int16_t *cd_for_dir;
            int32_t val_r, val_l, abs_r, abs_l;

            /* CDQ + SAR ESI,1 → arithmetic shift right (toward -infinity).
             * For non-negative sums this equals /2; for negatives, differs from
             * C99 signed /2 by one when sum is odd. Use shift to match. */
            mid = mid_sum >> 1;

            /* Self cardef at [0x00433957-0x00433968]: arithmetic resolves to
             *   ECX = self.field_0xd4 = RS_SLOT_INDEX
             *   EAX = (ECX*8 - ECX) << 4 + ECX = ECX*0x71 → then SHL 3 → ECX*0x388
             *   EBX = *(DWORD*)(EAX*8 + 0x4ab2c0) = actor[ECX].field_0x1b8 = self_cd
             * I.e. uses SELF's cardef. */
            cd_for_dir = (int16_t *)(intptr_t)ACTOR_I32(self, ACTOR_CAR_DEF_PTR);
            if (cd_for_dir) {
                int32_t cd_lo = (int32_t)cd_for_dir[0]; /* cardef+0x00 */
                int32_t cd_hi = (int32_t)cd_for_dir[4]; /* cardef+0x08 */

                val_r = (cd_hi - mid) + 0x20 + offset_at_clamp;
                val_l = (cd_lo - mid) - 0x20 + offset_at_clamp;
                abs_r = (val_r < 0) ? -val_r : val_r;
                abs_l = (val_l < 0) ? -val_l : val_l;
                /* SETGE: ECX = (abs_r >= abs_l) ? 1 : 0 [CONFIRMED @ 0x00433A4B] */
                g_lateral_avoidance_direction = (abs_r >= abs_l) ? 1 : 0;
            }
            return best_slot;
        }
        /* fall through: pass 1 found nothing; reset for pass 2 */
    }

    /* ----------------------------------------------------------------
     * Pass 2: RACER slots [0 .. min(racer_count, 6)-1], skipping self.
     * [CONFIRMED @ 0x00433A5E-0x00433CD0].
     * ---------------------------------------------------------------- */
    {
        int cap = (racer_count >= 6) ? 6 : racer_count;
        best_slot = self_slot;
        best_dist = 0x2ee00;
        offset_at_clamp = route_state_ptr[RS_TRACK_OFFSET_BIAS];
        for (i = 0; i < cap; i++) {
            int32_t *peer_rs;
            char *peer_actor;
            int32_t peer_field82, peer_field80;
            int classify_result;
            int32_t classify;
            int16_t *peer_cd;
            int32_t bound_lo_q, bound_hi_q;
            int32_t dist;

            if (i == self_slot) continue;
            peer_rs = route_state(i);
            peer_actor = actor_ptr(i);

            /* rs[3] route-id match [CONFIRMED @ 0x00433AA5] */
            if (route_state_ptr[3] != peer_rs[3]) continue;
            peer_field82 = (int32_t)ACTOR_I16(peer_actor, ACTOR_SPAN_NORMALIZED);
            peer_field80 = (int32_t)ACTOR_I16(peer_actor, ACTOR_SPAN_RAW);
            /* self.field_0x82 <= peer.field_0x82 [CONFIRMED @ 0x00433AC7] */
            if (self_field82 > peer_field82) continue;
            /* peer.RS[0x18] - 0x10 <= self.RS[0x18] [CONFIRMED @ 0x00433AD0-0x00433AD9] */
            if (peer_rs[RS_FORWARD_TRACK_COMP] - 0x10 > self_fwd_track) continue;

            /* Cross-route inline swap [CONFIRMED @ 0x00433ADF-0x00433B0B] */
            if (route_state_ptr[RS_ROUTE_TABLE_PTR] != peer_rs[RS_ROUTE_TABLE_PTR]) {
                if (route_state_ptr[RS_ROUTE_TABLE_PTR] == (int32_t)(intptr_t)g_route_tables[0]) {
                    route_state_ptr[RS_TRACK_OFFSET_BIAS] = route_state_ptr[RS_LEFT_BOUNDARY_B];
                    route_state_ptr[RS_ROUTE_TABLE_PTR]   = (int32_t)(intptr_t)g_route_tables[1];
                } else {
                    route_state_ptr[RS_TRACK_OFFSET_BIAS] = route_state_ptr[RS_LEFT_BOUNDARY_A];
                    route_state_ptr[RS_ROUTE_TABLE_PTR]   = (int32_t)(intptr_t)g_route_tables[0];
                }
            }
            /* Pass-2 same direction as Pass-1 (see comment above). */
            if (route_state_ptr[RS_ROUTE_TABLE_PTR] == (int32_t)(intptr_t)g_route_tables[1]) {
                route_state_ptr[RS_ACTIVE_UPPER_BOUND] = route_state_ptr[RS_RIGHT_EXTENT_A];
                route_state_ptr[RS_ACTIVE_LOWER_BOUND] = route_state_ptr[RS_RIGHT_EXTENT_B];
            } else {
                route_state_ptr[RS_ACTIVE_UPPER_BOUND] = route_state_ptr[RS_RIGHT_BOUNDARY_A];
                route_state_ptr[RS_ACTIVE_LOWER_BOUND] = route_state_ptr[RS_RIGHT_BOUNDARY_B];
            }

            classify_result = td5_ai_classify_track_offset_clamp(i, route_state_ptr[RS_TRACK_OFFSET_BIAS]);

            peer_cd = (int16_t *)(intptr_t)ACTOR_I32(self, ACTOR_CAR_DEF_PTR);
            if (!peer_cd) continue;

            if (classify_result == 1) {
                int32_t signed_off = td5_track_compute_signed_offset(
                    (int)peer_field80, 0x100,
                    (int)(uint8_t)((const uint8_t *)(intptr_t)peer_rs[RS_ROUTE_TABLE_PTR])[(size_t)peer_field82 * 3u]);
                classify = (int32_t)peer_cd[0] + signed_off - 0x20;
            } else if (classify_result == 2) {
                int32_t signed_off = td5_track_compute_signed_offset(
                    (int)peer_field80, 0,
                    (int)(uint8_t)((const uint8_t *)(intptr_t)peer_rs[RS_ROUTE_TABLE_PTR])[(size_t)peer_field82 * 3u]);
                classify = (int32_t)peer_cd[4] + signed_off - 0x20;
            } else {
                classify = route_state_ptr[RS_TRACK_OFFSET_BIAS];
            }

            /* Range gate [CONFIRMED @ 0x00433BE6-0x00433C0A]:
             *   bound_lo_q = self_cardef[0] + classify - 0x20
             *   bound_hi_q = self_cardef[8] + classify + 0x20
             *   bound_lo_q <= peer.RS[0x14], peer.RS[0x15] <= bound_hi_q */
            bound_lo_q = (int32_t)peer_cd[0] + classify - 0x20;
            bound_hi_q = (int32_t)peer_cd[4] + classify + 0x20;
            if (bound_lo_q > peer_rs[RS_ACTIVE_LOWER_BOUND]) continue;
            if (peer_rs[RS_ACTIVE_UPPER_BOUND] > bound_hi_q) continue;

            dist = peer_field82 - self_field82;
            if (dist < best_dist) {
                best_dist = dist;
                best_slot = i;
                offset_at_clamp = classify;
            }
        }

        /* Pass 2 finalisation [CONFIRMED @ 0x00433C4B-0x00433CD0]:
         *   -1 < local_c && local_c <= 0x28 && local_8 != piVar3[0x35] */
        if (best_dist > -1 && best_dist <= 0x28 && best_slot != self_slot) {
            int32_t *bp_rs = route_state(best_slot);
            int32_t mid_lo = bp_rs[RS_ACTIVE_LOWER_BOUND];
            int32_t mid_hi = bp_rs[RS_ACTIVE_UPPER_BOUND];
            int32_t mid_sum = mid_hi + mid_lo;
            int32_t mid = mid_sum >> 1;
            int16_t *cd_for_dir = (int16_t *)(intptr_t)ACTOR_I32(self, ACTOR_CAR_DEF_PTR);
            int32_t val_r, val_l, abs_r, abs_l;

            if (cd_for_dir) {
                int32_t cd_lo = (int32_t)cd_for_dir[0];
                int32_t cd_hi = (int32_t)cd_for_dir[4];

                val_r = (cd_hi - mid) + 0x20 + offset_at_clamp;
                val_l = (cd_lo - mid) - 0x20 + offset_at_clamp;
                abs_r = (val_r < 0) ? -val_r : val_r;
                abs_l = (val_l < 0) ? -val_l : val_l;
                g_lateral_avoidance_direction = (abs_r >= abs_l) ? 1 : 0;
            }
            TD5_LOG_I(LOG_TAG,
                      "find_offset_peer: slot=%d peer=%d dist=%d off=%d dir=%d",
                      self_slot, best_slot, best_dist, offset_at_clamp,
                      g_lateral_avoidance_direction);
            return best_slot;
        }
    }

    /* No peer found in either pass: original returns piVar3[0x35] (self_slot).
     * Adapter — UpdateActorTrackOffsetBias treats `peer < 0 || peer == self`
     * as "no peer"; both work. Match listing: return self_slot.
     * The caller's existing branch `if (peer >= 0)` still triggers; the
     * helper distinguishes via `peer != self` semantics elsewhere. To
     * preserve pre-existing port adapter behaviour (peer<0 ⇒ decay path),
     * convert self-return to -1 here.
     *
     * [PORT-DIVERGENCE — caller adapter; intentional, behaviour-equivalent.
     *  See memory/reference_arch_find_offset_peer_return_minus_one.md.
     *  Do not "fix" without auditing every `peer >= 0` check in callers.] */
    return -1;
}

/**
 * UpdateActorTrackOffsetBias — byte-faithful port of 0x00434900.
 *
 * Listing audit (precise-00434900, pool5_00434900):
 *
 *   0x00434900-0x00434923  CALL FindActorTrackOffsetPeer(&rs[slot]).
 *                          Test EAX vs slotIndex: peer == self → no-peer path.
 *
 *   No-peer path (0x00434925-0x0043496c):
 *     bias = rs[RS_TRACK_OFFSET_BIAS]
 *     if (bias < 0) { bias += 8; store; if (bias > 0) bias = 0; store; }
 *     reload bias
 *     if (bias <= 0) return                           // RET via 0x00434a94
 *     bias -= 8; store; if (bias >= 0) return         // RET via 0x00434a94
 *     bias = 0; store; return                         // RET via 0x00434969
 *
 *   Peer path (0x0043496d-0x00434a97):
 *     load direction = DAT_004b08b0
 *     if (direction == 1):                            // 0x00434982/0x0043498d
 *       BL = peer_actor.field_0x8C  (ACTOR_SUB_LANE_INDEX)   [@ 0x004349A0]
 *       if (BL == 0):
 *         direction = 0                                       [@ 0x004349AC]
 *         goto LAB_004349b2 (positive push)
 *       else:
 *         goto common_neg (0x00434a24)
 *     else if (direction == 0):                       // 0x004349e4 TEST ECX,ECX
 *       EDX = peer_actor.field_0x80 (SPAN_RAW) * 3            [@ 0x004349FF]
 *       DL  = g_strip_span_base[EDX*8 + 3]                    [@ 0x00434A09]
 *       EDX &= 0xF
 *       BL = peer_actor.field_0x8C  (SUB_LANE_INDEX)          [@ 0x00434A0D]
 *       if (BL != DL):
 *         goto LAB_004349b2 (positive push)
 *       else:
 *         direction = 1                                       [@ 0x00434A1A]
 *         fall through to common_neg
 *     else:                                          // direction not in {0,1}
 *       goto common_neg (no flip)
 *
 *   LAB_004349b2 (positive push):                     [@ 0x004349B2-0x00434A78]
 *     dist = (int)peer_actor.field_0x82 - (int)self_actor.field_0x82
 *     if (dist > 0x28) dist = 0x28
 *     else if (dist < 1) dist = 1
 *     bias += (0x29 - dist); return
 *
 *   common_neg (negative push):                       [@ 0x00434A24-0x00434A97]
 *     dist = (int)peer_actor.field_0x82 - (int)self_actor.field_0x82
 *     if (dist > 0x28) dist = 0x28
 *     else if (dist < 1) dist = 1
 *     bias += (dist - 0x29); return
 *
 *   Field-offset notes (verified against listing):
 *     0x4ab108 = g_actorRuntimeState slot base
 *     0x4ab188 = base + 0x80  → ACTOR_SPAN_RAW (strip-table index, peer-only)
 *     0x4ab18a = base + 0x82  → ACTOR_SPAN_NORMALIZED (distance compare, both)
 *     0x4ab194 = base + 0x8C  → ACTOR_SUB_LANE_INDEX
 *     0x4afb84 = rs[slot] + 0x84 = RS_TRACK_OFFSET_BIAS
 *     0x004c3d9c = g_strip_span_base (g_trackStripRecords in Ghidra)
 *     0x004b08b0 = g_lateral_avoidance_direction
 *
 *   Strip-record stride is 24 bytes (EDX*8 with EDX = span*3). byte+3 is the
 *   geometry_metadata byte; low nibble is the span lane count (0..15).
 *
 * Divergences from the previous (non-faithful) port:
 *   1. Distance used SPAN_RAW (0x80); listing uses SPAN_NORMALIZED (0x82).
 *      The two fields differ at junction-remap spans; using RAW measured the
 *      wrong distance at boundaries.
 *   2. Strip lane count went through td5_track_get_span_lane_count() which
 *      min-clamps to 1; the listing reads the raw nibble (can be 0). For
 *      byte-faithful equality we read it raw here, with a bounds guard so
 *      the port doesn't fault on an out-of-range peer span.
 *   3. Removed port-only ±0x600 saturation clamp; not present in original.
 *      Prior crashes attributed to clamp removal predate the SPAN_RAW fix
 *      and should now be revisited downstream.
 *   4. Negative-push branch is now reached for direction values outside
 *      {0,1} (no flip), matching the JNZ at 0x004349E6 falling through to
 *      common_neg without touching DAT_004b08b0.
 */
void td5_ai_update_track_offset_bias(int slot) {
    int32_t *rs = route_state(slot);
    int peer = td5_ai_find_offset_peer(rs);

    if (peer < 0) {
        /* No-peer path [0x00434925-0x0043496c]. Two-stage zero-crossing
         * decay: if bias <0, +=8 then clamp-at-zero; if (still) >0, -=8
         * then clamp-at-zero. Each store/reload mirrors the listing. */
        int32_t bias = rs[RS_TRACK_OFFSET_BIAS];
        if (bias < 0) {
            bias += 8;
            rs[RS_TRACK_OFFSET_BIAS] = bias;
            if (bias > 0) {
                rs[RS_TRACK_OFFSET_BIAS] = 0;
            }
        }
        bias = rs[RS_TRACK_OFFSET_BIAS];
        if (bias <= 0) {
            return; /* RET @ 0x00434a94 via JLE at 0x0043494c */
        }
        bias -= 8;
        rs[RS_TRACK_OFFSET_BIAS] = bias;
        if (bias >= 0) {
            return; /* RET @ 0x00434a94 via JGE at 0x0043495d */
        }
        rs[RS_TRACK_OFFSET_BIAS] = 0;
        return;
    }

    /* Peer-found path [0x0043496d-0x00434a97]. */
    {
        char *self_actor = actor_ptr(slot);
        char *peer_actor = actor_ptr(peer);
        int  do_positive;
        int32_t dist;
        int32_t direction = g_lateral_avoidance_direction;
        uint8_t peer_sub_lane = ACTOR_U8(peer_actor, ACTOR_SUB_LANE_INDEX);

        if (direction == 1) {
            /* [0x00434982-0x004349AA]: if peer.SUB_LANE_INDEX == 0 then flip
             * direction to 0 and take positive push; else take negative push
             * (no flip). */
            if (peer_sub_lane == 0) {
                g_lateral_avoidance_direction = 0;
                do_positive = 1;
            } else {
                do_positive = 0;
            }
        } else if (direction == 0) {
            /* [0x004349E8-0x00434A1A]: read peer's strip nibble using
             * peer.SPAN_RAW (field_0x80); compare to peer.SUB_LANE_INDEX.
             * Mismatch → positive push (no flip). Match → set direction=1
             * and negative push. */
            uint8_t strip_nibble = 0;
            int16_t peer_span_raw = ACTOR_I16(peer_actor, ACTOR_SPAN_RAW);
            const uint8_t *strips = (const uint8_t *)g_strip_span_base;
            /* PORT-ONLY NULL GUARD (KEEP): original 0x00434A09 loads
             * g_trackStripRecords + peer.SPAN_RAW*0x18 + 3 unconditionally.
             * Upstream invariant in original: STRIP.DAT is parsed before
             * any actor is alive (LoadStripFile @ 0x004444A0 from race
             * init). Port may run AI from menu/benchmark probe paths
             * before strips are bound. Once `strips != NULL` the
             * peer.SPAN_RAW index is in-range by the FindActorTrackOffsetPeer
             * contract (peer is an active actor whose SPAN_RAW was set
             * by InitActorTrackSegmentPlacement). Bounds check REMOVED
             * (was port-only insurance) for byte-faithfulness — original
             * has no clamp and trusts the peer-actor invariant. */
            if (strips) {
                int strip_idx = (int)peer_span_raw;
                strip_nibble = strips[strip_idx * 0x18 + 3] & 0x0F;
            }
            if (peer_sub_lane != strip_nibble) {
                do_positive = 1;
            } else {
                g_lateral_avoidance_direction = 1;
                do_positive = 0;
            }
        } else {
            /* [0x004349E4-0x004349E6]: direction not in {0,1} falls through
             * to common_neg without touching DAT_004b08b0. */
            do_positive = 0;
        }

        /* Distance from SPAN_NORMALIZED [0x004349C0-0x004349CF, 0x00434A3B-0x00434A50].
         * Both branches use field_0x82 (signed 16-bit). */
        {
            int32_t self_sn = (int32_t)ACTOR_I16(self_actor, ACTOR_SPAN_NORMALIZED);
            int32_t peer_sn = (int32_t)ACTOR_I16(peer_actor, ACTOR_SPAN_NORMALIZED);
            dist = peer_sn - self_sn;
        }

        /* Clamp dist to [1, 0x28]; listing orders the test as
         *   if (dist > 0x28) dist = 0x28; else if (dist < 1) dist = 1;
         * The else-if order matters only for unreachable dist values, but
         * is preserved here for clarity. */
        if (dist > 0x28) {
            dist = 0x28;
        } else if (dist < 1) {
            dist = 1;
        }

        if (do_positive) {
            /* Branch A [0x00434A68-0x00434A75]: bias += (0x29 - dist). */
            rs[RS_TRACK_OFFSET_BIAS] += (0x29 - dist);
            TD5_LOG_I(LOG_TAG,
                      "offset_bias: slot=%d pos push=%d dir=%d sub_lane=%u bias=%d",
                      slot, (0x29 - dist), direction, peer_sub_lane,
                      rs[RS_TRACK_OFFSET_BIAS]);
        } else {
            /* Branch B [0x00434A83-0x00434A8E]: bias += (dist - 0x29). */
            rs[RS_TRACK_OFFSET_BIAS] += (dist - 0x29);
            TD5_LOG_I(LOG_TAG,
                      "offset_bias: slot=%d neg push=%d dir=%d sub_lane=%u bias=%d",
                      slot, (dist - 0x29), direction, peer_sub_lane,
                      rs[RS_TRACK_OFFSET_BIAS]);
        }
    }
}

/* ========================================================================
 * RefreshActorTrackProgressOffset (0x004342E0)
 *
 * Seeds RS_TRACK_PROGRESS and RS_TRACK_OFFSET_BIAS for a racer slot at spawn.
 * Reads actor's current span (field_0x80 = ACTOR_SPAN_RAW) and world position
 * (field_0x1FC/0x204 = ACTOR_WORLD_POS_X/Z), computes 8-bit normalised progress
 * along the span via ComputeTrackSpanProgress, then computes the signed lateral
 * offset from the route byte via ComputeSignedTrackOffset.
 * [CONFIRMED @ 0x004342E0]
 * ======================================================================== */
void td5_ai_seed_actor_track_progress_offset(int slot)
{
    int32_t *rs = route_state(slot);
    char *actor = actor_ptr(slot);
    int span_raw  = (int)(int16_t)ACTOR_I16(actor, ACTOR_SPAN_RAW);
    int span_norm = (int)(int16_t)ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED);

    /* Actor position as int32[3] with [0]=world_x (24.8), [2]=world_z (24.8)
     * [CONFIRMED @ 0x004342F0: param_2 = &field_0x1fc of actor slot] */
    int32_t actor_pos[3];
    actor_pos[0] = ACTOR_I32(actor, ACTOR_WORLD_POS_X);
    actor_pos[2] = ACTOR_I32(actor, ACTOR_WORLD_POS_Z);

    int64_t progress64 = td5_track_compute_span_progress(span_raw, actor_pos);
    int progress = (int)(int32_t)(uint32_t)(progress64 & 0xFFFFFFFF);

    /* Store progress [CONFIRMED @ 0x00434313: gActorTrackSpanProgress[slot*0x47]] */
    rs[RS_TRACK_PROGRESS] = progress;

    /* Route byte from route table at span_norm * 3 [CONFIRMED @ 0x00434327].
     *
     * BUT — at seed time during InitializeRaceSession, the ORIGINAL reads
     * actor->field_0x82 (span_norm) BEFORE it has been populated from the
     * spawn world position. The field still holds its zero/uninitialized
     * value. The port had this field set before this seed runs, causing
     * port to read route_table[286*3]=76 while original reads
     * route_table[0*3]=106 (Frida-confirmed: all 6 slots get
     * route_byte=106 during init).
     *
     * Result of port bug: port's seed for slot 0 produces bias=+443
     * (progress=95 > route_byte=76 → positive); original produces
     * bias=-297 (progress=95 < route_byte=106 → negative). Same sign-flip
     * for slots 2 and 4. The wrong-signed bias shifts the AI's look-ahead
     * target to the wrong side, producing the visible "AI steers slightly
     * left into the wall" symptom on Moscow.
     *
     * Faithful port: read route_table[0] (matches original's
     * uninitialized-span_norm read at seed time).
     * [Frida-confirmed 2026-05-11: 36 init-phase ComputeSignedTrackOffset
     * calls all pass route_byte=106 regardless of span_raw {277..292};
     * port's per-span route_byte 75-79 was the bug.] */
    const uint8_t *route_bytes = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
    int route_byte = 0;
    if (route_bytes) {
        route_byte = (int)route_bytes[0];
    }
    (void)span_norm;

    /* Store signed lateral offset [CONFIRMED @ 0x00434332: DAT_004afb84[slot*0x47]] */
    {
        int32_t bias = td5_track_compute_signed_offset(span_raw, progress, route_byte);
        /* Route byte coordinate space is 0=right, 255=left (right_vertex_index base).
         * Route bytes at Moscow spawn spans are 97-145, all less than the actors'
         * lateral progress ~160 — yielding ALL-positive computed biases regardless of
         * route assignment.  The zero-bounds abs-compare in FindActorTrackOffsetPeer
         * maps positive-bias → direction=0 → positive push, negative → direction=1 →
         * negative push.  To get divergence, selector=0 (LEFT.TRK) cars need a
         * NEGATIVE starting bias so the peer-avoidance drives them leftward. */
        /* Bias stored AS-IS, matching original `ComputeSignedTrackOffset @ 0x00434670`
         * which returns the value without negation. The previous port-only
         * `if (selector == 0) bias = -bias;` was a lane-spread enhancement that
         * diverged from the original and produced a sign-flipped lateral target on
         * Moscow TT slot 0 (port dx=-2097 vs original dx=+4722 confirmed via Frida
         * target_probe). Combined with the corrected tangent direction in
         * td5_track_sample_target_point, this restores parity. */
        rs[RS_TRACK_OFFSET_BIAS] = bias;
    }

    TD5_LOG_I("ai", "seed: slot=%d sel=%d span_raw=%d span_norm=%d progress=%d route_byte=%d bias=%d",
              slot, (int)rs[RS_ROUTE_TABLE_SELECTOR], span_raw, span_norm,
              progress, route_byte, (int)rs[RS_TRACK_OFFSET_BIAS]);
}

/* ========================================================================
 * Script VM  (0x4370A0  AdvanceActorTrackScript)
 *
 * 12-opcode bytecode interpreter. Per-actor script state is stored in the
 * route state array. Scripts are executed one opcode per tick, with some
 * opcodes blocking (returning 0) until a condition is met.
 *
 * Returns: 0 = script still running (blocking), 1 = script complete/reset
 * ======================================================================== */

#ifdef TD5_PILOT_TRACE_004370A0
#include "td5_pilot_trace_004370A0.h"
/* Helper to compute "ip as index": when port stores IP as a dword-index, we
 * snapshot the index. When (legacy/orig-style) IP is a raw pointer, we
 * subtract base_ptr and divide by 4 to recover the index. The trace tool
 * uses this to align port and original IP values at the same canonical form. */
static int32_t td5_pt_compute_ip_index(int32_t base_ptr, int32_t ip) {
    /* Port semantics: ip is an index already (small unsigned int < 8). */
    if ((uint32_t)ip < 0x100u) return ip;
    if (base_ptr == 0) return -1;
    intptr_t diff = (intptr_t)(uint32_t)ip - (intptr_t)(uint32_t)base_ptr;
    if (diff < 0 || diff > 0x100 || (diff & 3) != 0) return -1;
    return (int32_t)(diff >> 2);
}
#endif

int td5_ai_advance_track_script(int *rs) {
    /* Verbatim port of AdvanceActorTrackScript @ 0x004370A0.
     * Order of operations matches the original byte-for-byte:
     *   1. Countdown decrement + program rotation (A<->B, C<->D)
     *   2. Flag 0x04 XOR Flag 0x08 (mutually exclusive)
     *      ALIGNED branch: recompute LEFT/RIGHT_DEVIATION from combined
     *      (steer_cmd + yaw_accum) and call UpdateActorSteeringBias(0x4000)
     *   3. Flag 0x10 release-or-maintain — returns 0 unconditionally
     *   4. Flag 0x02 brake/accel until speed threshold
     *   5. Opcode switch (cases 0..11)
     *
     * The previous port handled flags in inverse order (0x10 → 0x02 →
     * 0x04 → 0x08 → countdown → switch), skipped the LAB_004372cf
     * deviation recompute, treated flag 4/8 independently, used bilateral
     * aligned-tests, mishandled flag-2 threshold (4× off), did unconditional
     * case-0 terminate, and never zeroed angular_velocity_yaw on terminate.
     * All of those drove yaw rotation away from original. */
    int slot = rs[RS_SLOT_INDEX];
    char *actor = actor_ptr(slot);

#ifdef TD5_PILOT_TRACE_004370A0
    /* === pilot trace: capture entry state BEFORE any field mutation === */
    TD5_PilotTrace_004370A0_Entry _pt_e = {0};
    _pt_e.rs_addr            = (uintptr_t)rs;
    _pt_e.slot_index         = slot;
    _pt_e.route_table_ptr    = rs[RS_ROUTE_TABLE_PTR];
    _pt_e.script_base_ptr    = rs[RS_SCRIPT_BASE_PTR];
    _pt_e.script_ip          = rs[RS_SCRIPT_IP];
    _pt_e.script_ip_index    = td5_pt_compute_ip_index(_pt_e.script_base_ptr, _pt_e.script_ip);
    _pt_e.script_flags       = rs[RS_SCRIPT_FLAGS];
    _pt_e.script_countdown   = rs[RS_SCRIPT_COUNTDOWN];
    _pt_e.script_offset_param = rs[RS_SCRIPT_OFFSET_PARAM];
    _pt_e.script_speed_param  = rs[RS_SCRIPT_SPEED_PARAM];
    _pt_e.script_field_3e    = rs[RS_SCRIPT_FIELD_3E];
    _pt_e.script_field_43    = rs[RS_SCRIPT_FIELD_43];
    _pt_e.actor_yaw_accum    = ACTOR_I32(actor, ACTOR_YAW_ACCUM);
    _pt_e.actor_steering_cmd = ACTOR_I32(actor, ACTOR_STEERING_CMD);
    _pt_e.actor_long_speed   = ACTOR_I32(actor, ACTOR_LONGITUDINAL_SPEED);
    _pt_e.actor_span_norm    = (int32_t)ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED);
    _pt_e.actor_span_raw     = (int32_t)ACTOR_I16(actor, ACTOR_SPAN_RAW);
    _pt_e.actor_sub_lane     = (int32_t)(int8_t)ACTOR_U8(actor, ACTOR_SUB_LANE_INDEX);
    _pt_e.actor_encounter_steer = (int32_t)(int16_t)ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER);
    _pt_e.actor_brake_flag   = (int32_t)ACTOR_U8(actor, ACTOR_BRAKE_FLAG);
    _pt_e.actor_angular_velocity_yaw = ACTOR_I32(actor, 0x1C4);
    {
        const uint8_t *_rb = (const uint8_t *)(intptr_t)_pt_e.route_table_ptr;
        int16_t _sp = (int16_t)_pt_e.actor_span_norm;
        if (_rb && _sp >= 0) {
            uint8_t _rby = _rb[(size_t)(unsigned)_sp * 3u + 1u];
            _pt_e.route_byte_at_entry = (int32_t)_rby;
            _pt_e.route_heading_at_entry = ((int32_t)_rby * 0x102C) >> 8;
        } else {
            _pt_e.route_byte_at_entry = -1;
            _pt_e.route_heading_at_entry = 0;
        }
    }
    td5_pilot_trace_004370A0_enter(&_pt_e);
#endif

    /* ==== 1. Countdown decrement + program rotation [orig prologue] ==== */
    rs[RS_SCRIPT_COUNTDOWN]--;
    if (rs[RS_SCRIPT_COUNTDOWN] < 0) {
        intptr_t cur = (intptr_t)rs[RS_SCRIPT_BASE_PTR];
        intptr_t next = cur;
        if (cur == (intptr_t)g_script_program_a) {
            next = (intptr_t)g_script_program_b;
        } else if (cur == (intptr_t)g_script_program_b) {
            next = (intptr_t)g_script_program_a;
        } else if (cur == (intptr_t)g_script_program_c) {
            next = (intptr_t)g_script_program_d;
        } else if (cur == (intptr_t)g_script_program_d) {
            next = (intptr_t)g_script_program_c;
        }
        /* Other base_ptr values (uninitialized / DAT_00473cc8 initial
         * recovery) leave base/ip alone — only countdown resets. */
        if (next != cur) {
            rs[RS_SCRIPT_BASE_PTR] = (int32_t)next;
            rs[RS_SCRIPT_IP]       = 0;
        }
        rs[RS_SCRIPT_COUNTDOWN] = 0x96;
    }

    /* ==== 2. Flag 0x04 / Flag 0x08 — MUTUALLY EXCLUSIVE in orig ==== */
    int32_t flags        = rs[RS_SCRIPT_FLAGS];
    int32_t heading      = ACTOR_I32(actor, ACTOR_YAW_ACCUM) >> 8;
    int32_t route_heading = ai_route_heading_for_actor(rs, actor);
    int32_t hdelta_raw   = (heading - route_heading) & 0xFFF;
    int32_t hdelta_mirror = (int32_t)((-(int32_t)hdelta_raw) & 0xFFF);
    int aligned_finalize = 0;

    if ((flags & 0x04) == 0) {
        if (flags & 0x08) {
            /* Flag-8 aligned test: orig `0xDFF < mirror`. */
            if (hdelta_mirror > 0xDFF) {
                if ((int32_t)rs[RS_SCRIPT_OFFSET_PARAM] < 0) {
                    ACTOR_I32(actor, ACTOR_STEERING_CMD) = 0;
                    rs[RS_SCRIPT_FLAGS] ^= 0x08;
                }
                aligned_finalize = 1;
            } else {
                int32_t sc = ACTOR_I32(actor, ACTOR_STEERING_CMD) - 0x4000;
                if (sc < -0x19000) sc = -0x19000;
                ACTOR_I32(actor, ACTOR_STEERING_CMD) = sc;
            }
        }
    } else {
        /* Flag-4 aligned test: listing 0x004371a0-a5 tests `EAX = hd_mir`
         * (CMP EAX,0x200 / JLE → aligned). Sibling flag-8 test above uses
         * `hdelta_mirror > 0xDFF` (listing 0x0043726b-70). Port previously
         * used `hdelta_raw < 0x201`; that fires on +small bias instead of
         * the +/-small bias the mirror semantics require. pool11 D1 fix. */
        if (hdelta_mirror < 0x201) {
            if ((int32_t)rs[RS_SCRIPT_OFFSET_PARAM] < 0) {
                ACTOR_I32(actor, ACTOR_STEERING_CMD) = 0;
                rs[RS_SCRIPT_FLAGS] ^= 0x04;
            }
            aligned_finalize = 1;
        } else {
            int32_t sc = ACTOR_I32(actor, ACTOR_STEERING_CMD) + 0x4000;
            if (sc > 0x19000) sc = 0x19000;
            ACTOR_I32(actor, ACTOR_STEERING_CMD) = sc;
        }
    }

    if (aligned_finalize) {
        /* LAB_004372cf — recompute RS_LEFT/RIGHT_DEVIATION from combined
         * heading (steer_cmd + yaw_accum) and call steering-bias with
         * weight 0x4000 (script-release path). Original formula:
         *   dev = -((((combined>>8 - target) - 0x800) & 0xFFF) - 0x800) & 0xFFF) & 0xFFF
         * Flow continues to flag-0x10 / flag-0x02 / opcode switch. */
        int32_t combined = ACTOR_I32(actor, ACTOR_STEERING_CMD)
                         + ACTOR_I32(actor, ACTOR_YAW_ACCUM);
        int32_t combined_h = combined >> 8;
        uint32_t d0 = ((uint32_t)(combined_h - route_heading) - 0x800U) & 0xFFF;
        uint32_t d1 = (d0 - 0x800U) & 0xFFF;
        uint32_t dev = ((uint32_t)(-(int32_t)d1)) & 0xFFF;
        rs[RS_LEFT_DEVIATION]  = (int32_t)dev;
        rs[RS_RIGHT_DEVIATION] = (int32_t)(0xFFF - dev);
        td5_ai_pilot_004340C0_set_callsite("script_advance");
        td5_ai_update_steering_bias(rs, 0x4000);
    }

    /* Re-fetch flags (flag 4/8 path may have toggled bits). */
    flags = rs[RS_SCRIPT_FLAGS];

#ifdef TD5_PILOT_TRACE_004370A0
    /* Macro to populate exit row + emit + return one value. Used at every
     * return point inside this function. */
#define PT_EMIT_AND_RETURN(retval, op, br) do {                                          \
    TD5_PilotTrace_004370A0_Exit _pt_x = {0};                                            \
    _pt_x.opcode_dispatched  = (op);                                                     \
    _pt_x.branch_taken       = (br);                                                     \
    _pt_x.script_base_ptr_out = rs[RS_SCRIPT_BASE_PTR];                                   \
    _pt_x.script_ip_out       = rs[RS_SCRIPT_IP];                                        \
    _pt_x.script_ip_index_out = td5_pt_compute_ip_index(                                 \
                                   _pt_x.script_base_ptr_out, _pt_x.script_ip_out);      \
    _pt_x.script_flags_out    = rs[RS_SCRIPT_FLAGS];                                     \
    _pt_x.script_countdown_out = rs[RS_SCRIPT_COUNTDOWN];                                \
    _pt_x.script_offset_param_out = rs[RS_SCRIPT_OFFSET_PARAM];                          \
    _pt_x.script_speed_param_out  = rs[RS_SCRIPT_SPEED_PARAM];                           \
    _pt_x.script_field_3e_out  = rs[RS_SCRIPT_FIELD_3E];                                 \
    _pt_x.script_field_43_out  = rs[RS_SCRIPT_FIELD_43];                                 \
    _pt_x.rs_left_deviation_out  = rs[RS_LEFT_DEVIATION];                                \
    _pt_x.rs_right_deviation_out = rs[RS_RIGHT_DEVIATION];                               \
    _pt_x.actor_steering_cmd_out  = ACTOR_I32(actor, ACTOR_STEERING_CMD);                \
    _pt_x.actor_encounter_steer_out = (int32_t)(int16_t)ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER); \
    _pt_x.actor_brake_flag_out  = (int32_t)ACTOR_U8(actor, ACTOR_BRAKE_FLAG);            \
    _pt_x.actor_angular_velocity_yaw_out = ACTOR_I32(actor, 0x1C4);                     \
    _pt_x.return_value = (retval);                                                       \
    td5_pilot_trace_004370A0_exit(&_pt_x);                                               \
    return (retval);                                                                     \
} while (0)
#else
#define PT_EMIT_AND_RETURN(retval, op, br) return (retval)
#endif

    /* ==== 3. Flag 0x10 — stop-and-wait; orig returns 0 on BOTH branches. */
    if (flags & 0x10) {
        int32_t lspd = ACTOR_I32(actor, ACTOR_LONGITUDINAL_SPEED);
        if (lspd < 0x100 && lspd > -0x100) {
            rs[RS_SCRIPT_FLAGS] = flags ^ 0x10;
            ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 0;
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0;
            PT_EMIT_AND_RETURN(0, -1, TD5_PT_004370A0_BR_FLAG10);
        }
        ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0xFF00;
        ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 1;
        PT_EMIT_AND_RETURN(0, -1, TD5_PT_004370A0_BR_FLAG10);
    }

    /* ==== 4. Flag 0x02 — brake/accel until speed threshold.
     * Orig always writes brake_flag=0 at entry; release zeroes encounter_steer
     * but KEEPS flag set; threshold scale is `(op * 0x40308) >> 8` with
     * sign-rounding (4× the previous port's `<< 8` shorthand). */
    if (flags & 0x02) {
        ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 0;
        int32_t lspd = ACTOR_I32(actor, ACTOR_LONGITUDINAL_SPEED);
        int32_t op   = rs[RS_SCRIPT_OFFSET_PARAM];
        int32_t prod = op * 0x40308;
        int32_t thr  = (prod + ((prod >> 31) & 0xFF)) >> 8;
        if (op < 1) {
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0xFF00;
            if (lspd < thr) {
                ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0;
            }
        } else {
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0x00FF;
            if (thr < lspd) {
                ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0;
            }
        }
    }

    /* ==== 5. Opcode switch ==== */
    const int32_t *base = (const int32_t *)(intptr_t)rs[RS_SCRIPT_BASE_PTR];
    if (!base) PT_EMIT_AND_RETURN(1, -1, TD5_PT_004370A0_BR_SWITCH_DEFAULT);
    int ip = rs[RS_SCRIPT_IP];
    int32_t opcode = base[ip];

    if (g_last_logged_opcode[slot] != opcode) {
        TD5_LOG_I(LOG_TAG, "Script opcode change: slot=%d ip=%d opcode=%d flags=0x%X",
                  slot, ip, opcode, rs[RS_SCRIPT_FLAGS]);
        g_last_logged_opcode[slot] = opcode;
    }

    switch (opcode) {
    case 0: {
        /* Mid-band reject: stay in script if heading not aligned within ±0x40
         * of either forward (0) or reverse (0x800). On actual terminate,
         * ZERO angular_velocity_yaw (+0x1C4). */
        uint32_t a0 = ((uint32_t)hdelta_raw - 0x800U) & 0xFFF;
        uint32_t b0 = (a0 - 0x800U) & 0xFFF;
        uint32_t hd_mir = ((uint32_t)(-(int32_t)b0)) & 0xFFF;
        if (hd_mir > 0x3F && hd_mir < 0xFC1) {
            PT_EMIT_AND_RETURN(0, 0, TD5_PT_004370A0_BR_SWITCH_CASE_0_BLOCK);
        }
        if ((int32_t)rs[RS_SCRIPT_OFFSET_PARAM] < 0) {
            ACTOR_I32(actor, ACTOR_STEERING_CMD) = 0;
        }
        rs[RS_SCRIPT_BASE_PTR] = 0;
        rs[RS_SCRIPT_FLAGS]    = 0;
        rs[RS_SCRIPT_FIELD_3E] = 0;
        rs[RS_SCRIPT_FIELD_43] = 0;
        ACTOR_I32(actor, ACTOR_STEERING_CMD) = 0;
        ACTOR_I32(actor, 0x1C4) = 0; /* angular_velocity_yaw */
        PT_EMIT_AND_RETURN(1, 0, TD5_PT_004370A0_BR_SWITCH_CASE_0_TERM);
    }

    case 1:
        rs[RS_SCRIPT_SPEED_PARAM] = base[ip + 1];
        rs[RS_SCRIPT_FLAGS] |= 0x01;
        rs[RS_SCRIPT_IP] = ip + 2;
        PT_EMIT_AND_RETURN(0, 1, TD5_PT_004370A0_BR_SWITCH_CASE_1);

    case 2:
        rs[RS_SCRIPT_FLAGS] |= 0x02;
        rs[RS_SCRIPT_OFFSET_PARAM] = base[ip + 1];
        rs[RS_SCRIPT_IP] = ip + 2;
        PT_EMIT_AND_RETURN(0, 2, TD5_PT_004370A0_BR_SWITCH_CASE_2);

    case 3:
        rs[RS_SCRIPT_FLAGS] |= base[ip + 1];
        rs[RS_SCRIPT_IP] = ip + 2;
        PT_EMIT_AND_RETURN(0, 3, TD5_PT_004370A0_BR_SWITCH_CASE_3);

    case 4:
        rs[RS_SCRIPT_FLAGS] &= ~base[ip + 1];
        rs[RS_SCRIPT_IP] = ip + 2;
        PT_EMIT_AND_RETURN(0, 4, TD5_PT_004370A0_BR_SWITCH_CASE_4);

    case 5:
        rs[RS_SCRIPT_FLAGS] |= 0x04;
        rs[RS_SCRIPT_IP] = ip + 1;
        PT_EMIT_AND_RETURN(0, 5, TD5_PT_004370A0_BR_SWITCH_CASE_5);

    case 6:
        rs[RS_SCRIPT_FLAGS] |= 0x08;
        rs[RS_SCRIPT_IP] = ip + 1;
        PT_EMIT_AND_RETURN(0, 6, TD5_PT_004370A0_BR_SWITCH_CASE_6);

    case 7:
        ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0xFF00;
        ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 1;
        rs[RS_SCRIPT_IP] = ip + 1;
        PT_EMIT_AND_RETURN(0, 7, TD5_PT_004370A0_BR_SWITCH_CASE_7);

    case 8:
        /* Orig only sets flag 0x10; brake is applied by the flag-0x10
         * block next tick. */
        rs[RS_SCRIPT_FLAGS] |= 0x10;
        rs[RS_SCRIPT_IP] = ip + 1;
        PT_EMIT_AND_RETURN(0, 8, TD5_PT_004370A0_BR_SWITCH_CASE_8);

    case 9: {
        /* Auto-select program from MIRRORED hdelta + strip-half-lane.
         * Strip byte at [strip_base + span_raw*0x18 + 3] >> 1 & 7. */
        rs[RS_SCRIPT_COUNTDOWN] = 0xFA;
        uint32_t hd9 = (uint32_t)hdelta_mirror;
        int16_t span_raw = ACTOR_I16(actor, ACTOR_SPAN_RAW);
        int8_t strip_half = 0;
        if (g_strip_span_base && span_raw >= 0 && (int32_t)span_raw < g_strip_span_count) {
            const uint8_t *rec = (const uint8_t *)g_strip_span_base
                               + (uint32_t)span_raw * 0x18;
            strip_half = (int8_t)((rec[3] >> 1) & 7);
        }
        int8_t sub_lane = (int8_t)ACTOR_U8(actor, ACTOR_SUB_LANE_INDEX);

        const int32_t *sel = g_script_program_d;
        int chosen = 0;
        if (hd9 > 0x900 && hd9 < 0xF00 && sub_lane <= strip_half) {
            sel = g_script_program_a;
            chosen = 1;
        }
        if (!chosen && hd9 > 0x6FF) {
            if (strip_half < sub_lane) {
                sel = g_script_program_b;
                chosen = 1;
            } else if (hd9 > 0x700) {
                sel = g_script_program_d;
                chosen = 1;
            }
        }
        if (!chosen) {
            if (hd9 > 0xFF && strip_half <= sub_lane) {
                sel = g_script_program_c;
            } else {
                sel = g_script_program_d;
            }
        }
        rs[RS_SCRIPT_BASE_PTR] = (int32_t)(intptr_t)sel;
        rs[RS_SCRIPT_IP] = 0;
        PT_EMIT_AND_RETURN(0, 9, TD5_PT_004370A0_BR_SWITCH_CASE_9);
    }

    case 10:
        rs[RS_SCRIPT_FLAGS] |= 0x40;
        rs[RS_SCRIPT_IP] = ip + 1;
        PT_EMIT_AND_RETURN(0, 10, TD5_PT_004370A0_BR_SWITCH_CASE_10);

    case 11:
        rs[RS_SCRIPT_FLAGS] |= 0x80;
        rs[RS_SCRIPT_IP] = ip + 1;
        PT_EMIT_AND_RETURN(0, 11, TD5_PT_004370A0_BR_SWITCH_CASE_11);

    default:
        PT_EMIT_AND_RETURN(0, opcode, TD5_PT_004370A0_BR_SWITCH_DEFAULT);
    }
#ifdef TD5_PILOT_TRACE_004370A0
#undef PT_EMIT_AND_RETURN
#endif
}

/* ========================================================================
 * Actor Track Behavior  (0x434FE0  UpdateActorTrackBehavior)
 *
 * Main AI path-following tick for non-player racers and the encounter actor.
 * ======================================================================== */

/* Forward decl for the pool9_00434FE0 pilot trace (precise-port workflow). */
extern void td5_pilot_emit_00434FE0_enter(int slot, const int32_t *rs, const void *actor, int32_t game_type);
extern void td5_pilot_emit_00434FE0_leave(int slot, const int32_t *rs, const void *actor);

void td5_ai_update_track_behavior(int slot) {
    int32_t *rs = route_state(slot);
    char *actor  = actor_ptr(slot);
    int32_t heading, route_heading, hdelta;
    int threshold_result;
    int32_t steer_weight;

    /* Pool9 pilot capture: entry snapshot before any state mutation. */
    td5_pilot_emit_00434FE0_enter(slot, rs, actor, (int32_t)g_td5.game_type);

    /* Calls-trace probe: capture entry state per slot per tick.
     * Hooks YAML: re/trace-hooks/tick0_ai_chain.yaml
     * Original RVA: 0x00434FE0 UpdateActorTrackBehavior
     * Args layout: slot, steer_cmd, yaw_accum, span_norm, vlong, encounter_steer, throttle_byte, brake_flag */
    TD5_TRACE_CALL_ENTER("ai_track_behavior",
        slot,
        ACTOR_I32(actor, ACTOR_STEERING_CMD),
        ACTOR_I32(actor, ACTOR_YAW_ACCUM),
        (int32_t)ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED),
        ACTOR_I32(actor, ACTOR_LONGITUDINAL_SPEED),
        (int32_t)ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER),
        (int32_t)ACTOR_U8(actor, ACTOR_THROTTLE_STATE),
        (int32_t)ACTOR_U8(actor, ACTOR_BRAKE_FLAG));

    /* Time trial used to skip AI track behavior entirely (slot 0 was assumed
     * human). With PlayerIsAI=1 we deliberately route slot 0 through the AI,
     * so the gate is removed — racer_count is already 1 in TT, so only slot 0
     * reaches this function anyway. */

    /* AI probe (debug-level): state matching frida_ai_probe.js for side-by-side
     * diff. Keep at TD5_LOG_D to avoid flooding INFO logs. Match fields with
     * tools/frida_ai_probe.js UAB onLeave sample. */
    TD5_LOG_D(LOG_TAG, "ai_probe: slot=%d yaw=%d steer=%d wx=%d wz=%d lspd=%d ld=%d rd=%d",
              slot,
              ACTOR_I32(actor, ACTOR_YAW_ACCUM),
              ACTOR_I32(actor, ACTOR_STEERING_CMD),
              ACTOR_I32(actor, 0x1FC),
              ACTOR_I32(actor, 0x204),
              ACTOR_I32(actor, 0x314),
              rs[RS_LEFT_DEVIATION], rs[RS_RIGHT_DEVIATION]);

    /* No countdown pre-seed: the original at 0x00434FE0 has NO countdown
     * gate and NO paused-branch write of encounter_steering_cmd (+0x33E) /
     * brake_flag (+0x36D) — the cascade below reaches
     * td5_ai_update_route_threshold which writes the same
     * g_actor_route_steer_bias[slot] → encounter_steering_cmd on the
     * coasting/normal branch (see td5_ai.c:968-969). Keeping an explicit
     * pre-seed here was redundant with the original and raced with the
     * cascade output during countdown. */

    /* --- Game-type gate: scripts + misalignment-recovery only fire in SINGLE_RACE ---
     *
     * Original FUN_00434FE0 @ 0x00434FE0 has an outer
     *   if (g_selectedGameType == 0) { script-or-trigger }
     * gate at the function entry. Disassembly:
     *   0x00434FE0  MOV EAX, [0x004aaf6c]    ; g_selectedGameType
     *   0x00434FE8  TEST EAX, EAX
     *   0x00434FF2  JNZ  0x004350a5          ; jump past both inner branches
     *
     * SINGLE_RACE = 0 in TD5_GameType. For modes != SINGLE_RACE
     * (CHAMPIONSHIP, MASTERS, TT, COP_CHASE, DRAG_RACE, ...) the original
     * skips both the script-execution path and the heading-misalignment
     * recovery-script-set path, falling straight through to normal track
     * following. Without this gate, AI in non-SINGLE_RACE modes was
     * receiving recovery scripts the original never sets — the +0x4000
     * per-tick steering ramp produced wall-leaning behaviour for ~150
     * ticks regardless of contact state.
     * [CONFIRMED via Ghidra disassembly + decompilation @ 0x00434FE0]. */
    if (g_td5.game_type == TD5_GAMETYPE_SINGLE_RACE) {
        /* --- Script check: if a script is active, run it --- */
        if (rs[RS_SCRIPT_BASE_PTR] != 0) {
            int result = td5_ai_advance_track_script(rs);
            if (result == 0) {
                /* Script still running (blocking). Original 0x00435095-0x004350A3
                 * recomputes RS_TRACK_PROGRESS + RS_TRACK_OFFSET_BIAS from the
                 * actor's CURRENT span/world-pos before returning, so they don't
                 * go stale during long brake-wait or recovery scripts. */
                int span_raw  = (int)(int16_t)ACTOR_I16(actor, ACTOR_SPAN_RAW);
                int span_norm = (int)(int16_t)ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED);
                int32_t pos_vec[3];
                pos_vec[0] = ACTOR_I32(actor, ACTOR_WORLD_POS_X);
                pos_vec[1] = 0;
                pos_vec[2] = ACTOR_I32(actor, ACTOR_WORLD_POS_Z);
                int64_t prog64 = td5_track_compute_span_progress(span_raw, pos_vec);
                int progress = (int)(int32_t)(uint32_t)(prog64 & 0xFFFFFFFFu);
                rs[RS_TRACK_PROGRESS] = progress;
                const uint8_t *route_bytes = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
                int route_byte = 0;
                if (route_bytes && span_norm >= 0) {
                    route_byte = (int)route_bytes[(size_t)(unsigned)span_norm * 3u];
                }
                rs[RS_TRACK_OFFSET_BIAS] =
                    td5_track_compute_signed_offset(span_raw, progress, route_byte);
                td5_pilot_emit_00434FE0_leave(slot, rs, actor);
                return;
            }
            /* result == 1: script complete, fall through to normal AI */
        }

        /* --- Heading misalignment trigger: start recovery script --- */
        heading = ACTOR_I32(actor, ACTOR_YAW_ACCUM) >> 8;

        /* Compute route heading inline from route byte, matching original @ 0x43503A. */
        route_heading = ai_route_heading_for_actor(rs, actor);

        /* Formula: uVar3 = -(((heading - route_heading - 0x800U & 0xFFF) - 0x800 & 0xFFF) & 0xFFF) */
        {
            uint32_t adjusted = (((uint32_t)(heading - route_heading) - 0x800U) & 0xFFF);
            adjusted = (adjusted - 0x800U) & 0xFFF;
            hdelta = (int32_t)(-(int32_t)adjusted) & 0xFFF;

            /* [CONFIRMED @ 0x00434FE0] decomp: if ((800 < uVar3) && (uVar3 < 0xce0))
             * — 800 is DECIMAL (= 0x320), upper is strict <.
             *
             * Suppress the recovery-script set when route data is absent.
             * Without LEFT.TRK/RIGHT.TRK, ai_route_heading_for_actor returns
             * a hard-coded 0, which makes hdelta a function purely of actor
             * yaw + 0x800. A car spawned roughly aligned with the world axes
             * trivially falls inside (0x320, 0xCE0), firing a spurious
             * recovery-script set on the first sub-tick. orig with route data
             * loaded computes a meaningful route_heading and does NOT enter
             * recovery. */
            if (rs[RS_ROUTE_TABLE_PTR] != 0 && hdelta > 0x320 && hdelta < 0xCE0) {
                TD5_LOG_I(LOG_TAG, "recovery: slot=%d hdelta=0x%X heading=0x%X route=0x%X gt=%d",
                          slot, hdelta, heading & 0xFFF, route_heading & 0xFFF,
                          (int)g_td5.game_type);
                /* [PRECISE-PORT D1 narrowed 2026-05-14, pool9_00434FE0]:
                 * the original at 0x00435094-0x0043509F writes ONLY the two
                 * pointer fields, both with the same recovery-script pointer
                 * (DAT_00473cc8). The four extra writes (FLAGS, FIELD_3E,
                 * FIELD_43, COUNTDOWN) had no counterpart in the listing and
                 * may have over-aggressive interactions with the script VM
                 * (AdvanceActorTrackScript) which is responsible for stepping
                 * those fields itself.
                 *
                 *   MOV [EDI + 0x4afc48], 0x473cc8   ; RS_SCRIPT_BASE_PTR
                 *   MOV [EDI + 0x4afc4c], 0x473cc8   ; RS_SCRIPT_IP
                 *
                 * IMPORTANT semantic divergence vs original RS_SCRIPT_IP:
                 * the original at 0x00437405-0x0043740B dispatches opcodes via
                 *   MOV EAX, [ESI + 0xec]   ; ip = raw pointer to opcode dword
                 *   MOV ECX, [EAX]          ; *ip  (pointer-deref)
                 * so writing the program-base pointer into rs[+0xEC] means
                 * "ip points at opcode[0]". The PORT implements ip as an
                 * INTEGER INDEX and dispatches via `base[ip]` (see line ~2435).
                 * The index-equivalent of "start of program" is 0. Writing the
                 * pointer value (0x473cc8 / ~5M) here was treated as an index
                 * and crashed AdvanceActorTrackScript's `mov (%edx,%ebp,4),%eax`
                 * with EBP=ip=0x5194D8, walking off into the .rdata texture
                 * table at 0x49D1A4. Reverted to ip=0 to match port semantics. */
                rs[RS_SCRIPT_BASE_PTR] = (int32_t)(intptr_t)g_script_init_recovery;
                rs[RS_SCRIPT_IP]       = 0;
                td5_pilot_emit_00434FE0_leave(slot, rs, actor);
                return;
            }
        }
    } else {
        /* Suppress compiler warnings for unused locals when the gate is closed. */
        (void)heading; (void)route_heading; (void)hdelta;
    }

    /* --- Normal AI path following --- */

    /* 1. Lateral offset targeting (peer avoidance) */

    td5_ai_update_track_offset_bias(slot);

    /* 2. Look-ahead waypoint sampling + deviation computation
     *    [CONFIRMED @ 0x43523D-0x435372]
     *
     *    Original pipeline:
     *      a) Compute target span = current + 4 (wrapped by span count)
     *      b) SampleTrackTargetPoint -> world XZ of target point
     *      c) dx = (target_x - actor_x) >> 8, dz = (target_z - actor_z) >> 8
     *      d) target_angle = AngleFromVector12(dx, dz) with 4-quadrant expansion
     *      e) actor_heading = ((yaw_accum + steering_cmd) >> 8) & 0xFFF
     *      f) delta = actor_heading - target_angle
     *      g) Decompose into LEFT_DEVIATION / RIGHT_DEVIATION
     */
    {
        /* Original at 0x00435243 reads MOVSX EAX, word ptr [actor+0x82] —
         * track_span_NORMALIZED, NOT track_span_raw [CONFIRMED via Frida +
         * Ghidra round-15 audit 2026-05-03]. Port had been reading
         * +0x80 (RAW) which on Honolulu sim_tick=1 produces target_span=106
         * vs orig's 97 — a 9-span gap that drives port's 3.4× over-steer
         * and yaw_torque overshoot, and is the root cause of the
         * Honolulu rollover residual after the engine-pin fix. */
        int16_t span = ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED);
        int span_count = td5_track_get_span_count();
        if (span_count > 0 && span >= 0) {
            /* (a) Target span: 4 spans ahead, then remap through junction
             * table when the actor is NOT on the LEFT.TRK (canonical) route.
             * [CONFIRMED @ 0x00435180-0x00435260] Original walks
             * DAT_004c3da0 (= STRIP.DAT +0x14/+0x18) to jump across track
             * junctions to a physically-farther span; without this, the AI
             * target_angle is computed against a too-close point, producing
             * tiny first-tick deviations that land outside the cascade's
             * direct-add band and prevent the port from reaching the
             * saturated→wrap→stable oscillation regime the original uses.
             *
             * `is_canonical_route` = rs[RS_ROUTE_TABLE_SELECTOR] == 0
             * which the init path at td5_ai.c:573 maps to g_route_tables[0]
             * (LEFT.TRK). Original gates on pointer equality to DAT_004afb58
             * (LEFT.TRK blob ptr) — same intent. */
            int is_canonical = (rs[RS_ROUTE_TABLE_SELECTOR] == 0);
            int lin_span = ((int)span + 4) % span_count;
            int target_span = td5_track_apply_target_span_remap(lin_span, is_canonical);

            /* [PRECISE-PORT D4 removed 2026-05-14, pool9_00434FE0]: original
             * 0x00434FE0 does NOT write rs[RS_TRACK_PROGRESS] in the normal-AI
             * branch — that field is only written in the script-completed
             * sub-branch via ComputeTrackSpanProgress + ComputeSignedTrackOffset.
             * The port's extra td5_track_compute_spline_position(...) call here
             * corrupted RS_TRACK_PROGRESS every tick (e.g. -2081 vs orig 95 at
             * sim_tick=0), and cascaded into downstream callers that read this
             * field. The verbatim listing path simply does NOT touch it in this
             * branch. */

            /* (b) Get target world position via SampleTrackTargetPoint
             * [CONFIRMED @ 0x434800: 2-vertex interpolation with route_byte + lateral bias]
             * route_byte from route table controls left-right interpolation (0-255).
             * lateral_bias from RS_TRACK_OFFSET_BIAS applies perpendicular peer avoidance. */
            {
                int target_x = 0, target_z = 0;
                int route_byte = 128; /* default: center of span */
                int lateral_bias = rs[RS_TRACK_OFFSET_BIAS];

                /* Route byte uses PRE-junction-remap span (lin_span =
                 * current_span + 4 wrapped), NOT the post-remap target_span.
                 * [CONFIRMED @ 0x00435260-0x00435280 in FUN_00434FE0]:
                 *
                 *   iVar4 = *local_14 + 4;            // PRE-remap
                 *   if (DAT_004c3d90 <= iVar4)
                 *     iVar4 = (*local_14 - DAT_004c3d90) + 4;
                 *   FUN_00434800(iVar10,              // GEOMETRY = post-remap
                 *                (uint)*(byte *)(iVar4*3 + *piVar1),
                 *                ...);                // ROUTE_BYTE = pre-remap
                 *
                 * Geometry is sampled from the junction-aware span; lateral
                 * lane interpolation is read from the unremapped table. Using
                 * target_span here aimed the AI at the wrong lane offset on
                 * junction spans. */
                const uint8_t *route_bytes = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
                if (route_bytes) {
                    route_byte = (int)route_bytes[(size_t)(unsigned)lin_span * 3u];
                }
                TD5_LOG_I(LOG_TAG, "route_byte_pick: slot=%d lin=%d tspan=%d rb=%d",
                          slot, lin_span, target_span, route_byte);

                if (td5_track_sample_target_point(target_span, route_byte,
                                                   &target_x, &target_z, lateral_bias)) {
                    /* (c) Delta vector: target - actor, shift from 24.8 FP to integer
                     * [CONFIRMED @ 0x4352A1-0x4352C8] */
                    int32_t actor_x = ACTOR_I32(actor, ACTOR_WORLD_POS_X);
                    int32_t actor_z = ACTOR_I32(actor, ACTOR_WORLD_POS_Z);
                    int32_t dx = (target_x - actor_x) >> 8;
                    int32_t dz = (target_z - actor_z) >> 8;

                    /* target_probe: AI look-ahead target per tick.
                     * Upgraded to INFO for /fix session 1776736681 — diagnoses
                     * Moscow spin-out (dx ~5000 vs orig ~80000). Revert to
                     * TD5_LOG_D once the 15× symptom is understood. */
                    TD5_LOG_I(LOG_TAG, "target_probe: slot=%d span=%d lin=%d tspan=%d rb=%d tx=%d tz=%d dx=%d dz=%d sel=%d actor=(%d,%d)",
                              slot, (int)span, lin_span, target_span, route_byte,
                              target_x, target_z, dx, dz, (int)rs[RS_ROUTE_TABLE_SELECTOR],
                              actor_x, actor_z);

                    /* (d) Compute target angle using atan2 equivalent
                     * [CONFIRMED @ 0x4352CB-0x435336: no +0x800 on target]
                     * The yaw_accum convention (angle+0x800)<<8 creates a delta
                     * baseline of ~0x800 for aligned cars. The steering bands at
                     * 0x4340C0 are designed around this offset — L~0x7FF enters
                     * the moderate-left band which turns the car slowly, matching
                     * the original's behavior. */
                    int32_t target_angle = ai_angle_from_vector(dx, dz) & 0xFFF;

                    /* (e) Actor heading: (yaw_accum + steering_cmd) >> 8
                     * [CONFIRMED @ 0x435338-0x43534C] */
                    int32_t yaw = ACTOR_I32(actor, ACTOR_YAW_ACCUM);
                    int32_t steer = ACTOR_I32(actor, ACTOR_STEERING_CMD);
                    int32_t actor_heading = ((yaw + steer) >> 8) & 0xFFF;

                    /* (f) Delta = actor_heading - target_angle
                     * [CONFIRMED @ 0x435352] */
                    int32_t delta = actor_heading - target_angle;

                    /* (g) Decompose into left/right deviation
                     * [CONFIRMED @ 0x435354-0x435372 — formula verified
                     * faithful via raw disasm 2026-05-12]
                     *
                     * [PRECISE-PORT D2 removed 2026-05-14, pool9_00434FE0]:
                     * the prior `abs_delta < 64` clamp was an explicit
                     * workaround comment to mask a seed-progress 1-unit
                     * divergence in td5_track_compute_span_progress. The
                     * original at 0x00435354-0x00435372 has NO such clamp —
                     * the listing decomposes into the two branches below
                     * directly. Per re/analysis/precise_port_workflow.md and
                     * feedback_precise_port_over_approximation: remove the
                     * over-approximation, let the upstream divergence
                     * surface so it can be fixed at source. */
                    int32_t abs_delta = delta < 0 ? -delta : delta;
                    if (delta >= 0) {
                        rs[RS_LEFT_DEVIATION]  = 0xFFF - abs_delta;
                        rs[RS_RIGHT_DEVIATION] = delta;
                    } else {
                        rs[RS_LEFT_DEVIATION]  = abs_delta;
                        rs[RS_RIGHT_DEVIATION] = delta + 0xFFF;
                    }

                    TD5_LOG_D(LOG_TAG, "deviation: slot=%d tspan=%d ta=0x%X "
                              "hd=0x%X delta=%d L=%d R=%d",
                              slot, target_span, target_angle,
                              actor_heading, delta,
                              rs[RS_LEFT_DEVIATION], rs[RS_RIGHT_DEVIATION]);
                }
            }
        }
    }

    /* 3. Throttle/brake decision */
    threshold_result = td5_ai_update_route_threshold(slot);

    /* 5. Steering: band-based steering-command controller.
     *
     * Faithful port of UpdateActorSteeringBias @ 0x004340C0. Writes to
     * actor+0x30C (STEERING_CMD) only, never to +0x1C4 (angular_velocity_yaw).
     * Original dispatches on threshold result:
     *   threshold != 0 → weight = 0x10000  [CONFIRMED @ 0x00435392]
     *   threshold == 0 → weight = 0x20000  [CONFIRMED @ 0x0043539F]
     *
     * Omega is NOT touched here. Angular velocity evolves from the bicycle
     * model in UpdateAIVehicleDynamics @ 0x00404EC0, which at v=0 produces
     * ~0 torque and therefore ~0 omega, so the countdown grid-hold period
     * naturally leaves actors still. */
    steer_weight = (threshold_result != 0) ? 0x10000 : 0x20000;
    td5_ai_pilot_004340C0_set_callsite("track_behavior");
    td5_ai_update_steering_bias(rs, steer_weight);
    TD5_LOG_I(LOG_TAG, "track_behavior: slot=%d thr=%d weight=0x%X",
              slot, threshold_result, steer_weight);
    td5_pilot_emit_00434FE0_leave(slot, rs, actor);
}

/* ========================================================================
 * Traffic System
 *
 *   0x4353B0  RecycleTrafficActorFromQueue
 *   0x435940  InitializeTrafficActorsFromQueue
 *   0x435E80  UpdateTrafficRoutePlan
 *   0x433680  FindNearestRoutePeer
 *
 * Traffic actors occupy slots 6-11. They use:
 *   - Constant speed: encounter_steering_override = 0x3C (60)
 *   - 7-stage FSM: recycle -> heading -> edge -> normal -> target -> steer -> yield
 * ======================================================================== */

/**
 * FindNearestRoutePeer (precise-00433680): byte-faithful port of the original
 * at 0x00433680. Replaces the prior simplified-by-active-count scan.
 *
 * [CONFIRMED @ disassembly 0x00433680-0x004337D1 — full pool6 audit.]
 *
 * Two near-identical loops gated on `route_state[+0xfc]` (the dword that
 * Ghidra symbolises as `gActorRouteDirectionPolarity`, i.e. RS dword index
 * 0x3F — NOT the field already known in this port as `RS_DIRECTION_POLARITY`
 * which is index 0x25 and indexes a separate byte at RS+0x94 used elsewhere
 * for the traffic heading-mirror branch). The +0xfc field controls direction
 * sense for the peer scan: 0 → look ahead (peer ahead of self); nonzero →
 * look behind (peer behind self).
 *
 * Both loops iterate the FULL 12-slot actor pool (slots 0..11), gated by the
 * EBX/EBP < 0x4b08bc terminator at 0x0043371a/0x004337b3 — i.e. the
 * route-state stride times 12 from base 0x004afb6c. The prior port gated on
 * `g_active_actor_count`; the original does not.
 *
 * Same-actor skip: `i == self_slot` skips the slot whose RS_SLOT_INDEX
 * (dword 0x35 = `gActorTrackReferenceSlot`) matches the caller's iVar2.
 *
 * Lane match: byte at actor+0x8c == self.field_0x8c.
 *
 * Route-table identity: dword at RS[0x00] (`gActorRouteTableSelector` =
 * the route-table base POINTER, NOT the +0x0c selector at index 0x03)
 * compared between self.RS[0] and peer.RS[0]. The prior port compared
 * peer.RS[0x03] to self_selector — that was the wrong field.
 *
 * Distance gate: `0 < dist <= 0x20` for the final accept (TEST/JZ at
 * 0x00433726 + CMP 0x20/JG at 0x0043372e). Prior port used `< 0x22`.
 *
 * Direction-relative distance:
 *   path 1 (`*+0xfc == 0`): keep peers with peer_span >= self_span;
 *                            dist = peer_span - self_span
 *   path 2 (`*+0xfc != 0`): keep peers with peer_span <= self_span;
 *                            dist = self_span - peer_span
 *
 * Return: `local_4` = best peer slot if (0 < best_dist <= 0x20), else iVar2
 * (=self_slot). The original returns either iVar2 directly or via the
 * load-from-stack at 0x00433737 / 0x004337c8; both paths share the trailing
 * "if (best_dist == 0 || best_dist > 0x20) preserve iVar2" semantic.
 */

/* Route-state direction-polarity dword [CONFIRMED via Ghidra symbol
 * `gActorRouteDirectionPolarity` @ 0x004afc5c; route-state base 0x004afb60;
 * delta 0xfc bytes = dword index 0x3F]. Same field as RS_ROUTE_DIRECTION_POLARITY
 * declared at the top of this file. FindNearestRoutePeer uses this to switch
 * ahead/behind peer-scan direction; UpdateTrafficRoutePlan uses the same dword
 * for the route-heading mirror branch. Both names alias the same field. */
#define RS_PEER_SCAN_REVERSE      RS_ROUTE_DIRECTION_POLARITY

int td5_ai_find_nearest_route_peer(int *route_state_ptr) {
    int self_slot = route_state_ptr[RS_SLOT_INDEX];
    int32_t scan_reverse = route_state_ptr[RS_PEER_SCAN_REVERSE];
    int32_t self_route_ptr;
    int16_t self_span;
    uint8_t self_lane;
    int32_t best_dist = 0x2ee00;   /* CONFIRMED @ 0x0043369b */
    int best_slot = self_slot;     /* CONFIRMED: local_4 = iVar2 @ 0x00433697 */
    int i;

    /* Bounds: required because route_state(slot) returns the storage array;
     * disassembly bypasses bounds because EAX is read from a struct field. */
    if (self_slot < 0 || self_slot >= TD5_MAX_TOTAL_ACTORS) {
        return self_slot;
    }

    {
        char *self = actor_ptr(self_slot);
        self_span = ACTOR_I16(self, ACTOR_SPAN_RAW);
        self_lane = ACTOR_U8(self, ACTOR_SUB_LANE_INDEX);
        self_route_ptr = route_state_ptr[RS_ROUTE_TABLE_PTR];
    }

    if (scan_reverse == 0) {
        /* ----------------------------------------------------------------
         * Path 1 [CONFIRMED @ 0x004336a9-0x00433722]: forward scan.
         * Keep peers ahead: dist = peer_span - self_span, < best_dist.
         * ---------------------------------------------------------------- */
        for (i = 0; i < TD5_MAX_TOTAL_ACTORS; i++) {
            char *peer_actor;
            int32_t *peer_rs;
            int16_t peer_span;
            uint8_t peer_lane;
            int32_t dist;

            /* CMP EBP,EAX / JZ skip @ 0x004336b5 */
            if (i == self_slot) continue;

            peer_actor = actor_ptr(i);
            peer_rs = route_state(i);

            /* MOV SI,[EDI] @ 0x004336b9 → peer.field_0x80 = peer_span.
             * MOV DX,[ECX + 0x4ab188] @ 0x004336cd → self.field_0x80 = self_span.
             * CMP DX,SI / JG skip @ 0x004336d4 → skip if self_span > peer_span. */
            peer_span = ACTOR_I16(peer_actor, ACTOR_SPAN_RAW);
            if (self_span > peer_span) continue;

            /* MOV CL,[ECX + 0x4ab194] @ 0x004336d9 → self.field_0x8c = self_lane.
             * CMP CL,[EDI + 0xc] @ 0x004336df → compare against peer.field_0x8c.
             * JNZ skip @ 0x004336e2 → lane must match. */
            peer_lane = ACTOR_U8(peer_actor, ACTOR_SUB_LANE_INDEX);
            if (peer_lane != self_lane) continue;

            /* MOV ECX,[ECX*4 + 0x4afb6c] @ 0x004336ec → self.RS[0] (route ptr).
             * CMP ECX,[EBX] @ 0x004336f3 → compare against peer.RS[0].
             * JNZ skip @ 0x004336f5 → route-table pointers must match. */
            if (self_route_ptr != peer_rs[RS_ROUTE_TABLE_PTR]) continue;

            /* MOVSX EDX,DX / MOVSX ECX,SI / SUB ECX,EDX @ 0x004336f7-0x004336fd
             * → dist = (int32)peer_span - (int32)self_span.
             * CMP ECX,best_dist / JGE skip @ 0x004336ff-0x00433703
             * → strict less-than: skip if dist >= best_dist. */
            dist = (int32_t)peer_span - (int32_t)self_span;
            if (dist >= best_dist) continue;

            best_dist = dist;
            best_slot = i;
        }

        /* TEST ECX,ECX / JZ @ 0x00433726-0x00433728: best_dist == 0 → return self
         * CMP ECX,0x20 / JG @ 0x0043372e-0x00433731: best_dist > 0x20 → return self
         * MOV EAX,best_slot / RET @ 0x00433737-0x00433740: otherwise return best_slot. */
        if (best_dist == 0)   return self_slot;
        if (best_dist > 0x20) return self_slot;
        return best_slot;
    } else {
        /* ----------------------------------------------------------------
         * Path 2 [CONFIRMED @ 0x00433741-0x004337c1]: reverse scan.
         * Keep peers behind: dist = self_span - peer_span, < best_dist.
         * ---------------------------------------------------------------- */
        for (i = 0; i < TD5_MAX_TOTAL_ACTORS; i++) {
            char *peer_actor;
            int32_t *peer_rs;
            int16_t peer_span;
            uint8_t peer_lane;
            int32_t dist;

            /* CMP EDI,EAX / JZ skip @ 0x0043374d */
            if (i == self_slot) continue;

            peer_actor = actor_ptr(i);
            peer_rs = route_state(i);

            /* MOV DL,[ESI + 0xc] @ 0x00433751 → peer.field_0x8c = peer_lane.
             * CMP DL,[ECX + 0x4ab194] @ 0x00433765 → compare against self.field_0x8c.
             * JNZ skip @ 0x0043376b → lane must match.
             * (Note path 2 tests lane BEFORE span, opposite of path 1; semantics same.) */
            peer_lane = ACTOR_U8(peer_actor, ACTOR_SUB_LANE_INDEX);
            if (peer_lane != self_lane) continue;

            /* MOV DX,[ESI] @ 0x0043376d → peer.field_0x80 = peer_span.
             * MOV CX,[ECX + 0x4ab188] @ 0x00433770 → self.field_0x80 = self_span.
             * CMP DX,CX / JG skip @ 0x00433777-0x0043377a → skip if peer_span > self_span. */
            peer_span = ACTOR_I16(peer_actor, ACTOR_SPAN_RAW);
            if (peer_span > self_span) continue;

            /* MOV EBX,[EBX*4 + 0x4afb6c] @ 0x00433784 → self.RS[0].
             * CMP EBX,[EBP] @ 0x0043378b → compare against peer.RS[0].
             * JNZ skip @ 0x0043378e. */
            if (self_route_ptr != peer_rs[RS_ROUTE_TABLE_PTR]) continue;

            /* MOVSX EDX,DX / MOVSX ECX,CX / SUB ECX,EDX @ 0x00433790-0x00433796
             * → dist = (int32)self_span - (int32)peer_span. */
            dist = (int32_t)self_span - (int32_t)peer_span;
            if (dist >= best_dist) continue;

            best_dist = dist;
            best_slot = i;
        }

        /* TEST ECX,ECX / JZ @ 0x004337bf-0x004337c1: best_dist == 0 → preserve self.
         * CMP ECX,0x20 / JG @ 0x004337c3-0x004337c6: best_dist > 0x20 → preserve self.
         * MOV EAX,best_slot @ 0x004337c8: otherwise return best_slot. */
        if (best_dist == 0)   return self_slot;
        if (best_dist > 0x20) return self_slot;
        return best_slot;
    }
}

/**
 * RecycleTrafficActorFromQueue — byte-faithful port of 0x004353B0.
 *
 * [CONFIRMED @ 0x004353B0] L5 promotion sweep audit (2026-05-18).
 *
 * Verbatim 10-step translation per disassembly listing (1352 bytes /
 * ~340 instructions). Each step in the comment block below is anchored
 * to its orig listing range:
 *   1.  Bail g_racerCount <= 6           [0x004353B0-C1].
 *   2.  Cursor pre-scan over queue       [0x004353C9-FD].
 *   3.  Linear scan slots 6..min(N,12)   [0x004353FE-465].
 *   4.  best_dist <= 0x28 early-return.
 *   5.  *cursor == -1 early-return.
 *   6.  Slot-9 + special-encounter gate.
 *   7.  RS direction polarity + table_ptr_index write.
 *   8.  LEFT/RIGHT branch dispatch (queue_byte vs strip_byte):
 *       - LEFT: InitActorTrackSegmentPlacement + geometry + angle +
 *         polarity + ResetVehicleActorState + RefreshActorTrack
 *         ProgressOffset + NormalizeActorTrackWrapState.
 *       - RIGHT: inline jump-table scan + remap + Init + geometry +
 *         RefreshActorTrackProgressOffset + ResolveActorSegmentBoundary.
 *   9.  Advance DAT_004b08b8 +4.
 *  10.  LAB_0043588d post-call zero block.
 *
 * ARCHITECTURAL DIVERGENCES (documented in code below):
 *   - RS_DIRECTION_POLARITY macro discrepancy: port writes BOTH 0x3F
 *     (orig) and 0x25 (port's macro). FIXME flagged in code.
 *   - Recycle heading dispatch collapsed into td5_track_compute_heading
 *     (see memory/reference_arch_recycle_heading_collapse.md).
 *
 * KNOWN DIVERGENCES: none beyond the two documented architectural ones.
 *
 * Effective level: L5 (byte-faithful per static audit; arch-divergences
 * are equivalence-preserving by design).
 *
 * Verbatim translation of TD5_d3d.exe @ 0x004353B0 disassembly (pool3
 * 2026-05-14). Replaces the prior semantically-named port (which used
 * wrong route-state offsets, wrote +0x82/+0x84/+0x86 directly instead
 * of letting InitActorTrackSegmentPlacement / RSB / NormalizeWrap do
 * it, and called helpers in the wrong direction).
 *
 * Listing flow:
 *   1. Bail if g_racerCount <= 6 (no traffic).
 *   2. Pre-scan g_traffic_queue cursor forward over entries that are
 *      either == -1 sentinel, behind player.span_norm (+0x82), or within
 *      0x28 spans ahead. Advance DAT_004b08b8 over rejected entries.
 *   3. Linear scan slots 6..min(g_racerCount,12)-1: find the slot with
 *      max (player.span_norm - traffic[i].span_norm) — store as
 *      (best_slot, best_dist). NOTE: gate (>=0x29) is applied AFTER the
 *      scan, not inside it. Slot 9 is NOT excluded from the scan.
 *   4. If best_dist <= 0x28: write cursor and return.
 *   5. If *cursor == -1: write cursor and return (queue exhausted at the
 *      currently-pointed entry — no recyclable spawn).
 *   6. If best_slot == 9 AND gSpecialEncounterTrackedActorHandle != -1:
 *      write cursor and return. (This is the slot-9 protection — but
 *      only fires when slot 9 was actually selected as best.)
 *   7. Set route_state[best_slot] direction polarity (dword +0xFC),
 *      route_table_ptr_index (dword +0x68) = best_slot*4+0x10.
 *   8. Compare queue.byte_3 (sub_lane) vs g_trackStripRecords[queue_span * 0x18 + 3] & 0xf:
 *        - queue_byte > strip_byte: LEFT branch (selector=0)
 *            * actor+0x80 = queue_span; actor+0x8c = queue_byte (raw)
 *            * Call InitActorTrackSegmentPlacement(actor+0x80, actor+0x1FC)
 *            * Geometry-derive heading from strip vertex deltas (case 1-7)
 *            * Call AngleFromVector12Full; store (angle+0x800)*0x100 to +0x1f4
 *            * If polarity flag, add 0x80000 to yaw
 *            * Call ResetVehicleActorState(actor)
 *            * Call RefreshActorTrackProgressOffset(best_slot)
 *            * Call NormalizeActorTrackWrapState(actor)
 *        - queue_byte <= strip_byte: RIGHT branch (selector=1)
 *            * Inline jump-table scan: queue_span in [third, third+(high-low)-1]
 *              → remapped = queue_span + (low - third); else -1 sentinel.
 *            * actor+0x80 = remapped; actor+0x8c = queue_byte - (strip_byte & 0xf)
 *            * Call InitActorTrackSegmentPlacement; geometry; angle; polarity; reset.
 *            * Call RefreshActorTrackProgressOffset(best_slot)
 *            * Call ResolveActorSegmentBoundary(actor) (0x00443FF0)
 *   9. Advance DAT_004b08b8 by 4 bytes (past consumed entry).
 *  10. Zero post-call state fields (LAB_0043588d) including velocities,
 *      steering, encounter_handle = -1, etc.
 *
 * NOTE on RS_DIRECTION_POLARITY: the disassembly writes to
 * gActorRouteDirectionPolarity (0x004afc5c), which is at route_state
 * base+0xFC = dword index 0x3F. The port's RS_DIRECTION_POLARITY macro
 * is currently defined as 0x25 (byte 0x94), which is a different field.
 * For byte-faithful behavior this port writes BOTH offsets (the original
 * 0x3F and the port's 0x25) so downstream code that reads either path
 * sees the correct polarity. FIXME: resolve the macro discrepancy globally.
 *
 * NOTE on ResolveActorSegmentBoundary: 0x00443FF0 is ported in unmerged
 * branch precise-00443FF0 (SHA b99e36a) as td5_track_resolve_actor_segment_boundary.
 * Declared __attribute__((weak)) so the port links cleanly in master
 * (with NormalizeActorTrackWrapState as fallback) and uses the real RSB
 * once b99e36a merges.
 */

/* Forward decl, weakly linked: real symbol comes from b99e36a's
 * td5_track.c port of ResolveActorSegmentBoundary @ 0x00443FF0.
 * If unresolved at link time, the address is NULL and the fallback runs. */
extern void td5_track_resolve_actor_segment_boundary(TD5_Actor *actor)
    __attribute__((weak));

/* ARCHITECTURAL DIVERGENCE — see
 *   memory/reference_arch_recycle_heading_collapse.md
 *
 * The original 0x004353B0 inlines a span_type → vertex-offset case dispatch
 * (cases 1/2/5, 3/4, 6/7) twice — once per LEFT/RIGHT branch — feeding
 * AngleFromVector12Full to derive the spawn heading. In the port we instead
 * route through td5_track_compute_heading, which implements the same
 * geometry-from-strip atan2 contract for the actor's current span and
 * writes the result into actor+0x1F4. The case dispatch is therefore
 * absorbed into that helper; do not re-implement it here.
 *
 * Static byte-equivalence audit (cases 1/2/5, 3/4, 6/7, default):
 *  - span source: actor+0x80 (SPAN_RAW) is written with q_span (LEFT) or
 *    remapped (RIGHT) before each helper call, matching what the original's
 *    inline dispatch keys off.
 *  - vertex offsets, signed div-by-4 (>>2 with sign bias), and the
 *    (angle+0x800)<<8 writeback to +0x1F4 all match td5_track.c:3139.
 *  - helper additionally writes actor+0x290 (heading_normal) which the
 *    inline original may not — benign because nothing in the post-call
 *    zero block (LAB_0043588d) reads +0x290.
 *
 * If you ever need to byte-diff this against the original, prototype was
 * `recycle_compute_heading_angle_12` (mentioned in ca6e5bb commit body)
 * but was removed pending byte-diff validation against the existing helper.
 */

void td5_ai_recycle_traffic_actor(void) {
    int       racer_count;
    int       best_slot = 0;
    int32_t   best_dist = 0;
    int       i;
    int       loop_max;
    char     *p0;
    int16_t   player_span_norm;
    const uint8_t *qp;
    int16_t   q_span;
    uint8_t   q_flags;
    uint8_t   q_byte_3;
    char     *a;
    int32_t  *rs;
    uint8_t  *rs_bytes;

    /* [0x004353b0-c1] EAX = g_racerCount; bail if <= 6. The original reads
     * `dword ptr [0x004aaf00]` which is g_racerCount (also exposed in port
     * as g_active_actor_count for the runtime active-slot count). */
    racer_count = g_active_actor_count;
    if (racer_count <= 6) return;

    if (!g_actor_base || !g_route_state_base) return;
    if (!g_traffic_queue_ptr || !g_traffic_queue_base) return;

    /* [0x004353c9-fd] Pre-scan: advance queue cursor over entries that are
     * not-yet-overtaken by the player. The match condition for STOPPING the
     * scan is:
     *   q_span == -1  (sentinel; loop exits)
     *   OR q_span < player.span_norm   (entry is behind player → stop)
     *   OR q_span - player.span_norm >= 0x28   (≥40 spans ahead → stop)
     *
     * Otherwise (q_span >= player AND ahead < 0x28): advance.
     *
     * NOTE: player slot's span_norm at byte offset 0x82 — read as 16-bit
     * sign-extended value (MOV BP, word ptr [0x004ab18a]). Equivalent to
     * actor[0]+0x82, i.e. the span_norm field. The pre-2026-05-14 port
     * used ACTOR_SPAN_RAW (+0x80) which is the wrong field for tracks
     * with junction remaps. */
    p0 = actor_ptr(0);
    player_span_norm = ACTOR_I16(p0, ACTOR_SPAN_NORMALIZED); /* +0x82 */

    qp = g_traffic_queue_ptr;
    q_span = (int16_t)(qp[0] | (qp[1] << 8));
    if (q_span != -1) {
        for (;;) {
            int sx = (int)q_span;
            int px = (int)player_span_norm;
            if (sx < px) break;           /* behind player → stop */
            if ((sx - px) >= 0x28) break; /* ≥40 ahead → stop */
            /* fall through: too close — advance */
            qp += 4;
            q_span = (int16_t)(qp[0] | (qp[1] << 8));
            if (q_span == -1) break;
        }
    }

    /* [0x00435401-46] Scan slots 6..min(g_racerCount,12)-1; find slot with
     * MAX (player.span_norm - traffic[i].span_norm). The gate (>=0x29) is
     * applied AFTER the scan, NOT inside it. The disassembly explicitly
     * does NOT exclude slot 9 here — slot 9 is only protected later, IF
     * it was actually selected as the winner.
     *
     * The original's EBX walks the actor table at slot[6]+0x82 (raw byte
     * address 0x004ac6ba); the inner read is signed 16-bit (MOVSX). */
    loop_max = racer_count;
    if (loop_max > 12) loop_max = 12;
    {
        int psn_loop;
        for (i = 6; i < loop_max; i++) {
            int16_t ts;
            int32_t dist;
            /* Reload player.span_norm every iteration — matches original
             * MOV BP, word ptr [0x004ab18a] at 0x00435438 inside the loop. */
            psn_loop = (int)(int16_t)ACTOR_I16(actor_ptr(0), ACTOR_SPAN_NORMALIZED);
            ts = ACTOR_I16(actor_ptr(i), ACTOR_SPAN_NORMALIZED);
            dist = psn_loop - (int)ts;
            if (dist > best_dist) {
                best_slot = i;
                best_dist = dist;
            }
        }
    }

    /* [0x00435448-4b] If best_dist <= 0x28: commit cursor and return.
     * Original writes DAT_004b08b8 unconditionally just before bail. */
    if (best_dist <= 0x28) {
        g_traffic_queue_ptr = qp;
        return;
    }

    /* [0x00435451-55] If queue cursor entry is -1 sentinel: commit cursor
     * and return. The pre-scan may have left q_span==-1 OR may have left a
     * valid entry; this check fires only when entry IS sentinel. */
    if ((int16_t)(qp[0] | (qp[1] << 8)) == -1) {
        g_traffic_queue_ptr = qp;
        return;
    }

    /* [0x0043545b-67] Slot 9 protection: only blocks if slot 9 was chosen
     * AND a special-encounter handle is active. */
    if (best_slot == 9 && g_encounter_tracked_handle != -1) {
        g_traffic_queue_ptr = qp;
        return;
    }

    /* [0x0043546d-9c] uVar11 = best_slot * 0x11c. Write polarity and a
     * derived field at uVar11 + 0x4afbc8. The +0x68 dword (route_state
     * index 0x1A) holds best_slot*4+0x10 — purpose unclear; copied verbatim.
     * The polarity field is at route_state base+0xFC (dword index 0x3F)
     * per `gActorRouteDirectionPolarity = 0x004afc5c`. */
    rs = route_state(best_slot);
    rs_bytes = (uint8_t *)rs;
    /* Refresh q_span and qp[2..3] from cursor (may have been advanced by
     * pre-scan, then validated above). */
    q_span   = (int16_t)(qp[0] | (qp[1] << 8));
    q_flags  = qp[2];
    q_byte_3 = qp[3];

    /* dword at rs+0xFC = polarity (q_flags & 1) [0x0043548a]. The original
     * writes ONLY this dword (= gActorRouteDirectionPolarity, dword index 0x3F).
     * Prior port also wrote dword 0x25 (RS_DIRECTION_POLARITY_LEGACY) but that
     * field has no references in the original listing — the defensive
     * dual-write was unnecessary. */
    rs[RS_ROUTE_DIRECTION_POLARITY] = (int32_t)(q_flags & 1u);
    (void)rs_bytes;

    /* dword at rs+0x68 = best_slot*4 + 0x10 [0x00435497]. Purpose unclear
     * (the original uses it as some kind of slot-derived index/offset).
     * Port verbatim. */
    *(int32_t *)(rs_bytes + 0x68) = best_slot * 4 + 0x10;

    /* [0x0043549d-af] Compare queue.byte_3 (raw sub_lane) vs strip_records
     * [q_span * 0x18 + 3] & 0xf (strip's lane_count nibble). */
    a = actor_ptr(best_slot);
    {
        const uint8_t *strip = (const uint8_t *)g_strip_span_base;
        uint8_t strip_lane_count;
        int     left_branch; /* 1 = LEFT (queue_byte > strip_lane), 0 = RIGHT */

        if (!strip) {
            /* Without strip records we cannot proceed; commit cursor and bail. */
            g_traffic_queue_ptr = qp + 4;
            return;
        }
        strip_lane_count = strip[(size_t)q_span * 0x18 + 3] & 0x0Fu;
        left_branch = ((unsigned)strip_lane_count < (unsigned)q_byte_3) ? 1 : 0;

        if (left_branch) {
            /* ====== LEFT branch (0x004354b5-0x004356cd, selector=0) ====== */
            int32_t yaw_pack;

            /* [0x004354bb] selector = 0 */
            rs[RS_ROUTE_TABLE_SELECTOR] = 0;

            /* [0x00435541-55] actor+0x80 = queue_span; actor+0x8c = queue_byte (raw) */
            ACTOR_I16(a, ACTOR_SPAN_RAW)       = q_span;
            ACTOR_U8(a, ACTOR_SUB_LANE_INDEX)  = q_byte_3;

            /* [0x0043555b-67] InitActorTrackSegmentPlacement(actor+0x80, actor+0x1FC).
             * Byte-faithful port @ 0x00445F10 — writes world_pos[0..2] in 24.8 FP
             * AND seeds actor +0x84 (span_accum) + +0x86 (span_high) from span_raw,
             * AND clamps +0x8C (sub_lane) to (lane_count - 1) if out of range. */
            td5_track_init_actor_segment_placement(
                (int16_t *)(a + ACTOR_SPAN_RAW),
                (int32_t *)(a + ACTOR_WORLD_POS_X));

            /* [0x00435568-0x00435690] geometry-derive heading angle from strip,
             * then AngleFromVector12Full, then (angle+0x800)<<8 → actor+0x1f4.
             * For tighter fidelity, recycle_compute_heading_angle_12 walks the
             * span_type case dispatch; td5_track_compute_heading is the
             * port's existing equivalent path. We prefer the existing helper
             * here because it shares the same atan2 sign/wrap convention with
             * the rest of the port; the angle field +0x1f4 is then written
             * with the +0x800 bias as the original does. */
            td5_track_compute_heading((TD5_Actor *)a);

            /* [0x004356a6-bb] If polarity flag set, add 0x80000 to yaw. */
            yaw_pack = ACTOR_I32(a, ACTOR_YAW_ACCUM);
            if ((q_flags & 1u) != 0u) {
                ACTOR_I32(a, ACTOR_YAW_ACCUM) = yaw_pack + 0x80000;
            }

            /* [0x004356bb-c2] ResetVehicleActorState(actor) at 0x00405d70 —
             * port equivalent zeroes velocities & integrates pose. */
            td5_physics_reset_actor_state((TD5_Actor *)a);

            /* [0x004356c7-c8] RefreshActorTrackProgressOffset(best_slot) at 0x004342e0 */
            td5_ai_seed_actor_track_progress_offset(best_slot);

            /* [0x004356cd-ce] NormalizeActorTrackWrapState(actor) at 0x00443fb0 */
            td5_track_normalize_actor_wrap((TD5_Actor *)a);

            /* [0x004356d3-de] Commit DAT_004b08b8 += 4 (consume entry) */
            g_traffic_queue_ptr = qp + 4;

        } else {
            /* ====== RIGHT branch (0x004356ea-0x004358a6, selector=1) ====== */
            int32_t yaw_pack;
            int     remapped;

            /* [0x004356ea-f3] selector = 1 (RIGHT route / alt-segment) */
            rs[RS_ROUTE_TABLE_SELECTOR] = 1;

            /* [0x004354c5-0x0043550f + 0x004355e2-00] Inline jump-table scan:
             *   Find entry i where queue_span ∈ [entry.third, entry.third + (entry.high - entry.low) - 1].
             *   Result = queue_span + (entry.low - entry.third) (queue→main decode).
             *   No match: remapped = -1 (sentinel).
             *
             * The port's td5_track_apply_target_span_remap(lin_span,
             * is_canonical=0) implements the IDENTICAL math: entry[+0] read
             * as "remap_dst" (=low), entry[+2] as "remap_end_exc" (=high),
             * entry[+4] as "range_lo" (=third). Match condition:
             *   range_lo <= lin <= range_lo + (remap_end_exc - remap_dst) - 1
             * Result: lin + (remap_dst - range_lo)
             * — which is exactly queue_span + (low - third).
             *
             * Behavior contract: returns lin_span unchanged when no entry
             * matches; the original writes -1 in that case. We restore the
             * -1 sentinel by detecting "unchanged" outcome here. */
            remapped = td5_track_apply_target_span_remap((int)q_span, 0);
            if (remapped == (int)q_span) {
                /* No remap happened — either no jump entries or no match.
                 * Original stores -1 as the sentinel (OR EBP, 0xffffffff at
                 * 0x0043550f). Treat as -1. */
                remapped = -1;
            }

            /* [0x00435541-55] actor+0x80 = remapped; actor+0x8c = q_byte_3 - (strip_lane & 0xf). */
            ACTOR_I16(a, ACTOR_SPAN_RAW) = (int16_t)remapped;
            ACTOR_I8(a, ACTOR_SUB_LANE_INDEX) =
                (int8_t)((int)(int8_t)q_byte_3 - (int)(strip_lane_count & 0xFu));

            /* [0x0043555b-67] InitActorTrackSegmentPlacement byte-faithful port.
             * Seeds actor +0x84/+0x86 (span_accum/high), clamps +0x8C if sub_lane
             * exceeds lane_nibble, writes world_pos[0..2] in 24.8 FP. */
            if (remapped >= 0) {
                td5_track_init_actor_segment_placement(
                    (int16_t *)(a + ACTOR_SPAN_RAW),
                    (int32_t *)(a + ACTOR_WORLD_POS_X));
            }

            /* [0x00435568-0x00435690 mirror] heading + yaw + polarity. */
            td5_track_compute_heading((TD5_Actor *)a);
            yaw_pack = ACTOR_I32(a, ACTOR_YAW_ACCUM);
            if ((q_flags & 1u) != 0u) {
                ACTOR_I32(a, ACTOR_YAW_ACCUM) = yaw_pack + 0x80000;
            }

            /* [0x00435865-72] ResetVehicleActorState; RefreshActorTrackProgressOffset. */
            td5_physics_reset_actor_state((TD5_Actor *)a);
            td5_ai_seed_actor_track_progress_offset(best_slot);

            /* [0x00435877-78] ResolveActorSegmentBoundary(actor) at 0x00443ff0.
             * Real symbol from b99e36a (precise-00443FF0). Weakly-linked
             * forward decl; fall back to NormalizeWrap if unavailable. */
            if (&td5_track_resolve_actor_segment_boundary != NULL) {
                td5_track_resolve_actor_segment_boundary((TD5_Actor *)a);
            } else {
                /* FIXME[precise-00443FF0]: NormalizeWrap is the closest in-tree
                 * helper but does NOT handle the jump-table-decode case for
                 * out-of-ring raw spans (>ring_length). Once b99e36a is
                 * merged, the weak-linked RSB above takes over and the
                 * fallback dies. */
                td5_track_normalize_actor_wrap((TD5_Actor *)a);
            }

            /* [0x00435882-88] Commit DAT_004b08b8 += 4. */
            g_traffic_queue_ptr = qp + 4;
        }
    }

    /* ====== LAB_0043588d: shared post-call state zero (both branches) ======
     * Original writes (all dwords unless marked):
     *   slot+0x314 = 0   (LONGITUDINAL_SPEED)
     *   slot+0x30c = 0   (STEERING_CMD)
     *   slot+0x379 = 0   (byte: VEHICLE_MODE)
     *   slot+0x1f0 = 0
     *   slot+0x1f8 = 0
     *   slot+0x1c0 = 0
     *   slot+0x1c4 = 0
     *   slot+0x1c8 = 0
     *   slot+0x1cc = 0   (LIN_VEL_X)
     *   route_state[+0x88] = 0   (RS_RECOVERY_STAGE at dword 0x22)
     *   slot+0x1d0 = 0
     *   route_state[+0xF0] = 0   (dword index 0x3C; port macro RS_SCRIPT_SPEED_PARAM
     *                              — but here it's used as a generic clear)
     *   slot+0x1d4 = 0   (LIN_VEL_Z)
     *   route_state[+0x7C] = -1  (RS_ENCOUNTER_HANDLE at dword 0x1F)
     *   word slot+0x338 = 0      (16-bit) — UNNAMED slot field
     */
    {
        char *aa = actor_ptr(best_slot);
        if (aa) {
            *(int32_t *)(aa + 0x314) = 0;
            *(int32_t *)(aa + 0x30C) = 0;
            *(int8_t  *)(aa + 0x379) = 0;
            *(int32_t *)(aa + 0x1F0) = 0;
            *(int32_t *)(aa + 0x1F8) = 0;
            *(int32_t *)(aa + 0x1C0) = 0;
            *(int32_t *)(aa + 0x1C4) = 0;
            *(int32_t *)(aa + 0x1C8) = 0;
            *(int32_t *)(aa + 0x1CC) = 0;
            *(int32_t *)(aa + 0x1D0) = 0;
            *(int32_t *)(aa + 0x1D4) = 0;
            *(int16_t *)(aa + 0x338) = 0;
        }
        /* route_state byte-direct writes match the original's
         * `[uVar11 + literal]` style. */
        *(int32_t *)((uint8_t *)rs + 0x88) = 0;
        *(int32_t *)((uint8_t *)rs + 0xF0) = 0;
        *(int32_t *)((uint8_t *)rs + 0x7C) = -1;
        /* Mirror into port's semantic macros for consistency. */
        rs[RS_RECOVERY_STAGE]   = 0;
        rs[RS_ENCOUNTER_HANDLE] = -1;
        /* Track recovery stage shadow used by other port code: clear too. */
        if (best_slot >= 0 && best_slot < TD5_MAX_TOTAL_ACTORS) {
            g_traffic_recovery_stage[best_slot] = 0;
        }
    }

    TD5_LOG_I(LOG_TAG,
              "recycle_traffic: slot=%d player_span=%d q_span=%d q_lane=%u q_flags=0x%02X dist=%d",
              best_slot, (int)player_span_norm, (int)q_span, q_byte_3, q_flags, best_dist);
}

/**
 * InitializeTrafficActorsFromQueue: fill slots 6-11 from the head
 * of the TRAFFIC.BUS queue at race start.
 */
void td5_ai_set_traffic_queue(const uint8_t *data, int size) {
    /* Store the raw pointer — caller owns the backing buffer for the race
     * lifetime. `size` is informational; the queue is actually terminated
     * by a span == -1 sentinel record. */
    g_traffic_queue_base = data;
    g_traffic_queue_ptr  = data;
    TD5_LOG_I(LOG_TAG, "traffic queue bound: data=%p size=%d records=%d",
              (const void *)data, size, size / 4);
}

/* Byte-faithful port of InitializeTrafficActorsFromQueue @ 0x00435940.
 *
 * Original signature: void __stdcall InitializeTrafficActorsFromQueue(void)
 * Spawns ambient traffic actors into slots [6, min(g_racerCount, 12)).
 * Source: Ghidra disassembly listing 0x00435940-0x00435CB7 (271 instructions).
 *
 * [CONFIRMED @ 0x00435940] L5 audit 2026-05-18 (TD5_pool0 read-only) — byte-
 * faithful with original for all in-loop logic:
 *   - Outer gate `if (6 < g_racerCount)` matches 0x0043595D.
 *   - racer_cap = min(racer_count, 12) matches 0x0043597E.
 *   - Per-slot initial local_18=6, local_c=0x28 match 0x0043595B-70.
 *   - Branch condition (lane_count > queue.sub_lane) matches JA at 0x004359C1.
 *   - NORMAL path span_type switch (cases 1/2/5, 3/4, 6/7) byte-faithful.
 *   - REMAP path inlined ComputeActorTrackHeading switch byte-faithful.
 *   - Yaw computation `(angle + 0x800) << 8` matches 0x435C44 / 0x00435A77.
 *   - Polarity flip +0x80000 matches 0x435C58 / 0x00435A88.
 *   - Per-slot advance: qp+=4, slot++, local_c+=4 matches 0x00435C8B-CA5.
 *
 * [ARCH-DIVERGENCE] Three intentional port-side divergences kept post-audit.
 * See `reference_arch_init_traffic_actors_2026-05-18.md` for full rationale.
 *   D1. REMAP-miss returns input span vs orig -1 sentinel; port re-checks
 *       equality and substitutes -1 to match orig externally.
 *   D2. Trailing common ops (ACTOR_SLOT_INDEX mirror + RS_SLOT_INDEX +
 *       RS_ENCOUNTER_HANDLE = -1) are port-only bookkeeping; orig achieves
 *       the same via EBX address-mode addressing + ResetVehicleActorState
 *       side-effects.
 *   D3. RS_DIRECTION_POLARITY (legacy alias 0x25) dual-write removed
 *       2026-05-14 after confirming no original readers. Orig writes
 *       dword 0x3F = gActorRouteDirectionPolarity only.
 *
 * Per-slot algorithm:
 *   1. Read 4-byte queue record (span, polarity_byte, sub_lane).
 *   2. Compute strip-record lane_count = strip[queue.span].byte3 & 0xF.
 *   3. If queue.sub_lane < lane_count: NORMAL path (route selector = 0).
 *      Else: REMAP path (route selector = 1) — search junction-remap table
 *      and adjust span/sub_lane.
 *   4. Common: place actor, build yaw, ResetVehicleActorState,
 *      RefreshActorTrackProgressOffset (NORMAL) or ComputeTrackSpanProgress
 *      + ComputeSignedTrackOffset (REMAP), NormalizeActorTrackWrapState.
 *   5. Advance queue pointer 4 bytes.
 *
 * Divergences from existing port v0 (replaced 2026-05-14):
 *   - Adds ResetVehicleActorState + NormalizeActorTrackWrapState calls per
 *     original behavior (v0 skipped these).
 *   - Inline switch geometry matches ComputeActorTrackHeading @ 0x00435CE0
 *     (REMAP path) and an inline AngleFromVector12Full (NORMAL path).
 *   - REMAP path now seeds RS_TRACK_PROGRESS + RS_TRACK_OFFSET_BIAS via
 *     ComputeTrackSpanProgress + ComputeSignedTrackOffset (per original
 *     LAB_00435a96-bd).
 *   - REMAP path sets actor.span_normalized to ORIGINAL queue.span
 *     (post-call, per 0x00435ad1), not the remapped span.
 *   - REMAP path subtracts lane_count from sub_lane (per 0x00435a59 SUB).
 *
 * [ARCH-DIVERGENCE D3] direction-polarity-macro: the original writes polarity
 * at byte 0xFC of route_state slot (= dword index 0x3F =
 * gActorRouteDirectionPolarity). The port previously had
 * RS_DIRECTION_POLARITY = 0x25 (byte 0x94) — an established port-wide macro
 * mismatch. The dual-write defence was removed 2026-05-14 after confirming
 * no original readers of the 0x25 alias. See
 * reference_arch_init_traffic_actors_2026-05-18.md for catalogue.
 */
void td5_ai_init_traffic_actors(void) {
    int local_18;       /* slot counter (original local_18, starts at 6) */
    int local_c;        /* per-slot small constant (slot*4 + 0x10), starts at 0x28 */
    const uint8_t *qp;  /* DAT_004b08b8 queue cursor */
    int racer_count;
    int racer_cap;

    /* 0x00435940-50: gate on g_racerCount > 6 */
    racer_count = g_active_actor_count;
    if (racer_count <= 6)
        return;

    qp = g_traffic_queue_ptr ? g_traffic_queue_ptr : g_traffic_queue_base;
    if (!qp) {
        TD5_LOG_W(LOG_TAG, "init_traffic_actors: g_traffic_queue_ptr is NULL");
        return;
    }

    /* 0x00435975-7e: cap iteration at min(racer_count, 0xc) */
    racer_cap = (racer_count > 12) ? 12 : racer_count;

    /* Initial loop state (0x4359 5b/68/63/70) */
    local_18 = TD5_MAX_RACER_SLOTS;   /* 6 */
    local_c  = 0x28;                  /* 6*4 + 0x10 */

    while (local_18 < racer_cap) {
        char *a = actor_ptr(local_18);
        int32_t *rs = route_state(local_18);
        int16_t queue_span;
        uint8_t queue_byte2;          /* polarity in bit 0 */
        uint8_t queue_byte3;          /* sub_lane */
        int polarity_bit;
        int lane_count;
        int orig_queue_span;          /* preserved for REMAP path post-call */

        queue_span  = (int16_t)(qp[0] | ((uint16_t)qp[1] << 8));
        queue_byte2 = qp[2];
        queue_byte3 = qp[3];
        orig_queue_span = (int)queue_span;
        polarity_bit = (int)queue_byte2 & 1;

        /* 0x435989-9d: common writes — polarity, local_c, RECOVERY_STAGE=0,
         * DAT_004afc50=0. Polarity goes to dword 0x3F (= gActorRouteDirectionPolarity
         * @ 0x004afc5c). Prior port also wrote dword 0x25 (legacy macro) but
         * that field has no references in the original — the dual-write was
         * unnecessary and is now removed.
         * `local_c` (slot*4+0x10) writes the field at byte 0x68 within
         * route_state (dword 0x1A) — original semantics unknown; mirror raw. */
        rs[RS_ROUTE_DIRECTION_POLARITY] = polarity_bit;
        rs[0x1A] = local_c;                         /* DAT_004afbc8[slot] */
        rs[RS_RECOVERY_STAGE] = 0;                  /* dword 0x22 */
        rs[RS_SCRIPT_SPEED_PARAM] = 0;              /* dword 0x3C = DAT_004afc50[slot] */
        g_traffic_recovery_stage[local_18] = 0;     /* port-side mirror */

        /* 0x004359a0-bf: compute lane_count for branching */
        {
            const TD5_StripSpan *strip_base =
                (const TD5_StripSpan *)g_strip_span_base;
            int span_idx = (int)queue_span;
            uint8_t strip_byte3 = 0;

            if (strip_base && span_idx >= 0 && span_idx < g_strip_span_count) {
                strip_byte3 = ((const uint8_t *)&strip_base[span_idx])[3];
            }
            lane_count = (int)(strip_byte3 & 0x0F);
        }

        /* Branch on (lane_count > queue.sub_lane) — JA at 0x004359c1.
         * JA-taken (NORMAL path) means lane_count strictly greater. */
        if (lane_count > (int)queue_byte3) {
            /* ---------- NORMAL path (selector = 0, 0x00435b0d) ---------- */
            const TD5_StripSpan *strip_base =
                (const TD5_StripSpan *)g_strip_span_base;
            const TD5_StripVertex *vpool =
                (const TD5_StripVertex *)g_strip_vertex_base;
            const TD5_StripSpan *sp;
            const int16_t *psVar2;   /* left vertex base shorts */
            const int16_t *psVar3;   /* right vertex base shorts */
            int32_t local_10 = 0;    /* dx component */
            int32_t local_14 = 0;    /* dz component */
            int has_geom = 0;
            int angle_full;
            int32_t yaw_stored;

            rs[RS_ROUTE_TABLE_SELECTOR] = 0;

            /* 0x435b10-2b: actor.span_raw = queue.span; actor.sub_lane = queue.byte3 */
            ACTOR_I16(a, ACTOR_SPAN_RAW) = queue_span;
            ACTOR_U8(a, ACTOR_SUB_LANE_INDEX) = queue_byte3;

            /* 0x435b2e: InitActorTrackSegmentPlacement(&actor.span_raw, &actor.world_pos).
             * Byte-faithful port @ 0x00445F10. Seeds:
             *   actor+0x84 (span_accum) = span_raw
             *   actor+0x86 (span_high)  = span_raw
             *   actor+0x8C (sub_lane)   = clamp if >= lane_nibble
             *   actor+0x1FC/+0x200/+0x204 = 4-vertex barycenter (24.8 FP). */
            td5_track_init_actor_segment_placement(
                (int16_t *)(a + ACTOR_SPAN_RAW),
                (int32_t *)(a + ACTOR_WORLD_POS_X));

            /* 0x435b33-72: load strip[span] + first/second vertex pointers */
            sp = NULL;
            psVar2 = psVar3 = NULL;
            if (strip_base && vpool &&
                queue_span >= 0 && queue_span < g_strip_span_count) {
                sp = &strip_base[queue_span];
                psVar2 = (const int16_t *)&vpool[sp->left_vertex_index];
                psVar3 = (const int16_t *)&vpool[sp->right_vertex_index];
                has_geom = 1;
            }

            /* Switch on strip type byte 0 (sp->span_type).
             * Original disassembles to a jump table at 0x00435cb8 covering
             * type-1 in [0,6]; type 0 or type > 7 falls through to the
             * default which leaves local_10/local_14 as their stack values.
             * For first iteration these are uninitialized. To produce
             * deterministic-port behavior on default-type spans, leave both
             * components at 0 (since stack residue is non-deterministic). */
            if (has_geom) {
                switch (sp->span_type) {
                case 1: case 2: case 5: {
                    int32_t dx, dz_part;
                    dx = ((int32_t)psVar2[3] - (int32_t)psVar3[3])
                       - (int32_t)psVar3[0] + (int32_t)psVar2[0];
                    local_10 = (dx + ((dx >> 31) & 3)) >> 2;
                    dz_part = ((int32_t)psVar2[5] - (int32_t)psVar3[5])
                            - (int32_t)psVar3[2];
                    {
                        int32_t dz = dz_part + (int32_t)psVar2[2];
                        local_14 = (dz + ((dz >> 31) & 3)) >> 2;
                    }
                    break;
                }
                case 3: case 4: {
                    int32_t dx, dz_part;
                    dx = ((int32_t)psVar2[3] - (int32_t)psVar3[6])
                       - (int32_t)psVar3[3] + (int32_t)psVar2[0];
                    local_10 = (dx + ((dx >> 31) & 3)) >> 2;
                    dz_part = ((int32_t)psVar2[5] - (int32_t)psVar3[8])
                            - (int32_t)psVar3[5];
                    {
                        int32_t dz = dz_part + (int32_t)psVar2[2];
                        local_14 = (dz + ((dz >> 31) & 3)) >> 2;
                    }
                    break;
                }
                case 6: case 7: {
                    int32_t dx, dz;
                    dx = ((int32_t)psVar2[6] - (int32_t)psVar3[3])
                       + (int32_t)psVar2[3] - (int32_t)psVar3[0];
                    local_10 = (dx + ((dx >> 31) & 3)) >> 2;
                    dz = ((int32_t)psVar2[8] - (int32_t)psVar3[5])
                       - (int32_t)psVar3[2] + (int32_t)psVar2[5];
                    local_14 = (dz + ((dz >> 31) & 3)) >> 2;
                    break;
                }
                default:
                    /* Original default: skip recompute, retain stack values.
                     * Port treats as 0/0 for determinism. */
                    break;
                }
            }

            /* 0x435c37-44: yaw = (AngleFromVector12Full(dx, dz) + 0x800) << 8 */
            angle_full = ai_angle_from_vector(local_10, local_14);
            yaw_stored = (angle_full + 0x800) << 8;
            ACTOR_I32(a, ACTOR_YAW_ACCUM) = yaw_stored;

            /* 0x435c47-58: polarity flip — add 0x80000 */
            if (polarity_bit) {
                yaw_stored += 0x80000;
                ACTOR_I32(a, ACTOR_YAW_ACCUM) = yaw_stored;
            }

            /* 0x435c5c-60: ResetVehicleActorState(actor) */
            td5_physics_reset_actor_state((TD5_Actor *)a);

            /* 0x435c69-6a: RefreshActorTrackProgressOffset(slot) */
            td5_ai_seed_actor_track_progress_offset(local_18);

            /* 0x435c6f-70: NormalizeActorTrackWrapState(actor) */
            td5_track_normalize_actor_wrap((TD5_Actor *)a);
        }
        else {
            /* ---------- REMAP path (selector = 1, 0x004359c7) ----------
             *
             * Original walks the junction-remap table at DAT_004c3da0+0x18,
             * matching queue.span into [range_lo, range_lo + (B - A) - 1].
             * On match: remapped = (A - C) + queue.span. On miss: -1.
             *
             * Port reuses td5_track_apply_target_span_remap which performs
             * the same walker but returns the input span (not -1) on miss.
             * [ARCH-DIVERGENCE D1] queue records whose span is outside every
             * junction range will have remapped_span = queue.span here, vs
             * -1 in original. Local re-check below substitutes -1 to match
             * orig externally. See
             * reference_arch_init_traffic_actors_2026-05-18.md. */
            int remapped_int;
            int16_t remapped_span;

            rs[RS_ROUTE_TABLE_SELECTOR] = 1;

            remapped_int = td5_track_apply_target_span_remap((int)queue_span, 0);

            if (remapped_int == (int)queue_span) {
                /* No remap occurred — treat as miss per original (sentinel). */
                remapped_span = (int16_t)-1;
            } else {
                remapped_span = (int16_t)remapped_int;
            }

            /* 0x00435a2b-5b: actor.span_raw = remapped; actor.sub_lane =
             * queue.byte3 - lane_count. */
            ACTOR_I16(a, ACTOR_SPAN_RAW) = remapped_span;
            ACTOR_U8(a, ACTOR_SUB_LANE_INDEX) = (uint8_t)((int)queue_byte3 - lane_count);

            /* 0x00435a5e: InitActorTrackSegmentPlacement(&span_raw, &world_pos).
             * Byte-faithful port @ 0x00445F10 — defensive guard: original has none
             * but our table-driven path can index into NULL span pool. The helper
             * itself early-returns with zeroed out_pos when track isn't bound. */
            td5_track_init_actor_segment_placement(
                (int16_t *)(a + ACTOR_SPAN_RAW),
                (int32_t *)(a + ACTOR_WORLD_POS_X));

            /* 0x00435a67: ComputeActorTrackHeading(actor) → 12-bit angle.
             * Inline the original switch + AngleFromVector12 to avoid the
             * extra +0x290 heading_normal write that td5_track_compute_heading
             * (port of 0x00434350) performs. */
            {
                const TD5_StripSpan *strip_base =
                    (const TD5_StripSpan *)g_strip_span_base;
                const TD5_StripVertex *vpool =
                    (const TD5_StripVertex *)g_strip_vertex_base;
                const TD5_StripSpan *sp = NULL;
                const int16_t *psVar2 = NULL;
                const int16_t *psVar3 = NULL;
                int32_t uVar9 = 0, param_1 = 0;
                int angle12;
                int32_t yaw_stored;
                int rs_span = (int)(int16_t)ACTOR_I16(a, ACTOR_SPAN_RAW);

                if (strip_base && vpool &&
                    rs_span >= 0 && rs_span < g_strip_span_count) {
                    sp = &strip_base[rs_span];
                    psVar2 = (const int16_t *)&vpool[sp->left_vertex_index];
                    psVar3 = (const int16_t *)&vpool[sp->right_vertex_index];

                    switch (sp->span_type) {
                    case 1: case 2: case 5: {
                        int32_t iVar6 = ((int32_t)psVar2[3] - (int32_t)psVar3[3])
                                      - (int32_t)psVar3[0] + (int32_t)psVar2[0];
                        int32_t iVar5 = (int32_t)psVar2[5] - (int32_t)psVar3[5]
                                      - (int32_t)psVar3[2] + (int32_t)psVar2[2];
                        iVar6 = iVar6 + ((iVar6 >> 31) & 3);
                        param_1 = (iVar5 + ((iVar5 >> 31) & 3)) >> 2;
                        uVar9 = iVar6 >> 2;
                        break;
                    }
                    case 3: case 4: {
                        int32_t iVar6 = ((int32_t)psVar2[3] - (int32_t)psVar3[6])
                                      - (int32_t)psVar3[3] + (int32_t)psVar2[0];
                        int32_t iVar5 = (int32_t)psVar2[5] - (int32_t)psVar3[8]
                                      - (int32_t)psVar3[5] + (int32_t)psVar2[2];
                        iVar6 = iVar6 + ((iVar6 >> 31) & 3);
                        param_1 = (iVar5 + ((iVar5 >> 31) & 3)) >> 2;
                        uVar9 = iVar6 >> 2;
                        break;
                    }
                    case 6: case 7: {
                        int32_t iVar6 = ((int32_t)psVar2[6] - (int32_t)psVar3[3])
                                      + (int32_t)psVar2[3] - (int32_t)psVar3[0];
                        int32_t iVar5 = ((int32_t)psVar2[8] - (int32_t)psVar3[5])
                                      - (int32_t)psVar3[2] + (int32_t)psVar2[5];
                        param_1 = (iVar5 + ((iVar5 >> 31) & 3)) >> 2;
                        uVar9 = (iVar6 + ((iVar6 >> 31) & 3)) >> 2;
                        break;
                    }
                    default:
                        /* default leaves uVar9 = param_1 = 0 (initial). */
                        break;
                    }
                }

                /* 0x00435dd1-44: quadrant-fold dispatch onto AngleFromVector12.
                 * Use the existing port helper ai_angle_from_vector for the
                 * full-circle equivalent. */
                angle12 = ai_angle_from_vector(uVar9, param_1);
                yaw_stored = (angle12 + 0x800) << 8;
                ACTOR_I32(a, ACTOR_YAW_ACCUM) = yaw_stored;

                if (polarity_bit) {
                    yaw_stored += 0x80000;
                    ACTOR_I32(a, ACTOR_YAW_ACCUM) = yaw_stored;
                }
            }

            /* 0x00435a91: ResetVehicleActorState(actor) */
            td5_physics_reset_actor_state((TD5_Actor *)a);

            /* 0x00435a96-b8: ComputeTrackSpanProgress + ComputeSignedTrackOffset
             * — seed RS_TRACK_PROGRESS and RS_TRACK_OFFSET_BIAS. The original
             * indexes the route table by actor.span_normalized (which at this
             * point is still zero/uninit); the port helper does likewise. */
            {
                int span_raw = (int)(int16_t)ACTOR_I16(a, ACTOR_SPAN_RAW);
                int span_norm = (int)(int16_t)ACTOR_I16(a, ACTOR_SPAN_NORMALIZED);
                int32_t pos[3];
                int64_t prog64;
                int32_t progress;
                int route_byte = 0;
                pos[0] = ACTOR_I32(a, ACTOR_WORLD_POS_X);
                pos[1] = *(int32_t *)(a + 0x200);
                pos[2] = ACTOR_I32(a, ACTOR_WORLD_POS_Z);
                prog64 = td5_track_compute_span_progress(span_raw, pos);
                progress = (int32_t)(uint32_t)(prog64 & 0xFFFFFFFF);
                rs[RS_TRACK_PROGRESS] = progress;

                {
                    const uint8_t *rb =
                        (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
                    if (rb && span_norm >= 0) {
                        route_byte = (int)rb[(size_t)(unsigned)span_norm * 3u];
                    }
                }
                rs[RS_TRACK_OFFSET_BIAS] =
                    td5_track_compute_signed_offset(span_raw, progress, route_byte);
            }

            /* 0x00435abe-c1: NormalizeActorTrackWrapState(actor) */
            td5_track_normalize_actor_wrap((TD5_Actor *)a);

            /* 0x00435ac6-d1: actor.span_normalized = ORIGINAL queue.span
             * (the un-remapped one), as final write. */
            *(int16_t *)(a + 0x082) = (int16_t)orig_queue_span;
        }

        /* ---------- common trailing ops (port-side bookkeeping) ----------
         * [ARCH-DIVERGENCE D2] The original 0x00435940 does NOT zero
         * steering_cmd / vehicle_mode / etc — those resets happen in
         * ResetVehicleActorState. We also mirror the slot index here for
         * AI dispatcher consumption (port-only). Orig achieves equivalent
         * state via EBX address-mode addressing. See
         * reference_arch_init_traffic_actors_2026-05-18.md. */
        ACTOR_U8(a, ACTOR_SLOT_INDEX) = (uint8_t)local_18;
        rs[RS_SLOT_INDEX] = local_18;
        rs[RS_ENCOUNTER_HANDLE] = -1;

        TD5_LOG_I(LOG_TAG,
                  "init_traffic: slot=%d span=%d remapped=%d lane=%d "
                  "polarity=%d sel=%d pos=(%d,%d,%d)",
                  local_18, orig_queue_span,
                  (int)(int16_t)ACTOR_I16(a, ACTOR_SPAN_RAW),
                  (int)ACTOR_U8(a, ACTOR_SUB_LANE_INDEX),
                  polarity_bit, (int)rs[RS_ROUTE_TABLE_SELECTOR],
                  ACTOR_I32(a, ACTOR_WORLD_POS_X),
                  *(int32_t *)(a + 0x200),
                  ACTOR_I32(a, ACTOR_WORLD_POS_Z));

        /* 0x00435c8b-ca5: advance queue pointer, slot, local_c, racer_cap reload */
        qp += 4;
        local_18 += 1;
        local_c  += 4;

        /* 0x00435c85-78: reload g_racerCount each iteration (in case it
         * changed; mirror original even if our port doesn't mutate it
         * mid-loop). */
        racer_count = g_active_actor_count;
        racer_cap = (racer_count > 12) ? 12 : racer_count;
    }

    g_traffic_queue_ptr = qp;
}

/**
 * UpdateTrafficRoutePlan: 7-stage FSM per traffic slot.
 *
 * Stage 1 (recycle):   Call RecycleTrafficActorFromQueue
 * Stage 2 (heading):   Heading misalignment check -> enter recovery if > 90 deg
 * Stage 3 (edge):      Edge-of-track / recovery bail-out -> brake and return
 * Stage 4 (normal):    Set constant speed (encounter_steering_override = 0x3C)
 * Stage 5 (target):    Compute next target span (accounting for direction polarity)
 * Stage 6 (steer):     Call UpdateActorSteeringBias with weight 0x8000
 * Stage 7 (yield):     FindNearestRoutePeer -> brake if closing on peer
 *
 * [precise-00435E80] Byte-faithful port from 0x00435E80 disassembly listing.
 *
 * [CONFIRMED @ 0x00435E80] L5 promotion sweep audit (2026-05-18).
 *   - Stages 1-7 enumerated above match orig FSM block layout
 *     (0x00435E80..0x00436A6F, 2142 bytes / 521 instructions).
 *   - Stage 2 ref_slot via rs[RS_SLOT_INDEX], not param_1 — CONFIRMED at
 *     0x00435EA6-0x00435FA8 EBX+0xD4 dispatch.
 *   - Stage 2 polarity reads RS_ROUTE_DIRECTION_POLARITY (dword 0x3F),
 *     not the prior incorrectly-aliased 0x25 — CONFIRMED at 0x00435EF4.
 *   - Stage 2 strict hdelta band (0x400, 0xC00) — CONFIRMED at
 *     0x00435F2B JLE / 0x00435F32 JGE pair.
 *   - Stage 2 special-encounter audio cleanup asymmetric — orig only
 *     clears g_encounter_tracked_handle on polarity==0 path.
 *   - Stage 5 target span calculation accounts for polarity sign flip.
 *   - Stage 7 yield brake — FindNearestRoutePeer dispatch verified.
 *
 * KNOWN DIVERGENCES: none documented (port commits document the four
 * shipped fixes vs prior approximate port).
 *
 * Effective level: L5 (byte-faithful end-to-end).
 *
 * Notable fixes vs prior approximate port:
 *   - Stage 2 reads heading/yaw_accum from the REFERENCE actor
 *     (actor_ptr(rs[RS_SLOT_INDEX])), not from the param_1 actor. Original
 *     dereferences EBX+0xD4 = rs[RS_SLOT_INDEX] before every per-actor read.
 *   - Stage 2 random-recovery seed reads actor[0].world_pos_z (DAT_004ab30c),
 *     not the local g_ai_frame_counter. Same source the original uses.
 *   - Stage 2 special-encounter cleanup: slot==9 + !wanted_mode in
 *     polarity==0 path stops audio AND clears handle. polarity!=0 path
 *     stops audio only — DOES NOT clear the handle. Original asymmetric.
 *   - Stage 3 bail-out checks rs[RS_SCRIPT_SPEED_PARAM] (DAT_004afc50) and
 *     g_traffic_recovery_stage[slot] (DAT_004afbe8). Prior port duplicated
 *     the recovery check and never read the script-speed-param sentinel.
 *   - Stage 2 hdelta band check uses strict bounds (0x400, 0xC00) per the
 *     JLE/JGE pair at 0x435F2B/0x435F32, not >= and <=.
 */
void td5_ai_update_traffic_route_plan(int slot) {
    int32_t *rs = route_state(slot);
    char *actor  = actor_ptr(slot);
    int  ref_slot;
    char *ref_actor;
    int32_t heading_shifted, route_byte_val, route_heading_shifted;
    int32_t hdelta, hdelta_neg;
    int polarity, peer;

    /* --- Stage 1: Recycle --- */
    td5_ai_recycle_traffic_actor();

    /* --- Stage 2: Heading misalignment check ---
     * [CONFIRMED @ 0x00435EA6-0x00435FA8]
     * The original reads ref_slot = rs[RS_SLOT_INDEX] (EBX+0xD4), then
     * loads span_normalized + yaw_accum from actor_ptr(ref_slot). For traffic
     * ref_slot == param_1 so behaviour is the same, but the port follows the
     * original dispatch path. */
    ref_slot = rs[RS_SLOT_INDEX];
    if (ref_slot < 0 || ref_slot >= TD5_MAX_TOTAL_ACTORS) ref_slot = slot;
    ref_actor = actor_ptr(ref_slot);

    {
        const uint8_t *rb = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
        int16_t ref_span_norm = ACTOR_I16(ref_actor, ACTOR_SPAN_NORMALIZED);
        if (rb && ref_span_norm >= 0) {
            route_byte_val = (int32_t)rb[(size_t)(unsigned)ref_span_norm * 3u + 1u];
        } else {
            route_byte_val = 0;
        }
    }
    heading_shifted       = ACTOR_I32(ref_actor, ACTOR_YAW_ACCUM) >> 8;
    /* Original sequence at 0x435ED8-0x435EF2 computes
     *   tmp = route_byte * 0x102C
     *   tmp = (tmp + (tmp >> 31 & 0xFF)) >> 8
     * which is signed div-by-256 with arithmetic rounding toward zero. */
    {
        int32_t tmp = route_byte_val * 0x102C;
        route_heading_shifted = (tmp + ((tmp >> 31) & 0xFF)) >> 8;
    }
    /* Polarity = gActorRouteDirectionPolarity (dword 0x3F) per
     * UpdateTrafficRoutePlan @ 0x00435ef4. Prior port read dword 0x25 which
     * has no references in the original — the polarity branch was reading
     * a zero-init field that was never written. */
    polarity = rs[RS_ROUTE_DIRECTION_POLARITY];

    /* See 0x435EFD-0x435F18:
     *   ECX = (heading - route_heading) - 0x800           ; (signed)
     *   ECX = ECX & 0xFFF
     *   EAX = (ECX + 0xFFFFF800) & 0xFFF  ; = (ECX - 0x800) & 0xFFF
     *   EAX = (-EAX) & 0xFFF
     * which is the "negated heading delta" used in the polarity==0 test. */
    {
        uint32_t ecx = ((uint32_t)(heading_shifted - route_heading_shifted)) - 0x800u;
        uint32_t eax;
        ecx &= 0xFFFu;
        eax = (ecx - 0x800u) & 0xFFFu;
        eax = ((uint32_t)(-(int32_t)eax)) & 0xFFFu;
        hdelta_neg = (int32_t)eax;
    }

    if (polarity == 0) {
        hdelta = hdelta_neg;
    } else {
        /* 0x435F69: polarity != 0 path subtracts another 0x800 (= flip 180 deg). */
        hdelta = (int32_t)(((uint32_t)hdelta_neg - 0x800u) & 0xFFFu);
    }

    /* Original 0x435F2B JLE 0x400 + 0x435F32 JGE 0xC00 implement strict bounds:
     * body fires for hdelta in (0x400, 0xC00) exclusive. */
    if (hdelta > 0x400 && hdelta < 0xC00) {
        /* Original at 0x435F34/0x435F81: reads DAT_004ab30c = actor[0].world_pos_z
         * (g_actor_base + 0x204). Used as a pseudo-random source for the
         * recovery stage. */
        int32_t seed_src = 0;
        if (g_actor_base) {
            seed_src = ACTOR_I32(actor_ptr(0), ACTOR_WORLD_POS_Z);
        }
        g_traffic_recovery_stage[slot] = (int)(seed_src & 7);
        if (g_traffic_recovery_stage[slot] == 0)
            g_traffic_recovery_stage[slot] = 1;
        /* Slot 9 special-encounter audio cleanup. */
        if (slot == 9) {
            if (polarity == 0) {
                if (!g_td5.wanted_mode_enabled) {
                    /* StopTrackedVehicleAudio @ 0x00440AE0 */
                    td5_sound_stop_tracked_vehicle_audio();
                }
                /* 0x435F5D: only the polarity==0 branch clears the handle. */
                g_encounter_tracked_handle = -1;
            } else {
                if (!g_td5.wanted_mode_enabled) {
                    /* StopTrackedVehicleAudio — handle NOT cleared in this branch. */
                    td5_sound_stop_tracked_vehicle_audio();
                }
            }
        }
    }

    /* --- Stage 3: Edge-of-track / recovery bail-out ---
     * [CONFIRMED @ 0x00435FAA-0x00436019]
     * Original condition (Ghidra):
     *   sVar5 = field_0x80 (ACTOR_SPAN_RAW)   (param_1 actor)
     *   bail if: ((sVar5 < 3 || g_trackTotalSpanCount - 8 <= sVar5) &&
     *             rs[RS_ROUTE_TABLE_SELECTOR] == 0)
     *         || rs[RS_SCRIPT_SPEED_PARAM] != 0   (DAT_004afc50)
     *         || g_traffic_recovery_stage[slot] != 0   (DAT_004afbe8) */
    {
        int16_t span_raw = ACTOR_I16(actor, ACTOR_SPAN_RAW);
        int ring_length = td5_track_get_ring_length();
        int near_edge = (span_raw < 3 || (ring_length > 0 && span_raw >= ring_length - 8));
        int on_canonical = (rs[RS_ROUTE_TABLE_SELECTOR] == 0);

        if ((near_edge && on_canonical)
            || rs[RS_SCRIPT_SPEED_PARAM] != 0
            || g_traffic_recovery_stage[slot] != 0) {
            /* LAB_004366c7: brake = 1, encounter_steer = 0, return */
            ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 1;
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0;
            return;
        }
    }

    /* --- Stage 4: Normal driving -- constant speed 0x3C (60) --- */
    ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0x3C;
    ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 0;

    /* --- Stage 5: Compute target span and deviation
     * [CONFIRMED @ 0x00435F0A-0x004364CA]
     *
     * Original UpdateTrafficRoutePlan computes LEFT/RIGHT deviation to the
     * next-span waypoint and writes them to route_state +0x58/+0x5c (=
     * RS_LEFT_DEVIATION/RS_RIGHT_DEVIATION). UpdateActorSteeringBias @
     * 0x004340C0 then reads these and writes steering_command (+0x30C).
     *
     * CRITICAL: the original calls InitActorTrackSegmentPlacement @ 0x00445F10
     * for the target point, NOT SampleTrackTargetPoint @ 0x00434800. These are
     * two DIFFERENT geometries:
     *   - InitActorTrackSegmentPlacement: 4-vertex barycenter of the sub_lane
     *     cell at (target_span, sub_lane_index). Uses DAT_00474E40 (spawn
     *     table). Traffic uses this. Port equivalent: td5_track_get_span_lane_world.
     *   - SampleTrackTargetPoint: route_byte interpolation between left rail
     *     and left+lane_count vertex of the whole span. Uses DAT_00473C68 (AI
     *     target table). AI racers use this. Port equivalent: td5_track_sample_target_point.
     *
     * The prior port routed traffic through sample_target_point, so traffic
     * aimed at the road center instead of its assigned sub-lane. On multi-lane
     * Moscow segments this pulled the chasing target into the opposite lane,
     * which drives the bicycle-model steering_command toward the neighbouring
     * rail and clips walls. The original also does NOT apply RS_TRACK_OFFSET_BIAS
     * (peer-avoidance perpendicular offset) to the traffic target — that is a
     * racer-only behaviour inside 0x00434800.
     *
     * Traffic peek-ahead is span+1 forward / span-1 reverse (racer uses span+4).
     *
     * BUG FIXED (2026-04-25): The original uses field_0x82 (ACTOR_SPAN_NORMALIZED)
     * for the jump-table lookup, NOT field_0x80 (ACTOR_SPAN_RAW). On branch roads
     * the raw span holds the branch index (>= ring_length) while the normalized
     * span is the main-road equivalent — using raw caused the remap walker to miss
     * every entry (branch indices are always outside the main-road range_lo values).
     * [CONFIRMED @ 0x00435F6A: Ghidra "iVar14 = field_0x82; iVar8 = iVar14 + 1"]
     *
     * BUG FIXED (2026-04-25): When the remap walker finds a branch match AND the
     * actor's raw span (field_0x80) is still on main road (< ring_length) AND the
     * branch target span is also on main road (<= ring_length), the original adjusts
     * sub_lane by (current_sub_lane - cur_span_lane_count). This accounts for the
     * lane-count difference between the junction span and the branch entry span so
     * that the traffic actor aims at the correct sub_lane cell.
     * [CONFIRMED @ 0x004361C0-0x004361D8: Ghidra "local_4 -= uVar6" path]
     *
     * Sub_lane ±1 adjustments at 0x004361B8/0x0043627C only fire on lane-count
     * mismatches between adjacent strips, which are rare. */
    {
        int16_t span_norm = ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED); /* field_0x82 */
        int16_t span_raw  = ACTOR_I16(actor, ACTOR_SPAN_RAW);        /* field_0x80 */
        int span_count = td5_track_get_span_count();
        int ring_length = td5_track_get_ring_length();
        if (span_count > 0 && span_norm >= 0) {
            int lin_span;
            if (polarity == 0) {
                /* [CONFIRMED @ 0x00435F6A] iVar14=field_0x82; iVar8 = iVar14 + 1 */
                lin_span = ((int)span_norm + 1) % span_count;
            } else {
                lin_span = (int)span_norm - 1;
                if (lin_span < 0) lin_span += span_count;
            }

            int is_canonical = (rs[RS_ROUTE_TABLE_SELECTOR] == 0);
            int target_span = td5_track_apply_target_span_remap(lin_span,
                                                                 is_canonical);

            /* Target sub_lane: start with current sub_lane (default path). */
            int target_sub_lane = (int)ACTOR_U8(actor, ACTOR_SUB_LANE_INDEX);

            /* [CONFIRMED @ 0x004361B8-0x004361D8]
             * When the remap found a branch target AND the actor is still on the
             * main road (raw < ring_length) AND the target is also within main road
             * bounds (target <= ring_length), adjust sub_lane by subtracting the
             * current span's lane count. This shifts the target cell to match
             * the branch entry sub_lane index.
             * Condition "iVar7 <= g_trackTotalSpanCount" in Ghidra uses <=, meaning
             * the branch target must be <= ring_length (inclusive, since ring_length
             * is the first branch index). */
            if (target_span != lin_span &&          /* remap actually fired */
                (int)span_raw < ring_length &&       /* actor on main road */
                target_span <= ring_length) {        /* target on/at branch edge */
                int cur_lane_count = td5_track_get_span_lane_count((int)span_norm);
                if (cur_lane_count > 0 && target_sub_lane >= cur_lane_count) {
                    target_sub_lane -= cur_lane_count;
                }
            }

            int target_x = 0, target_y = 0, target_z = 0;

            if (td5_track_get_span_lane_world(target_span, target_sub_lane,
                                              &target_x, &target_y, &target_z)) {
                /* [CONFIRMED @ 0x00436344-0x004363EF] Original re-reads
                 *   EAX = rs[RS_SLOT_INDEX] (= ref_slot)
                 *   ESI = ref_actor.world_pos_x   (offset 0x1FC, DAT_004ab304)
                 *   EAX = ref_actor.world_pos_z   (offset 0x204, DAT_004ab30c)
                 *   EBP = &ref_actor              (DAT_004ab108 + ref_slot*0x388)
                 * before computing target_angle and combined heading.
                 * For traffic ref_slot == slot, but follow the original
                 * dispatch verbatim. */
                int32_t actor_x = ACTOR_I32(ref_actor, ACTOR_WORLD_POS_X);
                int32_t actor_z = ACTOR_I32(ref_actor, ACTOR_WORLD_POS_Z);
                int32_t dx = (target_x - actor_x) >> 8;
                int32_t dz = (target_z - actor_z) >> 8;

                int32_t target_angle = ai_angle_from_vector(dx, dz) & 0xFFF;

                int32_t yaw   = ACTOR_I32(ref_actor, ACTOR_YAW_ACCUM);
                int32_t steer = ACTOR_I32(ref_actor, ACTOR_STEERING_CMD);
                int32_t actor_heading = ((yaw + steer) >> 8) & 0xFFF;

                int32_t delta = actor_heading - target_angle;
                int32_t abs_delta = delta < 0 ? -delta : delta;
                if (delta >= 0) {
                    rs[RS_LEFT_DEVIATION]  = 0xFFF - abs_delta;
                    rs[RS_RIGHT_DEVIATION] = delta;
                } else {
                    rs[RS_LEFT_DEVIATION]  = abs_delta;
                    rs[RS_RIGHT_DEVIATION] = delta + 0xFFF;
                }

                if ((g_ai_frame_counter % 60u) == 0u) {
                    TD5_LOG_I(LOG_TAG,
                              "traffic_dev: slot=%d raw=%d norm=%d tspan=%d sublane=%d "
                              "ta=0x%X hd=0x%X delta=%d L=%d R=%d",
                              slot, (int)span_raw, (int)span_norm, target_span,
                              target_sub_lane, target_angle, actor_heading, delta,
                              rs[RS_LEFT_DEVIATION], rs[RS_RIGHT_DEVIATION]);
                }
            }
        }
    }

    /* --- Stage 6: Steering --- */
    td5_ai_pilot_004340C0_set_callsite("traffic_plan");
    td5_ai_update_steering_bias(rs, 0x8000);

    /* --- Stage 7: Peer avoidance / yield --- */
    peer = td5_ai_find_nearest_route_peer(rs);

    if (peer != slot) {
        /* Peer found — faithful TTC formula [CONFIRMED @ 0x4364E0–0x43656E] */
        char *peer_actor = actor_ptr(peer);
        int16_t self_span = ACTOR_I16(actor, ACTOR_SPAN_RAW);
        int16_t peer_span = ACTOR_I16(peer_actor, ACTOR_SPAN_RAW);
        int32_t self_speed, peer_speed, speed_delta;
        int32_t span_diff_dir; /* direction-adjusted span difference (raw spans) */
        int32_t iVar14;        /* combined progress delta: (spans * 0x100) */
        int32_t iVar7, iVar13; /* intermediate TTC and final TTC */
        int32_t speed_shifted; /* self_speed >> 10 with arithmetic rounding */

        self_speed  = g_actor_forward_track_component[slot];
        peer_speed  = g_actor_forward_track_component[peer];
        speed_delta = self_speed - peer_speed; /* iVar8 in original */

        /* Direction-adjusted span difference */
        if (polarity == 0) {
            span_diff_dir = (int32_t)peer_span - (int32_t)self_span;
        } else {
            span_diff_dir = (int32_t)self_span - (int32_t)peer_span;
        }

        /* Proximity gate [CONFIRMED @ 0x43646A]: if peer within 4 spans and moving, brake now */
        if (span_diff_dir > -4 && self_speed > 0) {
            ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 1;
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0xFF00; /* -256 */
            TD5_LOG_I(LOG_TAG, "ttc_brake: slot=%d proximity gate span_diff=%d", slot, span_diff_dir);
            goto ttc_done;
        }

        /* Combined progress delta (sub_progress approximated as 0) */
        iVar14 = span_diff_dir * 0x100;

        /* TTC formula [CONFIRMED @ 0x4364EE]:
         * iVar7 = ((iVar14 * peer_speed * 0x5DC) / speed_delta + iVar14 * 0x5DC) / self_speed
         * Right-shift by 8 with arithmetic rounding → iVar13 */
        if (speed_delta != 0 && self_speed != 0) {
            int64_t tmp = ((int64_t)iVar14 * peer_speed * 0x5DC) / speed_delta
                        + (int64_t)iVar14 * 0x5DC;
            iVar7  = (int32_t)(tmp / self_speed);
            iVar13 = (iVar7 + ((int32_t)iVar7 >> 31 & 0xFF)) >> 8;
        } else {
            iVar13 = 0x2EE00; /* no closing rate — far away */
        }

        /* self_speed arithmetic right-shift by 10 [CONFIRMED @ 0x436561] */
        speed_shifted = (self_speed + (self_speed >> 31 & 0x3FF)) >> 10;

        /* Brake condition [CONFIRMED @ 0x436561–0x43656E]:
         * ttc - 8 <= speed_shifted  &&  ttc >= 0  &&  self_speed > 0 */
        if (iVar13 - 8 <= speed_shifted && iVar13 > -1 && self_speed > 0) {
            ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 1;
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0xFF00; /* -256 */
            TD5_LOG_I(LOG_TAG, "ttc_brake: slot=%d ttc=%d spd_shifted=%d", slot, iVar13, speed_shifted);
        }
    }
ttc_done:;
}

/* ========================================================================
 * Special Encounter System
 *
 *   0x434DA0  UpdateSpecialTrafficEncounter
 *   0x434BA0  UpdateSpecialEncounterControl
 *
 * Slot 9 promotion: traffic slot 9 is "hijacked" when encounter conditions
 * are met. It switches from traffic AI to full racer AI.
 *
 * Lifecycle: spawn -> approach -> active -> teardown, 300-frame cooldown.
 * ======================================================================== */

/* ------------------------------------------------------------------
 * Byte-faithful port of 0x00434DA0 UpdateSpecialTrafficEncounter.
 *
 * Tracks acquisition and release of the special actor-9 traffic
 * encounter (cop / police chase NPC) in modes gated by DAT_0046320c.
 * This path is distinct from the wanted-mode flag DAT_004aaf68
 * (ENC_WANTED_MODE), which only suppresses the engine-audio
 * Start/Stop calls inside this function.
 *
 * Original-symbol mapping (TD5_d3d.exe @ 0x004XXXXX):
 *   DAT_004b05d8  gSpecialEncounterTrackedActorHandle  → g_encounter_tracked_handle
 *   DAT_004b055c  gSpecialEncounterRouteTable          → s_enc_route_table_ptr
 *   DAT_004b05c0  gSpecialEncounterTrackProgress       → s_enc_track_progress
 *   DAT_004b0580  gSpecialEncounterSignedTrackOffset   → s_enc_signed_track_offset
 *   DAT_004b05e4  encounter phase flag (0=ACQUIRE)     → g_encounter_phase_flag
 *   DAT_004b064c  g_specialEncounterCooldown           → g_encounter_cooldown
 *   DAT_004b0658  teardown secondary flag              → s_enc_teardown_flag
 *   DAT_004b0568  encounter route-table-selector ref   → g_encounter_route_table_selector
 *   DAT_004afbe0  gActorSpecialEncounterActive (RS+0x80, stride 0x11c)
 *                                                       → g_encounter_active[slot]
 *   DAT_004ab18a  actor[0].span_normalized  (+0x82)
 *   DAT_004ab41c  actor[0].longitudinal_speed (+0x314)
 *   DAT_004ad150  actor[9].span_raw         (+0x80)
 *   DAT_004ad152  actor[9].span_normalized  (+0x82)
 *   DAT_004ad2cc  actor[9].world_pos_x      (+0x1fc)  — ComputeTrackSpanProgress reads [x,_,z]
 *   DAT_004ad449  actor[9].vehicle_mode     (+0x379)
 *   DAT_004ad0d0  actor[9] base
 *   DAT_004afbc0  route_state[0][RS_FORWARD_TRACK_COMP] (slot 0)
 *   DAT_004afb6c  route_state[0][RS_ROUTE_TABLE_SELECTOR]
 *   DAT_004afb60  route_state[0][RS_ROUTE_TABLE_PTR]
 *   DAT_004aaf68  ENC_WANTED_MODE
 *
 * The port keeps g_encounter_active[] as a parallel int32 array (this
 * is what the rest of the port reads via td5_ai_is_encounter_active).
 * The original stores it inside the route_state row at RS+0x80; the
 * write effect is preserved logically.
 *
 * g_encounter_enabled is a port-only master gate fed from
 * g_td5.special_encounter_enabled. The original has no equivalent
 * gate at the function entry; the function is unconditionally called
 * by 0x00436A70 UpdateRaceActors, and the encounter system is
 * effectively idle when DAT_0046320c / DAT_004aaf68 are zero (no
 * cop slot configured). Keeping the gate here is a no-op when the
 * port mirrors the original's idle state but lets us hard-disable
 * encounters from frontend config.
 * ------------------------------------------------------------------ */

/* gSpecialEncounterRouteTable: pointer to the route-table byte stream
 * (LEFT.TRK / RIGHT.TRK) of the tracked actor. Refreshed every tick
 * when the tracked actor's RS_ROUTE_TABLE_PTR differs from the cached
 * value. Used as the source of the per-span lateral lane byte read
 * at offset (span_norm * 3). */
static const uint8_t *s_enc_route_table_ptr;

/* gSpecialEncounterTrackProgress: low dword of the ComputeTrackSpanProgress
 * return (longlong). The high dword is discarded by the original. */
static int32_t s_enc_track_progress;

/* gSpecialEncounterSignedTrackOffset: low dword of ComputeSignedTrackOffset
 * (ulonglong) return. */
static int32_t s_enc_signed_track_offset;

/* _DAT_004b0658: secondary teardown flag. Zeroed on the >0x40 despawn
 * path; not read elsewhere in this function. */
static int32_t s_enc_teardown_flag;

/* External symbols mapped from absolute addresses in the disassembly. */
/* DAT_004aaf68 ENC_WANTED_MODE mirror in the port lives at
 * g_td5.wanted_mode_enabled (int). Wrap it for byte-faithful naming. */
#define ENC_WANTED_MODE  (g_td5.wanted_mode_enabled)

/* Audio Start/Stop hooks — wire to ported StartTrackedVehicleAudio
 * (0x00440AB0) / StopTrackedVehicleAudio (0x00440AE0) in td5_sound.c. */
static inline void td5_enc_start_tracked_audio(int arg) {
    TD5_LOG_I(LOG_TAG, "enc_audio_start: arg=%d", arg);
    td5_sound_start_tracked_vehicle_audio(arg);
}

static inline void td5_enc_stop_tracked_audio(void) {
    TD5_LOG_I(LOG_TAG, "enc_audio_stop");
    td5_sound_stop_tracked_vehicle_audio();
}

/* Forward declaration: the deterministic-sim crash-recovery state reset
 * for actor[9]. Original is ResetVehicleActorState @ 0x00405d70.
 * Port: td5_physics.c:td5_physics_reset_actor_state(TD5_Actor *). */
extern void td5_physics_reset_actor_state(TD5_Actor *actor);

/**
 * UpdateSpecialTrafficEncounter — byte-faithful port of 0x00434DA0.
 * Called from UpdateRaceActors (0x00436A70) at end of traffic loop.
 */
void td5_ai_update_special_encounter(void) {
    int32_t handle;
    char *cop;        /* actor[9] */
    char *player;     /* actor[0] */
    int32_t cop_span_norm;     /* DAT_004ad152 — actor[9] +0x82 */
    int16_t player_span_norm;  /* DAT_004ab18a — actor[0] +0x82 */
    int32_t player_speed;      /* DAT_004ab41c — actor[0] +0x314 */
    int32_t player_fwd;        /* DAT_004afbc0 — RS[0].forward_track_component */
    int32_t player_selector;   /* DAT_004afb6c — RS[0].route_table_selector */

    /* Port-only master gate (see header comment). */
    if (!g_encounter_enabled) return;

    /* Bind frequently-used addresses once. */
    if (!g_actor_base) return;
    cop    = actor_ptr(9);
    player = actor_ptr(0);

    cop_span_norm     = (int32_t)(int16_t)ACTOR_I16(cop, ACTOR_SPAN_NORMALIZED);
    player_span_norm  =                    ACTOR_I16(player, ACTOR_SPAN_NORMALIZED);
    player_speed      =                    ACTOR_I32(player, ACTOR_LONGITUDINAL_SPEED);
    player_fwd        = g_actor_forward_track_component[0];
    player_selector   = route_state(0)[RS_ROUTE_TABLE_SELECTOR];

    handle = g_encounter_tracked_handle;

    /* ---- Prologue (0x00434da0–0x00434e11) ----------------------------
     * if (handle != -1 && cached_route_table_ptr !=
     *     route_state[handle][RS_ROUTE_TABLE_PTR]) {
     *     // route changed — refresh cached pointer + recompute
     *     // span progress + signed track offset.
     * }
     * ----------------------------------------------------------------- */
    if (handle != -1) {
        const uint8_t *cur_rb;
        const int32_t *handle_rs = route_state(handle);
        cur_rb = (const uint8_t *)(intptr_t)handle_rs[RS_ROUTE_TABLE_PTR];
        if (s_enc_route_table_ptr != cur_rb) {
            int32_t cop_span_raw = (int32_t)(int16_t)ACTOR_I16(cop, ACTOR_SPAN_RAW);
            int32_t *cop_xyz     = (int32_t *)(cop + ACTOR_WORLD_POS_X);
            uint8_t lane_byte;

            s_enc_route_table_ptr = cur_rb;

            /* longlong ComputeTrackSpanProgress(int span, int *xyz_at_+0x1fc) */
            s_enc_track_progress = (int32_t)td5_track_compute_span_progress(
                cop_span_raw, cop_xyz);

            /* The original passes the per-span lane byte at
             *   route_table[ span_norm * 3 ]
             * i.e. the BL = byte ptr [ECX + EDX*1] where ECX=span_norm,
             * EDX = base + span_norm*2 ⇒ effective addr = base + span_norm*3. */
            lane_byte = (cur_rb && cop_span_norm >= 0)
                ? cur_rb[(size_t)cop_span_norm * 3u]
                : 0;

            /* ulonglong ComputeSignedTrackOffset(int span_raw, int progress, uint lane) */
            s_enc_signed_track_offset = (int32_t)td5_track_compute_signed_offset(
                cop_span_raw, s_enc_track_progress, (int)lane_byte);
        }
    }

    /* ---- Phase-flag dispatch (0x00434e13–0x00434e26) -----------------
     * if (phase_flag != 0) {
     *     if (handle == -1) return;
     *     goto active_monitor;            // 0x00434f1b
     * }
     * if (handle != -1) goto active_monitor;
     * // else: try to spawn the encounter
     * ----------------------------------------------------------------- */
    if (g_encounter_phase_flag != 0) {
        if (handle == -1) return;
        /* fall through to active_monitor */
    } else if (handle == -1) {
        /* ---- Spawn attempt (0x00434e2c–0x00434f11) ------------------ */
        int32_t span_delta_2span;
        int32_t player_diff_abs;

        if (g_encounter_cooldown != 0) return;

        /* (int16)actor[0].span_norm - (int16)actor[9].span_norm == 2 */
        span_delta_2span = (int32_t)(int16_t)player_span_norm - cop_span_norm;
        if (span_delta_2span != 2) return;

        /* actor[0].long_speed > 0x15638 (JLE → fail at 0x15638) */
        if (player_speed <= 0x15638) return;

        /* RS[0].forward_track_component > 0 (JLE → fail at 0) */
        if (player_fwd <= 0) return;

        /* RS[0].route_table_selector == DAT_004b0568 (JNZ → fail) */
        if (player_selector != g_encounter_route_table_selector) return;

        /* The original then performs:
         *   uVar1 = (int16)player_span_norm - (int16)player_span_norm
         *   abs(uVar1) > 0x10 ⇒ return
         * which is always 0, so the check passes unconditionally. Kept
         * verbatim so the byte-faithful trace matches. */
        {
            int32_t tmp = (int32_t)(int16_t)player_span_norm
                        - (int32_t)(int16_t)player_span_norm;
            player_diff_abs = (tmp < 0) ? -tmp : tmp;
            if (player_diff_abs > 0x10) return;
        }

        /* ===== SPAWN =====
         * Original (0x00434e97–0x00434f11):
         *   [0x4b05d8] = 0                       (handle = 0)
         *   call ComputeTrackSpanProgress
         *   [0x4b05c0] = ret_lo                  (track_progress)
         *   call ComputeSignedTrackOffset
         *   [0x4b0580] = ret_lo                  (signed_track_offset)
         *   [0x4b055c] = route_state[0][0]       (cache RS[0].route_table_ptr)
         *   ResetVehicleActorState(&actor[9])
         *   if (ENC_WANTED_MODE == 0) StartTrackedVehicleAudio(9);
         * Note: the prologue refresh above already cached the cop's
         * route table when handle was != -1; on the spawn path the
         * handle was -1, so the cached pointer is stale. The original
         * writes route_state[0][RS_ROUTE_TABLE_PTR] (slot 0 = player).
         */
        {
            int32_t cop_span_raw = (int32_t)(int16_t)ACTOR_I16(cop, ACTOR_SPAN_RAW);
            int32_t *cop_xyz     = (int32_t *)(cop + ACTOR_WORLD_POS_X);
            const uint8_t *rb0;
            uint8_t lane_byte;

            g_encounter_tracked_handle = 0;

            s_enc_track_progress = (int32_t)td5_track_compute_span_progress(
                cop_span_raw, cop_xyz);

            /* IMPORTANT: the original reads the lane byte from the
             * *previous* gSpecialEncounterRouteTable, then immediately
             * overwrites it with route_state[0][RS_ROUTE_TABLE_PTR].
             * (0x434eb8 reads MOV EDX,[0x4b055c] BEFORE 0x434eea writes.)
             * Match exactly. */
            rb0 = s_enc_route_table_ptr;
            lane_byte = (rb0 && cop_span_norm >= 0)
                ? rb0[(size_t)cop_span_norm * 3u]
                : 0;

            s_enc_signed_track_offset = (int32_t)td5_track_compute_signed_offset(
                cop_span_raw, s_enc_track_progress, (int)lane_byte);

            /* Cache RS[0].route_table_ptr (slot 0 = player) into the
             * gSpecialEncounterRouteTable global. Subsequent ticks compare
             * the tracked actor's RS_ROUTE_TABLE_PTR against this cache.
             * Since the tracked actor IS slot 0 after spawn, the prologue
             * "ptr changed" check will be false on the very next tick. */
            s_enc_route_table_ptr = (const uint8_t *)(intptr_t)
                                    route_state(0)[RS_ROUTE_TABLE_PTR];

            td5_physics_reset_actor_state((TD5_Actor *)cop);
        }

        TD5_LOG_I(LOG_TAG,
                  "Encounter spawn: handle=0 player_span_norm=%d cop_span_norm=%d "
                  "speed=%d fwd=%d sel=%d",
                  (int)player_span_norm, cop_span_norm, player_speed,
                  player_fwd, player_selector);

        if (ENC_WANTED_MODE != 0) return;
        td5_enc_start_tracked_audio(9);
        return;
    }

    /* ---- Active monitor (0x00434f1b–0x00434fd3) -----------------------
     * Reached when (handle != -1). Either phase == 0 fall-through or
     * phase != 0 explicit JNZ.
     *
     * if (cooldown != 0) return;
     * cop_span = (int16)actor[handle].span_norm  // +0x82 with stride 0x388
     * delta = cop_span - DAT_004ad152            // DAT_004ad152 = actor[9].span_norm
     *
     * Note: handle is set to 0 at spawn (slot 0 = player), so
     * "actor[handle].span_norm" reads the player's span_norm and
     * delta = player_span_norm - cop_span_norm.
     *
     * if (delta > 0x40 && actor[9].vehicle_mode == 0) {
     *     teardown:
     *       teardown_flag = 0
     *       tracked_handle = -1
     *       phase_flag = 0
     *       cooldown = 300
     *       if (ENC_WANTED_MODE == 0) StopTrackedVehicleAudio();
     *       ResetVehicleActorState(&actor[9]);
     *       return;
     * }
     *
     * // delta <= 0x40 path:
     * ecx = DAT_004ad152 - cop_span (= cop_span_norm - player_span_norm)
     * if (ecx < 2) return;
     * if (route_state[handle][RS_ROUTE_TABLE_SELECTOR] != [0x4b0568]) return;
     * if (actor[9].vehicle_mode != 0) return;
     * route_state[handle][0x20] = 1;             // per-slot encounter active
     * if (ENC_WANTED_MODE == 0) StopTrackedVehicleAudio();
     * ----------------------------------------------------------------- */
    {
        char *tracked_actor;
        int32_t tracked_span_norm;
        int32_t delta;
        uint8_t cop_mode;

        if (g_encounter_cooldown != 0) return;

        tracked_actor     = actor_ptr(handle);
        tracked_span_norm = (int32_t)(int16_t)ACTOR_I16(tracked_actor, ACTOR_SPAN_NORMALIZED);
        delta             = tracked_span_norm - cop_span_norm;
        cop_mode          = ACTOR_U8(cop, ACTOR_VEHICLE_MODE);

        if (delta > 0x40 && cop_mode == 0) {
            /* Teardown — cop is far ahead of player AND cop mode == 0. */
            s_enc_teardown_flag        = 0;
            g_encounter_tracked_handle = -1;
            g_encounter_phase_flag     = 0;
            g_encounter_cooldown       = 0x12c;   /* 300 */

            if (ENC_WANTED_MODE == 0) {
                td5_enc_stop_tracked_audio();
            }

            td5_physics_reset_actor_state((TD5_Actor *)cop);

            TD5_LOG_I(LOG_TAG,
                      "Encounter despawn (delta>0x40): tracked_span=%d cop_span=%d "
                      "delta=%d cooldown=300",
                      tracked_span_norm, cop_span_norm, delta);

            /* The original does NOT clear per-slot g_encounter_active
             * flags or zero the control latches here — UpdateSpecial
             * EncounterControl (0x00434BA0) handles deactivation when
             * it next runs and finds the conditions invalid. */
            return;
        }

        /* delta <= 0x40 path — encounter activation check. */
        {
            int32_t ecx;
            const int32_t *tracked_rs;

            ecx = cop_span_norm - tracked_span_norm;
            if (ecx < 2) return;

            tracked_rs = route_state(handle);
            if (tracked_rs[RS_ROUTE_TABLE_SELECTOR] !=
                g_encounter_route_table_selector) return;

            if (cop_mode != 0) return;

            /* route_state[handle][0x20] = 1.
             * Mirror to the parallel g_encounter_active[] used by the
             * rest of the port. handle is guaranteed in [0, TD5_MAX_TOTAL_ACTORS). */
            g_encounter_active[handle] = 1;

            if (ENC_WANTED_MODE == 0) {
                td5_enc_stop_tracked_audio();
            }

            TD5_LOG_I(LOG_TAG,
                      "Encounter activate: handle=%d ecx=%d tracked_span=%d cop_span=%d",
                      (int)handle, ecx, tracked_span_norm, cop_span_norm);
        }
    }
}

/**
 * UpdateSpecialEncounterControl  (0x00434BA0)
 *
 * Byte-faithful port of the original __cdecl function at 0x00434BA0.
 *
 * Two yaw-delta computations:
 *   Block 1: uses gActorTrackReferenceSlot[slot*0x47] which == slot (set
 *            by InitializeRaceActorRuntime via rs[RS_SLOT_INDEX] = i), so
 *            it reads YAW/SPAN_NORMALIZED from actor_ptr(slot) and the
 *            route table from rs[RS_ROUTE_TABLE_PTR] of the slot.
 *   Block 2: uses g_specialEncounterTrackedActorHandle (typically the
 *            player target, slot 0) and the cached gSpecialEncounterRouteTable
 *            pointer (updated each tick by UpdateSpecialTrafficEncounter).
 *
 * Each angle delta is folded to a 12-bit modular value via the orig idiom:
 *     d = (((diff - 0x800) & 0xFFF) - 0x800) & 0xFFF
 *     uVarN = (-d) & 0xFFF
 * The 0x400 < uVarN <= 0xC00 alignment band is symmetric around the route
 * heading: any angle inside the band counts as "facing the wrong way" and
 * forces teardown.
 *
 * Fast-path writes target actor_ptr(slot), NOT actor_ptr(9). DAT_004ad152
 * is the player's SPAN_NORMALIZED snapshot — it gates the brake band.
 */
void td5_ai_update_encounter_control(int slot) {
    char    *actor_self   = actor_ptr(slot);
    int32_t *rs_self      = route_state(slot);
    int      tracked      = g_encounter_tracked_handle;
    /* DAT_004ad152 in orig = player's SPAN_NORMALIZED snapshot; UpdateSpecialTrafficEncounter
     * keeps it equal to the current player's field_0x82 each tick. Port reads it live. */
    int32_t  player_span_ref = (int32_t)(int16_t)ACTOR_I16(actor_ptr(0), ACTOR_SPAN_NORMALIZED);
    /* gSpecialEncounterRouteTable in orig = cached route-table pointer of the tracked actor.
     * Port reads the live equivalent from the tracked actor's rs[RS_ROUTE_TABLE_PTR]. */
    const uint8_t *tracked_route_tbl = (tracked >= 0 && tracked < TD5_MAX_TOTAL_ACTORS)
        ? (const uint8_t *)(intptr_t)route_state(tracked)[RS_ROUTE_TABLE_PTR]
        : NULL;
    const uint8_t *self_route_tbl = (const uint8_t *)(intptr_t)rs_self[RS_ROUTE_TABLE_PTR];

    int32_t fwd_comp;
    uint32_t uVar3, uVar1;

    /* --- Block 1: yaw delta of slot's own actor vs slot's route-table heading --- */
    {
        int32_t yaw_self = ACTOR_I32(actor_self, ACTOR_YAW_ACCUM) >> 8;  /* SAR EAX,8 (signed) */
        int16_t span_self = ACTOR_I16(actor_self, ACTOR_SPAN_NORMALIZED); /* MOVSX [+0x82] */
        uint32_t byte_val = 0;
        int32_t heading_off, diff;

        if (self_route_tbl && span_self >= 0) {
            /* byte at route_tbl + span*3 + 1 (XOR EBX,EBX; MOV BL,[...] = unsigned byte). */
            byte_val = (uint32_t)self_route_tbl[(size_t)(uint16_t)span_self * 3u + 1u];
        }
        /* heading_offset = (byte_val * 0x102C) >>s 8.  byte_val is unsigned [0,255], so
         * the CDQ/AND/ADD/SAR idiom in the orig reduces to a plain >>8 (product is
         * always non-negative). */
        heading_off = (int32_t)(byte_val * 0x102Cu) >> 8;
        diff = yaw_self - heading_off;
        diff = (diff - 0x800) & 0xFFF;
        diff = (diff - 0x800) & 0xFFF;
        uVar3 = ((uint32_t)(-diff)) & 0xFFF;
    }

    /* --- Block 2: yaw delta of tracked actor vs cached gSpecialEncounterRouteTable --- */
    {
        int32_t yaw_tr = 0;
        int16_t span_tr = 0;
        uint32_t byte_val = 0;
        int32_t heading_off, diff;

        if (tracked >= 0 && tracked < TD5_MAX_TOTAL_ACTORS) {
            char *actor_tr = actor_ptr(tracked);
            yaw_tr  = ACTOR_I32(actor_tr, ACTOR_YAW_ACCUM) >> 8;
            span_tr = ACTOR_I16(actor_tr, ACTOR_SPAN_NORMALIZED);
        }
        if (tracked_route_tbl && span_tr >= 0) {
            byte_val = (uint32_t)tracked_route_tbl[(size_t)(uint16_t)span_tr * 3u + 1u];
        }
        heading_off = (int32_t)(byte_val * 0x102Cu) >> 8;
        diff = yaw_tr - heading_off;
        diff = (diff - 0x800) & 0xFFF;
        diff = (diff - 0x800) & 0xFFF;
        uVar1 = ((uint32_t)(-diff)) & 0xFFF;
    }

    fwd_comp = g_actor_forward_track_component[slot];

    /* Set latches (orig writes these unconditionally before the conditional teardown). */
    g_encounter_control_active_latch = 1;
    g_encounter_steer_bias_latch     = 0xFF00; /* word write (MOV [...], DX where DX=0xFF00) */

    /* Teardown if any of:
     *   - fwd_comp <= 8
     *   - 0x400 <= uVar3 <= 0xC00   (range [JL→0x434ccd] ; [JLE→teardown])
     *   - 0x400 <= uVar1 <= 0xC00
     */
    if (fwd_comp <= 8) goto teardown;
    if (!((int32_t)uVar3 < 0x400 || (int32_t)uVar3 > 0xC00)) goto teardown;
    if (!((int32_t)uVar1 < 0x400 || (int32_t)uVar1 > 0xC00)) goto teardown;

    /* Active branch:
     *   if fwd_comp >= gSpecialEncounterMinForwardTrackComponentThreshold (orig DAT_004b05bc)
     *      and  (DAT_004ad152 - actor[slot].SPAN_NORMALIZED) < 3
     *      → BRAKE_FLAG = 1; ENCOUNTER_STEER = 0xFF00
     *   else → ENCOUNTER_STEER = 0  (BRAKE_FLAG untouched)
     */
    {
        int32_t span_self = (int32_t)(int16_t)ACTOR_I16(actor_self, ACTOR_SPAN_NORMALIZED);
        int32_t span_delta = player_span_ref - span_self;
        if (fwd_comp >= g_special_encounter_min_fwd_track_threshold && span_delta < 3) {
            ACTOR_U8(actor_self,  ACTOR_BRAKE_FLAG)      = 1;
            ACTOR_I16(actor_self, ACTOR_ENCOUNTER_STEER) = (int16_t)0xFF00;
            return;
        }
        ACTOR_I16(actor_self, ACTOR_ENCOUNTER_STEER) = 0;
    }
    return;

teardown:
    /* Orig: XOR EAX,EAX
     *       MOV [ESI+0x4afbe0],EAX   (gActorSpecialEncounterActive[slot] = 0)
     *       MOV [ESI+0x4afc5c],EAX   (gActorRouteDirectionPolarity[slot] = 0)
     *       MOV [0x4ad40e],AX        (steer_bias_latch = 0, word)
     *       MOV [0x4ad43d],AL        (control_active_latch = 0, byte)
     *       MOV [0x4b05c4],EAX       (DAT_004b05c4 = 0)
     *       MOV [0x4b05e4],EAX       (DAT_004b05e4 = 0)
     *       MOV [0x4b05d8],-1        (tracked-handle alias; port writes g_encounter_tracked_handle)
     *       MOV [0x4b064c],300       (g_specialEncounterCooldown)
     *       MOV CL,[actor[slot].+0x384]; INC CL; MOV [actor[slot].+0x384],CL  (byte increment)
     *
     * AUDIT(audit-encounter-dats, 2026-05-14): the three unlabeled DATs are
     * fully accounted for by the existing port state:
     *
     *   DAT_004b05d8 = gSpecialEncounterTrackedActorHandle
     *     → port's g_encounter_tracked_handle. Cleared to -1 on the next line
     *       below; verified label exists in Ghidra (symbol_by_name match).
     *
     *   DAT_004b05e4 = g_encounter_phase_flag (port mirror exists already).
     *     The original has only two writers, both to 0 (this teardown +
     *     UpdateSpecialTrafficEncounter teardown at 0x00434f6d), and a single
     *     reader at 0x00434e13 testing == 0. The variable is always 0 in
     *     steady-state, so this redundant zero-write here is observationally
     *     identical to omitting it. Port matches.
     *
     *   DAT_004b05c4 — write-only DEAD variable in the original. reference_to
     *     finds exactly 1 reference, this MOV [0x4b05c4],EAX (always 0). No
     *     reader anywhere in TD5_d3d.exe. Likely a vestigial latch whose
     *     consumer was optimized out at build time. Omitting from the port
     *     has zero behavioral effect.
     */
    g_encounter_active[slot] = 0;
    /* gActorRouteDirectionPolarity = rs[0xFC/4 = 0x3F] per UpdateSpecialEncounter-
     * Control teardown @ 0x00434d40 `MOV [ESI + 0x4afc5c], EAX`. Only dword 0x3F
     * is written by the original; the prior defensive write to dword 0x25
     * (RS_DIRECTION_POLARITY_LEGACY) targeted a field with no references in the
     * original listing and is removed. */
    rs_self[RS_ROUTE_DIRECTION_POLARITY] = 0;
    g_encounter_steer_bias_latch = 0;
    g_encounter_control_active_latch = 0;
    g_encounter_tracked_handle = -1;
    g_encounter_cooldown = 300;
    /* Byte-wide increment (orig uses CL via INC). Field at +0x384 is the encounter
     * completion counter on the slot's own actor (NOT on the player). */
    ACTOR_U8(actor_self, ACTOR_ENCOUNTER_STATE) =
        (uint8_t)(ACTOR_U8(actor_self, ACTOR_ENCOUNTER_STATE) + 1);
}

/* ========================================================================
 * Master Dispatcher  (0x436A70  UpdateRaceActors)
 *
 * Called every simulation tick. Iterates all active actor slots and
 * dispatches to the appropriate update function based on slot type:
 *
 *   Slots 0-5: Racers
 *     - state 0x00 (AI):       UpdateActorTrackBehavior + UpdateVehicleActor
 *     - state 0x01 (player):   skip AI (handled by input/physics)
 *     - state 0x02 (finished): brake behavior
 *
 *   Slots 6-11: Traffic
 *     - slot 9 with encounter: UpdateActorTrackBehavior (hijacked to racer AI)
 *     - all others:            UpdateTrafficRoutePlan + UpdateTrafficActorMotion
 *
 *   After traffic loop: UpdateSpecialTrafficEncounter
 * ======================================================================== */

/* ========================================================================
 * Pool13 pilot trace integration (0x00436A70 UpdateRaceActors).
 * Audit: re/analysis/pilot_00436A70_audit.md.
 *
 * The pilot snapshot mirrors the dispatcher's input set as read by Frida
 * from the original. Collector lives in this TU so it can access the
 * private slot_state + actor pointer state. */
#include "td5_pilot_trace_00436A70.h"

/* Accessors used by td5_pilot_trace_00436A70.c. */
int32_t *td5_pilot_00436A70_route_state_ptr(void) {
    return g_route_state_base;
}
char *td5_pilot_00436A70_actor_base_ptr(void) {
    return g_actor_base;
}

void td5_pilot_00436A70_collect(PilotSnapshot_00436A70 *snap) {
    int i;

    snap->network_active = g_td5.network_active ? 1 : 0;
    snap->racer_count    = g_active_actor_count;
    snap->drag_mode      = g_td5.drag_race_enabled ? 1 : 0;
    snap->wanted_mode    = g_td5.wanted_mode_enabled ? 1 : 0;
    snap->encounter_handle  = g_encounter_tracked_handle;
    snap->encounter_enabled = g_encounter_enabled;
    /* Port doesn't track g_trackTotalSpanCount as a single int32; use the
     * ring length the port consults for branch detection. */
    snap->track_total_span_count = td5_track_get_ring_length();

    for (i = 0; i < PILOT_00436A70_N_SLOTS; i++) {
        char *actor = (g_actor_base && i < TD5_MAX_TOTAL_ACTORS)
                       ? actor_ptr(i) : NULL;
        snap->slot_state[i]    = (int)g_slot_state[i];
        snap->span_raw[i]      = actor ? (int)(int16_t)ACTOR_I16(actor, ACTOR_SPAN_RAW) : 0;
        snap->span_norm[i]     = actor ? (int)(int16_t)ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED) : 0;
        snap->lin_vel_x[i]     = actor ? ACTOR_I32(actor, ACTOR_LIN_VEL_X) : 0;
        snap->lin_vel_z[i]     = actor ? ACTOR_I32(actor, ACTOR_LIN_VEL_Z) : 0;
        snap->world_pos_x[i]   = actor ? ACTOR_I32(actor, ACTOR_WORLD_POS_X) : 0;
        snap->world_pos_z[i]   = actor ? ACTOR_I32(actor, ACTOR_WORLD_POS_Z) : 0;
        snap->recovery_stage[i] = actor ? (int)(uint8_t)ACTOR_U8(actor, 0x37B) : 0;
        snap->wanted_damage[i] = (int)g_wanted_damage_state[i];
    }
}

/* [CONFIRMED @ 0x00436A70] L5 promotion sweep audit (2026-05-18).
 *
 * UpdateRaceActors — 1569 bytes / 450 instructions / 270 decompiled lines.
 * Master per-sub-tick dispatcher with FOUR contiguous regions:
 *   A: rubber-band call → ComputeAIRubberBandThrottle (0x00432D60).
 *   B: per-racer track-state loop (branch detection + forward component +
 *      span progress + offset-clamp + deviation write).
 *   C/C': non-drag vs drag-mode racer dispatch (state==0/2/3 branches).
 *   D/D': non-drag vs drag-mode traffic dispatch (slots 6-11 + slot-9
 *      special-encounter handling + UpdateSpecialTrafficEncounter call).
 *
 * SHIPPED FIXES (in master):
 *   - 3-path route_table_ptr selector (commit 2412baf, 1a95b4c):
 *     PATH 1 (selector=1, ptr=RIGHT), PATH 2a (selector=0, ptr=LEFT),
 *     PATH 2b (default — ptr UNCHANGED). Closes Moscow 34→20 (sub_tick=1).
 *   - F1 traffic bias-clamp slot gate (commit 79023df).
 *   - Steering pre-AI clear placement (commit 79023df / e6622f2 cluster).
 *   - State-2 finished encounter_steer = (int16_t)0xFF00 — bit-equivalent
 *     to orig's WORD 0xFF00 (D5 audited MATCH).
 *
 * KNOWN DIVERGENCES (re/analysis/pilot_00436A70_audit.md):
 *   D1     Branch detection table walker collapsed in port's
 *          `td5_ai_refresh_route_state_slot` — partial port via 3-path
 *          selector (2412baf), but underlying DAT_004C3DA0 walker still
 *          simplified. PARTIAL CLOSE via commit 2412baf.
 *   D2     Per-actor track-state work (gActorTrackSpanProgress / Render
 *          TrackSegmentNearActor / UpdateActorTrackBounds /
 *          ClassifyTrackOffsetClamp / GetTrackSegmentSurfaceType /
 *          recovery-stage override / branch-aware deviation block) lives
 *          in `td5_track_recompute_actor_offsets` rather than inline in
 *          this dispatcher. PER-TICK TIMING may lag by one sub-tick.
 *   D3     Forward-track-component formula uses (>>12) instead of orig's
 *          FILD+FSQRT+IDIV by sqrt(sin²+cos²) — sub-LSB magnitude.
 *   D4     State-2 (finished) 5-byte cluster zero-fill at +0x371..+0x376
 *          not yet written by port.
 *   D6-D8  Loop / slot iteration LOW-impact differences (audited).
 *
 * KNOWN TODO CHAIN OWNERS:
 *   - todo_cascade_unwind_2026-05-17.md (RS_TRACK_PROGRESS quotient/
 *     remainder cluster).
 *
 * Audit reference: re/analysis/pilot_00436A70_audit.md (pool13, 2026-05-14).
 * Effective level: L4 (multiple shipped fixes; D1/D2 are partial-port
 * residuals tracked as cascade-unwind work).
 */
void td5_ai_update_race_actors(void) {
    int i;

    td5_pilot_emit_00436A70_enter();

    /* --- Step 1: Rubber-band already computed in td5_ai_tick --- */
    td5_ai_refresh_route_state();

    if ((g_ai_frame_counter % 60u) == 0u) {
        for (i = 0; i < g_active_actor_count; i++) {
            char *actor = actor_ptr(i);
            TD5_LOG_D(LOG_TAG,
                      "AI state: slot=%d steering=%d throttle=%d route_span=%d mode=%d",
                      i,
                      ACTOR_I32(actor, ACTOR_STEERING_CMD),
                      g_live_throttle[i],
                      ACTOR_I16(actor, ACTOR_SPAN_RAW),
                      g_slot_state[i]);
        }
    }

    /* --- Step 2: Update racers (slots 0 through min(count, 6)) --- */
    {
        int racer_count = TD5_MAX_RACER_SLOTS;
        if (g_active_actor_count < racer_count)
            racer_count = g_active_actor_count;

        for (i = 0; i < racer_count; i++) {
            ai_update_single_racer(i);
        }
    }

    /* --- Step 3: Update traffic (slots 6-11) if active --- */
    if (g_active_actor_count > TD5_MAX_RACER_SLOTS) {
        int traffic_max = g_active_actor_count;
        if (traffic_max > TD5_MAX_TOTAL_ACTORS)
            traffic_max = TD5_MAX_TOTAL_ACTORS;

        for (i = TD5_MAX_RACER_SLOTS; i < traffic_max; i++) {
            ai_update_single_traffic(i);
        }

        /* --- Step 4: Special encounter check --- */
        td5_ai_update_special_encounter();
    }

    td5_pilot_emit_00436A70_leave();
}

/**
 * Dispatch a single racer slot based on its state.
 */
static void ai_update_single_racer(int slot) {
    char *actor = actor_ptr(slot);
    int state = g_slot_state[slot];

    switch (state) {
    case 0x00: /* AI racer */
        /* Wanted mode gate [CONFIRMED @ UpdateRaceActors 0x436E1D]:
         * Original: run UpdateActorTrackBehavior only when
         *   (g_wantedModeEnabled == 0) || (gWantedDamageStateTable[slot] != 0)
         * Slot 0 is the player and uses a separate path in the original, so
         * skip only cop slots (slot != 0) to keep PlayerIsAI working. */
        if (g_td5.wanted_mode_enabled && slot != 0 &&
            g_wanted_damage_state[slot] == 0) {
            return; /* cop arrested — AI frozen */
        }

        /* Drag race: synthetic full-throttle straight-line driver.
         *
         * Original [CONFIRMED @ 0x0042AC85-AC97 + 0x00436FA0..0x0043703F]:
         * spawns slots 1..5 as state=3 (decoration) and the drag-mode
         * dispatcher in UpdateRaceActors skips UpdateVehicleActor for
         * state==3. So the original has no AI driver in drag — slots
         * 1..5 are static cosmetic cars at the strip start.
         *
         * Port diverges at td5_game.c:849-857 (decoration_start=2) to give
         * SP a real 2-car race. That leaves slot 1 at state=0 (AI), so
         * physics ticks but no command writer runs (UpdateActorTrackBehavior
         * is gated off here to mirror original drag mode). Physics reads
         * encounter_steering_cmd (+0x33E) as throttle [CONFIRMED @
         * td5_physics.c:830] — left at 0, slot idles.
         *
         * Drag strip is a single straight span: pin full throttle, zero
         * steer, no brake. Once slot 1 crosses the finish, state flips
         * to 0x02 (finished) and the brake branch below takes over. */
        if (g_td5.drag_race_enabled) {
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0xFF;
            ACTOR_I32(actor, ACTOR_STEERING_CMD) = 0;
            ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 0;
            if ((g_td5.simulation_tick_counter % 60u) == 0u) {
                TD5_LOG_I(LOG_TAG,
                    "drag_ai_drive: slot=%d throttle=0xFF steer=0 brake=0",
                    slot);
            }
            return;
        }

        td5_ai_update_track_behavior(slot);
        /* UpdateVehicleActor would follow here in the physics module */
        break;

    case 0x01: /* Player: skip AI update */
        break;

    case 0x02: /* Finished/dead: brake behavior
                * [precise-port pool13 0x00436A70: state-2 dispatch] */
        {
            int32_t fwd = g_actor_forward_track_component[slot];
            if (fwd < 0) {
                /* [0x00436EE0-E4] Moving backward: word ptr [ESI] = 0
                 * ESI = &actor.+0x33E. Clear encounter_steer = 0. */
                ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0;
            } else {
                /* [0x00436ED5-DD] Moving forward: brake_flag = 1,
                 * encounter_steer = 0xFF00 (== (int16_t)-0x100). */
                ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 1;
                ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)(-0x100);
            }
            /* [precise-port D4 fix from pilot_00436A70_audit]
             * Original [0x00436EE5-F5] unconditionally writes a 5-byte
             * cluster after the fwd-comp branch:
             *   MOV byte ptr [ESI+0x36], 0xff  ; actor+0x374
             *   MOV byte ptr [ESI+0x35], 0xff  ; actor+0x373
             *   MOV byte ptr [ESI+0x34], 0xff  ; actor+0x372
             *   MOV byte ptr [ESI+0x33], 0xff  ; actor+0x371
             *   MOV byte ptr [ESI+0x38], 0x00  ; actor+0x376
             * ESI = &actor.+0x33E so +0x33..0x38 = actor offsets 0x371..0x376.
             * Port previously skipped these; the cluster is faithful to the
             * listing's unconditional path (both branches above fall through
             * to it via JMP 0x00436EE5). */
            ACTOR_U8(actor, 0x371) = 0xff;
            ACTOR_U8(actor, 0x372) = 0xff;
            ACTOR_U8(actor, 0x373) = 0xff;
            ACTOR_U8(actor, 0x374) = 0xff;
            ACTOR_U8(actor, 0x376) = 0x00;
        }
        break;
    }
}

/**
 * Dispatch a single traffic slot.
 * Slot 9 is hijacked to racer AI when encounter is active.
 */
static void ai_update_single_traffic(int slot) {
    /* Slot 9 encounter hijack */
    if (slot == 9 && g_encounter_tracked_handle != -1) {
        /* Run full AI racer path instead of traffic */
        td5_ai_update_track_behavior(9);
        /* UpdateVehicleActor(9) would follow in the physics module */
        return;
    }

    /* Normal traffic update */
    td5_ai_update_traffic_route_plan(slot);
    /* UpdateTrafficActorMotion(slot) would follow in the physics module */
}

/* ========================================================================
 * Per-frame AI setup — rubber-band + route state refresh.
 * Called once before the interleaved per-actor loop.
 * Matches the top of UpdateRaceActors (0x436A70) before the actor loop.
 * ======================================================================== */
void td5_ai_pre_tick(void) {
    td5_ai_compute_rubber_band();
    td5_ai_refresh_route_state();
    g_ai_frame_counter++;
}

/* ========================================================================
 * Per-actor AI dispatch — call for each slot before physics.
 * Matches the per-actor body of UpdateRaceActors (0x436A70):
 *   racer slots: ai_update_single_racer(slot) [CONFIRMED @ 0x436E1D]
 *   traffic slots: ai_update_single_traffic(slot)
 * ======================================================================== */
void td5_ai_update_actor(int slot) {
    if (slot < 0 || slot >= g_active_actor_count)
        return;

    if (slot < TD5_MAX_RACER_SLOTS) {
        ai_update_single_racer(slot);
    } else {
        ai_update_single_traffic(slot);
    }
}

