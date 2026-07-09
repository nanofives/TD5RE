# Phase 5(e) L4→L5 audit: td5_camera.c + td5_vfx.c

Date: 2026-05-21
Pool slot: TD5_pool12 (acquire returned stale pool0, used pool12 directly)
Total in scope: 23 functions (14 camera + 9 vfx)

## Summary

| Disposition | Count |
|---|---|
| Promoted L5 (CONFIRMED) | 7 |
| Promoted L5 (ARCH-DIVERGENCE) | 4 |
| Left at L4 (real divergence) | 9 |
| Left at L4 (NOT PORTED) | 3 |

## Promoted L5 (CONFIRMED) — 7

1. **0x0042D5B0 OrientCameraTowardTarget** — td5_camera.c:359
   Byte-faithful look-at + yaw + flip. All 3 MultiplyRotationMatrices3x3 calls and FinalizeCameraProjectionMatrices in identical order.

2. **0x00401E10 UpdateRaceCameraTransitionState** — td5_camera.c:1280
   Level 0..3 dispatch (presets 0xD/0xC/0xB/0xA) with __ftol-truncated radius/orbit deltas. Port intentionally drops g_subTickFraction multiplier to match orig's frame-rate-independence.

3. **0x004020B0 InitializeTracksideCameraProfiles** — td5_camera.c:1436
   Profile count scan, anchor geometry seeding, view 0→1 copy, dual rand timers, circuit-track fallback to orbit mode (behavior 4). Match.

4. **0x00402200 SelectTracksideCameraProfile** — td5_camera.c:1508
   11-case (0..10) switch dispatch matches orig constants byte-for-byte: 0x200/0xE2/0xFB9C/0xFE00/0xFF38/0x1000 offsets, mode/timer init.

5. **0x00441F90 BuildCubicSpline3D** — td5_camera.c:2477
   Catmull-Rom builder: P[1] base + (P[i]-P[1])>>8 delta + basis-matrix multiply + /2 normalization. Port inlines orig's DAT_00474bc0 16-int matrix.

6. **0x00401410 InitializeRaceSmokeSpritePool** — td5_vfx.c:614
   Track-config gate at +0x04==1, FindArchiveEntryByName("SMOKE"), pool alloc num_actors*0x170. Match.

7. **0x0042DBD0 TransformVector3ByBasis** — td5_render.c:5336 (NOTE: this lives in td5_render.c, not td5_camera.c)
   Matrix*vec dot products in 3 rows. Math identical. Comment placed in render file (see ARCH-DIV note below).

## Promoted L5 (ARCH-DIVERGENCE) — 4

8. **0x00401950 UpdateTracksideOrbitCamera** — td5_camera.c:1034
   Orbit camera math line-by-line match. ARCH-DIVERGENCE: FPU rounding mode (orig ROUND vs port +0.5f). Visual-only. Flagged dead scribble cruft at lines 1104-1117 (overwritten, not behavior-affecting).

9. **0x0040A480 InitializeRaceCameraTransitionDuration** — td5_camera.c footer
   Trivial 10-byte setter (g_cameraTransitionActive = 0xA000) inlined into port's reset_race_countdown() helper. Same observable state via static init + per-race reset.

10. **0x0042DB40 ConvertFloatVec3ToIntVec3** + **0x0042DC30 ConvertFloatVec3ToIntVec3B** — td5_camera.c footer
    matrix*vec + __ftol + (int)(short) sign-extend → int[3]. Port replaces with TransformVector3ByBasis @ 0x0042DBD0 (no short clamp). Identical for offsets within ±32767.

11. **0x0042DBD0 TransformVector3ByBasis** — td5_render.c:5336 (signature & output type divergence)
    Orig signature is (float*, float*, float*) writing 3 floats via FSTP. Port reuses symbol with (float*, void* as short[3], int*) and adds (int) cast. Camera-side callers in port use this instead of orig's ConvertFloatVec3ToIntVec3 — see camera-side note.

## Left at L4 (real divergence, needs fix) — 9

12. **0x00402950 UpdateStaticTracksideCamera** — td5_camera.c:1641
    Terrain Y sampled at `g_camHeightSampleOfs[v]` (never written → 0) instead of `g_cameraProfileVertOffset[v]` @ 0x00482eb8. Trackside cam Y baseline biased by 1 vertex column.

13. **0x00402AD0 UpdateSplineTracksideCamera** — td5_camera.c:1716
    `s_splineTemplates[6][8]` table is WRONG vs orig's local_5e stack pattern. Orig type-0 effective pairs (-1,0)(0,0)(40,0)(41,0); port type-0 has (0,0)(0,-1)(0,0)(0,-1). Spline flyby arc broken.

