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

/* [CAR SHADOW 2026-08-06] 0 (default) = cars do NOT cast the RT sun shadow (fed
 * to the TLAS with the sun-shadow caster bit cleared) and are grounded by the
 * soft terrain-conforming blob instead; 1 = restore the old RT car-cast (car
 * body BLAS casts the sun shadow, blob dropped). Env TD5RE_RT_CAR_CAST. */
int td5_rt_car_cast_shadow(void);

/* Set the requested quality (0 = LOW, 1 = HIGH). Menu/INI drive this (Phase 4);
 * until then it is env-seeded (TD5RE_RT). Ignored upward when RT unavailable. */
void td5_rt_set_quality(int high);

/* [RT2 P8] Map the LIGHTING OPTIONS INI tiers (g_td5.ini.rt_*) onto the
 * TD5RE_RT_* env knobs the RT passes read. Call once at startup after the INI
 * loads (and best-effort after a menu change). An explicitly-set env wins. */
void td5_rt_apply_lighting_options(void);
int  td5_rt_quality_high(void);

/* [Phase 1] Build the track acceleration geometry from the (finalized) strip
 * span table. Call once at level-load completion; safe no-op when RT is
 * unavailable. Idempotent (destroys prior track meshes first). */
void td5_rt_level_build(void);

/* [Phase 1] Destroy all RT meshes (track + cached actor meshes) at level unload. */
void td5_rt_level_unload(void);

/* [RT WARMUP 2026-08-08] Front-load the first-HIGH-frame RT cost onto the loading
 * screen. Call once at the END of InitRace (MODELS.DAT parsed, actors spawned):
 * it performs the full-scene mesh feed that td5_rt_frame otherwise defers to race
 * frame 1 and marks it done. Returns 1 when RT is HIGH-active and the caller
 * should pump the loading-screen BLAS-drain warmup (td5_plat_rt_warmup_*), 0 when
 * RT is LOW/unavailable (nothing to warm). */
int td5_rt_warmup_prepare(void);

/* [RT WINDOW] 1 once the player-relative scenery window has been built at least
 * once, or windowing is not in use (RT off / full feed). The streamed-track
 * countdown hold waits on this so the road around the grid has its ray-traced
 * shadows before the lights go green, rather than building them as a hitch in
 * the first seconds of the drive. Cheap; safe to call every frame. */
int td5_rt_scenery_window_ready(void);

/* [Phase 1] Per-pane RT frame driver. Called once per viewport `vp` from the
 * render loop BEFORE the deferred passes, with the pane rect. On vp==0 it
 * (re)builds the TLAS from the track + active actors; every pane it uploads the
 * pane camera view. When TD5RE_RT_DEBUGVIEW is set it dispatches the primary-ray
 * debug view over the pane. No-op when RT is unavailable. */
void td5_rt_frame(int vp, int pane_x, int pane_y, int pane_w, int pane_h);

#endif /* TD5_RT_H */
