/*
 * td5_trace.h -- Modular race-trace harness for differential validation
 *
 * Splits the legacy monolithic race_trace.csv into narrow per-module CSVs.
 * Each module owns one concern (pose, motion, track, controls, progress,
 * view, frame) and writes only the columns relevant to that concern.
 *
 *   log/race_trace_frame.csv    -- game-state / sim-accum / fps
 *   log/race_trace_pose.csv     -- slot, world_x/y/z, ang_roll/yaw/pitch
 *   log/race_trace_motion.csv   -- slot, vel + long/lat speed + slip
 *   log/race_trace_track.csv    -- slot, span_*, contact flags
 *   log/race_trace_controls.csv -- slot, steering, engine, gear, mode
 *   log/race_trace_progress.csv -- slot, position, timers, metrics
 *   log/race_trace_view.csv     -- viewport idx, cam_world_*
 *   log/calls_trace.csv         -- function-call trace (unchanged schema)
 *
 * Selection is at session granularity via INI/CLI:
 *   [Trace] Modules = pose,motion,track   (CSV; "*" / unset = all)
 *   [Trace] Stages  = post_physics        (CSV; "*" / unset = all)
 *
 * Hot path = single bitmask AND, narrow row, large per-file output buffer.
 */

#ifndef TD5_TRACE_H
#define TD5_TRACE_H

#include <stdint.h>

/* -------- Module bitmask --------------------------------------------------
 * One bit per CSV file. Used by both the runtime gate and the INI parser. */
#define TD5_TRACE_MOD_FRAME     0x01
#define TD5_TRACE_MOD_POSE      0x02
#define TD5_TRACE_MOD_MOTION    0x04
#define TD5_TRACE_MOD_TRACK     0x08
#define TD5_TRACE_MOD_CONTROLS  0x10
#define TD5_TRACE_MOD_PROGRESS  0x20
#define TD5_TRACE_MOD_VIEW      0x40
#define TD5_TRACE_MOD_CALLS     0x80
#define TD5_TRACE_MOD_ALL       0xFF

/* -------- Stage bitmask --------------------------------------------------
 * One bit per emit call site in RunRaceFrame's tick loop. Each module emits
 * only on stages that semantically diverge for it (e.g. pose only post-
 * physics). The cross-product is encoded per-module in td5_trace.c. */
#define TD5_TRACE_STG_FRAME_BEGIN     (1u << 0)
#define TD5_TRACE_STG_PRE_PHYSICS     (1u << 1)
#define TD5_TRACE_STG_POST_PHYSICS    (1u << 2)
#define TD5_TRACE_STG_POST_AI         (1u << 3)
#define TD5_TRACE_STG_POST_TRACK      (1u << 4)
#define TD5_TRACE_STG_POST_CAMERA     (1u << 5)
#define TD5_TRACE_STG_POST_PROGRESS   (1u << 6)
#define TD5_TRACE_STG_FRAME_END       (1u << 7)
#define TD5_TRACE_STG_PAUSE_MENU      (1u << 8)
#define TD5_TRACE_STG_COUNTDOWN       (1u << 9)
#define TD5_TRACE_STG_ALL             0xFFFFu

/* -------- Lifecycle ------------------------------------------------------ */
void td5_trace_init(void);
void td5_trace_shutdown(void);

/* Returns 1 if any module is enabled and the per-frame cap has not been hit.
 * Call once per frame to gate the snapshot scaffolding (struct fills, loops). */
int  td5_trace_begin_frame(uint32_t frame_index);

/* True if the trace is open and at least one module is selected.
 * Use td5_trace_active(mod, stage_bit) for hot-path gating instead of this. */
int  td5_trace_is_enabled(void);

/* Hot-path gate: does this (module, stage_bit) pair want to emit right now?
 * Cheaper than calling td5_trace_is_enabled() + module check separately.
 * stage_bit is a single TD5_TRACE_STG_* flag. */
int  td5_trace_active(unsigned int mod, unsigned int stage_bit);

/* Per-slot filter (RaceTraceSlot=N). slot < 0 means "always emit". */
int  td5_trace_selected_slot(int slot);

/* -------- Module emit calls ---------------------------------------------- */
/* Each writes one CSV row to its own file. All take the common (frame,
 * sim_tick, stage, slot/view_idx) prefix. Stage is the human-readable name;
 * the stage_bit is implied by the call-site (the gate already filtered). */

typedef struct TD5_TraceFrameRow {
    int      game_state;
    int      paused;
    int      pause_menu_active;
    int      fade_state;
    int      countdown_timer;
    uint32_t sim_time_accumulator;
    float    sim_tick_budget;
    float    frame_dt;
    float    instant_fps;
    int      viewport_count;
    int      split_screen_mode;
    int      ticks_this_frame;
} TD5_TraceFrameRow;

