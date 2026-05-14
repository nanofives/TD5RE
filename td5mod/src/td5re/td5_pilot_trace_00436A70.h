/*
 * td5_pilot_trace_00436A70.h -- Per-function CSV trace emitter for the
 * UpdateRaceActors pilot (pool13 / 0x00436A70).
 *
 * Workflow: re/analysis/precise_port_workflow.md.
 * Audit:    re/analysis/pilot_00436A70_audit.md.
 *
 * Schema mirrors tools/frida_pool13_00436A70.js. One CSV row per racer slot
 * per call (per RunRaceFrame sub-tick). Emit slots 0..5 unconditionally so
 * the diff can spot slot-state branch divergences as well as per-slot
 * dispatcher-output divergences.
 *
 * Output: log/port/pool13_00436A70.csv
 */
#ifndef TD5_PILOT_TRACE_00436A70_H
#define TD5_PILOT_TRACE_00436A70_H

#include <stdint.h>

#define PILOT_00436A70_N_SLOTS 6

/* Snapshot of inputs the dispatcher reads. Collected at entry. */
typedef struct {
    int network_active;
    int racer_count;
    int drag_mode;
    int wanted_mode;
    int encounter_handle;
    int encounter_enabled;
    int track_total_span_count;

    int   slot_state[PILOT_00436A70_N_SLOTS];
    int   span_raw[PILOT_00436A70_N_SLOTS];
    int   span_norm[PILOT_00436A70_N_SLOTS];
    int32_t lin_vel_x[PILOT_00436A70_N_SLOTS];
    int32_t lin_vel_z[PILOT_00436A70_N_SLOTS];
    int32_t world_pos_x[PILOT_00436A70_N_SLOTS];
    int32_t world_pos_z[PILOT_00436A70_N_SLOTS];
    int     recovery_stage[PILOT_00436A70_N_SLOTS];
    int     wanted_damage[PILOT_00436A70_N_SLOTS];
} PilotSnapshot_00436A70;

/* Called BEFORE the dispatcher work runs. Snapshots all inputs. */
void td5_pilot_emit_00436A70_enter(void);

/* Called AFTER the dispatcher work runs. Emits one row per slot. */
void td5_pilot_emit_00436A70_leave(void);

/* Implemented in td5_ai.c — reads arrays + actor pointer private to that TU. */
void td5_pilot_00436A70_collect(PilotSnapshot_00436A70 *snap);

#endif /* TD5_PILOT_TRACE_00436A70_H */
