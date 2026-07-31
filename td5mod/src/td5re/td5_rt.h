/**
 * td5_rt.h -- game-side entry to the ray-traced lighting system
 * (LIGHTING QUALITY: HIGH). PORT-ONLY. See docs/plans/RT_LIGHTING_PLAN.md.
 *
 * This is the thin game-facing layer above the platform bridge
 * (td5_plat_rt_*) which fronts the wrapper's Backend_RT* API. Phase 0 exposes
 * only capability + activation predicates; the geometry feed / per-frame driver
 * (td5_rt_level_build / td5_rt_frame / ...) arrive in Phase 1+.
 */
#ifndef TD5_RT_H
#define TD5_RT_H

/* 1 when the GPU/driver support DXR and RT is not force-disabled. Reflects the
 * wrapper's Backend_RTAvailable(); 0 after device-lost until re-queried. When 0
 * the game runs the LOW (screen-space) lighting stack unchanged. */
int td5_rt_available(void);

/* 1 when the RT lighting stack should actually run THIS frame: available AND
 * LIGHTING QUALITY == HIGH AND in a race (not frontend/FMV/garage) AND lighting
 * enabled. LOW behaves byte-identically to master, so this must be 0 there. */
int td5_rt_active(void);

/* Set the requested quality (0 = LOW, 1 = HIGH). Menu/INI drive this (Phase 4);
 * until then it is env-seeded (TD5RE_RT). Ignored upward when RT unavailable. */
void td5_rt_set_quality(int high);
int  td5_rt_quality_high(void);

#endif /* TD5_RT_H */
