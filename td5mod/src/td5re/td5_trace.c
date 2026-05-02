/*
 * td5_trace.c -- Modular per-module CSV race-trace emitter
 *
 * Each module (frame, pose, motion, track, controls, progress, view, calls)
 * owns its own CSV file with a narrow column set. Selection is via INI/CLI
 * bitmasks. Per-file output uses a 256 KiB setvbuf buffer; flush only on
 * shutdown / cap-hit, so the hot path is one printf per active module.
 */

#include "td5_trace.h"

#include "td5_platform.h"
#include "td5re.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define LOG_TAG "trace"

/* 256 KiB per file. Big enough to absorb a multi-second trace burst between
 * fflushes while still being small relative to host RAM. */
#define TRACE_FILE_BUF_BYTES (256 * 1024)

/* -------- per-module file table ---------------------------------------- */
typedef struct ModuleFile {
    unsigned int mask;        /* TD5_TRACE_MOD_* */
    const char  *name;        /* token used in INI/CLI lists */
    const char  *suffix;      /* race_trace_<suffix>.csv */
    const char  *header;      /* CSV header line (no trailing \n) */
    const char  *env_var;     /* per-session path override env */
    FILE        *fp;
    char        *buf;
} ModuleFile;

static ModuleFile s_modules[] = {
    { TD5_TRACE_MOD_FRAME,    "frame",    "frame",
      "frame,sim_tick,stage,game_state,paused,pause_menu,fade_state,countdown_timer,sim_accum,sim_budget,frame_dt,instant_fps,viewport_count,split_mode,ticks_this_frame",
      "TD5RE_TRACE_FRAME_PATH",    NULL, NULL },
    { TD5_TRACE_MOD_POSE,     "pose",     "pose",
      "frame,sim_tick,stage,slot,world_x,world_y,world_z,ang_roll,ang_yaw,ang_pitch,disp_roll,disp_yaw,disp_pitch",
      "TD5RE_TRACE_POSE_PATH",     NULL, NULL },
    { TD5_TRACE_MOD_MOTION,   "motion",   "motion",
      "frame,sim_tick,stage,slot,vel_x,vel_y,vel_z,long_speed,lat_speed,front_slip,rear_slip",
      "TD5RE_TRACE_MOTION_PATH",   NULL, NULL },
    { TD5_TRACE_MOD_TRACK,    "track",    "track",
      "frame,sim_tick,stage,slot,span_raw,span_norm,span_accum,span_high,track_contact,wheel_mask",
      "TD5RE_TRACE_TRACK_PATH",    NULL, NULL },
    { TD5_TRACE_MOD_CONTROLS, "controls", "controls",
      "frame,sim_tick,stage,slot,slot_state,steering,engine,gear,vehicle_mode",
      "TD5RE_TRACE_CONTROLS_PATH", NULL, NULL },
    { TD5_TRACE_MOD_PROGRESS, "progress", "progress",
      "frame,sim_tick,stage,slot,race_pos,finish_time,accum_distance,pending_finish,metric_checkpoint,metric_mask,metric_norm_span,metric_timer,metric_display_pos,metric_speed_bonus,metric_top_speed",
      "TD5RE_TRACE_PROGRESS_PATH", NULL, NULL },
    { TD5_TRACE_MOD_VIEW,     "view",     "view",
      "frame,sim_tick,stage,view_index,actor_slot,cam_world_x,cam_world_y,cam_world_z",
      "TD5RE_TRACE_VIEW_PATH",     NULL, NULL },
    { TD5_TRACE_MOD_CALLS,    "calls",    "calls",
      "sim_tick,fn_name,call_idx,n_args,arg_0,arg_1,arg_2,arg_3,arg_4,arg_5,arg_6,arg_7,has_ret,ret",
      "TD5RE_CALLS_TRACE_PATH",    NULL, NULL },
};
static const int s_module_count = (int)(sizeof(s_modules) / sizeof(s_modules[0]));

/* -------- per-module stage policy --------------------------------------
 * Hard-coded gate that says, for each module, which stages it cares about.
 * Stage filter (s_stage_mask) ANDs on top of this. */
