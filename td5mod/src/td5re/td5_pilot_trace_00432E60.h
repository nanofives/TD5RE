/*
 * td5_pilot_trace_00432E60.h -- CSV emitter for the precise-port pilot
 * 0x00432E60 InitializeRaceActorRuntime.
 *
 * The function fires exactly once per race-init; the probe emits one
 * "entry" row + one "exit" row at hookable boundaries inside
 * td5_ai_init_race_actor_runtime. Captures inputs (tier, mode flags,
 * difficulty, template pre-scaling values) and outputs (rb_* tuples,
 * template post-scaling values, racer_count).
 *
 * Output path: log/port/pool12_00432E60.csv
 * Audit:       re/analysis/pilot_00432E60_audit.md
 */
#ifndef TD5_PILOT_TRACE_00432E60_H
#define TD5_PILOT_TRACE_00432E60_H

#include <stdint.h>

/* Snapshot inputs at function entry, before any state mutation. */
void td5_pilot_emit_00432E60_enter(int tier_in,
                                   int is_circuit,
                                   int has_traffic,
                                   int wanted_mode,
                                   int difficulty,
                                   int time_trial,
                                   int16_t tpl_steer,
                                   int16_t tpl_grip,
                                   int16_t tpl_brake,
                                   int16_t tpl_lspd_brake,
                                   int16_t tpl_top_spd);

/* Snapshot outputs after both scaling layers complete. */
void td5_pilot_emit_00432E60_leave(int32_t rb_behind_scale,
                                   int32_t rb_behind_range,
                                   int32_t rb_ahead_scale,
                                   int32_t rb_ahead_range,
                                   int16_t tpl_steer_out,
                                   int16_t tpl_grip_out,
                                   int16_t tpl_brake_out,
                                   int16_t tpl_lspd_brake_out,
                                   int16_t tpl_top_spd_out,
                                   int     racer_count);

#endif
