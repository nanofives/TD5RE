/* ========================================================================
 * td5_chaos_fold.c — CHAOS CO-OP: N-seat -> 2-car input fold (PORT-ONLY)
 *
 * Pure function: folds the per-seat polled words into the two team-car
 * words under the current role map. Called from td5_input.c between the raw
 * poll pass and the per-slot post-processing pass. Design:
 * docs/plans/CHAOS_COOP_MODE_PLAN.md section 4.
 *
 * [CONTRACT 2026-09-29] Stub — always inert. The INPUT zone implements it;
 * it must keep the td5_chaos.h signature and stay free of globals other than
 * the td5_chaos_* role queries.
 * ======================================================================== */
#include "td5_chaos.h"

int td5_chaos_fold_inputs(const uint32_t *seat_bits, const int16_t *seat_ax,
                          const int16_t *seat_ay, int seat_count,
                          uint32_t *out_bits, int16_t *out_ax, int16_t *out_ay) {
    (void)seat_bits; (void)seat_ax; (void)seat_ay; (void)seat_count;
    (void)out_bits; (void)out_ax; (void)out_ay;
    return 0;
}
