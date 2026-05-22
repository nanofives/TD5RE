---
tier: T3
date: 2026-05-20
batches: [11, 12, 13, 14, 15]
status: ready_for_consolidation
total_proposals: 136
---

# T3 Global Naming Sweep — Per-frame orchestrator globals

## Headline stats

| batch | area | functions | globals | high | med | low |
|---|---|---|---|---|---|---|
| 11 | per_frame_state | 10 | 20 | 12 | 5 | 3 |
| 12 | camera_view_state | 18 | 38 | 24 | 9 | 5* |
| 13 | sound_audio_state | 13 | 25 | 18 | 7 | 0 |
| 14 | input_controller | 11 | 26 | 18 | 6 | 2 |
| 15 | vfx_render_frame | 28 | 27 | 17 | 6 | 4 |
| **TOTAL** | | ~80 functions | **136** | **89** | **33** | **14** |

*T3.12 reported 3 low; consolidating "low" + already-mid → low band shows 3+2 mid edge cases

## Major TODO impacts

### TODOs CLOSED via T3 findings

1. **todo_smoke_render_broken_2026-05-19** — **CLOSED** by T3.15
   - Source: race-particle-pool +0x08 (16-bit alpha word)
   - Render at 0x004297D0: `MOV CX, word ptr [EAX+0x4A3178]; FILD; FMUL [0x00466D3C]` (= 1/48)
   - Spawn alpha=0x2080 (8320), decrement -0x3000/lifetime (15/30/45/60 random frames)
   - **Fix recipe**: replace hardcoded `0xFFFFFFFF` at `td5_vfx.c:797/802/807/812` with per-vertex fade from BuildSpriteQuadTemplate's flag-4 path

2. **todo_car_shadow_oversized_2026-05-17** — **PARTIAL CLOSE** by T3.15
   - Constants located: 66.0f (inner) + 126.0f (outer) in `InitializeVehicleShadowAndWheelSpriteTemplates @ 0x0040BB70`
   - Port should cross-check; likely a single-line constant fix

### TODOs ADVANCED (root cause pivoted)

3. **todo_camera_lags_post_race_start_2026-05-17** — **ROOT CAUSE UNIFIED**
   - Key finding (T3.11 + T3.12): `g_cameraTransitionActive @ 0x004aaef0` is misnamed — it's the **race countdown timer** (init 0xA000, decrement -0x100/sub-tick, level = active/0x2800 → 5,4,3,2,1,0)
   - At level==0 in orig: BOTH `g_gamePaused = 0` AND `gRaceCameraTransitionGate = 0` flip simultaneously
   - Port commit 630d797 decoupled them — flips `g_paused` at level==0 but the camera gate stays at the old `<0x101` trigger
   - **Two fix options**: (a) revert 630d797 timing or (b) move spring-target reset to the same trigger that flips `g_paused`
   - Current port workaround `s_flyin_preset_reloaded` at `td5_camera.c:2217` is port-only divergence

4. **todo_countdown_1_stuck_after_race_start_2026-05-17** — **SAME ROOT CAUSE as #3**
   - Sister bug; same fix template (retie indicator-hide to `!g_td5.paused`)
   - This TODO was marked SHIPPED earlier but agents confirm the underlying timing-contract regression is broader than just the indicator hide

### TODOs PIVOTED (hypothesis overturned)

5. **todo_default_transmission_auto_2026-05-19** — **DIAGNOSIS PIVOTS**
   - T3.14 found the bit-28 writer (`DAT_0048f378`/`0048f37c` in `PollRaceSessionInput @ 0x0042c516`)
   - Default in orig is **AUTO** (flag=0, bit-28=0, `~(0)&1=1=auto_gearbox`)
   - **Port already gets bit-28 polarity right at td5_input.c:755** — so transmission bug is NOT a bit-28 fix
   - New candidates: (a) port-side write that overrides per-tick `field_0x378` recompute, OR (b) inverted semantics for `field_0x376` (port treats as grounded-mask; orig is "recently-airborne timer counting to 0")

6. **todo_reverse_not_triggered_2026-05-19** — same pivot as #5

7. **todo_view_replay_restarts_race_2026-05-19** — **PATH CLARIFIED**
   - T3.14 found the writer: `DXInput::WriteOpen` call at `0x0042b29c` inside `InitializeRaceSession`
   - Port `td5_input.c:170 s_rec` is the right replacement — just needs to be wired to the record/playback lifecycle

## Other structural reveals

### T3.11 — per-frame state
- `g_floatOne @ 0x0045d5f4` (39 xrefs) — global `1.0f` constant; high-leverage name
- 5 per-view 2-element arrays at fixed addresses — should be promoted to `g_perViewState[2]` struct in a future pass

### T3.12 — camera
- `CameraViewState` at `0x00482DEC..0x00482FA8` is functionally a `struct CameraView[2]` (~98 bytes/view); port already implements as 32 `g_camXxx[2]` arrays
- `UpdateChaseCamera` doesn't route through the FSM switch
- `UpdateTracksideCamera @ 0x00402480` is the trackside/spline/orbit dispatcher (cases 0..10)

### T3.13 — audio
- Audio mixer state at `0x004c3768..0x004c38e0` is a ~380-byte flat C struct
- Cop-chase audio chain CONFIRMED byte-faithful (only break is missing `g_wantedModeEnabled` writer per T1.2)
- Listener pos = camera pos (written inline in `RunRaceFrame @ 0x0042bdb4`)

### T3.14 — input
- Complete `g_playerControlBits` 16-bit map decoded — high-value artifact for future input audits
- Persistent auto/manual selection per player: `DAT_0048f378` (P1) / `DAT_0048f37c` (P2); transient screen-local state: `DAT_0048f338`
- Only `game_type==7` (drag-race) defaults to manual

### T3.15 — VFX render
- Race particle pool architecture mapped end-to-end (base + scratch bitmap + scratch table + UV templates)
- View matrices fully tagged (`g_currentViewRotationMatrix`, `g_currentViewWorldOrigin`, `g_nearClipDistance`, `g_farClipDistance`)
- Translucent draw queue named
- `g_audioOptionsOverlayActive` is MISNAMED — actually a general "freeze state mutations" gate
- Tire-track pool: 50 entries × 0xEC, 71 xrefs
- 4 LAB_ unregistered code labels surfaced for function promotion (smoke/streak update/render callbacks)

## Cumulative status (T1 + T2 + T3)

- **T1**: 37 globals — 6 TODOs root-caused
- **T2**: 126 globals — cascade focal point named (RS_TRACK_OFFSET_BIAS), minor lost writer (cheat propagation)
- **T3**: 136 globals — 2 TODOs CLOSED (smoke, partial shadow), 2 unified (camera+countdown), 3 pivoted (transmission/reverse/replay)
- **Combined**: **299 globals proposed**

## Files

- `re/analysis/global_naming/batch_11_per_frame_state.md` (238 lines, 20 globals)
- `re/analysis/global_naming/batch_12_camera_view.md` (167 lines, 38 globals)
- `re/analysis/global_naming/batch_13_sound_audio.md` (182 lines, 25 globals)
- `re/analysis/global_naming/batch_14_input_controller.md` (166 lines, 26 globals)
- `re/analysis/global_naming/batch_15_vfx_render_frame.md` (167 lines, 27 globals)

Next: T4 (config + asset loaders), then T5 (long tail).
