/*
 * td5_trace.c -- Structured race-trace output for differential debugging
 */

#include "td5_trace.h"

#include "td5_platform.h"
#include "td5re.h"

#include <stdio.h>
#include <string.h>

#define LOG_TAG "trace"

static FILE *s_trace_fp;
static int s_trace_enabled;
static int s_trace_slot;
static int s_trace_max_frames;
static uint32_t s_frames_started;
static uint32_t s_last_frame_index;

/* --- call-trace state ---------------------------------------------------- */
/* Separate CSV for TD5_TRACE_CALL_ENTER/RET rows. Opened alongside the main
 * race_trace.csv when tracing is enabled. Closed by td5_trace_shutdown. */
static FILE *s_calls_fp;

/* Tiny per-tick call-index table, reset whenever sim_tick changes.
 *  - A sim tick rarely fires the same function more than a handful of times,
 *    so a small linear-probe array keeps this simple + allocation-free. */
#define TRACE_CALL_IDX_CAP 64
typedef struct TraceCallIdxEntry {
    const char *name;   /* pointer compared (macros pass string literals) */
    uint32_t    count;  /* next call_idx to assign */
} TraceCallIdxEntry;
static TraceCallIdxEntry s_call_idx_table[TRACE_CALL_IDX_CAP];
static uint32_t          s_call_idx_tick = 0xffffffffu;
static int               s_call_idx_size = 0;

static uint32_t td5_trace_next_call_idx_internal(const char *name, uint32_t sim_tick)
{
    if (sim_tick != s_call_idx_tick) {
        s_call_idx_tick = sim_tick;
        s_call_idx_size = 0;
    }
    for (int i = 0; i < s_call_idx_size; ++i) {
        if (s_call_idx_table[i].name == name ||
            (s_call_idx_table[i].name && strcmp(s_call_idx_table[i].name, name) == 0)) {
            return s_call_idx_table[i].count++;
        }
    }
    if (s_call_idx_size < TRACE_CALL_IDX_CAP) {
        s_call_idx_table[s_call_idx_size].name = name;
        s_call_idx_table[s_call_idx_size].count = 1;
        ++s_call_idx_size;
    }
    return 0;
}

static void td5_trace_write_header(FILE *fp)
{
    fputs(
        "frame,sim_tick,stage,kind,id,"
        "game_state,paused,pause_menu,fade_state,countdown_timer,sim_accum,sim_budget,frame_dt,instant_fps,viewport_count,split_mode,ticks_this_frame,"
        "slot_state,slot_comp1,slot_comp2,view_target,"
        "world_x,world_y,world_z,vel_x,vel_y,vel_z,ang_roll,ang_yaw,ang_pitch,disp_roll,disp_yaw,disp_pitch,"
        "span_raw,span_norm,span_accum,span_high,steer,engine,long_speed,lat_speed,front_slip,rear_slip,finish_time,accum_distance,pending_finish,gear,vehicle_mode,track_contact,wheel_mask,race_pos,"
        "metric_checkpoint,metric_mask,metric_norm_span,metric_timer,metric_display_pos,metric_speed_bonus,metric_top_speed,"
        "cam_world_x,cam_world_y,cam_world_z,cam_x,cam_y,cam_z\n",
        fp
    );
}

static void td5_trace_write_prefix(uint32_t frame_index, uint32_t sim_tick,
                                   const char *stage, const char *kind, int id)
{
    fprintf(s_trace_fp, "%u,%u,%s,%s,%d,",
            (unsigned int)frame_index,
            (unsigned int)sim_tick,
            stage ? stage : "",
            kind,
            id);
}

