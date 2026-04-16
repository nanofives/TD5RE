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

#endif /* TD5_TRACE_H */
