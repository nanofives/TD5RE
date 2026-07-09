# Agent C — Visual L5 Audit Report (camera / vfx / hud)

**Date:** 2026-05-21
**Scope:** `td5mod/src/td5re/td5_camera.c`, `td5_vfx.c`, `td5_hud.c`
**Manifests:** 19 + 18 + 9 = 46 functions

## Summary

| Bucket | Count |
|--------|-------|
| Promoted L5 (byte-faithful) | 3 |
| Promoted L5 (ARCH-DIVERGENCE) | 4 |
| Left at L4 | 39 |
| Suspected regressions | 1 |

Conservative pass — favored honesty over coverage. Most L4 entries are either
(a) large multi-hundred-line functions where line-by-line verification did
not complete in budget, (b) density-match citations without port body in
scope, or (c) functions with subtle FPU/rounding divergences typical of
this codebase that warrant a dedicated audit pass.

## Promoted L5 — byte-faithful (3)

| Address | Name | Rationale |
|---------|------|-----------|
| 0x00402A80 | CacheVehicleCameraAngles (td5_camera.c:1610) | 3-line short writes match Ghidra decomp; offsets 0x208/0x20A/0x20C and constants 0x800 verified. |
| 0x00402E00 | CycleRaceCameraPreset (td5_camera.c:1873) | Trivial: `(old+delta)%7` store, `(old+delta)/7` return; matches Ghidra exactly. |
| 0x00428570 | FlushQueuedRaceHudText (td5_hud.c:1105) | Queue walk with 0xB8 stride and SubmitImmediateTranslucentPrimitive per quad; reset to 0. Equivalent loop semantics. |

## Promoted L5 — ARCH-DIVERGENCE (4)

| Address | Name | Rationale |
|---------|------|-----------|
| 0x0042D410 | FinalizeCameraProjectionMatrices (td5_camera.c:314) | Math sequence byte-equivalent. Port adds `g_cameraSecondaryUnscaled` snapshot for D3D11 billboard path; uses `+0.5f` rounding instead of FISTP-RNE (tracked under chassis-snap FPU cleanup). |
| 0x00429690 | ProjectRaceParticlesToView (td5_vfx.c:656) | World→view multiply identical. Port uses `td5_camera_get_position` API + always-fresh `g_cameraBasis` instead of original's stale `g_renderBasisMatrix`. Pure addressing change. |
| 0x00429720 | DrawRaceParticleEffects (td5_vfx.c:705) | Per-particle perspective math matches orig (focal*inv_z, ±half_w/h, z/far_clip). Port writes `TD5_D3DVertex` with sampler-normalized UVs queried from runtime tex dims (D3D11 vs DDraw quad format). |
| 0x00428240 | InitializeRaceHudFontGlyphAtlas (td5_hud.c:539) | 4x16 grid layout + 0x22/0x26 width overrides match. Port adds PNG-first/GDI-fallback asset loading via `td5_asset_find_atlas_entry` because original "font" atlas asset is loaded differently. |

## Left at L4 (39)

### td5_camera.c (15)
- 0x00401950 **UpdateTracksideOrbitCamera** — has stale draft remnants (lines 1078-1093 with multiple sequential overwrites of g_camOrbitOffset[v][0]/[2]). Final writes appear correct but messy authoring should be cleaned before L5.
- 0x00401E10 UpdateRaceCameraTransitionState — 482-byte body; line-by-line audit incomplete.
- 0x004020B0 InitializeTracksideCameraProfiles — 322-byte; incomplete.
- 0x00402200 SelectTracksideCameraProfile — 585-byte; incomplete.
- 0x00402480 UpdateTracksideCamera — 1182-byte dispatcher; incomplete.
- 0x00402950 UpdateStaticTracksideCamera — 294-byte; incomplete.
- 0x00402AD0 UpdateSplineTracksideCamera — 813-byte; FOV/proj math uses `+0.5f` rounding pattern (FPU divergence).
- 0x0040A480 InitializeRaceCameraTransitionDuration — density-match only; no port body in scope (folded elsewhere).
- 0x00429790 UpdateRaceParticleEffects — body lives in td5_vfx.c (out of camera.c scope row), promotion deferred to vfx file.
- 0x0042D5B0 OrientCameraTowardTarget — 708-byte; uses `+0.5f` rounding + look-at construction; needs full FPU-divergence audit.
- 0x0042DB40 ConvertFloatVec3ToIntVec3 — density-match only; no port body in scope.
- 0x0042DBD0 TransformVector3ByBasis — referenced but body not separately audited; appears small/correct, but verify pass not completed.
- 0x0042DC30 ConvertFloatVec3ToIntVec3B — density-match only; no port body in scope.
- 0x0042E030 ExtractEulerAnglesFromMatrix — density-match only; no port body in scope.
- 0x00441F90 BuildCubicSpline3D — Port hardcodes Catmull-Rom basis matrix; orig reads `DAT_00474bc0` runtime. Matrix not byte-verified.
- 0x00442090 EvaluateCubicSpline3D — Port uses raw `>> 12` on signed sums; orig uses signed-correct rounding `(x + (x>>31 & 0xFFF)) >> 12`. **Subtle rounding divergence on negative coeffs** (see "Suspected regressions" below).

