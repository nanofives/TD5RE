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
#define RS_DIRECTION_POLARITY     0x25
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

#define ACTOR_SPAN_RAW            0x080   /* int16 */
#define ACTOR_SPAN_NORMALIZED     0x082   /* int16 */
#define ACTOR_SUB_LANE_INDEX      0x08C   /* byte  */
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
#define ACTOR_ENCOUNTER_STATE     0x384   /* int32 */

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
static int32_t g_rb_behind_scale;       /* 0x473D9C */
static int32_t g_rb_ahead_scale;        /* 0x473DA0 */
static int32_t g_rb_behind_range;       /* 0x473DA4 */
static int32_t g_rb_ahead_range;        /* 0x473DA8 */

/* Default throttle table (0x473D64, 6+padding entries) */
static int32_t g_default_throttle[14] = {
    0x0100, 0x0100, 0x0140, 0x0118, 0x0122, 0x0140,
    0, 0, 0, 0, 0, 0, 0, 0
};

/* Live throttle table (0x473D2C) -- copied from default each tick,
 * then rubber-band-modified per slot */
static int32_t g_live_throttle[14];

/* Per-actor throttle bias output consumed by route threshold */
static int32_t g_actor_route_steer_bias[TD5_MAX_TOTAL_ACTORS];

/* Per-actor forward track component */
static int32_t g_actor_forward_track_component[TD5_MAX_TOTAL_ACTORS];

/* Slot state: 0x00=AI, 0x01=player, 0x02=finished/dead */
static int32_t g_slot_state[TD5_MAX_TOTAL_ACTORS];

/* Total active actor count (6=no traffic, 12=with traffic, 2=time trial) */
static int32_t g_active_actor_count;

/* Traffic recovery stage per slot (0=normal, 1-7=recovering) */
static int32_t g_traffic_recovery_stage[TD5_MAX_TOTAL_ACTORS];

/* Lateral avoidance direction state (0x4B08B0) */
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

/* Per-car torque triplets table (9 cars x 3 dwords at 0x466F90) */
static int32_t g_car_torque_triplets[9 * 3];

/* AI physics template (128 bytes at 0x473DB0) */
static uint8_t g_ai_physics_template[128];

/* Difficulty tier (0/1/2) */
static int32_t g_race_difficulty_tier;

/* Traffic queue pointer and base */
static const uint8_t *g_traffic_queue_base;
static const uint8_t *g_traffic_queue_ptr;

/* Frame counter for misc timing */
static uint32_t g_ai_frame_counter;

/* ========================================================================
 * Forward declarations (internal helpers)
 * ======================================================================== */

static void ai_update_single_racer(int slot);
static void ai_update_single_traffic(int slot);
static int32_t ai_cos_fixed12(int32_t angle);
static int32_t ai_sin_fixed12(int32_t angle);
static void td5_ai_refresh_route_state_slot(int slot);
static int32_t ai_angle_from_vector(int32_t dx, int32_t dz);

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
 * Matches inline computation at 0x43503A in the original. */