static const struct { unsigned int mod; unsigned int stages; } s_module_stages[] = {
    { TD5_TRACE_MOD_FRAME,
      TD5_TRACE_STG_FRAME_BEGIN | TD5_TRACE_STG_FRAME_END |
      TD5_TRACE_STG_PAUSE_MENU  | TD5_TRACE_STG_COUNTDOWN |
      TD5_TRACE_STG_POST_PROGRESS },
    { TD5_TRACE_MOD_POSE,     TD5_TRACE_STG_POST_PHYSICS },
    { TD5_TRACE_MOD_MOTION,   TD5_TRACE_STG_PRE_PHYSICS  | TD5_TRACE_STG_POST_PHYSICS },
    { TD5_TRACE_MOD_TRACK,    TD5_TRACE_STG_POST_TRACK   | TD5_TRACE_STG_POST_PHYSICS },
    { TD5_TRACE_MOD_CONTROLS, TD5_TRACE_STG_POST_AI      | TD5_TRACE_STG_PRE_PHYSICS },
    { TD5_TRACE_MOD_PROGRESS, TD5_TRACE_STG_POST_PROGRESS | TD5_TRACE_STG_FRAME_END },
    { TD5_TRACE_MOD_VIEW,     TD5_TRACE_STG_POST_CAMERA  | TD5_TRACE_STG_FRAME_END },
    { TD5_TRACE_MOD_CALLS,    TD5_TRACE_STG_ALL },
};

/* -------- runtime state ------------------------------------------------- */
static int          s_trace_open;
static int          s_trace_slot;
static int          s_trace_max_frames;
static unsigned int s_module_mask;   /* selected modules */
static unsigned int s_stage_mask;    /* selected stages */
static uint32_t     s_frames_started;
static uint32_t     s_last_frame_index;

/* call-trace per-tick call_idx table */
#define TRACE_CALL_IDX_CAP 64
typedef struct TraceCallIdxEntry {
    const char *name;
    uint32_t    count;
} TraceCallIdxEntry;
static TraceCallIdxEntry s_call_idx_table[TRACE_CALL_IDX_CAP];
static uint32_t          s_call_idx_tick = 0xffffffffu;
static int               s_call_idx_size = 0;

/* -------- helpers ------------------------------------------------------- */

static ModuleFile *module_by_mask(unsigned int mask)
{
    for (int i = 0; i < s_module_count; ++i) {
        if (s_modules[i].mask == mask) return &s_modules[i];
    }
    return NULL;
}

/* Returns the static stage policy for a module, or 0 if module unknown. */
static unsigned int module_stage_policy(unsigned int mod)
{
    for (size_t i = 0; i < sizeof(s_module_stages) / sizeof(s_module_stages[0]); ++i) {
        if (s_module_stages[i].mod == mod) return s_module_stages[i].stages;
    }
    return 0;
}

static int ieq_token(const char *a, size_t la, const char *b)
{
    size_t lb = strlen(b);
    if (la != lb) return 0;
    for (size_t i = 0; i < la; ++i) {
        char ca = (char)tolower((unsigned char)a[i]);
        char cb = (char)tolower((unsigned char)b[i]);
        if (ca != cb) return 0;
    }
    return 1;
}

/* Common parser for both modules and stages. Each token is matched against
 * the supplied (name, mask) table. "*" or empty input = all_mask. */
typedef struct TokenEntry { const char *name; unsigned int mask; } TokenEntry;

static unsigned int parse_csv_mask(const char *csv,
                                   const TokenEntry *table, int n,
                                   unsigned int all_mask, const char *what)
{
    if (!csv || !*csv) return all_mask;
    /* skip whitespace */
    while (*csv && isspace((unsigned char)*csv)) ++csv;
    if (!*csv) return all_mask;
    if (csv[0] == '*' && (csv[1] == '\0' || isspace((unsigned char)csv[1]))) return all_mask;

    unsigned int mask = 0;
    const char *p = csv;
    while (*p) {
        while (*p == ',' || isspace((unsigned char)*p)) ++p;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',' && !isspace((unsigned char)*p)) ++p;
        size_t len = (size_t)(p - start);
        if (!len) continue;

        int matched = 0;
        for (int i = 0; i < n; ++i) {
            if (ieq_token(start, len, table[i].name)) {
                mask |= table[i].mask;
                matched = 1;
                break;
            }
        }
        if (!matched) {
            TD5_LOG_W(LOG_TAG, "Unknown %s token: '%.*s' (ignored)",
                      what, (int)len, start);
        }
    }
    return mask ? mask : all_mask;
}

