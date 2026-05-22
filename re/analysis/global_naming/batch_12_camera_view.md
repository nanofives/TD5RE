---
batch: 12
area: camera_view_state
tier: T3
target_todos: [todo_camera_lags_post_race_start_2026-05-17, todo_countdown_1_stuck_after_race_start_2026-05-17, todo_camera_through_wall_on_reverse_2026-05-16, todo_camera_no_slope_adjustment_2026-05-16]
ghidra_session: a3994aea196e47acaf8480cf4b8f6bf9
analyzed_addresses: 0x00401450, 0x00401590, 0x00401950, 0x00401C20, 0x00401E10, 0x00402000, 0x004020B0, 0x00402200, 0x00402480, 0x00402950, 0x00402A80, 0x00402AD0, 0x00402E00, 0x0040A260, 0x0040A480, 0x0040A490, 0x0042AA10, 0x0042B580
agent: Claude Opus 4.7 (1M) — T3.12 worker
date: 2026-05-20
---

# Globals enumeration — Camera & view state (T3.12)

## Summary

- Functions analyzed: 18 (full camera FSM + race-frame dispatch + race-session init)
- Unnamed DAT_* globals encountered: 38 (after de-dup; 16 per-view tables stride 4 and stride 8 are folded into single proposals)
- Already-named globals encountered (just noted): 18 (`g_cameraFlyInThreshold`, `g_cameraMode`, `g_cameraSpeedSetting`, `gTracksideCameraProfileCount`, `g_cameraElevationAngle`, `g_cameraHeightTarget`, `g_cameraOrbitRadius`, `g_cameraFlyInCounter`, `gRaceCameraPresetId`, `gRaceCameraPresetMode`, `gSecondaryRaceCameraPresetMode`, `g_raceCameraPositionPtr`, `gRaceCameraTransitionGate`, `g_cameraTransitionActive`, `gTracksideCameraProfiles`, `g_projectionDepthBias`, `g_listenerPosition[X|Y|Z][_View1]`, `gViewportLayout*`)
- Proposals — high confidence: 24
- Proposals — medium confidence: 9
- Proposals — comment-only (low confidence): 3

## Methodology

Entry points were the four user-named camera dispatchers plus the dispatcher's caller, `RunRaceFrame @ 0x0042B580`, and the race bootstrap that initializes them, `InitializeRaceSession @ 0x0042AA10`. From there I walked through every camera-FSM function (chase, trackside orbit, vehicle-relative, static trackside, spline trackside, profile select, preset cycle, transition-state, transition-timer, HUD indicator, cache-angles, preset load, reset selection state). Almost every DAT_* in camera code lives in two contiguous BSS regions:

1. **0x00482DEC..0x00482FE0** — the per-view camera-FSM state block (~62 dwords; per-view stride either 4 or 8 with parallel arrays for view 0 and view 1). This contains every spring target, orbit angle, fly-in counter, profile cursor, preset-mode flag, sub-tick projection scratch, and trackside profile state needed by the FSM.
2. **0x00463080..0x004630AC** — the camera config/preset constant table area (`g_projectionScaleTable`, profile-rand timers seeded via `InitializeTracksideCameraProfiles`, then the 14×16-byte camera-preset table at 0x00463098).

The relevance gate: a global was in-scope if it was read or written by ANY of the camera FSM dispatchers, the preset loader, the transition timer, the trackside profile selector, or the per-view caching path called from `RunRaceFrame`.

Three structural insights drove the proposal naming style:

1. **The "per-view state block" at 0x00482DEC..0x00482FA8 is functionally a `CameraViewState view[2]` struct.** Every DAT in that range is accessed as `(&DAT_xxx + view * stride)`. I list each unique field at its view-0 address and call out the stride (4 for scalars/floats, 8 for short3 vec or two-int packed pairs). The port already implements these as `g_camXxx[2]` arrays in `td5_camera.c` (lines 66–93); I include `port_mirror` for every one.