static int32_t ai_route_heading_for_actor(const int32_t *rs, const char *actor) {
    const uint8_t *rb = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
    int16_t sp = ACTOR_I16(actor, ACTOR_SPAN_RAW);
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

static void td5_ai_refresh_route_state_slot(int slot) {
    int32_t *rs;
    char *actor;
    int selector;
    const uint8_t *route_table;
    size_t route_count;
    int16_t span;
    int32_t route_heading;
    int32_t forward_heading;
    int32_t actor_heading;

    if (!g_route_state_base || !g_actor_base ||
        slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) {
        return;
    }

    rs = route_state(slot);
    actor = actor_ptr(slot);
    selector = rs[RS_ROUTE_TABLE_SELECTOR] & 1;
    route_table = g_route_tables[selector];
    route_count = g_route_table_sizes[selector] / 3u;

    rs[RS_ROUTE_TABLE_PTR] = (int32_t)(intptr_t)route_table;
    if (!route_table || route_count == 0u) {
        rs[RS_FORWARD_TRACK_COMP] = 0;
        g_actor_forward_track_component[slot] = 0;
        return;
    }

    span = ACTOR_I16(actor, ACTOR_SPAN_RAW);
    if (span < 0) {
        span = 0;
    } else if ((size_t)span >= route_count) {
        span = (int16_t)(route_count - 1u);
    }

    /* Route-byte → 12-bit angle: (byte * 0x102C) >> 8 [CONFIRMED @ 0x434FFB] */
    route_heading = (((int32_t)route_table[(size_t)span * 3u + 1u] * 0x102C) >> 8) & 0xFFF;

    forward_heading = route_heading;
    if (rs[RS_DIRECTION_POLARITY] != 0) {
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

void td5_ai_compute_rubber_band(void) {
    int i, racer_count;
    int32_t player0_span, ai_span, delta, modifier;

    /* Step 1: copy default throttle to live array */
    memcpy(g_live_throttle, g_default_throttle, 14 * sizeof(int32_t));

    /* Network mode: disable rubber-banding entirely */
    if (g_td5.network_active) {
        racer_count = TD5_MAX_RACER_SLOTS;
        if (g_active_actor_count < racer_count)
            racer_count = g_active_actor_count;
        for (i = 0; i < racer_count; i++) {
            if (g_slot_state[i] == 0) { /* AI slot */
                g_live_throttle[i] = 0x100;
                g_actor_route_steer_bias[i] = 0x100;
            }
        }
        return;
    }

    /* Time trial: no rubber band (scales are zero from init) */

    /* Get player 0 span position */
    player0_span = (int16_t)ACTOR_I16(actor_ptr(0), ACTOR_SPAN_RAW);

    racer_count = TD5_MAX_RACER_SLOTS;
    if (g_active_actor_count < racer_count)
        racer_count = g_active_actor_count;

    /* Step 2: per-slot rubber-band modifier */
    for (i = 0; i < racer_count; i++) {
        if (g_slot_state[i] != 0) /* skip non-AI (player/finished) */
            continue;

        ai_span = (int16_t)ACTOR_I16(actor_ptr(i), ACTOR_SPAN_RAW);
        delta = ai_span - player0_span;

        if (delta < 0) {
            /* AI is behind player -- catch-up boost.
             * Original: modifier = (scale * negative_delta) / positive_range = negative
             *           bias = 0x100 - negative = > 0x100 (throttle boost)
             * Clamp: original checks (behind_range < delta) which never fires for
             * negative delta; port mirrors with equivalent negative-delta clamp. */
            if (g_rb_behind_range != 0) {
                if (delta < -g_rb_behind_range)
                    delta = -g_rb_behind_range;
                modifier = (g_rb_behind_scale * delta) / g_rb_behind_range;  /* negative / positive = negative */
            } else {
                modifier = 0;
            }
        } else {
            /* AI is ahead of player -- slow down.
             * modifier = (scale * positive_delta) / positive_range = positive
             * bias = 0x100 - positive = < 0x100 (throttle reduction) */
            if (g_rb_ahead_range != 0) {
                if (delta > g_rb_ahead_range)
                    delta = g_rb_ahead_range;
                modifier = (g_rb_ahead_scale * delta) / g_rb_ahead_range;
            } else {
                modifier = 0;
            }
        }

        g_actor_route_steer_bias[i] = 0x100 - modifier;
    }
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
    int tier = g_race_difficulty_tier;
    int is_circuit = (g_td5.track_type == TD5_TRACK_CIRCUIT);
    int has_traffic = g_td5.traffic_enabled;
    int is_pitbull = (g_td5.race_rule_variant == 4);
    int is_time_trial = g_td5.time_trial_enabled;
    int racer_count;

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
        rs[RS_DIRECTION_POLARITY] = 0;

        if (g_actor_base) {
            selector = (ACTOR_U8(actor_ptr(i), ACTOR_SUB_LANE_INDEX) >= 2u) ? 1 : 0;
        }
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

    /* --- First layer: global difficulty scaling on AI physics template ---
     *
     * Template field offsets:
     *   +0x68 (26*4): steering factor (short)
     *   +0x2C (11*4): base grip (short)
     *   +0x6E:        brake force (short)
     *   +0x70 (28*4): low-speed brake coefficient (short)
     *   +0x74 (29*4): max speed / rev limiter (short)
     */
    {
        int16_t *steer  = (int16_t *)(g_ai_physics_template + 0x68);
        int16_t *grip   = (int16_t *)(g_ai_physics_template + 0x2C);
        int16_t *brake  = (int16_t *)(g_ai_physics_template + 0x6E);

        switch (g_td5.difficulty) {
        case TD5_DIFFICULTY_EASY:
            /* No change */
            break;

        case TD5_DIFFICULTY_NORMAL:
            /* Steering * 0x168/256 (~88%), Grip * 300/256 (~117%) */
            *steer = (int16_t)(((int32_t)*steer * 0x168) >> 8);
            *grip  = (int16_t)(((int32_t)*grip  * 300)   >> 8);
            break;

        case TD5_DIFFICULTY_HARD:
            /* Steering * 0x28A/256 (~255%), Grip * 0x17C/256 (~148%),
             * Brake * 0x1C2/256 (~177%) */
            *steer = (int16_t)(((int32_t)*steer * 0x28A) >> 8);
            *grip  = (int16_t)(((int32_t)*grip  * 0x17C) >> 8);
            *brake = (int16_t)(((int32_t)*brake * 0x1C2) >> 8);
            break;
        }
    }

    /* --- Second layer: mode/circuit/traffic/tier decision tree ---
     *
     * Sets: rb_behind_scale, rb_behind_range, rb_ahead_scale, rb_ahead_range
     * Also optionally scales AI template top-speed (+0x74) and grip (+0x2C).
     */

    if (is_time_trial) {
        /* Time Trial: rubber-banding completely disabled */
        g_rb_behind_scale = 0;
        g_rb_behind_range = 0x40;
        g_rb_ahead_scale  = 0;
        g_rb_ahead_range  = 0x40;

    } else if (is_pitbull) {
        /* Pitbull / Mode 4 */
        {
            int16_t *spd = (int16_t *)(g_ai_physics_template + 0x74);
            int16_t *grp = (int16_t *)(g_ai_physics_template + 0x2C);
            *spd = (int16_t)(((int32_t)*spd * 0x91) >> 8);
            *grp = (int16_t)(((int32_t)*grp * 0xB9) >> 8);
        }
        g_rb_behind_scale = 0x8C;   /* 140 */
        g_rb_behind_range = 100;
        g_rb_ahead_scale  = 0xC0;   /* 192 */
        g_rb_ahead_range  = 0x40;   /* 64  */

    } else if (is_circuit) {
        /* Circuit races */
        int16_t *spd = (int16_t *)(g_ai_physics_template + 0x74);
        int16_t *grp = (int16_t *)(g_ai_physics_template + 0x2C);

        switch (tier) {
        case 0:
            *spd = (int16_t)(((int32_t)*spd * 0x91) >> 8);
            *grp = (int16_t)(((int32_t)*grp * 0xC8) >> 8);
            g_rb_behind_scale = 0x8C;   /* 140 */
            g_rb_behind_range = 100;
            g_rb_ahead_scale  = 0xC8;   /* 200 */
            g_rb_ahead_range  = 0x37;   /* 55  */
            break;
        case 1:
            *spd = (int16_t)(((int32_t)*spd * 0xA0) >> 8);
            *grp = (int16_t)(((int32_t)*grp * 0xEC) >> 8);
            g_rb_behind_scale = 0x96;   /* 150 */
            g_rb_behind_range = 100;
            g_rb_ahead_scale  = 0xC0;   /* 192 */
            g_rb_ahead_range  = 0x40;   /* 64  */
            break;
        default: /* tier 2 */
            *spd = (int16_t)(((int32_t)*spd * 0xC3) >> 8);
            *grp = (int16_t)(((int32_t)*grp * 0x104) >> 8);
            g_rb_behind_scale = 0xC8;   /* 200 */
            g_rb_behind_range = 100;
            g_rb_ahead_scale  = 0x78;   /* 120 */
            g_rb_ahead_range  = 0x40;   /* 64  */
            break;
        }

    } else if (has_traffic) {
        /* Point-to-point WITH traffic */
        int16_t *spd = (int16_t *)(g_ai_physics_template + 0x74);
        int16_t *grp = (int16_t *)(g_ai_physics_template + 0x2C);

        switch (tier) {
        case 0:
            *spd = (int16_t)(((int32_t)*spd * 0xB4) >> 8);
            /* grip stays 0x100/256 = 100% */
            g_rb_behind_scale = 0xB4;   /* 180 */
            g_rb_behind_range = 0x4B;   /* 75  */
            g_rb_ahead_scale  = 0xBE;   /* 190 */
            g_rb_ahead_range  = 100;
            break;
        case 1:
            *spd = (int16_t)(((int32_t)*spd * 0xBE) >> 8);
            *grp = (int16_t)(((int32_t)*grp * 0x10E) >> 8);
            g_rb_behind_scale = 0xC8;   /* 200 */
            g_rb_behind_range = 0x3C;   /* 60  */
            g_rb_ahead_scale  = 0xBE;   /* 190 */
            g_rb_ahead_range  = 100;
            break;
        default: /* tier 2 */
            *spd = (int16_t)(((int32_t)*spd * 0xDC) >> 8);
            *grp = (int16_t)(((int32_t)*grp * 0x122) >> 8);
            g_rb_behind_scale = 0xDC;   /* 220 */
            g_rb_behind_range = 0x3C;   /* 60  */
            g_rb_ahead_scale  = 100;
            g_rb_ahead_range  = 0x40;   /* 64  */
            break;
        }

    } else {
        /* Point-to-point, NO traffic */
        int16_t *spd = (int16_t *)(g_ai_physics_template + 0x74);
        int16_t *grp = (int16_t *)(g_ai_physics_template + 0x2C);

        switch (tier) {
        case 0:
            *spd = (int16_t)(((int32_t)*spd * 0xAA) >> 8);
            /* grip 0x100/256 = 100% -- no change */
            g_rb_behind_scale = 0xA0;   /* 160 */
            g_rb_behind_range = 100;
            g_rb_ahead_scale  = 0x96;   /* 150 */
            g_rb_ahead_range  = 0x50;   /* 80  */
            break;
        case 1:
            *spd = (int16_t)(((int32_t)*spd * 0xB4) >> 8);
            /* grip 0x100/256 = 100% -- no change */
            g_rb_behind_scale = 0xC8;   /* 200 */
            g_rb_behind_range = 0x4B;   /* 75  */
            g_rb_ahead_scale  = 0xC0;   /* 192 */
            g_rb_ahead_range  = 0x4B;   /* 75  */
            break;
        default: /* tier 2 */
            *spd = (int16_t)(((int32_t)*spd * 0xDC) >> 8);
            *grp = (int16_t)(((int32_t)*grp * 0x10E) >> 8);
            g_rb_behind_scale = 0x10E;  /* 270 */
            g_rb_behind_range = 0x41;   /* 65  */
            g_rb_ahead_scale  = 0x96;   /* 150 */
            g_rb_ahead_range  = 0x50;   /* 80  */
            break;
        }
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
 * ======================================================================== */

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
                 * Original uses CosFixed12 [CONFIRMED @ 0x4340C0: FUN_0040a700]
                 * which works with 0x800 baseline (cos(small)≈max). Port uses
                 * SinFixed12 because the operating point is near delta=0 where
                 * sin(0)=0 prevents overshoot. The port lacks the boundary
                 * pre-loop that stabilizes the original's cos-based correction.
                 * TODO: revert to cos once boundary system is ported. */
                int32_t t   = ai_sin_fixed12(left_dev);
                int32_t mul = t * param_2;
                ACTOR_I32(actor, ACTOR_STEERING_CMD) =
                    ACTOR_I32(actor, ACTOR_STEERING_CMD)
                    + ((mul + ((mul >> 31) & 0xFFF)) >> 12);
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
                /* SinFixed12 — see left fine band comment above */
                int32_t t   = ai_sin_fixed12(right_dev);
                int32_t mul = t * param_2;
                ACTOR_I32(actor, ACTOR_STEERING_CMD) =
                    ACTOR_I32(actor, ACTOR_STEERING_CMD)
                    - ((mul + ((mul >> 31) & 0xFFF)) >> 12);
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
}

/* ========================================================================
 * Route Threshold State  (0x434AA0  UpdateActorRouteThresholdState)
 *
 * Throttle/brake controller operating on the route speed-threshold byte.
 * Consumes the rubber-band-modified throttle bias.
 *
 * Returns: 1 if braking (caller passes 0x10000 to steering bias),
 *          0 if coasting/accelerating (caller passes 0x20000).
 * ======================================================================== */

int td5_ai_update_route_threshold(int slot) {
    int32_t *rs = route_state(slot);
    char *actor  = actor_ptr(slot);

    int32_t fwd_comp  = g_actor_forward_track_component[slot];
    int32_t speed     = ACTOR_I32(actor, ACTOR_LONGITUDINAL_SPEED);

    /* Read the route speed-threshold byte for the current span.
     * Route table: route[span*3 + 2] = threshold byte.
     * 0x00 = emergency, 0x01-0xFE = scaled, 0xFF = no limit. */
    int32_t *route_table = (int32_t *)rs[RS_ROUTE_TABLE_PTR];
    int16_t span = ACTOR_I16(actor, ACTOR_SPAN_RAW);
    int threshold = 0xFF; /* default: no limit */

    if (route_table) {
        uint8_t *route_bytes = (uint8_t *)route_table;
        threshold = route_bytes[span * 3 + 2];
    }

    if (threshold == 0x00) {
        /* Emergency stop: full brake if forward > 0x80 and speed < 0x10000 */
        if (fwd_comp > 0x80 && speed < 0x10000) {
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0xFF00;
            ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 1;
            return 1;
        }
    }

    if (threshold > 0x00 && threshold < 0xFF) {
        /* Scaled threshold: threshold * 0x400 / 0xFF */
        int32_t scaled = (threshold * 0x400) / 0xFF;

        if (fwd_comp >= scaled) {
            /* Coasting: above speed threshold -- no throttle, no brake */
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0;
            ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 0;
            return 0;
        }
    }

    /* Below threshold or no limit: accelerate with rubber-band bias */
    ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)g_actor_route_steer_bias[slot];
    ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 0;
    return 0;
}

/* ========================================================================
 * Track Offset Bias / Peer Avoidance
 *   0x4337E0  FindActorTrackOffsetPeer
 *   0x434900  UpdateActorTrackOffsetBias
 * ======================================================================== */

/**
 * FindActorTrackOffsetPeer: scan all actors on the same route band for the
 * nearest one ahead within 0x28 spans whose lateral range overlaps.
 * Returns peer slot index, or -1 if none found.
 */
int td5_ai_find_offset_peer(int *route_state_ptr) {
    int slot = route_state_ptr[RS_SLOT_INDEX];
    char *self = actor_ptr(slot);
    int16_t self_span = ACTOR_I16(self, ACTOR_SPAN_RAW);
    uint8_t self_lane = ACTOR_U8(self, ACTOR_SUB_LANE_INDEX);
    int self_selector = route_state_ptr[RS_ROUTE_TABLE_SELECTOR];

    int best_slot = -1;
    int32_t best_dist = 0x29; /* must be strictly less than 0x29 */

    int i;
    for (i = 0; i < g_active_actor_count; i++) {
        int32_t *peer_rs;
        int16_t peer_span;
        int32_t dist;

        if (i == slot) continue;

        peer_rs = route_state(i);
        if (peer_rs[RS_ROUTE_TABLE_SELECTOR] != self_selector) continue;

        peer_span = ACTOR_I16(actor_ptr(i), ACTOR_SPAN_RAW);
        dist = peer_span - self_span;

        /* Must be ahead (positive distance), within 0x28 spans */
        if (dist <= 0 || dist >= 0x29) continue;

        /* Check lateral overlap (same lane or adjacent) */
        {
            uint8_t peer_lane = ACTOR_U8(actor_ptr(i), ACTOR_SUB_LANE_INDEX);
            int lane_diff = (int)peer_lane - (int)self_lane;
            if (lane_diff < -1 || lane_diff > 1) continue;
        }

        if (dist < best_dist) {
            best_dist = dist;
            best_slot = i;
            /* Record whether the peer is on the outer or inner lane */
            if (ACTOR_U8(actor_ptr(i), ACTOR_SUB_LANE_INDEX) > self_lane)
                g_lateral_avoidance_direction = 1;  /* peer outer */
            else
                g_lateral_avoidance_direction = -1; /* peer inner */
        }
    }

    return best_slot;
}

/**
 * UpdateActorTrackOffsetBias: adjust lateral offset for peer avoidance.
 *   If peer found: offset += (0x29 - distance) * direction
 *   If no peer: offset decays toward 0 at 8 units/tick
 */
void td5_ai_update_track_offset_bias(int slot) {
    int32_t *rs = route_state(slot);
    int peer = td5_ai_find_offset_peer(rs);

    if (peer >= 0) {
        char *self = actor_ptr(slot);
        int16_t self_span = ACTOR_I16(self, ACTOR_SPAN_RAW);
        int16_t peer_span = ACTOR_I16(actor_ptr(peer), ACTOR_SPAN_RAW);
        int32_t dist = peer_span - self_span;
        int32_t push;

        if (dist < 1) dist = 1;
        if (dist > 0x28) dist = 0x28;
        push = 0x29 - dist;

        if (g_lateral_avoidance_direction > 0) {
            /* Peer is outer -> push inward (negative offset) */
            rs[RS_TRACK_OFFSET_BIAS] -= push;
        } else {
            /* Peer is inner -> push outward (positive offset) */
            rs[RS_TRACK_OFFSET_BIAS] += push;
        }
    } else {
        /* No peer: decay toward zero at 8 units/tick */
        int32_t bias = rs[RS_TRACK_OFFSET_BIAS];
        if (bias > 0) {
            bias -= 8;
            if (bias < 0) bias = 0;
        } else if (bias < 0) {
            bias += 8;
            if (bias > 0) bias = 0;
        }
        rs[RS_TRACK_OFFSET_BIAS] = bias;
    }

    /* Port-only safeguard: clamp to ±0x200 to prevent runaway accumulation.
     * The original @ 0x434900 has no explicit clamp but its peer-search
     * (0x4337E0) + direction-toggle on DAT_004b08b0 keeps bias oscillating
     * around zero. The port's simplified find_offset_peer always returns
     * a peer and never toggles direction, so without this clamp bias grows
     * unboundedly (observed ~7000 after 15s), which pulls the sampled
     * target point far perpendicular to the track and saturates AI steer.
     * TODO: port the original's toggle logic for bit-accurate behavior. */
    if (rs[RS_TRACK_OFFSET_BIAS] >  0x200) rs[RS_TRACK_OFFSET_BIAS] =  0x200;
    if (rs[RS_TRACK_OFFSET_BIAS] < -0x200) rs[RS_TRACK_OFFSET_BIAS] = -0x200;
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

int td5_ai_advance_track_script(int *rs) {
    int slot = rs[RS_SLOT_INDEX];
    char *actor = actor_ptr(slot);
    int32_t flags = rs[RS_SCRIPT_FLAGS];

    /* --- Process active flag effects before advancing IP --- */

    /* Flag 0x10: Wait for speed near zero */
    if (flags & 0x10) {
        int32_t spd = ACTOR_I32(actor, ACTOR_LONGITUDINAL_SPEED);
        if (spd < 0) spd = -spd;

        if (spd < 0x100) {
            /* Speed is near zero -- clear flag, clear override */
            rs[RS_SCRIPT_FLAGS] &= ~0x10;
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0;
            ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 0;
        } else {
            /* Still moving -- maintain brake */
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)(-0x100);
            ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 1;
            return 0; /* block */
        }
    }

    /* Flag 0x02: Lateral offset tracking */
    if (flags & 0x02) {
        int32_t offset_param = rs[RS_SCRIPT_OFFSET_PARAM];
        int32_t fwd = ACTOR_I32(actor, ACTOR_LONGITUDINAL_SPEED);

        if (offset_param < 0) {
            /* Negative offset -> brake */
            if (fwd > (-offset_param << 8)) {
                ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)(-0x100);
            } else {
                ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0;
                rs[RS_SCRIPT_FLAGS] &= ~0x02;
            }
        } else {
            /* Positive offset -> accelerate */
            if (fwd < (offset_param << 8)) {
                ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0xFF;
            } else {
                ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0;
                rs[RS_SCRIPT_FLAGS] &= ~0x02;
            }
        }
    }

    /* Flag 0x04: Steer left (toward route alignment) */
    if (flags & 0x04) {
        int32_t heading = ACTOR_I32(actor, ACTOR_YAW_ACCUM) >> 8;
        int32_t route_heading = ai_route_heading_for_actor(rs, actor);
        int32_t hdelta = (heading - route_heading) & 0xFFF;

        if (hdelta < 0x201 || hdelta > 0xDFF) {
            /* Within threshold: aligned -- clear flag */
            rs[RS_SCRIPT_FLAGS] &= ~0x04;
            ACTOR_I32(actor, ACTOR_STEERING_CMD) = 0;
        } else {
            /* Apply leftward steering: +0x4000 per tick, max 0x19000 */
            int32_t sc = ACTOR_I32(actor, ACTOR_STEERING_CMD);
            sc += 0x4000;
            if (sc > 0x19000) sc = 0x19000;
            ACTOR_I32(actor, ACTOR_STEERING_CMD) = sc;
            return 0; /* block until aligned */
        }
    }

    /* Flag 0x08: Steer right (opposite to route alignment) */
    if (flags & 0x08) {
        int32_t heading = ACTOR_I32(actor, ACTOR_YAW_ACCUM) >> 8;
        int32_t route_heading = ai_route_heading_for_actor(rs, actor);
        int32_t hdelta = (heading - route_heading) & 0xFFF;

        if (hdelta < 0x201 || hdelta > 0xDFF) {
            rs[RS_SCRIPT_FLAGS] &= ~0x08;
            ACTOR_I32(actor, ACTOR_STEERING_CMD) = 0;
        } else {
            int32_t sc = ACTOR_I32(actor, ACTOR_STEERING_CMD);
            sc -= 0x4000;
            if (sc < -0x19000) sc = -0x19000;
            ACTOR_I32(actor, ACTOR_STEERING_CMD) = sc;
            return 0; /* block */
        }
    }

    /* --- Countdown timer check: cycle to next program bank when expired --- */
    rs[RS_SCRIPT_COUNTDOWN]--;
    if (rs[RS_SCRIPT_COUNTDOWN] < 0) {
        /* Round-robin through 4 program banks */
        int bank = g_script_bank_index[slot];
        bank = (bank + 1) & 3;
        g_script_bank_index[slot] = bank;

        rs[RS_SCRIPT_BASE_PTR] = (int32_t)(intptr_t)g_script_banks[bank];
        rs[RS_SCRIPT_IP] = 0;
        rs[RS_SCRIPT_FLAGS] = 0;
        rs[RS_SCRIPT_COUNTDOWN] = 0x96; /* 150 frames */
    }

    /* --- Fetch and execute the current opcode --- */
    {
        const int32_t *base = (const int32_t *)(intptr_t)rs[RS_SCRIPT_BASE_PTR];
        int ip = rs[RS_SCRIPT_IP];
        int32_t opcode;

        if (!base) return 1; /* no script active */

        opcode = base[ip];
        if (g_last_logged_opcode[slot] != opcode) {
            TD5_LOG_I(LOG_TAG, "Script opcode change: slot=%d ip=%d opcode=%d flags=0x%X",
                      slot, ip, opcode, rs[RS_SCRIPT_FLAGS]);
            g_last_logged_opcode[slot] = opcode;
        }

        switch (opcode) {

        case 0: /* Terminate/reset */
            /* Clear script state */
            rs[RS_SCRIPT_BASE_PTR] = 0;
            rs[RS_SCRIPT_IP] = 0;
            rs[RS_SCRIPT_SPEED_PARAM] = 0;
            rs[RS_SCRIPT_FLAGS] = 0;
            rs[RS_SCRIPT_FIELD_3E] = 0;
            rs[RS_SCRIPT_FIELD_43] = 0;
            ACTOR_I32(actor, ACTOR_STEERING_CMD) = 0;
            return 1; /* script complete */

        case 1: /* Set countdown timer */
            rs[RS_SCRIPT_SPEED_PARAM] = base[ip + 1];
            rs[RS_SCRIPT_FLAGS] |= 0x01;
            rs[RS_SCRIPT_IP] = ip + 2;
            break;

        case 2: /* Set lateral offset target + flag 0x02 */
            rs[RS_SCRIPT_FLAGS] |= 0x02;
            rs[RS_SCRIPT_OFFSET_PARAM] = base[ip + 1];
            rs[RS_SCRIPT_IP] = ip + 2;
            break;

        case 3: /* Set flag bits */
            rs[RS_SCRIPT_FLAGS] |= base[ip + 1];
            rs[RS_SCRIPT_IP] = ip + 2;
            break;

        case 4: /* Clear flag bits */
            rs[RS_SCRIPT_FLAGS] &= ~base[ip + 1];
            rs[RS_SCRIPT_IP] = ip + 2;
            break;

        case 5: /* Steer left: flag 0x04 */
            rs[RS_SCRIPT_FLAGS] |= 0x04;
            rs[RS_SCRIPT_IP] = ip + 1;
            break;

        case 6: /* Steer right: flag 0x08 */
            rs[RS_SCRIPT_FLAGS] |= 0x08;
            rs[RS_SCRIPT_IP] = ip + 1;
            break;

        case 7: /* Force brake: encounter_steering = -0x100 */
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)(-0x100);
            ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 1;
            rs[RS_SCRIPT_IP] = ip + 1;
            break;

        case 8: /* Stop and wait: flag 0x10 */
            rs[RS_SCRIPT_FLAGS] |= 0x10;
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)(-0x100);
            ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 1;
            rs[RS_SCRIPT_IP] = ip + 1;
            return 0; /* block until speed near zero */

        case 9: { /* Auto-select program based on heading vs route geometry */
            int32_t heading = ACTOR_I32(actor, ACTOR_YAW_ACCUM) >> 8;
            int32_t hdelta = (heading - ai_route_heading_for_actor(rs, actor)) & 0xFFF;
            uint8_t lane = ACTOR_U8(actor, ACTOR_SUB_LANE_INDEX);
            const int32_t *selected;

            /* Decision based on heading delta ranges and lane availability:
             *   0x900-0xF00 + lane fits -> Program A (left recovery)
             *   > 0x6FF + lane exceeds actor width -> Program B (right)
             *   0x100-0x700 + lane fits -> Program C (right recovery)
             *   default -> Program D (right back) */
            if (hdelta >= 0x900 && hdelta <= 0xF00 && lane > 0) {
                selected = g_script_program_a;
            } else if (hdelta > 0x6FF && lane < 3) {
                selected = g_script_program_b;
            } else if (hdelta >= 0x100 && hdelta <= 0x700 && lane > 0) {
                selected = g_script_program_c;
            } else {
                selected = g_script_program_d;
            }

            rs[RS_SCRIPT_BASE_PTR] = (int32_t)(intptr_t)selected;
            rs[RS_SCRIPT_IP] = 0;
            rs[RS_SCRIPT_FLAGS] = 0;
            rs[RS_SCRIPT_COUNTDOWN] = 0xFA; /* 250 frames */
            break;
        }

        case 10: /* Set flag 0x40 (latent) */
            rs[RS_SCRIPT_FLAGS] |= 0x40;
            rs[RS_SCRIPT_IP] = ip + 1;
            break;

        case 11: /* Set flag 0x80 (latent) */
            rs[RS_SCRIPT_FLAGS] |= 0x80;
            rs[RS_SCRIPT_IP] = ip + 1;
            break;

        default:
            /* Unknown opcode: advance and terminate */
            rs[RS_SCRIPT_IP] = ip + 1;
            return 1;
        }
    }

    return 0; /* script still running */
}