14. **0x00402480 UpdateTracksideCamera** — td5_camera.c:1825
    Inherits cases 0 (height sample), 1/2 (TransformVector3ByBasis vs ConvertFloatVec3ToIntVec3) divergences. Dispatch structure otherwise faithful.

15. **0x00429A30 SpawnVehicleSmokeVariant** — td5_vfx.c:2051 (vfx_spawn_smoke_at_position)
    PSLOT_SIZE_W=0x4000 vs orig 0x7000; PSLOT_SIZE_H=0x26C0 vs orig 0x2080. Velocity lacks orig's yaw-rotated random jitter (orig rotates (rand%0x6000-0x3000) by display_yaw cos/sin); port copies actor's linear_velocity_x/z directly. Vel_y=0x600 vs orig 0x1800.

16. **0x0043F030 AcquireTireTrackEmitter** — td5_vfx.c:1378
    Wheel anchor source +0xf0+wheel*0xc (orig probe-position fields) vs +0x298+wheel*0xc (port hires wheel). Subtle 1-tick lag in tire-track anchor.

17. **0x0043F420 UpdateFrontWheelSoundEffects** — td5_vfx.c:2358
    Same wheel-offset divergence as AcquireTireTrackEmitter. Intensity-rounding (SAR11) divergence unreachable for excess>0 hot path.

18. **0x0043F600 UpdateRearWheelSoundEffects** — td5_vfx.c:2473
    Symmetric to 0x0043F420. Same wheel-offset divergence.

19. **0x0043F7E0 UpdateRearTireEffects** — td5_vfx.c:2176
    Same wheel-offset divergence. Port adds ARCH-DIVERGENCE vfx_tire_mark_spawn (float ring buffer for visible tire marks) that orig lacks — intentional port enhancement.

20. **0x0043F960 UpdateFrontTireEffects** — td5_vfx.c:2272
    Symmetric to 0x0043F7E0.

## Left at L4 (NOT PORTED) — 3

21. **0x00401370 SpawnRandomVehicleSmokePuff** — orig 159B. Engine-rev-based rand smoke at rear-probe midpoint. No port symbol; orig's gate not wired.

22. **0x00429FD0 SpawnVehicleSmokePuffAtPoint** — orig 688B helper. Port collapses smoke-spawn pipeline into single vfx_spawn_smoke_at_position with simpler gating.

23. **0x0042E030 ExtractEulerAnglesFromMatrix** — orig 246B. Recovery-mode Euler decomposition (AngleFromVector12 × 3 + gimbal-lock branch on |pitch|=0x400). Sole caller RefreshScriptedVehicleTransforms NOT ported (vehicle_mode==1 scripted recovery — see td5_physics.c:996-1009).

## Suspected regressions (bonus findings)

A. **g_camHeightSampleOfs is a never-written 2-int array** at td5_camera.c:70. Should be `g_cameraProfileVertOffset[2]` written by SelectTracksideCameraProfile (port:1508). Terrain Y baseline in trackside cam types 0 and inline-dispatched case 0 biased.

B. **s_splineTemplates table values do not match orig stack pattern.** Port type 0 has flat-anchor pairs; orig sweeps spans 40-41 ahead of anchor. Spline trackside cam path is incorrect for ALL 6 spline types.

C. **Wheel-anchor read offset wrong in 5 vfx callsites** (AcquireTireTrackEmitter, both wheel-sound updaters, both tire-effect updaters): port reads +0x298 hires-wheel position; orig reads +0xf0 probe position. These actor struct fields are populated by different physics passes; the +0x298 field will lag the +0xf0 field by ~1 sim tick when the vehicle accelerates. Tire tracks visibly trail behind wheel by 1 frame.

D. **SpawnVehicleSmokeVariant size fields swapped or misordered** in vfx_spawn_smoke_at_position. Port SIZE_W=0x4000 ≠ orig 0x7000; SIZE_H=0x26C0 ≠ orig 0x2080. Plus the rotated-jitter velocity pattern (orig rotates (rand%0x6000)-0x3000 by display_yaw) is replaced by direct vel-copy. Net effect: smoke puffs are wrong size and lack organic spread.

E. **UpdateTracksideOrbitCamera has dead scribble code** lines 1104-1117 (orbit_offset writes that are immediately overwritten by 1118-1119). Final state is correct but maintenance hazard. Recommend cleanup pass.

F. **Camera-side use of TransformVector3ByBasis** (port) where orig uses ConvertFloatVec3ToIntVec3 — output type and short-clamp diverge. Within ±32767 range identical; near saturation port retains higher precision.

## Notes
- Pool cleanup script returned exit 1 (no slots to clean) — expected after manual program_close.
- All edits are pure comment additions; no executable code changed.
- File line counts unchanged in semantics; only audit headers added.
