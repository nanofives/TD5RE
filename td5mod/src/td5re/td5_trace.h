/*
 * td5_trace.h -- Lightweight race-trace harness for differential validation
 *
 * The goal is not to replace logs. It emits structured per-stage snapshots
 * that can be diffed against traces captured from the retail binary.
 */

#ifndef TD5_TRACE_H
#define TD5_TRACE_H

#include <stdint.h>

typedef struct TD5_TraceFrameState {
    int game_state;
    int paused;
    int pause_menu_active;
    int fade_state;
    int countdown_timer;
    uint32_t sim_time_accumulator;
    float sim_tick_budget;
    float frame_dt;
    float instant_fps;
    int viewport_count;
    int split_screen_mode;
    int ticks_this_frame;
} TD5_TraceFrameState;

typedef struct TD5_TraceActorState {
    int slot;
    int slot_state;
    int slot_companion_1;
    int slot_companion_2;
    int view_target;
    int32_t world_x;
    int32_t world_y;
    int32_t world_z;
    int32_t vel_x;
    int32_t vel_y;
    int32_t vel_z;
    int32_t ang_roll;
    int32_t ang_yaw;
    int32_t ang_pitch;
    int16_t disp_roll;
    int16_t disp_yaw;
    int16_t disp_pitch;
    int16_t span_raw;
    int16_t span_norm;
    int16_t span_accum;
    int16_t span_high;
    int32_t steering_cmd;
    int32_t engine_speed;
    int32_t long_speed;
    int32_t lat_speed;
    int32_t front_slip;
    int32_t rear_slip;
    int32_t finish_time;
    int32_t accum_distance;
    uint16_t pending_finish_timer;
    uint8_t current_gear;
    uint8_t vehicle_mode;
    uint8_t track_contact_flag;
    uint8_t wheel_contact_mask;
    uint8_t race_position;
    int16_t metric_checkpoint_index;
    uint8_t metric_checkpoint_mask;
    int16_t metric_normalized_span;
    int16_t metric_timer_ticks;
    int16_t metric_display_position;
    int32_t metric_speed_bonus;
    int32_t metric_top_speed;
} TD5_TraceActorState;

typedef struct TD5_TraceViewState {
    int view_index;
    int actor_slot;
    int32_t cam_world_x;
    int32_t cam_world_y;
    int32_t cam_world_z;
    float cam_x;
    float cam_y;
    float cam_z;
} TD5_TraceViewState;

void td5_trace_init(void);
void td5_trace_shutdown(void);
int td5_trace_begin_frame(uint32_t frame_index);
int td5_trace_is_enabled(void);
int td5_trace_selected_slot(int slot);
void td5_trace_write_frame(uint32_t frame_index, uint32_t sim_tick, const char *stage,
                           const TD5_TraceFrameState *state);
void td5_trace_write_actor(uint32_t frame_index, uint32_t sim_tick, const char *stage,
                           const TD5_TraceActorState *state);
void td5_trace_write_view(uint32_t frame_index, uint32_t sim_tick, const char *stage,
                          const TD5_TraceViewState *state);

/* Per-tick physics-chain capture for the gravity + ground-snap diff.
 * Schema is column-aligned with frida_race_trace.js MOD_PHYSICS so a
 * straightforward column-by-column diff names the first divergent step.
 * Pass a TD5_Actor * here — the function reads the same struct fields
 * the Frida side reads from the actor table at the same byte offsets. */
struct TD5_Actor;
void td5_trace_write_physics(uint32_t frame_index, uint32_t sim_tick,
                             const char *stage,
                             const struct TD5_Actor *actor);

/* -------- call trace (for diff-race hook specs) ----------------------------
 * Emits one row per function invocation, keyed by (sim_tick, fn_name,
 * call_idx). Mirrors Frida-side Interceptor.attach output so the comparator
 * can diff both sides using the same schema. Up to 8 int32 args + optional
 * return value. See re/trace-hooks/README.md.
 *
 * call_idx is assigned internally by td5_trace_write_call: it tracks a
 * per-(sim_tick, fn_name) counter so the Nth call to the same function in
 * the same sim tick gets the same key on both sides.
 */

#define TD5_TRACE_CALL_MAX_ARGS 8

void td5_trace_write_call(uint32_t sim_tick, const char *fn_name,
                          int n_args, const int32_t *args,
                          int has_ret, int32_t ret);

/* Helper exposed for the macros (defined in td5_game.c so td5_trace.c avoids
 * pulling in the full td5re.h for a single counter field). */
int td5_trace_current_sim_tick(void);

/* Variadic convenience macros. Zero runtime cost when tracing is disabled.
 * Requires at least one arg; pass a dummy 0 if the function is nullary.
 *
 *     TD5_TRACE_CALL_ENTER("suspension_step", actor_idx, wheel);
 *     ... body ...
 *     TD5_TRACE_CALL_RET  ("suspension_step", result, actor_idx, wheel);
 */
#define TD5_TRACE_CALL_ENTER(_name, _a0, ...) \
    do { if (td5_trace_is_enabled()) { \
        int32_t _td5_args[] = { (int32_t)(_a0), ##__VA_ARGS__ }; \
        td5_trace_write_call( \
            (uint32_t)td5_trace_current_sim_tick(), (_name), \
            (int)(sizeof(_td5_args) / sizeof(_td5_args[0])), \
            _td5_args, 0, 0); \
    } } while (0)

#define TD5_TRACE_CALL_RET(_name, _ret, _a0, ...) \
    do { if (td5_trace_is_enabled()) { \
        int32_t _td5_args[] = { (int32_t)(_a0), ##__VA_ARGS__ }; \
        td5_trace_write_call( \
            (uint32_t)td5_trace_current_sim_tick(), (_name), \
            (int)(sizeof(_td5_args) / sizeof(_td5_args[0])), \
            _td5_args, 1, (int32_t)(_ret)); \
    } } while (0)

#endif /* TD5_TRACE_H */