unsigned int td5_trace_parse_modules(const char *csv)
{
    static const TokenEntry tbl[] = {
        { "frame",    TD5_TRACE_MOD_FRAME    },
        { "pose",     TD5_TRACE_MOD_POSE     },
        { "motion",   TD5_TRACE_MOD_MOTION   },
        { "track",    TD5_TRACE_MOD_TRACK    },
        { "controls", TD5_TRACE_MOD_CONTROLS },
        { "progress", TD5_TRACE_MOD_PROGRESS },
        { "view",     TD5_TRACE_MOD_VIEW     },
        { "calls",    TD5_TRACE_MOD_CALLS    },
    };
    return parse_csv_mask(csv, tbl, (int)(sizeof(tbl)/sizeof(tbl[0])),
                          TD5_TRACE_MOD_ALL, "module");
}

unsigned int td5_trace_parse_stages(const char *csv)
{
    static const TokenEntry tbl[] = {
        { "frame_begin",   TD5_TRACE_STG_FRAME_BEGIN   },
        { "pre_physics",   TD5_TRACE_STG_PRE_PHYSICS   },
        { "post_physics",  TD5_TRACE_STG_POST_PHYSICS  },
        { "post_ai",       TD5_TRACE_STG_POST_AI       },
        { "post_track",    TD5_TRACE_STG_POST_TRACK    },
        { "post_camera",   TD5_TRACE_STG_POST_CAMERA   },
        { "post_progress", TD5_TRACE_STG_POST_PROGRESS },
        { "frame_end",     TD5_TRACE_STG_FRAME_END     },
        { "pause_menu",    TD5_TRACE_STG_PAUSE_MENU    },
        { "countdown",     TD5_TRACE_STG_COUNTDOWN     },
    };
    return parse_csv_mask(csv, tbl, (int)(sizeof(tbl)/sizeof(tbl[0])),
                          TD5_TRACE_STG_ALL, "stage");
}

/* -------- file open / close --------------------------------------------- */

static void module_open(ModuleFile *m)
{
    char path[640];
    const char *override_path = m->env_var ? getenv(m->env_var) : NULL;
    if (override_path && *override_path) {
        snprintf(path, sizeof(path), "%s", override_path);
    } else {
        snprintf(path, sizeof(path), "%srace_trace_%s.csv",
                 td5_plat_log_dir(), m->suffix);
    }
    /* calls_trace.csv keeps its historic name (no race_ prefix) — schema is
     * unchanged so the comparator + Frida side aren't disturbed. */
    if (m->mask == TD5_TRACE_MOD_CALLS && !(override_path && *override_path)) {
        snprintf(path, sizeof(path), "%scalls_trace.csv", td5_plat_log_dir());
    }

    m->fp = fopen(path, "w");
    if (!m->fp) {
        TD5_LOG_E(LOG_TAG, "Failed to open trace output: %s", path);
        return;
    }
    /* Big buffer keeps the hot path cheap — fflush only at shutdown / cap. */
    m->buf = (char *)malloc(TRACE_FILE_BUF_BYTES);
    if (m->buf) {
        setvbuf(m->fp, m->buf, _IOFBF, TRACE_FILE_BUF_BYTES);
    }
    fputs(m->header, m->fp);
    fputc('\n', m->fp);
    TD5_LOG_I(LOG_TAG, "Trace module '%s' enabled: path=%s", m->name, path);
}

static void module_close(ModuleFile *m)
{
    if (m->fp) {
        fflush(m->fp);
        fclose(m->fp);
        m->fp = NULL;
    }
    if (m->buf) {
        free(m->buf);
        m->buf = NULL;
    }
}

/* -------- lifecycle ----------------------------------------------------- */