/* ========================================================================
 * Actor Track Behavior  (0x434FE0  UpdateActorTrackBehavior)
 *
 * Main AI path-following tick for non-player racers and the encounter actor.
 * ======================================================================== */

void td5_ai_update_track_behavior(int slot) {
    int32_t *rs = route_state(slot);
    char *actor  = actor_ptr(slot);
    int32_t heading, route_heading, hdelta;
    int threshold_result;
    int32_t steer_weight;

    /* Time trial: skip AI track behavior entirely */
    if (g_td5.time_trial_enabled)
        return;

    /* During countdown (paused), pass through the rubber-band throttle so
     * the RPM integrator revs to ~7200 before the lights go out, then
     * short-circuit the track-behavior cascade. [CONFIRMED via fresh decomp
     * of UpdateActorTrackBehavior @ 0x00434FE0: the original has NO internal
     * countdown gate — it runs the full cascade every tick. It self-extinguishes
     * because UpdateAIVehicleDynamics is paused-gated (master gate), so
     * linear_velocity_x/z stay 0, and UpdateActorSteeringBias's vx<0x100
     * branch computes sin(vx)*param_2 = 0.]
     *
     * We take a conservative middle-ground: skip the cascade but DO NOT zero
     * ACTOR_STEERING_CMD or ACTOR_STEERING_RAMP_ACCUM. The port's bias function
     * branches on route deviation rather than velocity, so un-gating could
     * drift; but zeroing the state mid-pause introduces a discontinuity on
     * the first active tick that the original never has. Preserve whatever
     * the pre-countdown state was (0 for fresh spawns). */
    if (g_td5.paused) {
        ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)g_actor_route_steer_bias[slot];
        ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 0;
        return;
    }

    /* --- Script check: if a script is active, run it --- */
    if (rs[RS_SCRIPT_BASE_PTR] != 0) {
        int result = td5_ai_advance_track_script(rs);
        if (result == 0) {
            /* Script still running (blocking): just update track progress */
            return;
        }
        /* result == 1: script complete, fall through to normal AI */
    }

    /* --- Heading misalignment trigger: start recovery script --- */
    heading = ACTOR_I32(actor, ACTOR_YAW_ACCUM) >> 8;

    /* Compute route heading inline from route byte, matching original @ 0x43503A.
     * The original does NOT read RS[0x18] here — it reads the route byte directly.
     * RS[0x18] stores velocity dot product (forward track component). */
    route_heading = ai_route_heading_for_actor(rs, actor);

    /* Skip recovery when route heading byte is 0 (no heading data).
     * Some tracks (e.g. level014 spans 0-174) have heading_byte=0 in the
     * route file. With route_heading=0 the recovery formula always fires
     * (comparing against a nonsense reference), trapping AI in an infinite
     * recovery→normal→recovery loop. The normal target-point steering
     * at step 2 handles guidance without heading data. */
    if (route_heading != 0) {
        /* Original FUN_00434FE0: adjusts for expected 0x800 offset between actor
         * yaw (atan2+0x800) and route heading (route_byte<<4), then checks if
         * the residual misalignment exceeds threshold.
         * Formula: uVar3 = -(((heading - route_heading - 0x800U & 0xFFF) - 0x800 & 0xFFF) & 0xFFF) */
        uint32_t adjusted = (((uint32_t)(heading - route_heading) - 0x800U) & 0xFFF);
        adjusted = (adjusted - 0x800U) & 0xFFF;
        hdelta = (int32_t)(-(int32_t)adjusted) & 0xFFF;

        if (hdelta > 0x800 && hdelta <= 0xCE0) {
            TD5_LOG_D(LOG_TAG, "recovery: slot=%d hdelta=0x%X heading=0x%X route=0x%X",
                      slot, hdelta, heading & 0xFFF, route_heading & 0xFFF);
            /* Significant misalignment: assign initial recovery script [8, 9, 0] */
            rs[RS_SCRIPT_BASE_PTR] = (int32_t)(intptr_t)g_script_init_recovery;
            rs[RS_SCRIPT_IP] = 0;
            rs[RS_SCRIPT_FLAGS] = 0;
            rs[RS_SCRIPT_FIELD_3E] = 0;
            rs[RS_SCRIPT_FIELD_43] = 0;
            rs[RS_SCRIPT_COUNTDOWN] = 0x96;
            return;
        }
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
        int16_t span = ACTOR_I16(actor, ACTOR_SPAN_RAW);
        int span_count = td5_track_get_span_count();
        if (span_count > 0 && span >= 0) {
            /* (a) Target span: 4 spans ahead, wrapped [CONFIRMED @ 0x43514D] */
            int target_span = ((int)span + 4) % span_count;

            /* Spline progress update (original also does this) */
            {
                int route_lane_val = 0;
                int32_t seg_dist = rs[RS_FORWARD_TRACK_COMP];
                const uint8_t *route_bytes = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
                if (route_bytes) {
                    route_lane_val = (int)route_bytes[(size_t)(unsigned)span * 3u];
                }
                rs[RS_TRACK_PROGRESS] = td5_track_compute_spline_position(
                    (int)span, seg_dist, route_lane_val);
            }

            /* (b) Get target world position via SampleTrackTargetPoint
             * [CONFIRMED @ 0x434800: 2-vertex interpolation with route_byte + lateral bias]
             * route_byte from route table controls left-right interpolation (0-255).
             * lateral_bias from RS_TRACK_OFFSET_BIAS applies perpendicular peer avoidance. */
            {
                int target_x = 0, target_z = 0;
                int route_byte = 128; /* default: center of span */
                int lateral_bias = rs[RS_TRACK_OFFSET_BIAS];

                const uint8_t *route_bytes = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
                if (route_bytes) {
                    route_byte = (int)route_bytes[(size_t)(unsigned)target_span * 3u];
                }

                if (td5_track_sample_target_point(target_span, route_byte,
                                                   &target_x, &target_z, lateral_bias)) {
                    /* (c) Delta vector: target - actor, shift from 24.8 FP to integer
                     * [CONFIRMED @ 0x4352A1-0x4352C8] */
                    int32_t actor_x = ACTOR_I32(actor, ACTOR_WORLD_POS_X);
                    int32_t actor_z = ACTOR_I32(actor, ACTOR_WORLD_POS_Z);
                    int32_t dx = (target_x - actor_x) >> 8;
                    int32_t dz = (target_z - actor_z) >> 8;

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
                     * [CONFIRMED @ 0x435354-0x435372] */
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

    /* 4. Steering weight based on threshold result:
     *    Original: braking = 0x10000, coasting = 0x20000.
     *    Scaled to 1/4 for the port's bicycle model — the original's full
     *    steer_weight produces yaw torque at the 0x578 clamp every tick,
     *    saturating angular velocity within 5 ticks and causing the car
     *    to spin out. Scaling down keeps the fine-band correction gentle
     *    enough for the port's dynamics while preserving the band system's
     *    proportional steering behavior.
     *    TODO: restore original weights once AI dynamics match. */
    steer_weight = threshold_result ? 0x4000 : 0x8000;

    /* 5. Steering: direct-set proportional controller.
     *
     * The original's band-based UpdateActorSteeringBias (0x4340C0) accumulates
     * corrections each tick. With the port's deviation baseline near 0 (vs the
     * original's 0x800), the band system oscillates around alignment and
     * accumulates runaway steer in one direction.
     *
     * This controller directly SETS steer_cmd = -delta * gain each tick
     * (no accumulation). The deviation's signed delta (heading - target)
     * naturally centers on 0 when aligned. Combined with the ±1500 angular
     * velocity clamp in td5_physics_update_ai, this produces stable
     * path-following without runaway.
     *
     * Gain: steer_weight >> 2 gives steer_angle = delta * (weight/1024).
     * At weight=0x8000: steer_angle = delta * 8 per heading-unit error.
     * A 50-unit error (4.4°) → steer_angle of 400 → 35° of front wheel
     * angle. This is strong enough to track curves but not so large that
     * the bicycle model saturates.
     *
     * TODO: restore band-based system once the spawn heading and AI dynamics
     * match the original's conventions. */
    {
        int32_t delta = rs[RS_RIGHT_DEVIATION];
        /* Use the signed deviation: RIGHT_DEVIATION is small when the car
         * needs to steer right (heading is CW of target). Convert to signed
         * error using the same convention as the deviation decomposition. */
        int32_t left_dev = rs[RS_LEFT_DEVIATION];
        int32_t right_dev = rs[RS_RIGHT_DEVIATION];
        int32_t signed_delta;
        if (left_dev < right_dev) {
            /* LEFT is smaller → heading is CW of target → need positive steer */
            signed_delta = -left_dev;
        } else {
            /* RIGHT is smaller → heading is CCW of target → need negative steer */
            signed_delta = right_dev;
        }
        /* Direct heading control: set angular_velocity_yaw proportional
         * to the heading error, bypassing the steer→torque→omega chain.
         *
         * The bicycle model adds yaw_torque to omega each tick AFTER the
         * AI writes it. With STEERING_CMD = 0, the bicycle model produces
         * only a small residual torque from weight asymmetry (~100-200),
         * so our direct omega write dominates heading control.
         *
         * Scale: signed_delta * 16 gives omega in accumulator units.
         * At delta=50 (4.4° off): omega = 800 → heading change = 3.1°/tick.
         * This matches typical track curvature rates. The ±1500 clamp in
         * td5_physics_update_ai limits extreme turns. */
        /* PD omega controller: P proportional to heading error drives
         * the heading toward the target; D on current omega prevents
         * overshoot. omega_prev is the value left by physics last tick. */
        int32_t omega_prev = ACTOR_I32(actor, 0x1C4);
        int32_t desired_omega = signed_delta * 12 - (omega_prev >> 8) * 6;
        if (desired_omega > 1500) desired_omega = 1500;
        if (desired_omega < -1500) desired_omega = -1500;
        ACTOR_I32(actor, 0x1C4) = desired_omega;
        /* Zero steer so the bicycle model doesn't fight the direct
         * heading control with its own (mismatched) yaw torque. */
        ACTOR_I32(actor, ACTOR_STEERING_CMD) = 0;
    }
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
 * FindNearestRoutePeer: search all actors for nearest same-lane,
 * same-route actor within 0x21 (33) spans.
 * Returns peer slot index, or the actor's own index if none found.
 */
int td5_ai_find_nearest_route_peer(int *route_state_ptr) {
    int slot = route_state_ptr[RS_SLOT_INDEX];
    char *self = actor_ptr(slot);
    int16_t self_span = ACTOR_I16(self, ACTOR_SPAN_RAW);
    uint8_t self_lane = ACTOR_U8(self, ACTOR_SUB_LANE_INDEX);
    int self_selector = route_state_ptr[RS_ROUTE_TABLE_SELECTOR];
    int self_polarity = route_state_ptr[RS_DIRECTION_POLARITY];

    int best_slot = slot; /* default: return own index */
    int32_t best_dist = 0x22; /* must be < 0x22 (34) to beat */

    int i;
    for (i = 0; i < g_active_actor_count; i++) {
        int32_t *peer_rs;
        int16_t peer_span;
        int32_t dist;

        if (i == slot) continue;

        peer_rs = route_state(i);

        /* Must be same route table */
        if (peer_rs[RS_ROUTE_TABLE_SELECTOR] != self_selector) continue;

        /* Must be same lane */
        if (ACTOR_U8(actor_ptr(i), ACTOR_SUB_LANE_INDEX) != self_lane) continue;

        peer_span = ACTOR_I16(actor_ptr(i), ACTOR_SPAN_RAW);

        /* Direction-aware distance: ahead for same-dir, behind for oncoming */
        if (self_polarity == 0) {
            dist = peer_span - self_span; /* same direction: ahead is positive */
        } else {
            dist = self_span - peer_span; /* oncoming: "behind" is ahead for us */
        }

        if (dist <= 0 || dist >= 0x22) continue;

        if (dist < best_dist) {
            best_dist = dist;
            best_slot = i;
        }
    }

    return best_slot;
}

/**
 * RecycleTrafficActorFromQueue: find the traffic slot furthest behind the
 * player (>= 41 spans), and reinitialize it from the next queue entry
 * that is >= 40 spans ahead of the player.
 *
 * Slot 9 is protected if the encounter system is using it.
 */
void td5_ai_recycle_traffic_actor(void) {
    int traffic_max, i;
    int best_slot = -1;
    int32_t best_behind = 0;
    int16_t player_span;
    char *p0 = actor_ptr(0);

    player_span = ACTOR_I16(p0, ACTOR_SPAN_RAW);

    traffic_max = g_active_actor_count;
    if (traffic_max > TD5_MAX_TOTAL_ACTORS)
        traffic_max = TD5_MAX_TOTAL_ACTORS;

    /* Find the traffic slot most behind the player (>= 0x29 = 41 spans) */
    for (i = TD5_MAX_RACER_SLOTS; i < traffic_max; i++) {
        int32_t dist;
        int16_t ts;

        /* Slot 9 protection: never recycle if encounter is active */
        if (i == 9 && g_encounter_tracked_handle != -1) continue;

        ts = ACTOR_I16(actor_ptr(i), ACTOR_SPAN_RAW);
        dist = player_span - ts;

        if (dist >= 0x29 && dist > best_behind) {
            best_behind = dist;
            best_slot = i;
        }
    }

    if (best_slot < 0) return; /* nothing to recycle */

    /* Scan the traffic queue for the next entry >= 40 spans ahead of player */
    if (!g_traffic_queue_ptr || !g_traffic_queue_base) return;

    {
        const uint8_t *qp = g_traffic_queue_ptr;
        int16_t q_span;

        /* Read the span from the queue entry (first 2 bytes, little-endian int16) */
        q_span = (int16_t)(qp[0] | (qp[1] << 8));

        /* Skip entries too close or behind the player */
        while (q_span != -1) {
            int32_t ahead = q_span - player_span;
            if (ahead >= 0x28) break; /* found a valid spawn point */

            qp += 4; /* advance to next 4-byte record */
            q_span = (int16_t)(qp[0] | (qp[1] << 8));
        }

        if (q_span == -1) return; /* end of queue */

        /* Reinitialize the recycled slot from this queue entry */
        {
            char *a = actor_ptr(best_slot);
            int32_t *rs = route_state(best_slot);
            uint8_t q_flags = qp[2];
            uint8_t q_lane  = qp[3];
            int16_t old_span = ACTOR_I16(a, ACTOR_SPAN_RAW);

            /* Set direction polarity from flags bit 0 */
            rs[RS_DIRECTION_POLARITY] = q_flags & 1;

            /* Route table selection: lane < span's lane count -> LEFT, else RIGHT */
            /* Simplified: use lane index directly */
            rs[RS_ROUTE_TABLE_SELECTOR] = (q_lane >= 2) ? 1 : 0;

            /* Place the actor at the queue span — full world placement */
            ACTOR_I16(a, ACTOR_SPAN_RAW) = q_span;
            *(int16_t *)(a + 0x082) = q_span;  /* span_norm  */
            *(int16_t *)(a + 0x084) = q_span;  /* span_accum */
            *(int16_t *)(a + 0x086) = q_span;  /* span_high  */
            ACTOR_U8(a, ACTOR_SUB_LANE_INDEX) = q_lane;

            /* Compute world position from strip geometry [CONFIRMED @ 0x4354E0] */
            {
                int32_t world_x, world_y, world_z;
                if (td5_track_get_span_lane_world(q_span, q_lane, &world_x, &world_y, &world_z)) {
                    ACTOR_I32(a, ACTOR_WORLD_POS_X) = world_x;
                    *(int32_t *)(a + 0x200) = world_y;
                    ACTOR_I32(a, ACTOR_WORLD_POS_Z) = world_z;
                }
            }

            /* Compute heading + oncoming flip */
            td5_track_compute_heading((TD5_Actor *)a);
            if (q_flags & 1) {
                ACTOR_I32(a, ACTOR_YAW_ACCUM) += 0x80000;
            }

            /* Build rotation matrix from heading (+0x120, float[9]) */
            {
                int32_t yaw12 = (ACTOR_I32(a, ACTOR_YAW_ACCUM) >> 8) & 0xFFF;
                float cf = (float)ai_cos_fixed12(yaw12) * (1.0f / 4096.0f);
                float sf = (float)ai_sin_fixed12(yaw12) * (1.0f / 4096.0f);
                float *rm = (float *)(a + 0x120);
                memset(rm, 0, 9 * sizeof(float));
                rm[0] =  cf;
                rm[2] =  sf;
                rm[4] =  1.0f;
                rm[6] = -sf;
                rm[8] =  cf;
                /* Zero velocities for clean re-spawn */
                ACTOR_I32(a, ACTOR_LIN_VEL_X) = 0;
                ACTOR_I32(a, 0x1D0) = 0; /* linear_velocity_y */
                ACTOR_I32(a, ACTOR_LIN_VEL_Z) = 0;
                ACTOR_I32(a, 0x1C0) = 0; /* angular_velocity_roll */
                ACTOR_I32(a, 0x1C4) = 0; /* angular_velocity_yaw */
                ACTOR_I32(a, 0x1C8) = 0; /* angular_velocity_pitch */
            }

            /* Clear all recovery/state fields */
            g_traffic_recovery_stage[best_slot] = 0;
            ACTOR_I32(a, ACTOR_STEERING_CMD) = 0;
            ACTOR_I32(a, ACTOR_LONGITUDINAL_SPEED) = 0;
            ACTOR_I16(a, ACTOR_ENCOUNTER_STEER) = 0;
            ACTOR_U8(a, ACTOR_BRAKE_FLAG) = 0;
            ACTOR_U8(a, ACTOR_VEHICLE_MODE) = 0;

            rs[RS_TRACK_OFFSET_BIAS] = 0;
            rs[RS_SCRIPT_FLAGS] = 0;
            rs[RS_RECOVERY_STAGE] = 0;

            TD5_LOG_I(LOG_TAG,
                      "Traffic recycled: slot=%d from_span=%d to_span=%d lane=%u flags=0x%02X pos=(%d,%d,%d)",
                      best_slot, old_span, q_span, q_lane, q_flags,
                      ACTOR_I32(a, ACTOR_WORLD_POS_X),
                      *(int32_t *)(a + 0x200),
                      ACTOR_I32(a, ACTOR_WORLD_POS_Z));
        }

        /* Advance queue pointer past this consumed entry */
        g_traffic_queue_ptr = qp + 4;
    }
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

void td5_ai_init_traffic_actors(void) {
    int i;
    const uint8_t *qp = g_traffic_queue_base;

    if (!qp) {
        TD5_LOG_W(LOG_TAG, "init_traffic_actors: g_traffic_queue_base is NULL — traffic slots will remain at origin");
        return;
    }

    g_traffic_queue_ptr = qp;

    for (i = TD5_MAX_RACER_SLOTS; i < TD5_MAX_TOTAL_ACTORS; i++) {
        int16_t q_span;
        uint8_t q_flags, q_lane;
        char *a = actor_ptr(i);
        int32_t *rs = route_state(i);

        q_span = (int16_t)(qp[0] | (qp[1] << 8));
        if (q_span == -1) break; /* end sentinel */

        q_flags = qp[2];
        q_lane  = qp[3];

        /* Direction polarity from flags bit 0 */
        rs[RS_DIRECTION_POLARITY] = q_flags & 1;

        /* Route table selection */
        rs[RS_ROUTE_TABLE_SELECTOR] = (q_lane >= 2) ? 1 : 0;

        /* Place on track — set all 4 span tracking fields like racer spawn */
        ACTOR_I16(a, ACTOR_SPAN_RAW) = q_span;
        *(int16_t *)(a + 0x082) = q_span;  /* span_norm  */
        *(int16_t *)(a + 0x084) = q_span;  /* span_accum */
        *(int16_t *)(a + 0x086) = q_span;  /* span_high  */
        ACTOR_U8(a, ACTOR_SUB_LANE_INDEX) = q_lane;
        ACTOR_U8(a, ACTOR_SLOT_INDEX) = (uint8_t)i;

        /* Compute world position from strip geometry [CONFIRMED @ 0x435A77] */
        {
            int32_t world_x, world_y, world_z;
            if (td5_track_get_span_lane_world(q_span, q_lane, &world_x, &world_y, &world_z)) {
                ACTOR_I32(a, ACTOR_WORLD_POS_X) = world_x;
                /* Use track Y directly — traffic has zero wheel positions
                 * (no carparam.dat), so reset_actor_state's integrate_pose
                 * ground snap probes land at body center and produce
                 * garbage Y ~5800 units underground. */
                *(int32_t *)(a + 0x200) = world_y;
                ACTOR_I32(a, ACTOR_WORLD_POS_Z) = world_z;
            } else {
                TD5_LOG_W(LOG_TAG, "init_traffic: slot=%d span=%d lane=%d — world pos lookup failed", i, q_span, q_lane);
            }
        }

        /* Compute heading from track strip direction [CONFIRMED @ 0x435C00] */
        td5_track_compute_heading((TD5_Actor *)a);

        /* Oncoming traffic: rotate 180 degrees (+ 0x80000 in 20-bit yaw) */
        if (q_flags & 1) {
            ACTOR_I32(a, ACTOR_YAW_ACCUM) += 0x80000;
        }

        /* Build rotation matrix from heading (+0x120, float[9]).
         * Traffic has no wheel positions so skip reset_actor_state (which
         * would corrupt Y via broken ground snap). Build Ry(yaw) only. */
        {
            int32_t yaw12 = (ACTOR_I32(a, ACTOR_YAW_ACCUM) >> 8) & 0xFFF;
            float cf = (float)ai_cos_fixed12(yaw12) * (1.0f / 4096.0f);
            float sf = (float)ai_sin_fixed12(yaw12) * (1.0f / 4096.0f);
            float *rm = (float *)(a + 0x120);
            memset(rm, 0, 9 * sizeof(float));
            rm[0] =  cf;     /* m00 = cos  */
            rm[2] =  sf;     /* m02 = sin  */
            rm[4] =  1.0f;   /* m11 = 1    */
            rm[6] = -sf;     /* m20 = -sin */
            rm[8] =  cf;     /* m22 = cos  */
        }

        /* Clear state */
        g_traffic_recovery_stage[i] = 0;
        ACTOR_I32(a, ACTOR_STEERING_CMD) = 0;
        ACTOR_I32(a, ACTOR_LONGITUDINAL_SPEED) = 0;
        ACTOR_I16(a, ACTOR_ENCOUNTER_STEER) = 0;
        ACTOR_U8(a, ACTOR_BRAKE_FLAG) = 0;
        ACTOR_U8(a, ACTOR_VEHICLE_MODE) = 0;

        rs[RS_TRACK_OFFSET_BIAS] = 0;
        rs[RS_SCRIPT_FLAGS] = 0;
        rs[RS_RECOVERY_STAGE] = 0;
        rs[RS_SLOT_INDEX] = i;
        rs[RS_ENCOUNTER_HANDLE] = -1;

        TD5_LOG_I(LOG_TAG, "init_traffic: slot=%d span=%d lane=%u flags=0x%02X pos=(%d,%d,%d)",
                  i, q_span, q_lane, q_flags,
                  ACTOR_I32(a, ACTOR_WORLD_POS_X),
                  *(int32_t *)(a + 0x200),
                  ACTOR_I32(a, ACTOR_WORLD_POS_Z));

        qp += 4; /* next 4-byte record */
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
 */
void td5_ai_update_traffic_route_plan(int slot) {
    int32_t *rs = route_state(slot);
    char *actor  = actor_ptr(slot);
    int32_t heading, route_heading, hdelta;
    int polarity, peer;

    /* --- Stage 1: Recycle --- */
    td5_ai_recycle_traffic_actor();

    /* --- Stage 2: Heading misalignment check --- */
    heading = ACTOR_I32(actor, ACTOR_YAW_ACCUM) >> 8;
    route_heading = ai_route_heading_for_actor(rs, actor);
    polarity = rs[RS_DIRECTION_POLARITY];

    /* For oncoming traffic, offset the heading comparison by 0x800 (180 deg) */
    if (polarity) {
        hdelta = ((heading + 0x800) - route_heading) & 0xFFF;
    } else {
        hdelta = (heading - route_heading) & 0xFFF;
    }

    /* If > 90 degrees off-course (hdelta in 0x400-0xC00), enter recovery */
    if (hdelta >= 0x400 && hdelta <= 0xC00) {
        /* Set recovery stage to a random nonzero value 1-7 */
        g_traffic_recovery_stage[slot] = (int)(g_ai_frame_counter & 7);
        if (g_traffic_recovery_stage[slot] == 0)
            g_traffic_recovery_stage[slot] = 1;
    }

    /* --- Stage 3: Edge-of-track / recovery bail-out --- */
    {
        int16_t span = ACTOR_I16(actor, ACTOR_SPAN_RAW);
        int recovery = g_traffic_recovery_stage[slot];

        /* If near span 0/end, on alternate route, already recovering, or special */
        if (span <= 1 || recovery != 0 || rs[RS_RECOVERY_STAGE] != 0) {
            /* Brake and return */
            ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 1;
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0;
            return;
        }
    }

    /* --- Stage 4: Normal driving -- constant speed 0x3C (60) --- */
    ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0x3C;
    ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 0;

    /* --- Stage 5: Compute next target span ---
     * For same-direction: look ahead (span + 1)
     * For oncoming: look behind (span - 1)
     * The actual world-space target point computation is handled by
     * the track module (SampleTrackTargetPoint). Here we just update
     * the route heading reference for steering. */
    /* (Route heading is maintained by external track sampling) */

    /* --- Stage 6: Steering --- */
    td5_ai_update_steering_bias(rs, 0x8000);

    /* --- Stage 7: Peer avoidance / yield --- */
    peer = td5_ai_find_nearest_route_peer(rs);

    if (peer != slot) {
        /* Peer found: compute closing rate and brake if needed */
        char *peer_actor = actor_ptr(peer);
        int16_t self_span = ACTOR_I16(actor, ACTOR_SPAN_RAW);
        int16_t peer_span = ACTOR_I16(peer_actor, ACTOR_SPAN_RAW);
        int32_t span_delta, self_speed, peer_speed, speed_delta;
        int32_t ttc;

        if (polarity == 0) {
            span_delta = peer_span - self_span;
        } else {
            span_delta = self_span - peer_span;
        }

        if (span_delta < 1) span_delta = 1;

        self_speed = g_actor_forward_track_component[slot];
        peer_speed = g_actor_forward_track_component[peer];
        speed_delta = self_speed - peer_speed;

        /* Time-to-collision estimate:
         * ttc = (span_delta * peer_speed * 0x5DC) /
         *       (speed_delta + span_delta * 0x5DC) / self_speed
         * Simplified: check if closing and close enough to brake */
        if (speed_delta > 0 && self_speed > 0) {
            ttc = (span_delta * 0x5DC) / (speed_delta > 0 ? speed_delta : 1);
            ttc = (ttc * peer_speed) / (self_speed > 0 ? self_speed : 1);
        } else {
            ttc = 0x2EE00; /* default: far away, no braking needed */
        }

        /* Brake if within closing distance */
        {
            int32_t cur_speed_thresh = self_speed >> 10;
            int32_t ttc_thresh = ttc;

            if (cur_speed_thresh > 0 && ttc_thresh > 0) {
                int32_t diff = cur_speed_thresh - ttc_thresh;
                if (diff < 0) diff = -diff;

                if (diff < 8 && speed_delta > 0) {
                    /* Brake! */
                    ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 1;
                    ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)(-0x100);
                }
            }
        }
    }
    /* If no peer found (peer == slot), ttc defaults to 0x2EE00 -> no brake */
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

/**
 * UpdateSpecialTrafficEncounter: spawn/despawn logic, cooldown, proximity.
 * Called from UpdateRaceActors at end of traffic loop.
 */
void td5_ai_update_special_encounter(void) {
    char *player;
    int32_t player_span, player_speed, player_fwd;
    int32_t *enc_rs;

    if (!g_encounter_enabled) return;

    /* Decrement cooldown timer */
    if (g_encounter_cooldown > 0) {
        g_encounter_cooldown--;
    }

    player = actor_ptr(0);
    player_span  = (int16_t)ACTOR_I16(player, ACTOR_SPAN_RAW);
    player_speed = ACTOR_I32(player, ACTOR_LONGITUDINAL_SPEED);
    player_fwd   = g_actor_forward_track_component[0];

    /* --- No active encounter: check spawn conditions --- */
    if (g_encounter_tracked_handle == -1) {
        int32_t *slot9_rs = route_state(9);
        int16_t slot9_span;
        int32_t span_delta;

        /* All conditions must be true simultaneously */
        if (g_encounter_cooldown != 0) return;

        slot9_span = ACTOR_I16(actor_ptr(9), ACTOR_SPAN_RAW);
        span_delta = player_span - slot9_span;

        /* Player must be exactly 2 spans ahead */
        if (span_delta != 2) return;

        /* Speed threshold: >= 0x15639 */
        if (player_speed < 0x15639) return;

        /* Forward track component > 0 */
        if (player_fwd <= 0) return;

        /* Route table match */
        if (route_state(0)[RS_ROUTE_TABLE_SELECTOR] !=
            g_encounter_route_table_selector) return;

        /* Span distance check: absolute delta < 0x10 (16 spans) */
        {
            int32_t abs_delta = span_delta;
            if (abs_delta < 0) abs_delta = -abs_delta;
            if (abs_delta >= 0x10) return;
        }

        /* --- SPAWN --- */
        g_encounter_tracked_handle = 0; /* target player 0 */
        g_encounter_phase_flag = 0;     /* acquisition phase */
        TD5_LOG_I(LOG_TAG,
                  "Encounter spawn: slot=9 target=%d player_span=%d slot9_span=%d speed=%d",
                  g_encounter_tracked_handle, player_span, slot9_span, player_speed);

        /* Reset slot 9 state */
        {
            char *s9 = actor_ptr(9);
            ACTOR_I32(s9, ACTOR_STEERING_CMD) = 0;
            ACTOR_I32(s9, ACTOR_LONGITUDINAL_SPEED) = 0;
            ACTOR_I16(s9, ACTOR_ENCOUNTER_STEER) = 0;
            ACTOR_U8(s9, ACTOR_BRAKE_FLAG) = 0;
            ACTOR_U8(s9, ACTOR_VEHICLE_MODE) = 0;
            ACTOR_I32(s9, ACTOR_ENCOUNTER_STATE) = 0;
        }

        /* StartTrackedVehicleAudio(9) would be called here */
        return;
    }

    /* --- Active encounter: monitor span distance for despawn --- */
    {
        int16_t enc_span = ACTOR_I16(actor_ptr(9), ACTOR_SPAN_RAW);
        int32_t distance = player_span - enc_span;

        /* Despawn if encounter actor > 0x40 (64) spans behind */
        if (distance > 0x40) {
            goto teardown;
        }

        /* Check for encounter activation: within 1 span on matching route */
        if (distance <= 1 && distance >= -1) {
            int32_t *slot9_rs = route_state(9);
            if (slot9_rs[RS_ROUTE_TABLE_SELECTOR] ==
                route_state(0)[RS_ROUTE_TABLE_SELECTOR]) {
                g_encounter_active[g_encounter_tracked_handle] = 1;
                g_encounter_phase_flag = 1;
                TD5_LOG_I(LOG_TAG,
                          "Encounter active: target=%d distance=%d route_selector=%d",
                          g_encounter_tracked_handle,
                          distance,
                          slot9_rs[RS_ROUTE_TABLE_SELECTOR]);
                /* StopTrackedVehicleAudio() -- transition from approach to active */
            }
        }

        /* Check if encounter actor has passed the player (1 span ahead) */
        if (distance < -1) {
            goto teardown;
        }
    }
    return;

teardown:
    /* --- TEARDOWN --- */
    TD5_LOG_I(LOG_TAG, "Encounter despawn: target=%d cooldown=%d",
              g_encounter_tracked_handle, 300);
    g_encounter_phase_flag = 0;
    g_encounter_tracked_handle = -1;
    g_encounter_cooldown = 300; /* 300-frame cooldown */

    /* Clear encounter state on slot 9 */
    {
        char *s9 = actor_ptr(9);
        ACTOR_I32(s9, ACTOR_STEERING_CMD) = 0;
        ACTOR_I32(s9, ACTOR_LONGITUDINAL_SPEED) = 0;
        ACTOR_I16(s9, ACTOR_ENCOUNTER_STEER) = 0;
        ACTOR_U8(s9, ACTOR_BRAKE_FLAG) = 0;
        ACTOR_U8(s9, ACTOR_VEHICLE_MODE) = 0;
    }

    /* StopTrackedVehicleAudio() would be called here */

    /* Increment encounter completion counter */
    {
        char *s9 = actor_ptr(9);
        ACTOR_I32(s9, ACTOR_ENCOUNTER_STATE) += 1;
    }

    /* Clear per-actor encounter active flags */
    {
        int i;
        for (i = 0; i < TD5_MAX_TOTAL_ACTORS; i++) {
            g_encounter_active[i] = 0;
        }
    }
    g_encounter_control_active_latch = 0;
    g_encounter_steer_bias_latch = 0;
}

/**
 * UpdateSpecialEncounterControl: per-frame active encounter override.
 * Called from UpdatePlayerVehicleControlState when encounter is active.
 *
 * Overrides player control: heading alignment check, steering/brake force.
 */
void td5_ai_update_encounter_control(int slot) {
    char *actor = actor_ptr(slot);
    char *enc   = actor_ptr(9);
    int32_t *rs = route_state(slot);

    int32_t enc_heading, target_heading, hdelta_enc, hdelta_target;
    int32_t fwd_comp;

    /* Set latches */
    g_encounter_control_active_latch = 1;
    g_encounter_steer_bias_latch = 0xFF00;

    /* Compute heading deltas for alignment check */
    enc_heading    = ACTOR_I32(enc, ACTOR_YAW_ACCUM) >> 8;
    target_heading = ai_route_heading_for_actor(rs, actor);

    hdelta_enc    = (enc_heading - target_heading) & 0xFFF;
    hdelta_target = ((ACTOR_I32(actor, ACTOR_YAW_ACCUM) >> 8) - target_heading) & 0xFFF;

    fwd_comp = g_actor_forward_track_component[9];

    /* Alignment check: encounter stays active if forward > 8 and headings
     * are within ~90 degrees of route direction */
    if (fwd_comp <= 8) goto deactivate;

    /* Both heading deltas must be < 0x400 or > 0xC00 (within 90 deg) */
    if (hdelta_enc >= 0x400 && hdelta_enc <= 0xC00) goto deactivate;
    if (hdelta_target >= 0x400 && hdelta_target <= 0xC00) goto deactivate;

    /* Encounter still valid: apply steering/brake override */
    {
        int16_t enc_span    = ACTOR_I16(enc, ACTOR_SPAN_RAW);
        int16_t player_span = ACTOR_I16(actor, ACTOR_SPAN_RAW);
        int32_t span_delta  = player_span - enc_span;

        if (fwd_comp > 0 && span_delta < 3) {
            /* Close enough: force brake on encounter actor */
            ACTOR_I16(enc, ACTOR_ENCOUNTER_STEER) = (int16_t)(-0x100);
            ACTOR_U8(enc, ACTOR_BRAKE_FLAG) = 1;
        } else {
            ACTOR_I16(enc, ACTOR_ENCOUNTER_STEER) = 0;
        }
    }
    return;

deactivate:
    /* --- Encounter deactivation / teardown --- */
    g_encounter_active[slot] = 0;
    route_state(slot)[RS_DIRECTION_POLARITY] = 0;
    g_encounter_steer_bias_latch = 0;
    g_encounter_control_active_latch = 0;
    g_encounter_tracked_handle = -1;
    g_encounter_cooldown = 300; /* 300-frame cooldown */

    /* Increment encounter completion counter on the player */
    ACTOR_I32(actor, ACTOR_ENCOUNTER_STATE) += 1;
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

void td5_ai_update_race_actors(void) {
    int i;

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
}

/**
 * Dispatch a single racer slot based on its state.
 */
static void ai_update_single_racer(int slot) {
    char *actor = actor_ptr(slot);
    int state = g_slot_state[slot];

    switch (state) {
    case 0x00: /* AI racer */
        /* Wanted mode check [CONFIRMED @ 0x436E1D]:
         * Original: if (g_wantedModeEnabled == 0 || *local_8 != 0)
         * AI runs track behavior when wanted mode is OFF, or when the actor
         * has a non-zero damage/encounter state.  Skip only when wanted mode
         * is ON and the actor has zero encounter state. */
        if (g_td5.wanted_mode_enabled &&
            ACTOR_I32(actor, ACTOR_ENCOUNTER_STATE) == 0) {
            return;
        }

        /* Drag race: AI does not use track behavior (straight-line only) */
        if (g_td5.drag_race_enabled) {
            return;
        }

        td5_ai_update_track_behavior(slot);
        /* UpdateVehicleActor would follow here in the physics module */
        break;

    case 0x01: /* Player: skip AI update */
        break;

    case 0x02: /* Finished/dead: brake behavior */
        {
            int32_t fwd = g_actor_forward_track_component[slot];
            if (fwd < 0) {
                /* Moving backward: zero controls */
                ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0;
            } else {
                /* Moving forward: brake */
                ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 1;
                ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)(-0x100);
            }
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
