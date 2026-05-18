/*
 * td5_pilot_trace_pool15_spline.h — Port-side CSV trace for paired pilot
 *  0x00434670 td5_track_compute_signed_offset
 *  0x00434800 td5_track_sample_target_point
 *
 * Output: log/port/pool15_spline.csv (single file; which_fn column
 * distinguishes the two addresses).
 *
 * Schema mirrors tools/frida_pool15_spline.js column-for-column.
 *
 * Workflow: re/analysis/precise_port_workflow.md
 *           re/analysis/pilot_spline_target_audit.md
 */
#ifndef TD5_PILOT_TRACE_POOL15_SPLINE_H
#define TD5_PILOT_TRACE_POOL15_SPLINE_H

#include <stdint.h>

/* Emit a row for the 0x00434670 path.
 *   out_value = function return (signed magnitude) */
void td5_pilot_trace_pool15_emit_signed_offset(int span_index,
                                               int progress,
                                               int route_byte,
                                               int32_t out_value);

/* Emit a row for the 0x00434800 path.
 *   out_x, out_z = 24.8 FP output written by the function. */
void td5_pilot_trace_pool15_emit_target_point(int span_index,
                                              int route_byte,
                                              int lateral_bias,
                                              int32_t out_x,
                                              int32_t out_z);

#endif
