/*
 * td5_pilot_trace_00409150.h — port-side capture for the precise-port pilot
 * targeting 0x00409150 ResolveVehicleContacts (V2V iteration driver).
 *
 * Build is gated on the TD5_PILOT_TRACE_00409150 macro so the production build
 * has zero overhead. Define this macro in build_standalone.bat to wire the
 * emit hooks.
 *
 * Two CSV streams (per pool naming, mirroring Frida's two-output JS):
 *   log/port/pool9_00409150_phase1.csv  -- AABB build (one row per slot per call)
 *   log/port/pool9_00409150_pair.csv    -- pair tests (one row per pair invocation)
 *
 * Pair rows are the matching point for cross-port diff (pair-event index by
 * sim_tick + slot_a + slot_b). Phase1 rows give the upstream state.
 */
#ifndef TD5_PILOT_TRACE_00409150_H
#define TD5_PILOT_TRACE_00409150_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call once at function entry. Emits no rows directly but increments the
 * per-call sequence so pair events can be coordinated. */
void td5_pilot_trace_00409150_enter(int racer_count);

/* Phase 1: one call per slot, after AABB is computed and chain is inserted.
 *   slot            -- racer slot index (0..racer_count-1)
 *   world_pos_x_sar8, world_pos_z_sar8  -- actor->world_pos.{x,z} >> 8 (port view)
 *   cardef_radius   -- (int16) cardef[+0x80]
 *   span            -- actor->track_span_normalized (raw, NOT clamped)
 *   bucket          -- the grid bucket index actually used (port mod 256)
 *   prev_head       -- the previous chain head (= bounds[slot].chain after insert)
 *   xmin,zmin,xmax,zmax -- AABB values written
 */
void td5_pilot_trace_00409150_phase1(int slot,
                                     int32_t world_x_sar8, int32_t world_z_sar8,
                                     int32_t cardef_radius,
                                     int32_t span, int32_t bucket,
                                     int32_t prev_head,
                                     int32_t xmin, int32_t zmin,
                                     int32_t xmax, int32_t zmax);

/* Phase 2 pair test (one row per dispatched pair).
 *   slot_a, slot_b  -- pair indices
 *   bucket_off      -- bucket walk offset relative to slot_a's base (-1,0,+1)
 *   walk_iter       -- chain walk iteration index (0..16)
 *   dispatch        -- 0 = collision_detect_full, 1 = collision_detect_simple
 *   mode_a, wcb_a, mode_b, wcb_b -- gate inputs
 *   pair_idx        -- monotonic per-call pair sequence (resets at enter)
 */
void td5_pilot_trace_00409150_pair(int slot_a, int slot_b,
                                   int bucket_off, int walk_iter,
                                   int dispatch,
                                   uint8_t mode_a, uint8_t wcb_a,
                                   uint8_t mode_b, uint8_t wcb_b,
                                   int pair_idx);

/* Function-exit. Flushes if necessary; emits a "leave" sentinel row in
 * the pair CSV with slot_a=-1, slot_b=-1, pair_idx=total_pairs. */
void td5_pilot_trace_00409150_leave(int total_pairs);

#ifdef __cplusplus
}
#endif

#endif /* TD5_PILOT_TRACE_00409150_H */
