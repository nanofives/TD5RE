/*
 * td5_trace_whole_state.h -- Tick-aligned whole-actor + globals snapshot
 *
 * Counterpart to tools/frida_whole_state_snapshot.js. Both sides emit one
 * binary record per sim tick at the same logical instant:
 *   - port:     end of fixed-tick loop body, just before
 *               g_td5.simulation_tick_counter++ (td5_game.c)
 *   - original: UpdateRaceCameraTransitionTimer @ 0x40A490 onEnter
 *
 * Record layout (5560 bytes) -- MUST match frida_whole_state_snapshot.js:
 *   +0x0000  uint32  frame
 *   +0x0004  uint32  sim_tick
 *   +0x0008  uint8[6][0x388]  actor slots verbatim
 *   +0x1538  uint8[128]       globals blob
 *   = 5560 bytes
 *
 * File layout begins with a 16-byte header (magic 'TD5W', version=1).
 *
 * Format spec: re/analysis/whole_state_diff_design.md
 */

#ifndef TD5_TRACE_WHOLE_STATE_H
#define TD5_TRACE_WHOLE_STATE_H

#include <stdint.h>

/* The caller (td5_game.c) fills this in from its access to g_td5 + local
 * statics, then hands it to td5_trace_whole_state_emit(). Each field maps
 * 1:1 to a position inside the 128-byte globals blob (see .c file for the
 * blob offset table). */
typedef struct TD5_WholeStateGlobals {
    int32_t  game_state;
    int32_t  game_paused;
    int32_t  race_end_fade_state;
    uint32_t sim_time_accumulator;
    float    sim_tick_budget;
    int32_t  simulation_tick_counter;
    float    normalized_frame_dt;
    float    instant_fps;
    int32_t  view_count;
    int32_t  splitscreen_count;
    int32_t  random_seed_for_race;
    int32_t  race_session_random_seed;
    int32_t  race_random_seed_table0;
    uint32_t player_control_bits[2];      /* slot 0 + slot 1 */
    uint32_t race_slot_state_table[6];    /* state/comp1/comp2/reserved packed */
    uint8_t  race_slot_player_flags[6];   /* human=1 / AI=0 */
    uint8_t  race_order_array[6];         /* sorted slot indices */
} TD5_WholeStateGlobals;

/* Open the snapshot output file. Returns 0 on failure (in which case the
 * emit function becomes a no-op). Safe to call before td5_trace_init.
 *
 * out_path: absolute path. If NULL, uses log/whole_state_port.bin relative
 *           to CWD.
 * max_ticks: cap; 0 = unlimited. Cap-hit auto-closes the file. */
int  td5_trace_whole_state_open(const char *out_path, int max_ticks);

/* True once at least one record has been written. Used by callers that
 * want to defer the first flush. */
int  td5_trace_whole_state_is_open(void);

/* Emit one record. Must be called at the same logical instant per tick on
 * both sides (see header doc above). actor_base points to the start of the
 * 6 x 0x388 actor array. */
void td5_trace_whole_state_emit(uint32_t frame, uint32_t sim_tick,
                                const void *actor_base,
                                const TD5_WholeStateGlobals *globals);

/* Flush and close. Idempotent. */
void td5_trace_whole_state_close(void);

#endif /* TD5_TRACE_WHOLE_STATE_H */