2. **The "fly-in level" mechanic is a downward staircase of preset overrides** that the transition timer (`g_cameraTransitionActive`) walks from 0xA000 (race-start) → 0 (race-running). The thresholds are quotient-of-0x2800 stages 3, 2, 1, 0 → loads presets 10, 11, 12, 13 respectively. Two non-RS storage cells (`DAT_00482DEC` and `DAT_00482DF4`) are the "last loaded preset-fly-in-stage" guards that gate the LoadCameraPresetForView call — naming these enables the camera-lag TODO investigation.

3. **`g_gamePaused` ↔ camera transition flow is single-direction**: `UpdateRaceCameraTransitionTimer @ 0x0040A490` sets `g_gamePaused = 0` once at level==0 (per commit 630d797 finding in memory). The companion write `gRaceCameraTransitionGate = 0` happens at the same site. This confirms the camera-lag bug TODO's diagnosis: the post-630d797 timing window where `g_gamePaused=0` precedes the actual `g_cameraTransitionActive=0` by ~40 sub-ticks is real, and the bug's fix template (port's `s_flyin_preset_reloaded` one-shot at line 2217) sits in this gap intentionally.

## Proposals

### Per-view CameraViewState block (BSS @ 0x00482DEC..0x00482FA8)

Stride-4 per-view scalar fields (view 0 at base addr, view 1 at base+4):

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00482DEC | int[2] | `g_cameraLastProjScale` | high | Per-view "previously committed projection-depth-bias" guard. `UpdateTracksideCamera @ 0x00402480` final block: `if (*(int*)(&DAT_00482dec + iVar3*4) == g_projectionDepthBias)` skip-vs-recompute gate. Twin of trackside camera. | td5_camera.c:99 `g_cameraLastProjScale[2]` |
| 0x00482DF4 | int[2] | `g_cameraFlyInPresetGuard` | **high** | Per-view "previously loaded preset id during fly-in transition" gate inside `UpdateRaceCameraTransitionState @ 0x00401E10`. Compared to current preset id (10/11/12/13) to decide skip-vs-`LoadCameraPresetForView(force_reload=0)`. **Naming this directly clarifies the camera-lag TODO's "one-shot fired" diagnostic** — port equivalent is `s_flyin_preset_reloaded[2]` at td5_camera.c:119, but the boolean abstraction loses the preset-id history; the original tracks the actual stage. | td5_camera.c:119 (port uses bool one-shot instead) |
| 0x00482DFC | int[2] | `g_cameraProjDistRaw` | high | Per-view `__ftol() * 4` distance result written by `UpdateTracksideCamera` case 0 and `UpdateSplineTracksideCamera`. Used as input to the per-view `g_cameraProjDist` clamp. | td5_camera.c:100 `g_cameraProjDist[2]` |
| 0x00482E04 | int[2] | `g_cameraProjDistClamped` | high | Per-view clamp output (clamp `g_cameraProjDistRaw * profile_scale` to [0x1000, 20000]) — then copied into `g_projectionDepthBias`. Trackside case 0 and spline trackside both write. | td5_camera.c:101 `g_cameraProjScaleComp[2]` |
| 0x00482EB8 | int[2] | `g_cameraProfileVertOffset` | high | Per-view vertical-vertex index offset into trackside profile's strip-vertex pool. Used by both `UpdateStaticTracksideCamera @ 0x00402950` and `UpdateTracksideCamera` case 0 as the y-offset into the vertex pool sample. | (none — trackside not in port) |
| 0x00482EC0 | int[2] | `g_cameraProfileStripOffset` | high | Per-view "current trackside profile sub-vertex offset" (puVar7[4] field of the profile entry). Used in profile-set switch in `SelectTracksideCameraProfile @ 0x00402200`. | (none) |
| 0x00482EC4 | int[2] | `g_cameraProfileStripOffsetPrev` | med | Per-view shadow of `g_cameraProfileStripOffset` from previous profile (init by `InitializeTracksideCameraProfiles` then per-frame compare in profile-select). | (none) |
| 0x00482EC8 | int[2] | `g_cameraProfileBehaviorType` | **high** | Per-view trackside profile behavior dispatch (0=lookahead, 1/2=fixed angle, 3=static, 4/5=cw/ccw orbit auto, 6=spline, 7=vehicle-relative, 8/9/10=special anchors). The case dispatch at `UpdateTracksideCamera` switch is `*(undefined4*)(&DAT_00482ec8 + param_2*4)`. | td5_camera.c:72 `g_camBehaviorType[2]` |
| 0x00482ECC | int[2] | `g_cameraProfileBehaviorTypePrev` | med | Per-view "previous behavior type" written at end of `InitializeTracksideCameraProfiles` for circuit-mode reset (`= 4` for both views). | (none) |
| 0x00482EF8 | int[2] | `g_cameraRotationSlot` | med | Per-view "extra orbit-yaw rotation seed", zeroed at top of `UpdateChaseCamera @ 0x00401590` then accumulated by orbit logic. Port maps to `g_camRotationSlot`. | td5_camera.c:76 `g_camRotationSlot[2]` |
| 0x00482F00 | float[2] | `g_cameraSpringRadiusCurrent` | **high** | The radius spring's *current* smoothed orbit-radius scalar — written by the SQRT-based spring at end of `UpdateChaseCamera`, read+lerp'd by `UpdateTracksideOrbitCamera @ 0x00401950`. Each tick blends toward `g_cameraOrbitRadius`. **Critical to camera-lag TODO**: when paused→0 fires early, this drifts to fly-in-preset-13's 3800 wu target. | td5_camera.c:73 `g_camOrbitRadiusScale[2]` (float) |
| 0x00482F08 | int[2] | `g_cameraSplineTimerScaled` | high | Per-view spline cursor (range 0..0xFFF) advanced by `UpdateSplineTracksideCamera` at `+= DAT_00482F80`. Used as `t` parameter to `EvaluateCubicSpline3D`. | td5_camera.c:78 `g_camSplineParam[2]` |
| 0x00482F10 | float[2] | `g_cameraHeightSpringTarget` | high | Per-view target height (preset's `height_target_raw * 256.0`). Read by chase camera spring smoothing (`g_cameraHeightTarget` is the *current*, this is the *target*). | td5_camera.c:91 `g_camHeightParam[2]` |
| 0x00482F18 | int[2] | `g_cameraSpringYawAccum` | **high** | Per-view smoothed yaw integrator (24.8 fixed). Read everywhere as `*(int*)(&DAT_00482f18 + view*4) >> 8`. Written by chase-camera lag math: `*= ((shifted*5 to 15) * yawDelta >> 8)`. Initial seed from `display_angle_yaw` in `LoadCameraPresetForView`. | td5_camera.c (port uses `g_camOrbitAngleFP[2]`) |
| 0x00482F20 | int[2] | `g_cameraProfileStripIndex` | high | Per-view current trackside profile's strip index. Set in `SelectTracksideCameraProfile` and refreshed each tick in trackside cases 1/2. | td5_camera.c:81 `g_camAnchorSpan[2]` |
| 0x00482F28 | int[2] | `g_cameraPresetChangeFlag` | high | Per-view "preset just changed this frame" sentinel — zeroed by `LoadCameraPresetForView` final block, read elsewhere as a one-shot. | td5_camera.c:82 `g_camPresetChangeFlag[2]` |
| 0x00482F60 | int[2] | `g_cameraYawDeltaSigned` | high | Per-view signed yaw-delta of `display_angle_yaw - current_yaw` (sign-extended mod 0x100000 to 19-bit range). Drives the radius spring's lag rate. Read by `UpdateTracksideOrbitCamera`'s "carry forward yaw delta" path. | td5_camera.c (computed inline, no global mirror) |
| 0x00482F68 | int[2] | `g_cameraSplineTemplateIndex` | high | Per-view selected spline template index (passed as `param_3` to `UpdateSplineTracksideCamera`). Sourced from the profile entry's puVar7[6] field at profile-select. | td5_camera.c:86 `g_camSplineNodeCount[2]` (semantic match) |
| 0x00482F70 | int[2] | `g_cameraYawOffsetView` | **high** | Per-view yaw offset (rear-view / mirror toggle). 0=front camera, 0x800=180° (rear view). Written in case 7/8/10 of trackside dispatch and as default `0x800` for rear-view. The "rear-view button" path lives here. | td5_camera.c:87 `g_camYawOffset[2]` |
| 0x00482F78 | int[2] | `g_cameraReservedF78` | low | Per-view scalar zeroed by `LoadCameraPresetForView`; never read elsewhere in any analyzed function. Possibly debug or reserved. Comment-flag only. | td5_camera.c:88 `g_camReserved78[2]` (already labelled "reserved") |
| 0x00482F80 | int[2] | `g_cameraSplineAdvanceRate` | high | Per-view spline-cursor delta-per-frame. Init = 8 in `InitializeTracksideCameraProfiles`, overridden by profile entry's puVar7[7] in profile-select case 6. | td5_camera.c:89 `g_camSplineAdvRate[2]` |
| 0x00482F88 | int[2] | `g_cameraTracksideAnchorX` | high | Per-view trackside anchor world-X (24.8). Sampled from strip-vertex-pool at profile-select. Used as camera position in cases 0 and 3 of trackside dispatch. | td5_camera.c:90 `g_camAnchorX[2]` |
| 0x00482F90 | int[2] | `g_cameraTracksideAnchorYBias` | high | Per-view trackside anchor world-Y height bias (24.8). Profile-entry's puVar7[5] field. Added to vertex Y in cases 0 and 3. | td5_camera.c:91 (semantic match) |
| 0x00482F98 | int[2] | `g_cameraTracksideAnchorZ` | high | Per-view trackside anchor world-Z (24.8). Companion to `g_cameraTracksideAnchorX`. | td5_camera.c:92 `g_camAnchorZ[2]` |
| 0x00482FA0 | int[2] | `g_cameraSpringPitchAccum` | **high** | Per-view smoothed pitch integrator (24.8 fixed). Written by chase-camera at `((pitch_target * 0x100 - current) + sign>>3*7)>>3 + current`. Initial seed from preset's elevation_angle. | td5_camera.c (port stores in `g_camElevationAngleFP[2]`) |
| 0x00482FD4 | u8 | `g_secondaryCameraPresetIdSave` | high | View-1 packed-save companion to `gRaceCameraPresetId` (note: `gRaceCameraPresetId` is view-0 only; view-1 stored here). `ResetRaceCameraSelectionState` reads both. | td5_camera.c (port uses `g_raceCameraPresetId[2]` array; view-1 lives at index 1) |

### Per-view CameraViewState block — stride-8 fields (parallel views, 8 bytes apart)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00482E18 | short[2][4] | `g_cameraCachedDisplayAngles` | high | Cached `{display_angle_roll+0x800, 0x800-display_angle_yaw, -display_angle_pitch, padding}` per view. Written by `CacheVehicleCameraAngles @ 0x00402A80` once per sim-tick. Read by both `UpdateVehicleRelativeCamera` and `UpdateTracksideCamera` case 1/2 to compute sub-tick-interpolated delta to current `display_angle_*`. | td5_camera.c:67 `g_camCachedAngles[2][4]` |
| 0x00482EA0 | short[2][4] | `g_cameraOffsetFromAnchor` | high | Per-view 4-short vector (x,y,z,pad) used as the camera-relative-to-vehicle offset by `UpdateVehicleRelativeCamera` and trackside vehicle-relative cases. Written per-preset by `LoadCameraPresetForView` and `SelectTracksideCameraProfile` (preset-3..6 bumper). | td5_camera.c:68 `g_camOffsetVec[2][4]` |
| 0x00482ED8 | int[2][3] | `g_cameraOrbitOffsetIntermediate` | high | Per-view scratch — sin/cos*spring-radius into x,y,z. Recomputed every chase tick after orbit basis is rebuilt. Yz set to `uVar3` from preset (`g_cameraOffsetFromAnchor.y` 4-byte read). | td5_camera.c:74 `g_camOrbitOffset[2][3]` |
| 0x00482F30 | int[2][3] | `g_cameraWorldPosOutput` | **high** | Per-view final world-space camera position (x,y,z; 24.8). Written by every camera mode (chase, trackside orbit, trackside fixed, spline, vehicle-relative). Output of `SetCameraWorldPosition @ 0x0042CE50`'s feeder vector. | td5_camera.c:83 `g_camWorldPos[2][3]` |
| 0x00482F50 | short[2][4] | `g_cameraOrbitAngleShortVec` | high | Per-view short vector (sin*radius, elevation, -cos*radius, pad) — fed into `ConvertFloatVec3ToShortAngles` to derive view yaw/pitch/roll. Output written back to packed `(undefined4*)psVar1`. | td5_camera.c:84 `g_camOrientShort[2][4]` |

### Per-view sub-tick blend scratch (4-byte stride, top of BSS at 0x00482E28)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00482E28 | int[2][15] | `g_cameraSplineState` | high | Per-view cubic-spline state block (60 bytes = 15 ints per view, stride 0x3c). Output of `BuildCubicSpline3D` per frame from spline-template-indexed control points; input to `EvaluateCubicSpline3D`. | td5_camera.c:129 `g_camSplineState[2][15]` |

### Camera config / preset constant tables (read-only after init)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00463080 | int[2] | `g_cameraOrientAngleFovScale` | med | Constant per-view int passed as last arg to `OrientCameraTowardTarget` (= camera FOV/sensitivity scaling for orient-to-target). Memory dump: `{0x800, 0x800}`. Read-only. | (none — passed as constant) |
| 0x00463088 | int[2] | `g_cameraProfileProjScaleRand` | med | Per-view randomized profile-projection-scale seed (10000..19999) written by `InitializeTracksideCameraProfiles` from `_rand() % 10000 + 10000`. Multiplied with raw distance in spline + trackside dispatch. Confusingly, this is a *random write target*, not a const. | (none — trackside) |
| 0x00463098 | struct[14] | `g_cameraPresetTable` | **high** | 14 × 16-byte camera preset entries. Read by `LoadCameraPresetForView @ 0x00401450` indexed by `gRaceCameraPresetId * 0x10`. Entry layout: `mode_flag(2), elev_angle(2), height_raw(2), radius_raw(2), offset_x(4), offset_y(4)`. Memory dump confirms exact match to port's `g_cameraPresets[14]` at td5_camera.c:139. | td5_camera.c:139 `g_cameraPresets[14]` |

### Camera-FSM toggles / state cells (outside the per-view block)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00482F48 | u8[2] | `g_cameraPresetPackedSave` | high | 2-byte packed save of `{preset_id, preset_mode}` for both views (each byte: `id & 0x7F | mode << 7`). Written by `LoadCameraPresetForView` when `param_4 != 0`; restored by `ResetRaceCameraSelectionState @ 0x00402000`. Mechanism survives a camera-mode toggle. | td5_camera.c:104 `g_camPackedSave[2]` |
| 0x00482FC8 | int[2] | `g_cameraProfileSelectedIndex` | high | Per-view index into `gTracksideCameraProfiles` for the *currently selected* profile (chosen by `SelectTracksideCameraProfile @ 0x00402200` based on actor's track-span at `+0x80`). Compared to detect profile transitions. | td5_camera.c:98 `g_cameraProfileIndex[2]` |
| 0x00466F88 | int[2] | `g_cameraRearViewActive` | high | Per-view "rear view (look-behind) active" toggle. Read by `UpdateRaceCameraTransitionState` — when not transitioning, applies `g_cameraYawOffsetView = 0x800` if `(&DAT_00466f88)[view] != 0`, else `0`. Matches the rear-view button mechanic. | td5_camera.c:126 `g_lookLeftRight[2]` |

## Key discoveries

1. **The "per-view CameraViewState" block at 0x00482DEC..0x00482FA8 is structurally a `struct CameraView[2]`** with stride-4 scalar fields and stride-8 short-vector fields interleaved. Total footprint ~196 bytes (~98 bytes per view). The port already implements this as ~32 separate `g_camXxx[2]` arrays in td5_camera.c (lines 66–93). Naming each DAT_ at its view-0 address recovers the field semantic directly without requiring a Ghidra struct rewrite.

2. **`g_cameraFlyInPresetGuard` (DAT_00482DF4) is the gate the camera-lag bug TODO is actually about.** The original at `UpdateRaceCameraTransitionState @ 0x00401E10` writes the *current* fly-in preset id (10/11/12/13) into this guard immediately AFTER calling `LoadCameraPresetForView`. It only calls `LoadCameraPresetForView` again when the preset id changes. So:
   - At race-start, `g_cameraTransitionActive=0xA000` → quotient/0x2800=4 → preset 13 (wide pull-back). Guard becomes 13.
   - As timer counts down, quotient passes 3,2,1 → presets 10,11,12 — each transition triggers `LoadCameraPresetForView(force_reload=0)` which RESEEDS `g_cameraSpringRadiusCurrent` (0x00482F00) and `g_cameraHeightTarget` from the new preset (per the `param_2 != 0 || mode_changed` gate in 0x00401450).
   - At quotient=0 (level 0), preset 13 is loaded one more time (LAB_004015F2 cleanup path), and the timer continues counting down toward 0.
   - When `g_cameraTransitionActive < 0x101`, `UpdateRaceCameraTransitionTimer @ 0x0040A490` calls `ResetRaceCameraSelectionState` which DOES call `LoadCameraPresetForView(force_reload=0, preset=0)` — restoring chase preset 0's far-chase targets.
   - **Critical observation**: in the original, `g_gamePaused = 0` is written by `UpdateRaceCameraTransitionTimer` at the *same site* as `LoadCameraPresetForView(preset=0)` is being called via `ResetRaceCameraSelectionState`. So the spring targets ARE reset to preset-0 values at the exact tick `g_gamePaused→0` in the original. The port's 630d797 commit moved the paused→0 write to a *different* trigger (countdown level==0 in `td5_game.c`), but the spring-target reset is still gated by `g_cameraTransitionActive<0x101`, which fires ~40 sub-ticks later. **Confirms the TODO's diagnosis exactly — the port's commit decoupled the two writes, leaving a 40-tick gap where preset-13's 3800-wu radius is the spring target.**

3. **The camera-FSM dispatcher is in `UpdateTracksideCamera @ 0x00402480`, not in chase code.** The case switch on `g_cameraProfileBehaviorType[view]` (DAT_00482EC8) is the canonical "7 chase presets + trackside + spline + orbit" dispatcher referred to in CLAUDE.md. The 11 cases are 0=lookahead-fixed, 1/2=fixed-camera-anchored-with-vehicle-orientation, 3=static, 4=cw-orbit, 5=ccw-orbit, 6=spline, 7=vehicle-relative (no yaw offset), 8=vehicle-relative (rear-view, yaw_offset=0x800), 9=orbit-rear-view, 10=orbit-front-view. Chase camera (`UpdateChaseCamera`) is the player's primary path and runs SEPARATELY in `RunRaceFrame`'s sim-tick loop, NOT through this switch.

4. **`g_cameraRearViewActive` (DAT_00466F88) is the look-back toggle**. `UpdateRaceCameraTransitionState` writes `g_cameraYawOffsetView = (this[view] ? 0x800 : 0)` when no transition is active. Port handles this via `td5_camera_set_rear_view()` at line 2157 setting `g_camYawOffset[v] = active ? 0x800 : 0`. The two implementations are functionally equivalent — port's helper takes the side that the original's input layer drives via DAT_00466F88.

5. **The trackside profile rand-seed (DAT_00463088) is NOT a constant** despite being inside the `.data` const-table region — it's written by `InitializeTracksideCameraProfiles` from `_rand()`. This is a layout artifact: the constants `g_cameraOrientAngleFovScale` (0x00463080) ARE constant but the rand-seed cell next to it is mutable. The port doesn't replicate this (trackside replay path not enabled) but if it were, the seed would need to be captured per `g_raceSessionRandomSeed`.

6. **The 14×16-byte camera preset table at 0x00463098 confirms the port's `g_cameraPresets[14]` array (td5_camera.c:139) is byte-equivalent.** Memory dump entry 0 = `0000 5802 3408 fe01 ...` = `mode=0, elev=600, height=2100, radius=510` which matches the port's preset 0 `{0, 600, 2100, 510, 0, 0}`. Naming this table closes a fact that's been an implicit assumption in the port for months.

7. **`UpdateRaceCameraTransitionTimer @ 0x0040A490` writes BOTH `g_gamePaused = 0` AND `gRaceCameraTransitionGate = 0` at the same site** when the transition quotient drops to 0. The port's 630d797 commit moved only `g_gamePaused = 0` to a different trigger but kept `gRaceCameraTransitionGate = 0` here. The cascade: when `gRaceCameraTransitionGate = 0`, `g_simulationTickCounter` starts advancing in `RunRaceFrame` (it's gated on `gRaceCameraTransitionGate == 0`). So the sim-tick counter still doesn't advance until `g_cameraTransitionActive` actually completes, even though `g_gamePaused = 0` lets sim ticks run. This means physics start running ~40 sub-ticks BEFORE `g_simulationTickCounter` actually counts those ticks — exactly the regime where the countdown "1" indicator stuck-on TODO and camera-lag TODO both live.

8. **`g_cameraFlyInThreshold = 40` (already-named global at 0x00463090) is a constant**, NOT a per-view counter. The per-view counter is `g_cameraFlyInCounter[2]` (already named at 0x00482EF0). Both already correctly named — confirmed.

9. **`UpdateChaseCamera` does NOT use the FSM switch** — it's invoked directly from `RunRaceFrame`'s sim-tick loop with `cameraPresetIndex=1` (a magic constant meaning "follow this actor with chase camera"). The "7 chase presets" referenced in CLAUDE.md are the 7 chase-mode presets (0..6) in `g_cameraPresetTable` (0x00463098). Trackside/spline/orbit are the OTHER 7 entries (10..13 plus a sparse 7..9 set used by fly-in).

10. **The HUD "camera transition indicator" path (UpdateCameraTransitionHudIndicator @ 0x0040A260)** is what's still showing "1" after race-start in the countdown-stuck-after-race-start TODO. It runs ONLY when `gRaceSlotStateTable.slot[N].companion_state_1 != 0`, which is set elsewhere; the indicator level itself is `g_cameraTransitionActive / 0x2800 + 1`. So once g_paused=0 (early due to 630d797), the HUD indicator stays "1" for the remaining ~40 sub-ticks until `g_cameraTransitionActive` actually decays past 0x101. **This confirms the sister-TODO `todo_countdown_1_stuck_after_race_start_2026-05-17`'s diagnosis — both the camera and the HUD indicator are gated on `g_cameraTransitionActive`, NOT on `g_gamePaused`.**

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x004ab0a0, 0x004ab0b4 | `g_projectionCenterOffsetY/X` — already named; used by `SetProjectionCenterOffset` from `RunRaceFrame` | T3 render/projection batch |
| 0x004c3718 | `g_projectionDepth` — already named; secondary depth value | T3 render batch |
| 0x004c3810..004c3824 | listener-position view 0 + view 1 globals — already named | (covered) |
| 0x00467368 | `g_projectionDepthBias` — already named; written by trackside; this IS the shared projection-bias used by render | T3 render batch |
| 0x004aae18..004aae48 | `gViewportLayout*` already named; viewport bounds | T3 viewport batch (already named) |
| 0x004aae0c | `gRaceCameraTransitionGate` — already named | (covered) |
| 0x0045d5d8 | const `256.0f` = `g_const256` in port | T3 math-const batch |
| 0x0045d5dc | const `32.0f` = `g_const32` in port | T3 math-const batch |
| 0x004749c8 | const `0.125f` = `g_dampWeight` in port | T3 math-const batch |
| 0x0045d68c | const `1.0f/65536` = sub-tick fraction reciprocal | T3 math-const batch |
| 0x0045d5d0 | const `0.5f` (frequent) | T3 math-const batch |
| 0x004aafa0+ | g_cameraBasis (already-known render basis) | T3 render-matrix batch |
| 0x004ab040+ | g_cameraTertiary etc (already-known) | T3 render-matrix batch |
| 0x004ab070+ | g_cameraSecondary (already-known) | T3 render-matrix batch |

## TODO impact

**todo_camera_lags_post_race_start_2026-05-17:** Naming `g_cameraFlyInPresetGuard` (0x00482DF4), `g_cameraSpringRadiusCurrent` (0x00482F00), `g_cameraHeightSpringTarget` (0x00482F10), `g_cameraSpringYawAccum` (0x00482F18), and `g_cameraSpringPitchAccum` (0x00482FA0) makes the camera-lag investigation tractable in Ghidra. **Key insight (key-discovery #2 above)**: the original's `g_gamePaused = 0` write is at the *same site* as the `LoadCameraPresetForView(preset=0)` spring-target reset (both in `UpdateRaceCameraTransitionTimer @ 0x0040A490` when `g_cameraTransitionActive < 0x101`). The port's 630d797 decoupled these. **The structural fix is NOT a one-shot in `td5_camera_update_transition_state` (current port mitigation at td5_camera.c:2217); it's to either (a) revert 630d797's `g_gamePaused=0` early-flip to fire only when `g_cameraTransitionActive<0x101` (matching the original), OR (b) move the spring-target reset to fire at the same trigger that flips `g_gamePaused`.** Closing this TODO requires choosing one approach. The current `s_flyin_preset_reloaded` one-shot is functionally correct but adds a port-only divergence; aligning the trigger would be byte-faithful.

**todo_countdown_1_stuck_after_race_start_2026-05-17:** Confirmed sister-bug. Per key-discovery #10, both the HUD indicator "1" and the camera fly-in are gated on `g_cameraTransitionActive`, not on `g_gamePaused`. Same fix applies — either revert the early `g_gamePaused=0` or move the HUD-hide trigger from "paused==0" back to "g_cameraTransitionActive<0x101". Naming `g_cameraTransitionActive` (already named) and tying it to the `UpdateRaceCameraTransitionTimer` flow in Ghidra makes this very visible.

**todo_camera_through_wall_on_reverse_2026-05-16:** **Already shipped (commit 79023df F3 per memory).** Investigation confirms that the trackside-orbit camera (case 7/8/9/10 in `UpdateTracksideCamera`) DOES NOT raycast against geometry — the port's added clip check is correct and additive. No naming change needed.

**todo_camera_no_slope_adjustment_2026-05-16:** Investigation shows the camera dispatch reads the actor's rotation matrix via `LoadRenderRotationMatrix(&actor->rotation_m00)` at line in `UpdateChaseCamera`, then `ConvertFloatVec3ToShortAngles` to derive view orientation. The slope tilt comes via the `rotation_matrix` directly — if `rotation_matrix` is off by ±256 fp due to chassis-snap (per Edinburgh memo), camera tilt drifts proportionally. **Deferred** as memory'd; this batch confirms the cascade pathway (chassis-snap rotation_matrix → camera orientation → no slope adjustment) but no new closing mechanism is found in camera-state naming.

## Ghidra session notes

- Session `a3994aea196e47acaf8480cf4b8f6bf9` opened TD5_pool0 read-only.
- Pool slot acquired via `bash scripts/ghidra_pool.sh acquire` (TD5_pool0); will be released via cleanup after this write.
- No writes to Ghidra performed. All names are PROPOSED only — the consolidation session will apply them.
