/*
 * td5_pilot_trace_0042EBF0.h -- Per-call CSV trace emitter for the
 * precise-port pilot of ComputeVehicleSurfaceNormalAndGravity (0x0042EBF0).
 *
 * Workflow doc: re/analysis/precise_port_workflow.md
 * Audit:        re/analysis/pilot_0042EBF0_audit.md
 *
 * One CSV row per call to td5_physics_compute_surface_gravity (chassis-scoped,
 * not per-wheel). Captures pre-normalize v1/v2 from the port body, plus the
 * inputs (4 probes + g + linear_velocity_xz) and the resulting deltas.
 */
#ifndef TD5_PILOT_TRACE_0042EBF0_H
#define TD5_PILOT_TRACE_0042EBF0_H

#include <stdint.h>
#include "td5_types.h"

/* Called from td5_physics_compute_surface_gravity AFTER computing v1/v2 but
 * BEFORE normalizing. Snapshots actor's probe state + v1/v2 pre-normalize. */
void td5_pilot_emit_0042EBF0_inputs(const TD5_Actor *actor,
                                     const int32_t v1[3],
                                     const int32_t v2[3]);

/* Called at the end of td5_physics_compute_surface_gravity. v1/v2 here are
 * the POST-normalize values (length=4096). cross_x/cross_z are the cross
 * components used in the gravity projection. The actor's
 * linear_velocity_x/z reflect the post-update state. */
void td5_pilot_emit_0042EBF0_outputs(const TD5_Actor *actor,
                                      const int32_t v1[3],
                                      const int32_t v2[3],
                                      int32_t cross_x,
                                      int32_t cross_z);

#endif