### td5_vfx.c (16)
- 0x00401370 SpawnRandomVehicleSmokePuff — density-match only; no port body in scope.
- 0x00401410 InitializeRaceSmokeSpritePool — Port uses atlas lookups + total_actor_count fallback; orig uses `FindArchiveEntryByName` + `g_racerCount`. Different mechanism, not strictly byte-faithful.
- 0x00429510 InitializeRaceParticleSystem — Port computes variant UV size as `(u1-u0)/2`; orig hardcodes 30.0. **Numerical divergence** at variant grid stride.
- 0x00429A30 SpawnVehicleSmokeVariant — 703-byte; incomplete.
- 0x00429FD0 SpawnVehicleSmokePuffAtPoint — density-match only.
- 0x0042A6B0 SpawnAmbientParticleStreak — only referenced internally; port body unclear.
- 0x0043E990 InitializeTireTrackPool — density-match only.
- 0x0043F030 AcquireTireTrackEmitter — incomplete.
- 0x0043F420 UpdateFrontWheelSoundEffects — incomplete.
- 0x0043F600 UpdateRearWheelSoundEffects — incomplete.
- 0x0043F7E0 UpdateRearTireEffects — incomplete.
- 0x0043F960 UpdateFrontTireEffects — incomplete.
- 0x00446240 InitializeWeatherOverlayParticles — incomplete.
- 0x004464B0 UpdateAmbientParticleDensityForSegment — incomplete.
- 0x00446560 RenderAmbientParticleStreaks — 1291-byte; clearly D3D11-divergent but full audit incomplete.
- 0x00446EA0 InitializeWheelPaletteUvTable — density-match only.

### td5_hud.c (8)
- 0x00414F40 RenderPositionerGlyphStrip — density-match only; no port body in scope.
- 0x004377B0 InitializeRaceOverlayResources — 1004-byte; incomplete.
- 0x00437BA0 InitializeRaceHudLayout — 3327-byte; incomplete (largest in scope).
- 0x00439B70 DrawRaceStatusText — 747-byte; incomplete.
- 0x00439E60 RenderHudRadialPulseOverlay — 928-byte; not implemented under name in port; reference comment only.
- 0x0043B7C0 InitializePauseMenuOverlayLayout — 1957-byte; incomplete.
- 0x0043E750 SetClipBounds — Port's `td5_hud_draw_race_fade` does the *visual effect* using these clip bounds via direct D3D11 quad render; the underlying float-clamp store-to-globals primitive is not separately ported. ARCH-DIVERGENCE in mechanism but per-function audit not completed.

## Suspected regressions

- **0x00442090 EvaluateCubicSpline3D** (td5_camera.c:2338) — port uses `>> 12` on signed
  intermediate sums (`a*t3 + b*t2 + c*t`), but orig uses `(x + (x>>31 & 0xFFF)) >> 12`,
  which is round-toward-zero for negatives. With `-fwrapv` MinGW does arithmetic-shift
  (toward -inf). For Catmull-Rom coefficients which are typically signed
  (`-d0+3d1-3d2+d3`, etc.), this produces off-by-one errors on every evaluation
  with mixed-sign sums. Visual impact: spline-camera (trackside type 8) may drift
  by 1 LSB per axis. Low priority (visual-only, sub-pixel) but flagged for
  future cleanup pass alongside the broader FISTP-RNE ports.

## Methodology notes

- Verified the L5 promotions byte-for-byte against Ghidra decomp from
  TD5_d3d.exe in pool slot TD5_pool3.
- The ARCH-DIVERGENCE entries are intentional source-port API choices
  (D3D11 instead of DDraw, atlas-based asset loading instead of direct
  archive headers, fresh camera basis instead of stale per-frame matrix).
- For consistency with prior project memory entries on FPU rounding-mode
  divergence (`reference_fpu_control_word_arch_divergence_2026-05-20`),
  `+0.5f` rounding was not by itself a disqualifier for L5 — it was called
  out as a documented divergence when present.
- Functions with no port body (density-match citations) were left at L4
  because there is nothing in scope to annotate.

## Output diff

3 byte-faithful audit headers + 4 ARCH-DIVERGENCE audit headers added.
Pure comment-addition diff. No executable code touched.
