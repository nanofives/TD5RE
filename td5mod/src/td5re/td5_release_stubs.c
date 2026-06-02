/*
 * td5_release_stubs.c -- RELEASE-build no-op definitions for the always-on
 * pilot-trace leaf emitters.
 *
 * In the DEV build these symbols are provided by the individual
 * td5_pilot_trace_*.c modules (full CSV instrumentation). The RELEASE build
 * excludes those modules (see build_standalone.bat), but a handful of emit call
 * sites in td5_physics.c / td5_ai.c / td5_track.c are UNGATED (not behind an
 * #ifdef TD5_PILOT_TRACE_* that the release build drops), so their symbols must
 * still resolve at link time. This file supplies empty definitions so the
 * release link succeeds with zero instrumentation cost — the optimizer inlines
 * these empty calls away, and no CSV files are ever opened.
 *
 * Compiled ONLY into the release build (guarded by TD5RE_RELEASE so that if it
 * ever lands in the dev source list it produces an empty translation unit
 * rather than clashing with the real definitions).
 *
 * Signatures mirror the prototypes in the matching td5_pilot_trace_*.h headers.
 * A mismatch would be a loud compile error here, not a silent runtime bug.
 */
#ifdef TD5RE_RELEASE

#include <stdint.h>
#include "td5_types.h"

/* Every parameter is intentionally unused — these are no-ops. */
#pragma GCC diagnostic ignored "-Wunused-parameter"

void td5_pilot_emit_00403720_enter(const TD5_Actor *actor, uintptr_t caller_ra) {}
void td5_pilot_emit_00403720_leave(const TD5_Actor *actor) {}

void td5_pilot_emit_00403A20_enter(const TD5_Actor *actor, int32_t accel_x, int32_t accel_z, uintptr_t caller_ra) {}
void td5_pilot_emit_00403A20_leave(const TD5_Actor *actor) {}

void td5_pilot_emit_00404030_enter(const TD5_Actor *actor, uintptr_t caller_ra) {}
void td5_pilot_emit_00404030_leave(const TD5_Actor *actor) {}

void td5_pilot_emit_00404EC0_enter(const TD5_Actor *actor, uintptr_t caller_ra) {}
void td5_pilot_emit_00404EC0_leave(const TD5_Actor *actor) {}

void td5_pilot_emit_004057F0_enter(const TD5_Actor *actor, uintptr_t caller_ra, int32_t gravity) {}
void td5_pilot_emit_004057F0_leave(const TD5_Actor *actor) {}

void td5_pilot_emit_00405B40_enter(const TD5_Actor *actor, uintptr_t caller_ra) {}
void td5_pilot_emit_00405B40_leave(const TD5_Actor *actor, int branch_taken) {}

void td5_pilot_emit_00405E80_enter(const TD5_Actor *actor, uintptr_t caller_ra) {}
void td5_pilot_emit_00405E80_leave(const TD5_Actor *actor) {}

void td5_pilot_emit_004063A0_enter(const TD5_Actor *actor, uintptr_t caller_ra) {}
void td5_pilot_emit_004063A0_leave(const TD5_Actor *actor) {}

void td5_pilot_emit_00406650_enter(const TD5_Actor *actor) {}
void td5_pilot_emit_00406650_leave(const TD5_Actor *actor) {}

void td5_pilot_emit_00406980_enter(const TD5_Actor *actor,
                                   int32_t force_x_fp8,
                                   int32_t force_y_fp8,
                                   int32_t force_z_fp8,
                                   uint32_t angle,
                                   int32_t magnitude,
                                   uint32_t flags) {}
void td5_pilot_emit_00406980_leave(const TD5_Actor *actor) {}

void td5_pilot_emit_0042EB10(uint32_t tick,
                             int      slot,
                             const char *kind,
                             int      wheel,
                             uint32_t caller_ra,
                             const void *param1,
                             const void *param2,
                             int16_t p0, int16_t p1, int16_t p2,
                             const float matrix[12],
                             int32_t out0, int32_t out1, int32_t out2) {}

void td5_pilot_emit_0042EBF0_inputs(const TD5_Actor *actor,
                                    const int32_t v1[3],
                                    const int32_t v2[3]) {}
void td5_pilot_emit_0042EBF0_outputs(const TD5_Actor *actor,
                                     const int32_t v1[3],
                                     const int32_t v2[3],
                                     int32_t cross_x,
                                     int32_t cross_z) {}

void td5_pilot_emit_0042F030_enter(const TD5_Actor *actor, uintptr_t caller_ra) {}
void td5_pilot_emit_0042F030_leave(const TD5_Actor *actor, int32_t return_value,
                                   int32_t lut_index_used, int32_t lut_frac_used) {}

void td5_pilot_emit_00432D60_enter(void) {}
void td5_pilot_emit_00432D60_leave(void) {}

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
                                   int16_t tpl_top_spd) {}
void td5_pilot_emit_00432E60_leave(int32_t rb_behind_scale,
                                   int32_t rb_behind_range,
                                   int32_t rb_ahead_scale,
                                   int32_t rb_ahead_range,
                                   int16_t tpl_steer_out,
                                   int16_t tpl_grip_out,
                                   int16_t tpl_brake_out,
                                   int16_t tpl_lspd_brake_out,
                                   int16_t tpl_top_spd_out,
                                   int     racer_count) {}

void td5_pilot_emit_004340C0_enter(int slot,
                                   const int32_t *rs,
                                   const void *actor,
                                   int32_t steer_weight,
                                   const char *call_site) {}
void td5_pilot_emit_004340C0_leave(int slot,
                                   const int32_t *rs,
                                   const void *actor) {}

void td5_pilot_emit_00434FE0_enter(int slot, const int32_t *rs, const void *actor, int32_t game_type) {}
void td5_pilot_emit_00434FE0_leave(int slot, const int32_t *rs, const void *actor) {}

void td5_pilot_emit_00436A70_enter(void) {}
void td5_pilot_emit_00436A70_leave(void) {}

void td5_pilot_emit_004440F0_enter(const void *probe,
                                   const int32_t *world_pos,
                                   uintptr_t caller_ra) {}
void td5_pilot_emit_004440F0_leave(const void *probe, int retval) {}

void td5_pilot_trig_emit(const char *which_fn, int32_t arg_raw, uint32_t ret_bits, float ret_val) {}

#endif /* TD5RE_RELEASE */
