/*
 * td5_pilot_trace_004440F0.h -- Port-side CSV trace emitter for the
 * UpdateActorTrackPosition pilot (pool10 / 0x004440F0).
 *
 * One row per call, keyed by (sim_tick, slot, call_idx). Inputs are read
 * before the walker runs; outputs after. Only the slot whose world position
 * matches the supplied world-pos pointer is emitted (resolves to TARGET_SLOT
 * during the pilot run = 0).
 *
 * Output: log/port/pool10_004440F0.csv
 *
 * Workflow: re/analysis/precise_port_workflow.md
 */
#ifndef TD5_PILOT_TRACE_004440F0_H
#define TD5_PILOT_TRACE_004440F0_H

#include <stdint.h>

/* `probe` points at a 16-byte TD5_TrackProbe (short[8] view, byte at +12).
 * `world_pos` is a 24.8 fixed-point int[3] with x at [+0], z at [+8]. */
void td5_pilot_emit_004440F0_enter(const void *probe,
                                   const int32_t *world_pos,
                                   uintptr_t caller_ra);
void td5_pilot_emit_004440F0_leave(const void *probe, int retval);

#endif
