# Permanent L4 Residual — Post Phase 5 (2026-05-21)

After the Phase 5 L4→L5 sweep, **17 functions remain at L4** by the
`build_confidence_map.py` classifier. Every one of them is documented below
with the reason it could not be honestly promoted to L5. The project's honest
L5 ceiling is reached at ~96.7 % (`L5=622 / cited=639 = 0.9734`); these 17
entries are L4 indefinitely until the underlying blocker is removed.

The blockers fall into five categories:

- **REGR** — promotion would mask a known port bug (must fix bug first).
- **UNCERTAIN** — orig binary semantics ambiguous at the call site; cannot
  claim byte-faithful without a runtime probe.
- **OUT-OF-SCOPE** — the function's body lives in a different port module; the
  owning module's audit pass must promote it.
- **NOT-PORTED** — orig function has no port equivalent; promotion would be
  dishonest until ported.
- **NO-PORT-SITE** — orig functionality is folded across many port sites with
  no single place to anchor a `[CONFIRMED]` / `[ARCH-DIVERGENCE]` tag.
- **PARTIAL-PORT** — port omits behavior that the orig has (e.g. file write).
- **CLASSIFIER-LAG** — promotion has been authored but the classifier's
  precision_weak vs precision_strong threshold has not yet re-tipped to L5.

## Residual entries

