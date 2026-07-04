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

/* Per-slot wanted/cop-chase damage state (orig gWantedDamageStateTable
 * @ 0x004bead4). Used by the smoke-spawn gate at td5_render.c that
 * mirrors orig 0x0040C79C: smoke emits only when this is 0. */
extern int16_t g_wanted_damage_state[TD5_MAX_RACER_SLOTS];

/* Returns a pointer to the 128-byte AI physics tuning template (DAT_00473DB0
 * in TD5_d3d.exe). Shared across all AI slots in the original. Used by the
 * physics init to point AI actors' tuning_data_ptr at this instead of each
 * slot's per-car carparam.dat tuning — the original bicycle solve was built
 * around these Wf=400/Wr=400/I=180000 values and flipping to per-car tuning
 * flipped the sign of D in the bicycle-solve matrix. */
uint8_t *td5_ai_get_physics_template(void);

/* --- Per-actor AI update --- */
void td5_ai_update_track_behavior(int slot);

/* [AI UNSTICK] Physics calls this from the V2V collision response to tell the
 * AI that `slot` is touching `peer` this tick. The racer unstick uses a leaky
 * accumulator of these to detect a persistent car-vs-car grind. Safe no-op for
 * out-of-range slots; cheap (two field writes). */
void td5_ai_note_v2v_contact(int slot, int peer);
void td5_ai_update_steering_bias(int *route_state, int32_t steer_weight);
int  td5_ai_update_route_threshold(int slot);
void td5_ai_update_track_offset_bias(int slot);
int  td5_ai_find_offset_peer(int *route_state);
void td5_ai_seed_actor_track_progress_offset(int slot);

/* --- Script VM (12 opcodes) --- */
int  td5_ai_advance_track_script(int *route_state);

/* --- Rubber-band system --- */
void td5_ai_compute_rubber_band(void);

/* Effective CATCHUP / rubber-band assist level (0 = off, 1..9 = on/softened).
 * Resolves the td5re.ini override (>=0 wins) else the persisted S05 toggle
 * value (default 1). [S06 2026-06-04 catchup restore] */
int  td5_ai_get_catchup_level(void);

/* --- Initialization --- */
void td5_ai_bind_actor_table(void *actor_base);
void td5_ai_set_route_tables(const uint8_t *left_route, size_t left_size,
                             const uint8_t *right_route, size_t right_size);
void     td5_ai_refresh_route_state(void);
void     td5_ai_init_race_actor_runtime(void);
int32_t *td5_ai_get_route_state(int slot);
/* Correct an AI actor's spawn heading to match the LEFT.TRK route byte.
 * Called from td5_game.c after td5_track_compute_heading() for AI slots.
 * Prevents the recovery-script loop that keeps throttle=0 forever. */
void td5_ai_correct_spawn_heading(int slot);

/* --- Traffic --- */
void td5_ai_recycle_traffic_actor(void);
void td5_ai_init_traffic_actors(void);
void td5_ai_update_traffic_route_plan(int slot);
int  td5_ai_find_nearest_route_peer(int *route_state);
/* Bind the per-level traffic spawn queue (TRAFFIC.BUS). 4-byte records:
 * int16 span, u8 flags, u8 lane — terminated by span == -1. The pointer
 * must outlive the race; pass NULL to clear. */
void td5_ai_set_traffic_queue(const uint8_t *data, int size);

/* --- Dynamic (GTA-style) traffic spawner [PORT ENHANCEMENT 2026-06-11] ---
 * Gated by [Traffic] Dynamic + the track-select Traffic volume row.
 * Distance-driven spawn/despawn with render fade; see td5_ai.c module
 * comment. Dynamic=0 keeps the faithful queue init/recycle untouched. */
int  td5_ai_traffic_dynamic_active(void);
/* 1 when `slot` is a traffic slot currently parked (despawned). Consumers
 * (traffic physics, V2V broadphase) skip parked slots entirely. */
int  td5_ai_traffic_dynamic_parked(int slot);
/* Render/audio fade for `slot`: 0 = hidden/parked, 255 = fully visible.
 * Always 255 for racer slots or when dynamic traffic is off. */
int  td5_ai_traffic_get_draw_alpha(int slot);
/* [PER-VIEWPORT TRAFFIC] 1 when per-viewport traffic partitioning is active this
 * race (each viewport has its own independent traffic partition). [MP TIME TRIAL
 * removed 2026-07-04] The only mode that ever enabled this was split-screen TIME
 * TRIAL, so this now always returns 0 — kept only because callers still probe it. */
int  td5_ai_traffic_per_viewport_active(void);
/* Viewport that OWNS traffic `slot` (0..viewport_count-1); -1 = shared/no-gating
 * (per-viewport off, or a racer slot). Render uses this to draw each viewport
 * only its own traffic. */
int  td5_ai_traffic_slot_owner_vp(int slot);
/* 1 when a collision between `slot_a` and `slot_b` must be SUPPRESSED for
 * per-viewport isolation (a traffic car only touches its own viewport's player,
 * and cross-partition traffic-traffic pairs never collide). 0 when per-vp off. */