void td5_trace_init(void)
{
    s_trace_open = 0;
    s_trace_slot = g_td5.ini.race_trace_slot;
    s_trace_max_frames = g_td5.ini.race_trace_max_frames;
    s_module_mask = g_td5.ini.trace_module_mask & TD5_TRACE_MOD_ALL;
    s_stage_mask  = g_td5.ini.trace_stage_mask  & TD5_TRACE_STG_ALL;
    s_frames_started = 0;
    s_last_frame_index = 0xffffffffu;
    s_call_idx_tick = 0xffffffffu;
    s_call_idx_size = 0;

    if (!g_td5.ini.race_trace_enabled) {
        return;
    }
    if (!s_module_mask || !s_stage_mask) {
        TD5_LOG_W(LOG_TAG, "Trace enabled but module_mask=0x%x stage_mask=0x%x — nothing to capture",
                  s_module_mask, s_stage_mask);
        return;
    }

    for (int i = 0; i < s_module_count; ++i) {
        if (s_modules[i].mask & s_module_mask) {
            module_open(&s_modules[i]);
        }
    }
    s_trace_open = 1;
    TD5_LOG_I(LOG_TAG, "Race trace enabled: modules=0x%x stages=0x%x slot=%d max_frames=%d",
              s_module_mask, s_stage_mask, s_trace_slot, s_trace_max_frames);
}

void td5_trace_shutdown(void)
{
    for (int i = 0; i < s_module_count; ++i) {
        module_close(&s_modules[i]);
    }
    s_trace_open = 0;
}

int td5_trace_begin_frame(uint32_t frame_index)
{
    if (!s_trace_open) return 0;
    if (frame_index != s_last_frame_index) {
        s_last_frame_index = frame_index;
        s_frames_started++;
        if (s_trace_max_frames > 0 && (int)s_frames_started > s_trace_max_frames) {
            TD5_LOG_I(LOG_TAG, "Race trace reached frame limit (%d), disabling",
                      s_trace_max_frames);
            td5_trace_shutdown();
            return 0;
        }
    }
    return 1;
}

int td5_trace_is_enabled(void)
{
    return s_trace_open;
}

int td5_trace_active(unsigned int mod, unsigned int stage_bit)
{
    if (!s_trace_open) return 0;
    if (!(s_module_mask & mod)) return 0;
    /* stage_bit may be TD5_TRACE_STG_ALL for non-stage-bound emits (calls). */
    if (stage_bit == TD5_TRACE_STG_ALL) return 1;
    if (!(s_stage_mask & stage_bit)) return 0;
    /* Module's own stage policy — saves writing rows that semantically
     * make no sense (e.g. pose at pre_physics). */
    unsigned int policy = module_stage_policy(mod);
    if (policy && !(policy & stage_bit)) return 0;
    return 1;
}

int td5_trace_selected_slot(int slot)
{
    if (!s_trace_open) return 0;
    return s_trace_slot < 0 || slot == s_trace_slot;
}

/* -------- emit helpers -------------------------------------------------- */

static FILE *fp_for(unsigned int mask)
{
    ModuleFile *m = module_by_mask(mask);
    return (m && m->fp) ? m->fp : NULL;
}

void td5_trace_emit_frame(uint32_t frame, uint32_t tick, const char *stage,
                          const TD5_TraceFrameRow *r)
{
    FILE *fp = fp_for(TD5_TRACE_MOD_FRAME);
    if (!fp || !r) return;
    fprintf(fp,
            "%u,%u,%s,%d,%d,%d,%d,%d,%u,%.6f,%.6f,%.6f,%d,%d,%d\n",
            (unsigned)frame, (unsigned)tick, stage ? stage : "",
            r->game_state, r->paused, r->pause_menu_active,
            r->fade_state, r->countdown_timer,
            (unsigned)r->sim_time_accumulator,
            r->sim_tick_budget, r->frame_dt, r->instant_fps,
            r->viewport_count, r->split_screen_mode, r->ticks_this_frame);
}

void td5_trace_emit_pose(uint32_t frame, uint32_t tick, const char *stage,
                         const TD5_TracePoseRow *r)
{
    FILE *fp = fp_for(TD5_TRACE_MOD_POSE);
    if (!fp || !r) return;
    fprintf(fp,
            "%u,%u,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
            (unsigned)frame, (unsigned)tick, stage ? stage : "", r->slot,
            r->world_x, r->world_y, r->world_z,
            r->ang_roll, r->ang_yaw, r->ang_pitch,
            (int)r->disp_roll, (int)r->disp_yaw, (int)r->disp_pitch);
}

void td5_trace_emit_motion(uint32_t frame, uint32_t tick, const char *stage,
                           const TD5_TraceMotionRow *r)
{
    FILE *fp = fp_for(TD5_TRACE_MOD_MOTION);
    if (!fp || !r) return;
    fprintf(fp,
            "%u,%u,%s,%d,%d,%d,%d,%d,%d,%d,%d\n",
            (unsigned)frame, (unsigned)tick, stage ? stage : "", r->slot,
            r->vel_x, r->vel_y, r->vel_z,
            r->long_speed, r->lat_speed, r->front_slip, r->rear_slip);
}