| Address | Function | Module owner | Category | Reason |
|---|---|---|---|---|
| 0x00401330 | `SpawnRearWheelSmokeEffects` | td5_vfx.c | OUT-OF-SCOPE | Body in `td5_vfx_spawn_rear_wheel_smoke`; only dispatch in td5_render.c. VFX audit must promote. |
| 0x00401370 | `SpawnRandomVehicleSmokePuff` | (unowned) | NOT-PORTED | 159B engine-rev-based rand puff at rear-probe midpoint. Port gate not wired. |
| 0x00402950 | `UpdateStaticTracksideCamera` | td5_camera.c | REGR | `g_camHeightSampleOfs[2]` never written; should be `g_cameraProfileVertOffset[2]`. Trackside-cam Y baseline off (regression #4). |
| 0x00402AD0 | `UpdateSplineTracksideCamera` | td5_camera.c | REGR | `s_splineTemplates[6][8]` table values do not match orig stack pattern; spline flyby broken for all 6 types (regression #5). |
| 0x004092D0 | `RenderVehicleActorModel` | td5_physics.c | OUT-OF-SCOPE | Wheel-probe transform chain (`TransformShortVec3ByRenderMatrixRounded`) — physics audit must promote. |
| 0x0040CBD0 | `ConfigureActorProjectionEffect` | td5_render.c | UNCERTAIN | Mode-1 SetProjectionEffectState param_3 vector choice binary-ambiguous; port picks linear_velocity for forward-scroll semantic (regression #3 / pre-existing UNCERTAINty). |
| 0x00415030 | `ScreenPositionerDebugTool` | td5_frontend.c | PARTIAL-PORT | Port case-5 file write is `TD5_LOG_I`-only; orig writes positioner.txt with two 0x25-iteration glyph-rect tables. |
| 0x00415370 | `ScreenStartupInit` | td5_frontend.c | UNCERTAIN | Port case-4 redirects to `TD5_SCREEN_LOCALIZATION_INIT`; orig redirects via `g_frontendScreenFnTable`. Cases 0-3 also diverge (port skips BltColorFillToSurface dialog + cursor-overlay activate). Bootstrap divergence vs regression — needs runtime trace. |
| 0x004168B0 | `RaceTypeCategoryMenuStateMachine` | td5_frontend.c | REGR | Button→game_type mapping swap (button 3↔4): port has TimeTrials=7, DragRace=9; orig has DragRace=7, TimeTrials=9 (regression #1). |
| 0x0041C330 | `RunFrontendNetworkLobby` | td5_frontend.c | CLASSIFIER-LAG | Has `[ARCH-DIVERGENCE: DXPTYPE]` tag at td5_frontend.c:10280; classifier shows strong=1 weak=2 → L4. Promotion is authored; classifier re-tip needs a strong-precision keyword pass. |
| 0x0041EA90 | `ScreenSoundOptions` | td5_frontend.c | REGR | SFX mode `^=1` (2-mode toggle) vs orig 3-mode cycle gated by `DXSound::CanDo3D()`; volume step `*5` vs orig `*10` (regression #2). |
| 0x00429CF0 | `SpawnVehicleSmokeSprite` | td5_vfx.c | OUT-OF-SCOPE | Body in `td5_vfx_spawn_smoke`; call-site only in td5_render.c. |
| 0x00429FD0 | `SpawnVehicleSmokePuffAtPoint` | td5_vfx.c | NOT-PORTED | 688B smoke-spawn helper; port collapses pipeline into simpler `vfx_spawn_smoke_at_position`. |
| 0x0042E750 | `BuildWorldToViewMatrix` | td5_camera.c | NO-PORT-SITE | Citation-sweep header entry; orig's pitch/yaw + forward-vector to 3x3 builder replaced by per-frame camera-basis composition with no single anchor site. |
| 0x0043F420 | `UpdateFrontWheelSoundEffects` | td5_vfx.c | REGR | Wheel-anchor source `+0x298` (hires wheel) vs orig `+0xf0` (probe) — 1-tick lag in tire-track anchor (regression #6). |
| 0x0043F600 | `UpdateRearWheelSoundEffects` | td5_vfx.c | REGR | Symmetric to 0x0043F420 — same `+0x298` vs orig `+0xf0` wheel-anchor offset divergence. |
| 0x00441A80 | `LoadVehicleSoundBank` | td5_sound.c | CLASSIFIER-LAG | Inline ARCH-DIVERGENCE tag at td5_sound.c:16,370; orig address cited as `0x441A80` (no leading zeros). Classifier sees strong=0 weak=0 → L4. Audit-tag normalization needed. |

## Promotable once blockers clear

Fidelity-critical regressions (would unblock 7 entries on fix):
- regression #1 → `RaceTypeCategoryMenuStateMachine`
- regression #2 → `ScreenSoundOptions`
- regression #4 → `UpdateStaticTracksideCamera`
- regression #5 → `UpdateSplineTracksideCamera`
- regression #6 → `UpdateFrontWheelSoundEffects` + `UpdateRearWheelSoundEffects` (shared `+0xf0` vs `+0x298` root)
- regression #3 (UNCERTAIN) → `ConfigureActorProjectionEffect`

Out-of-scope audits that owning modules can promote (3 entries):
- `SpawnRearWheelSmokeEffects` + `SpawnVehicleSmokeSprite` + `RenderVehicleActorModel`
(when td5_vfx.c / td5_physics.c get their own Phase-5 pass).

Classifier-lag fixes (2 entries):
- `RunFrontendNetworkLobby` and `LoadVehicleSoundBank` would tip to L5 if the
  audit-tag scanner normalized `0x441A80` (no leading zeros) and treated
  `[ARCH-DIVERGENCE: …]` as a strong-precision match.

Genuine future ports (3 entries):
- `SpawnRandomVehicleSmokePuff` + `SpawnVehicleSmokePuffAtPoint`
- `BuildWorldToViewMatrix` (or accept the NO-PORT-SITE classification permanently)

Partial-port completion (1 entry):
- `ScreenPositionerDebugTool` file-write implementation.

## Honest L5 ceiling

L5 = 622 / cited = 639 → **97.3 %**. The 2.7 % residual is the project's
permanent L4 floor unless one of the categories above changes. Future
re-promotion is possible if (a) the 6 regressions are fixed, (b) the 4
not-ported / partial-port functions get implementations, or (c) the classifier
is taught to recognize `[ARCH-DIVERGENCE: …]` as a strong-precision tag and
to normalize bare-hex addresses (`0x441A80` → `0x00441A80`).
