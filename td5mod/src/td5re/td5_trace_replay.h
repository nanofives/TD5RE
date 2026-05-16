/*
 * td5_trace_replay.h -- Per-sub-tick state replay / injection harness.
 *
 * Counterpart to tools/frida_state_snapshot.js. Both sides operate on the
 * same binary format (magic 'TD5R', see frida_state_snapshot.js or
 * tools/diff_replay_frames.py for the layout).
 *
 * Modes ([Trace] StateReplayMode):
 *   off    -- harness disabled (no file I/O, no inject)
 *   dump   -- append port state to log/port_state_snapshot.bin each sub-tick
 *   inject -- read orig_frames[N] from StateReplayPath and overwrite port
 *             actor + RS state before the NEXT sub-tick body runs
 *   both   -- dump THEN inject (the differential mode used by the diff
 *             harness: port_frames[N] = result of running sub-tick N
 *             starting from orig_frames[N-1])
 *
 * Sub-tick alignment: the harness maintains its own monotonic counter that
 * increments once per call to td5_trace_replay_step(). Both port and Frida
 * must call this at the same logical instant per sub-tick (end of tick,
 * just before g_simulationTickCounter would increment), so frame N on one
 * side corresponds to frame N on the other.
 */

#ifndef TD5_TRACE_REPLAY_H
#define TD5_TRACE_REPLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize from INI keys. Returns 1 if any mode is active, 0 if off. */
int  td5_trace_replay_init(void);

/* Flush dump file (if open) and free orig-frame buffer. Idempotent. */
void td5_trace_replay_shutdown(void);

/* Per-sub-tick step. Called from td5_game.c at "post_progress" and
 * "countdown" stages, AFTER the existing whole-state emit. The sub_tick
 * counter is internal and monotonic from the first call. */
void td5_trace_replay_step(void);

/* True once init has succeeded with mode != off. */
int  td5_trace_replay_active(void);

/* Current sub_tick index (next-call value). For logging only. */
int  td5_trace_replay_sub_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* TD5_TRACE_REPLAY_H */