void td5_trace_emit_track(uint32_t frame, uint32_t tick, const char *stage,
                          const TD5_TraceTrackRow *r)
{
    FILE *fp = fp_for(TD5_TRACE_MOD_TRACK);
    if (!fp || !r) return;
    fprintf(fp,
            "%u,%u,%s,%d,%d,%d,%d,%d,%u,%u\n",
            (unsigned)frame, (unsigned)tick, stage ? stage : "", r->slot,
            (int)r->span_raw, (int)r->span_norm,
            (int)r->span_accum, (int)r->span_high,
            (unsigned)r->track_contact_flag, (unsigned)r->wheel_contact_mask);
}

void td5_trace_emit_controls(uint32_t frame, uint32_t tick, const char *stage,
                             const TD5_TraceControlsRow *r)
{
    FILE *fp = fp_for(TD5_TRACE_MOD_CONTROLS);
    if (!fp || !r) return;
    fprintf(fp,
            "%u,%u,%s,%d,%d,%d,%d,%u,%u\n",
            (unsigned)frame, (unsigned)tick, stage ? stage : "", r->slot,
            r->slot_state, r->steering_cmd, r->engine_speed,
            (unsigned)r->current_gear, (unsigned)r->vehicle_mode);
}

void td5_trace_emit_progress(uint32_t frame, uint32_t tick, const char *stage,
                             const TD5_TraceProgressRow *r)
{
    FILE *fp = fp_for(TD5_TRACE_MOD_PROGRESS);
    if (!fp || !r) return;
    fprintf(fp,
            "%u,%u,%s,%d,%u,%d,%d,%u,%d,%u,%d,%d,%d,%d,%d\n",
            (unsigned)frame, (unsigned)tick, stage ? stage : "", r->slot,
            (unsigned)r->race_position, r->finish_time, r->accum_distance,
            (unsigned)r->pending_finish_timer,
            (int)r->metric_checkpoint_index,
            (unsigned)r->metric_checkpoint_mask,
            (int)r->metric_normalized_span, (int)r->metric_timer_ticks,
            (int)r->metric_display_position,
            r->metric_speed_bonus, r->metric_top_speed);
}

void td5_trace_emit_view(uint32_t frame, uint32_t tick, const char *stage,
                         const TD5_TraceViewRow *r)
{
    FILE *fp = fp_for(TD5_TRACE_MOD_VIEW);
    if (!fp || !r) return;
    fprintf(fp,
            "%u,%u,%s,%d,%d,%d,%d,%d\n",
            (unsigned)frame, (unsigned)tick, stage ? stage : "",
            r->view_index, r->actor_slot,
            r->cam_world_x, r->cam_world_y, r->cam_world_z);
}

/* -------- calls trace --------------------------------------------------- */

static uint32_t next_call_idx_internal(const char *name, uint32_t sim_tick)
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

void td5_trace_write_call(uint32_t sim_tick, const char *fn_name,
                          int n_args, const int32_t *args,
                          int has_ret, int32_t ret)
{
    FILE *fp = fp_for(TD5_TRACE_MOD_CALLS);
    if (!fp || !fn_name) return;
    if (n_args < 0) n_args = 0;
    if (n_args > TD5_TRACE_CALL_MAX_ARGS) n_args = TD5_TRACE_CALL_MAX_ARGS;

    uint32_t call_idx = next_call_idx_internal(fn_name, sim_tick);
    int32_t padded[TD5_TRACE_CALL_MAX_ARGS];
    for (int i = 0; i < TD5_TRACE_CALL_MAX_ARGS; ++i) {
        padded[i] = (i < n_args && args) ? args[i] : 0;
    }

    fprintf(fp,
            "%u,%s,%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
            (unsigned)sim_tick, fn_name, (unsigned)call_idx, n_args,
            (int)padded[0], (int)padded[1], (int)padded[2], (int)padded[3],
            (int)padded[4], (int)padded[5], (int)padded[6], (int)padded[7],
            has_ret ? 1 : 0, (int)(has_ret ? ret : 0));
}