typedef struct TD5_TracePoseRow {
    int     slot;
    int32_t world_x, world_y, world_z;
    int32_t ang_roll, ang_yaw, ang_pitch;
    int16_t disp_roll, disp_yaw, disp_pitch;
} TD5_TracePoseRow;

typedef struct TD5_TraceMotionRow {
    int     slot;
    int32_t vel_x, vel_y, vel_z;
    int32_t long_speed, lat_speed;
    int32_t front_slip, rear_slip;
} TD5_TraceMotionRow;

typedef struct TD5_TraceTrackRow {
    int     slot;
    int16_t span_raw, span_norm, span_accum, span_high;
    uint8_t track_contact_flag;
    uint8_t wheel_contact_mask;
} TD5_TraceTrackRow;

typedef struct TD5_TraceControlsRow {
    int     slot;
    int32_t steering_cmd;
    int32_t engine_speed;
    uint8_t current_gear;
    uint8_t vehicle_mode;
    int     slot_state;
} TD5_TraceControlsRow;

typedef struct TD5_TraceProgressRow {
    int     slot;
    uint8_t race_position;
    int32_t finish_time;
    int32_t accum_distance;
    uint16_t pending_finish_timer;
    int16_t  metric_checkpoint_index;
    uint8_t  metric_checkpoint_mask;
    int16_t  metric_normalized_span;
    int16_t  metric_timer_ticks;
    int16_t  metric_display_position;
    int32_t  metric_speed_bonus;
    int32_t  metric_top_speed;
} TD5_TraceProgressRow;

typedef struct TD5_TraceViewRow {
    int      view_index;
    int      actor_slot;
    int32_t  cam_world_x, cam_world_y, cam_world_z;
} TD5_TraceViewRow;

void td5_trace_emit_frame   (uint32_t frame, uint32_t tick, const char *stage,
                             const TD5_TraceFrameRow *r);
void td5_trace_emit_pose    (uint32_t frame, uint32_t tick, const char *stage,
                             const TD5_TracePoseRow *r);
void td5_trace_emit_motion  (uint32_t frame, uint32_t tick, const char *stage,
                             const TD5_TraceMotionRow *r);
void td5_trace_emit_track   (uint32_t frame, uint32_t tick, const char *stage,
                             const TD5_TraceTrackRow *r);
void td5_trace_emit_controls(uint32_t frame, uint32_t tick, const char *stage,
                             const TD5_TraceControlsRow *r);
void td5_trace_emit_progress(uint32_t frame, uint32_t tick, const char *stage,
                             const TD5_TraceProgressRow *r);
void td5_trace_emit_view    (uint32_t frame, uint32_t tick, const char *stage,
                             const TD5_TraceViewRow *r);

/* -------- Calls trace (unchanged schema) --------------------------------- */
#define TD5_TRACE_CALL_MAX_ARGS 8

void td5_trace_write_call(uint32_t sim_tick, const char *fn_name,
                          int n_args, const int32_t *args,
                          int has_ret, int32_t ret);

/* Helper exposed to the macros below (defined in td5_game.c). */
int  td5_trace_current_sim_tick(void);

#define TD5_TRACE_CALL_ENTER(_name, _a0, ...) \
    do { if (td5_trace_active(TD5_TRACE_MOD_CALLS, TD5_TRACE_STG_ALL)) { \
        int32_t _td5_args[] = { (int32_t)(_a0), ##__VA_ARGS__ }; \
        td5_trace_write_call( \
            (uint32_t)td5_trace_current_sim_tick(), (_name), \
            (int)(sizeof(_td5_args) / sizeof(_td5_args[0])), \
            _td5_args, 0, 0); \
    } } while (0)

#define TD5_TRACE_CALL_RET(_name, _ret, _a0, ...) \
    do { if (td5_trace_active(TD5_TRACE_MOD_CALLS, TD5_TRACE_STG_ALL)) { \
        int32_t _td5_args[] = { (int32_t)(_a0), ##__VA_ARGS__ }; \
        td5_trace_write_call( \
            (uint32_t)td5_trace_current_sim_tick(), (_name), \
            (int)(sizeof(_td5_args) / sizeof(_td5_args[0])), \
            _td5_args, 1, (int32_t)(_ret)); \
    } } while (0)

/* -------- Configuration parsing (called from main.c) -------------------- */
/* Parse a comma-separated module/stage list. "*" or empty = all. Unknown
 * tokens log a warning and are ignored. Returns the resulting bitmask. */
unsigned int td5_trace_parse_modules(const char *csv);
unsigned int td5_trace_parse_stages (const char *csv);

#endif /* TD5_TRACE_H */