void td5_trace_init(void)
{
    char path[640];

    s_trace_fp = NULL;
    s_trace_enabled = 0;
    s_trace_slot = g_td5.ini.race_trace_slot;
    s_trace_max_frames = g_td5.ini.race_trace_max_frames;
    s_frames_started = 0;
    s_last_frame_index = 0xffffffffu;

    if (!g_td5.ini.race_trace_enabled) {
        return;
    }

    snprintf(path, sizeof(path), "%srace_trace.csv", td5_plat_log_dir());
    s_trace_fp = fopen(path, "w");
    if (!s_trace_fp) {
        TD5_LOG_E(LOG_TAG, "Failed to open race trace output: %s", path);
        return;
    }

    td5_trace_write_header(s_trace_fp);
    fflush(s_trace_fp);
    s_trace_enabled = 1;

    /* Open the separate calls-trace CSV. Schema matches the Frida side. */
    {
        char calls_path[640];
        snprintf(calls_path, sizeof(calls_path), "%scalls_trace.csv",
                 td5_plat_log_dir());
        s_calls_fp = fopen(calls_path, "w");
        if (s_calls_fp) {
            fputs("sim_tick,fn_name,call_idx,n_args,arg_0,arg_1,arg_2,arg_3,"
                  "arg_4,arg_5,arg_6,arg_7,has_ret,ret\n",
                  s_calls_fp);
            fflush(s_calls_fp);
            TD5_LOG_I(LOG_TAG, "Calls trace enabled: path=%s", calls_path);
        } else {
            TD5_LOG_W(LOG_TAG, "Failed to open calls trace: %s", calls_path);
        }
    }

    s_call_idx_tick = 0xffffffffu;
    s_call_idx_size = 0;

    TD5_LOG_I(LOG_TAG,
              "Race trace enabled: path=%s slot=%d max_frames=%d",
              path, s_trace_slot, s_trace_max_frames);
}

void td5_trace_shutdown(void)
{
    if (s_trace_fp) {
        fflush(s_trace_fp);
        fclose(s_trace_fp);
        s_trace_fp = NULL;
    }
    if (s_calls_fp) {
        fflush(s_calls_fp);
        fclose(s_calls_fp);
        s_calls_fp = NULL;
    }
    s_trace_enabled = 0;
}

int td5_trace_begin_frame(uint32_t frame_index)
{
    if (!s_trace_enabled || !s_trace_fp) {
        return 0;
    }

    if (frame_index != s_last_frame_index) {
        s_last_frame_index = frame_index;
        s_frames_started++;
        if (s_trace_max_frames > 0 && (int)s_frames_started > s_trace_max_frames) {
            TD5_LOG_I(LOG_TAG,
                      "Race trace reached frame limit (%d), disabling",
                      s_trace_max_frames);
            td5_trace_shutdown();
            return 0;
        }
    }

    return 1;
}

int td5_trace_is_enabled(void)
{
    return s_trace_enabled && s_trace_fp != NULL;
}

int td5_trace_selected_slot(int slot)
{
    if (!td5_trace_is_enabled()) {
        return 0;
    }
    return s_trace_slot < 0 || slot == s_trace_slot;
}

void td5_trace_write_frame(uint32_t frame_index, uint32_t sim_tick, const char *stage,
                           const TD5_TraceFrameState *state)
{
    if (!td5_trace_is_enabled() || !state) {
        return;
    }

    td5_trace_write_prefix(frame_index, sim_tick, stage, "frame", 0);
    fprintf(s_trace_fp,
            "%d,%d,%d,%d,%d,%u,%.6f,%.6f,%.6f,%d,%d,%d,"
            "0,0,0,0,"
            "0,0,0,0,0,0,0,0,0,0,0,0,"
            "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,"
            "0,0,0,0,0,0,0,"
            "0,0,0,0.000000,0.000000,0.000000\n",
            state->game_state,
            state->paused,
            state->pause_menu_active,
            state->fade_state,
            state->countdown_timer,
            (unsigned int)state->sim_time_accumulator,
            state->sim_tick_budget,
            state->frame_dt,
            state->instant_fps,
            state->viewport_count,
            state->split_screen_mode,
            state->ticks_this_frame);
}

