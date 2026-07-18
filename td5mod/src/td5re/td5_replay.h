/* td5_replay.h -- full ghost-state "View Replay" recorder (PORT-ONLY).
 *
 * WHY THIS EXISTS
 * ---------------
 * The legacy "View Replay" re-SIMULATES the race: it records only the two
 * player-input control DWORDs per frame (td5_input.c) and, on playback,
 * restores the per-race RNG seed and re-feeds those inputs, hoping the whole
 * sim (physics + AI branch picks + dynamic traffic spawn) re-runs identically.
 * In practice it drifts -- floating-point accumulation over thousands of ticks,
 * traffic spawn cadence coupled to exact tick/cooldown reproduction, and any
 * stray shared-rand()/frame-rate coupling make the replay "not the race I just
 * drove" (opponents and traffic diverge).
 *
 * WHAT THIS DOES INSTEAD
 * ----------------------
 * Record the ACTUAL per-tick transform of EVERY actor (player, AI opponents,
 * AND traffic) into a memory buffer during the real race. On "View Replay",
 * DISABLE the sim (AI, physics, dynamic-traffic spawn) entirely and POSE each
 * actor straight from the recorded buffer. Traffic is not spawned by logic --
 * its exact per-tick visibility (alpha) is recorded and re-applied manually.
 * Because nothing is re-simulated, the replay cannot deviate.
 *
 * Memory-only (matches the original M2DX, which never persisted replays). The
 * buffer survives between the recorded race ending and the "View Replay" race
 * starting exactly like the legacy input buffer does (both are process-lifetime
 * statics); no disk file is written.
 *
 * Gated by TD5RE_GHOST_REPLAY (cached, default ON). "0" disables the recorder
 * and the pose path, falling back to the legacy input re-sim replay.
 *
 * LIFECYCLE (driven from td5_game.c InitRace + the sub-tick loop):
 *   normal race init  -> td5_replay_begin_record()
 *   each live sub-tick -> td5_replay_record_tick(sim_tick)   (end of tick)
 *   View Replay init   -> td5_replay_begin_playback()
 *   each live sub-tick -> td5_replay_pose_tick(sim_tick)     (in place of sim)
 */
#ifndef TD5_REPLAY_H
#define TD5_REPLAY_H

#include <stdint.h>

/* 1 when the ghost recorder/pose path is enabled (TD5RE_GHOST_REPLAY != "0").
 * Cached + logged once on first read. The tick loop uses this to choose
 * pose-from-buffer vs run-the-sim on a replay. */
int  td5_replay_ghost_enabled(void);

/* Start a fresh recording for the race about to run. Clears any previous
 * buffer and captures the actor count. No-op when ghost replay is disabled. */
void td5_replay_begin_record(void);

/* Switch to playback for a "View Replay" race. Keeps the buffer recorded by the
 * preceding race. No-op when ghost replay is disabled or nothing was recorded. */
void td5_replay_begin_playback(void);

/* Snapshot every actor's transform for `sim_tick` into the buffer. Called once
 * per LIVE sub-tick at end of tick, only while recording. */
void td5_replay_record_tick(uint32_t sim_tick);

/* Pose every actor from the recorded snapshot for `sim_tick` (clamped to the
 * last recorded tick if the replay runs longer than the recording). Called once
 * per LIVE sub-tick in place of the AI/physics sim, only while playing back. */
void td5_replay_pose_tick(uint32_t sim_tick);

/* 1 while a recording is in progress (recorder armed and not yet capped). */
int  td5_replay_is_recording(void);

/* Number of live sub-tick frames captured in the current buffer. */
int  td5_replay_frame_count(void);

#endif /* TD5_REPLAY_H */
