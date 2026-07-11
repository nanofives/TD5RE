/* td5_ai_internal.h -- PRIVATE seam between td5_ai.c and td5_ai_traffic.c
 * (S5 module split, see REFACTOR_PLAN.md).
 *
 * Route-state / actor addressing macros and the shared cross-file globals
 * the traffic subsystem needs. Not a public API -- only td5_ai.c and
 * td5_ai_traffic.c include this. Declarations here must match td5_ai.c's
 * definitions verbatim (same rule as td5_physics_internal.h).
 */

#ifndef TD5_AI_INTERNAL_H
#define TD5_AI_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include "td5_types.h"

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
 * ======================================================================== */

#define ACTOR_STRIDE              0x388

#define ACTOR_SPAN_RAW            0x080
#define ACTOR_SPAN_NORMALIZED     0x082
#define ACTOR_SPAN_ACCUM          0x084
#define ACTOR_SUB_LANE_INDEX      0x08C
#define ACTOR_CAR_DEF_PTR         0x1B8
#define ACTOR_YAW_ACCUM           0x1F4
#define ACTOR_STEERING_CMD        0x30C
#define ACTOR_LONGITUDINAL_SPEED  0x314
#define ACTOR_REAR_AXLE_SLIP      0x320
#define ACTOR_STEERING_RAMP_ACCUM 0x33A
#define ACTOR_LIN_VEL_X           0x1CC
#define ACTOR_LIN_VEL_Z           0x1D4
#define ACTOR_WORLD_POS_X         0x1FC
#define ACTOR_WORLD_POS_Z         0x204
#define ACTOR_ENCOUNTER_STEER     0x33E
#define ACTOR_BRAKE_FLAG          0x36D
#define ACTOR_THROTTLE_STATE      0x36F
#define ACTOR_SLOT_INDEX          0x375
#define ACTOR_VEHICLE_MODE        0x379
#define ACTOR_TRACK_CONTACT_FLAG  0x37B
#define ACTOR_ENCOUNTER_STATE     0x384

#define ACTOR_PROBE_FL_BASE       0x090
#define ACTOR_PROBE_FR_BASE       0x09C
#define ACTOR_PROBE_RL_BASE       0x0A8
#define ACTOR_PROBE_RR_BASE       0x0B4

/* ========================================================================
 * Helper accessors (cast through char* arithmetic) -- shared state, defined
 * (non-static) in td5_ai.c
 * ======================================================================== */

extern int32_t *g_route_state_base;     /* 0x4AFB60 */
extern char    *g_actor_base;           /* 0x4AB108 */
extern const uint8_t *g_route_tables[2];
extern size_t         g_route_table_sizes[2];

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
 * Shared cross-file globals (defined non-static in td5_ai.c; the traffic
 * subsystem in td5_ai_traffic.c reads/writes these).
 * ======================================================================== */

extern int32_t g_active_actor_count;
extern int32_t g_actor_forward_track_component[TD5_MAX_TOTAL_ACTORS];
extern int32_t g_slot_state[TD5_MAX_TOTAL_ACTORS];
extern int32_t g_traffic_recovery_stage[TD5_MAX_TOTAL_ACTORS];
extern int8_t  g_cop_target[TD5_MAX_TOTAL_ACTORS];
extern uint8_t g_cop_is_cop[TD5_MAX_TOTAL_ACTORS];
extern uint8_t g_cop_phase[TD5_MAX_TOTAL_ACTORS];
extern uint8_t g_actor_broken_down[TD5_MAX_TOTAL_ACTORS];
extern int16_t g_actor_broken_ticks[TD5_MAX_TOTAL_ACTORS];
extern int32_t g_encounter_tracked_handle;
extern int32_t g_encounter_enabled;
extern int32_t s_cop_spawn_counter;
extern int8_t  s_traffic_escape_lane[TD5_MAX_TOTAL_ACTORS];
extern int16_t s_traffic_escape_lane_ttl[TD5_MAX_TOTAL_ACTORS];
extern int8_t  s_traffic_lane_bias[TD5_MAX_TOTAL_ACTORS];
extern const uint8_t *g_traffic_queue_base;
extern const uint8_t *g_traffic_queue_ptr;
extern uint32_t g_ai_frame_counter;

/* Cop chase phase (shared enum -- master dispatcher + traffic subsystem both
 * read/write g_cop_phase against these values). */
enum {
    COP_IDLE     = 0,
    COP_CHASING  = 1,
    COP_PULLOVER = 2
};

/* Smart-AI perception structs (shared -- Smart Traffic S20 reuses the same
 * corner/sensing evaluation as the racer Smart Opponent AI overhaul). */
typedef struct {
    int    turn_dir;
    double sharpness;
    int    apex_d;
    double u_line;
    double speed_cap;
} SmartCorner;

typedef struct {
    double avoid_u;
    int    danger;
    int    car_ahead;
    double front_clear;
    int    wall_imminent;
    double span_len;
} SmartSense;

/* ========================================================================
 * Functions defined in td5_ai_traffic.c, called from the remaining
 * td5_ai.c (master dispatcher / race-runtime-init call sites).
 * ======================================================================== */

void td5_traffic_smart_reset(void);
void traffic_collision_escape(int slot);
void racer_collision_escape(int slot);

/* ========================================================================
 * Functions defined in the remaining td5_ai.c (racer AI / Smart Opponent AI),
 * called from td5_ai_traffic.c's smart-traffic and cop-chase logic.
 * ======================================================================== */

int32_t ai_angle_from_vector(int32_t dx, int32_t dz);
int     ai_peer_is_present(int i);
void    cop_state_reset(void);
void    cop_release(int slot);
int     traffic_diag_enabled(void);
int     td5_ai_smart_active(void);
int     td5_ai_smart_traffic_cruise_scale(int slot);
float   td5_ai_smart_skill(int slot);
void    smart_corner_eval(int slot, int span_raw, int span_count,
                          double u_base, float skill, SmartCorner *out);
void    smart_sense(int slot, int span_raw, int span_count,
                    float skill, SmartSense *out);
int     td5_ai_smart_traffic_lane(int slot, int target_span,
                                   int lane_count, int base_sub_lane,
                                   int polarity);
int32_t td5_ai_td6_steer_weight(int32_t w);

#endif /* TD5_AI_INTERNAL_H */