int  td5_ai_traffic_pair_blocked(int slot_a, int slot_b);
/* Once-per-sim-tick driver (called from td5_ai_pre_tick). */
void td5_ai_traffic_dynamic_tick(void);
/* Race-start seeding (called from td5_ai_init_traffic_actors). */
void td5_ai_traffic_dynamic_race_init(void);

/* --- Wanted mode (cop chase game type 8) --- */
/* Award damage to a SUSPECT on cop<->suspect V2V collision.
 * Mirrors AwardWantedDamageScore @ 0x43D690. Decrements gWantedDamageStateTable
 * by 0x200 (impact<=20000) or 0x400 (impact>20000). When the suspect's state
 * reaches 0 it is arrested (its AI frozen), the arrest is credited to the
 * ramming `cop_slot` (so multi-cop chases score per-cop), and its arrest time
 * is stamped. Returns 1 when THIS hit completed the arrest, else 0 (lets the
 * caller fire the arrest force-feedback jolt for both cop and suspect).
 * Called from td5_physics.c. */
int td5_ai_wanted_cop_hit(int cop_slot, int suspect_slot, int32_t impact_mag);

/* Slot whose DAMAGE bar the HUD shows (last-rammed suspect 1..5, or -1).
 * Mirrors g_wantedDamageHudOverlayCount @ 0x004bf504. Read by td5_hud.c. */
int  td5_ai_get_wanted_overlay_slot(void);
/* Reset per-race cop-chase transient state (overlay slot -> -1). */
void td5_ai_reset_wanted_state(void);

/* --- Special encounter (cop chase) --- */
void td5_ai_update_special_encounter(void);
void td5_ai_update_encounter_control(int slot);
/* Per-slot encounter latch (g_encounter_active[slot] != 0) [CONFIRMED @ 0x00403180].
 * Original gates the player-input encounter-control branch on a per-slot field
 * at stride 0x11c, NOT on the global g_wantedModeEnabled flag. Mirroring that
 * here so the player's throttle write is not blocked for the entire Cop Chase
 * session (only while a tracked encounter is actually engaged). */
int  td5_ai_is_encounter_active(int slot);
/* |ACTOR_LONGITUDINAL_SPEED| (raw 24.8 fixed, td5_ai.c) at/under which a
 * pulled-over target counts as stopped and the chase scheduler releases
 * the hold (td5_ai.c: td5_ai_update_special_encounter, COP_PULLOVER phase).
 * Exported so td5_input.c's player-side coast-hold branch uses the SAME
 * "stopped" threshold instead of an independently-hardcoded value — see
 * the 2026-07-02 police-brake-deadzone fix. */
#define COP_STOP_SPEED  0x2000

/* --- Police chase rewrite (2026-06-19) — cosmetic-layer queries ---------
 * The chase logic is deterministic sim state (td5_ai.c); these are read by
 * the per-frame render/audio path. See the "Police chase rewrite" block in
 * td5_ai.c. */
/* 1 when `slot` is a cop currently chasing (or pulling over) a racer. */
int  td5_ai_cop_is_chasing(int slot);
/* [COP OVERHAUL 2026-06-29] Q8 (0x100 = 1.0) propulsion multiplier for a chasing
 * traffic-slot cop, applied to the throttle force in td5_physics_update_traffic
 * so the cop out-accelerates a faster suspect on straights (acceleration
 * rubber-band; the simplified traffic model has no engine/top-speed term). 1.0
 * for non-cops / idle cops / at the catch-up cap / an imminent corner.
 * Deterministic (replicated cop+target speeds) -> lockstep-safe. */
int  td5_ai_cop_chase_throttle_boost_q8(int slot);
/* [MP COP CHASE 2026-06-22] Arm a racer slot as an AI-driven cop (is_ai=0 =
 * human cop, no-op). The chase driver takes the slot over each tick. */
void td5_ai_cop_chase_setup(int cop_slot, int is_ai);
/* Steady strobe intensity (0..0x1000) for the cop-light glow; 0 if not chasing. */
int  td5_ai_cop_glow_intensity(int slot);
/* 1 when `slot` (racer or traffic/cop) is in the broken-down/parked state. */
int  td5_ai_actor_is_broken_down(int slot);
/* 1 when `slot` is currently being chased by a cop (read by td5_physics.c). */
int  td5_ai_actor_is_pursued(int slot);
/* 1 when `slot` is a cop car (any phase) — td5_render.c draws the police mesh. */
int  td5_ai_actor_is_cop(int slot);
/* Flag `slot` broken down for cop_smoke_ticks (called from td5_physics.c on a
 * hard traffic/cop collision). */
void td5_ai_mark_actor_broken_down(int slot);
/* Clear the broken-down/parked flag for `slot` (called by the manual reset-car
 * recovery so a knocked-out racer is no longer treated as wrecked). */
void td5_ai_clear_actor_broken_down(int slot);
/* Nearest chasing cop to a listener world pos (24.8), or -1 — drives the siren. */
int  td5_ai_nearest_chasing_cop(int32_t listener_x, int32_t listener_z);

/* 0x434040: Compute actor route heading delta */
uint32_t td5_compute_heading_delta(void *route_entry);

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
