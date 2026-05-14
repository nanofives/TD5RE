/*
 * td5_pilot_trace_trig.h -- CSV emitter for the precise-port trig-pair pilot
 * (0x0040A6A0 CosFloat12bit + 0x0040A6C0 SinFloat12bit).
 *
 * Hook is in td5_render.c CosFloat12bit/SinFloat12bit. Writes one row per call
 * to log/port/pool3_trig.csv matching tools/frida_pool3_trig.js column-for-
 * column so tools/diff_func_trace.py can pair rows by
 * (sim_tick, which_fn, call_idx).
 *
 * Audit: re/analysis/pilot_trig_audit.md
 */
#ifndef TD5_PILOT_TRACE_TRIG_H
#define TD5_PILOT_TRACE_TRIG_H

#include <stdint.h>

/* Capture one row for a CosFloat12bit/SinFloat12bit call.
 *   which_fn  -- "cos" or "sin" (literal string, no copy)
 *   arg_raw   -- the raw 32-bit signed input the caller passed
 *   ret       -- the bit pattern (u32 reinterpret) of the returned float
 *   ret_val   -- the float value (for human inspection only) */
void td5_pilot_trig_emit(const char *which_fn, int32_t arg_raw, uint32_t ret_bits, float ret_val);

#endif