void td5_trace_write_actor(uint32_t frame_index, uint32_t sim_tick, const char *stage,
                           const TD5_TraceActorState *state)
{
    if (!td5_trace_is_enabled() || !state) {
        return;
    }

    /* The 7 metric_* schema columns are port-only bookkeeping — the original
     * binary has no equivalent actor fields, and tools/frida_race_trace.js
     * writes literal "0,0,0,0,0,0,0," for them (line 344-345). Emitting
     * zeros here keeps the comparator from reporting spurious divergences
     * on columns that have no reference data. */
    td5_trace_write_prefix(frame_index, sim_tick, stage, "actor", state->slot);
    fprintf(s_trace_fp,
            "0,0,0,0,0,0,0.000000,0.000000,0.000000,0,0,0,"
            "%d,%d,%d,%d,"
            "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
            "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u,%u,%u,"
            "0,0,0,0,0,0,0,"
            "0,0,0,0.000000,0.000000,0.000000\n",
            state->slot_state,
            state->slot_companion_1,
            state->slot_companion_2,
            state->view_target,
            state->world_x,
            state->world_y,
            state->world_z,
            state->vel_x,
            state->vel_y,
            state->vel_z,
            state->ang_roll,
            state->ang_yaw,
            state->ang_pitch,
            state->disp_roll,
            state->disp_yaw,
            state->disp_pitch,
            state->span_raw,
            state->span_norm,
            state->span_accum,
            state->span_high,
            state->steering_cmd,
            state->engine_speed,
            state->long_speed,
            state->lat_speed,
            state->front_slip,
            state->rear_slip,
            state->finish_time,
            state->accum_distance,
            (unsigned int)state->pending_finish_timer,
            (unsigned int)state->current_gear,
            (unsigned int)state->vehicle_mode,
            (unsigned int)state->track_contact_flag,
            (unsigned int)state->wheel_contact_mask,
            (unsigned int)state->race_position);
}

void td5_trace_write_call(uint32_t sim_tick, const char *fn_name,
                          int n_args, const int32_t *args,
                          int has_ret, int32_t ret)
{
    if (!td5_trace_is_enabled() || !s_calls_fp || !fn_name) {
        return;
    }
    if (n_args < 0) n_args = 0;
    if (n_args > TD5_TRACE_CALL_MAX_ARGS) n_args = TD5_TRACE_CALL_MAX_ARGS;

    uint32_t call_idx = td5_trace_next_call_idx_internal(fn_name, sim_tick);

    int32_t padded[TD5_TRACE_CALL_MAX_ARGS];
    for (int i = 0; i < TD5_TRACE_CALL_MAX_ARGS; ++i) {
        padded[i] = (i < n_args && args) ? args[i] : 0;
    }

    fprintf(s_calls_fp,
            "%u,%s,%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
            (unsigned int)sim_tick,
            fn_name,
            (unsigned int)call_idx,
            n_args,
            (int)padded[0], (int)padded[1], (int)padded[2], (int)padded[3],
            (int)padded[4], (int)padded[5], (int)padded[6], (int)padded[7],
            has_ret ? 1 : 0,
            (int)(has_ret ? ret : 0));
}

void td5_trace_write_view(uint32_t frame_index, uint32_t sim_tick, const char *stage,
                          const TD5_TraceViewState *state)
{
    if (!td5_trace_is_enabled() || !state) {
        return;
    }

    td5_trace_write_prefix(frame_index, sim_tick, stage, "view", state->view_index);
    fprintf(s_trace_fp,
            "0,0,0,0,0,0,0.000000,0.000000,0.000000,0,0,0,"
            "0,0,0,%d,"
            "0,0,0,0,0,0,0,0,0,0,0,0,"
            "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,"
            "0,0,0,0,0,0,0,"
            "%d,%d,%d,%.6f,%.6f,%.6f\n",
            state->actor_slot,
            state->cam_world_x,
            state->cam_world_y,
            state->cam_world_z,
            state->cam_x,
            state->cam_y,
            state->cam_z);
}
