/*
 * td5_pilot_trace_0040A720.h — port-side capture for the precise-port pilot
 * targeting 0x0040A720 AngleFromVector12.
 *
 * Build is gated on the TD5_PILOT_TRACE_0040A720 macro so the production build
 * has zero overhead. Define this macro in build_standalone.bat (or directly in
 * the calling .c) to wire the emit hook.
 */
#ifndef TD5_PILOT_TRACE_0040A720_H
#define TD5_PILOT_TRACE_0040A720_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called once per call to AngleFromVector12 (from inside the function itself).
 * Writes one CSV row to log/port/pool2_0040A720.csv with the schema:
 *   sim_tick,call_idx,caller_ra,p1,p2,ret
 * sim_tick is read from td5_trace_current_sim_tick().
 * call_idx increments monotonically per process; caller_ra is the return PC. */
void td5_pilot_trace_0040A720_call(int p1, int p2, int ret);

#ifdef __cplusplus
}
#endif

#endif /* TD5_PILOT_TRACE_0040A720_H */
