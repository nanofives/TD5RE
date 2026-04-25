/**
 * td5_ai.h -- AI routing, rubber-banding, traffic, script VM
 *
 * 4-layer AI pipeline:
 *   Layer 1: Route selection (LEFT.TRK / RIGHT.TRK)
 *   Layer 2: Target point sampling (SampleTrackTargetPoint)
 *   Layer 3: Heading delta & steering bias (UpdateActorSteeringBias)
 *   Layer 4: Track script override (AdvanceActorTrackScript, 12 opcodes)
 *
 * Original functions:
 *   0x434FE0  UpdateActorTrackBehavior (main AI tick)
 *   0x4340C0  UpdateActorSteeringBias
 *   0x4370A0  AdvanceActorTrackScript (12-opcode bytecode VM)
 *   0x434900  UpdateActorTrackOffsetBias (peer avoidance)
 *   0x4337E0  FindActorTrackOffsetPeer
 *   0x434AA0  UpdateActorRouteThresholdState (throttle/brake control)
 *   0x432D60  ComputeAIRubberBandThrottle
 *   0x432E60  InitializeRaceActorRuntime (AI config, difficulty)
 *   0x434DA0  UpdateSpecialTrafficEncounter (cop chase NPC)
 *   0x434BA0  UpdateSpecialEncounterControl
 *   0x433680  FindNearestRoutePeer (traffic peer search)
 *   0x4353B0  RecycleTrafficActorFromQueue
 *   0x435940  InitializeTrafficActorsFromQueue
 *   0x435E80  UpdateTrafficRoutePlan
 *   0x436A70  UpdateRaceActors (master dispatcher)
 */

#ifndef TD5_AI_H
#define TD5_AI_H

#include <stddef.h>
#include "td5_types.h"

/* --- Module lifecycle --- */
int  td5_ai_init(void);
void td5_ai_shutdown(void);
void td5_ai_tick(void);

/* Returns a pointer to the 128-byte AI physics tuning template (DAT_00473DB0
 * in TD5_d3d.exe). Shared across all AI slots in the original. Used by the
 * physics init to point AI actors' tuning_data_ptr at this instead of each
 * slot's per-car carparam.dat tuning — the original bicycle solve was built
 * around these Wf=400/Wr=400/I=180000 values and flipping to per-car tuning
 * flipped the sign of D in the bicycle-solve matrix. */
uint8_t *td5_ai_get_physics_template(void);

/* --- Per-actor AI update --- */
void td5_ai_update_track_behavior(int slot);
void td5_ai_update_steering_bias(int *route_state, int32_t steer_weight);
int  td5_ai_update_route_threshold(int slot);
void td5_ai_update_track_offset_bias(int slot);
int  td5_ai_find_offset_peer(int *route_state);

/* --- Script VM (12 opcodes) --- */
int  td5_ai_advance_track_script(int *route_state);

/* --- Rubber-band system --- */
void td5_ai_compute_rubber_band(void);

/* --- Initialization --- */
void td5_ai_bind_actor_table(void *actor_base);
void td5_ai_set_route_tables(const uint8_t *left_route, size_t left_size,
                             const uint8_t *right_route, size_t right_size);
void     td5_ai_refresh_route_state(void);
void     td5_ai_init_race_actor_runtime(void);
int32_t *td5_ai_get_route_state(int slot);

/* --- Traffic --- */
void td5_ai_recycle_traffic_actor(void);
void td5_ai_init_traffic_actors(void);
void td5_ai_update_traffic_route_plan(int slot);
int  td5_ai_find_nearest_route_peer(int *route_state);
/* Bind the per-level traffic spawn queue (TRAFFIC.BUS). 4-byte records:
 * int16 span, u8 flags, u8 lane — terminated by span == -1. The pointer
 * must outlive the race; pass NULL to clear. */
void td5_ai_set_traffic_queue(const uint8_t *data, int size);

/* --- Special encounter (cop chase) --- */
void td5_ai_update_special_encounter(void);
void td5_ai_update_encounter_control(int slot);

/* --- Master dispatcher --- */
void td5_ai_update_race_actors(void);

/* --- Per-frame AI setup (rubber-band + route refresh, no per-actor work) --- */
void td5_ai_pre_tick(void);

/* --- Per-actor AI dispatch (call before physics for each slot) --- */
void td5_ai_update_actor(int slot);

/* ========================================================================
 * Route State Array (per-actor, stride 0x47 dwords = 0x11C bytes)
 *
 * Base: 0x4AFB60. Offsets within each entry (dword indices):
 *   [0x00] route_table_ptr      -- pointer to LEFT.TRK or RIGHT.TRK
 *   [0x03] route_table_selector -- 0=LEFT, 1=RIGHT
 *   [0x04] default_throttle     -- copied from live throttle table each tick
 *   [0x09] track_offset_bias    -- lateral offset for peer avoidance
 *   [0x0E] left_boundary_A
 *   [0x0F] left_boundary_B
 *   [0x10] right_boundary_A
 *   [0x11] right_boundary_B
 *   [0x12] right_extent_A
 *   [0x13] right_extent_B
 *   [0x14] active_lower_bound
 *   [0x15] active_upper_bound
 *   [0x16] left_deviation
 *   [0x17] right_deviation
 *   [0x18] forward_track_component
 *   [0x19] track_progress
 *   [0x1B] script_offset_param
 *   [0x1F] encounter_tracked_handle -- -1 = none
 *   [0x22] recovery_stage
 *   [0x25] direction_polarity  -- 0=same, 1=oncoming
 *   [0x35] slot_index
 *   [0x3A] script_base_ptr
 *   [0x3B] script_ip
 *   [0x3C] script_speed_param
 *   [0x3D] script_flags
 *   [0x3E] script_field_3E
 *   [0x43] script_field_43
 *   [0x45] script_countdown
 * ======================================================================== */

/* ========================================================================
 * Actor Runtime State Array (per-actor, stride 0xE2 dwords = 0x388 bytes)
 *
 * Base: 0x4AB108 (same as actor base, recast for field access).
 *
 * Key field offsets used by AI (byte offsets from actor base):
 *   0x080: track_span_raw (short)
 *   0x082: track_span_normalized (short)
 *   0x08C: track_sub_lane_index (byte)
 *   0x1F4: euler_accum.yaw (int)
 *   0x30C: steering_command (int)
 *   0x314: longitudinal_speed (int)
 *   0x33E: encounter_steering_cmd (short)
 *   0x36D: brake_flag (byte)
 *   0x36F: throttle_state (byte)
 *   0x375: slot_index (byte)
 *   0x379: vehicle_mode (byte)
 *   0x384: special_encounter_state (int)
 * ======================================================================== */

#endif /* TD5_AI_H */
