---
batch: 14
area: input_controller
tier: T3
target_todos: [todo_default_transmission_auto_2026-05-19, todo_reverse_not_triggered_2026-05-19, todo_view_replay_restarts_race_2026-05-19]
ghidra_session: e9c4f1b5157f4c08bd09b21a98ce30c7
analyzed_addresses: 0x0042c470, 0x00402e60, 0x00402e30, 0x00402e40, 0x0040dfc0, 0x004213d0, 0x0040fe00, 0x004288e0, 0x004285b0, 0x00428880, 0x0042aa10
agent: Claude Opus 4.7 (1M)
date: 2026-05-20
---

# Globals enumeration — Input / controller

## Summary

- Functions analyzed: 11 (1 primary `PollRaceSessionInput` + 10 callers/related dispatch sites)
- Unnamed DAT_* globals encountered: 28 (after de-dup)
- Already-named globals encountered: 10 (`g_playerControlBits`, `gPlayerControlBuffer`, `g_player1InputSource`, `g_player2InputSource`, `g_inputPlaybackActive`, `g_inputPollDeferFlag`, `g_smoothedFrameDt`, `gPrimarySelectedSlot`, `g_audioOptionsOverlayActive`, `g_networkControlBufferReset`, `g_cheatRemoteBraking`)
- Proposals — high confidence: 18
- Proposals — medium confidence: 6
- Proposals — comment-only (low confidence): 2

## Methodology

Primary entry was `PollRaceSessionInput @ 0x0042c470` — the per-frame race input bridge. Walked WRITERS of `g_playerControlBits @ 0x00482ffc` and identified `_DAT_0048f378 != 0` as the **bit-28 OR-in writer**. Followed `DAT_0048f378` xrefs to its 6 WRITE sites:
- `ScreenQuickRaceMenu @ 0x004213d0` (reset to 0)
- `CarSelectionScreenStateMachine @ 0x0040dfc0` (5 writes total — both per-player branches + 1 toggle commit)
- `RestoreRaceStatusSnapshot @ 0x004112c0`
- `RunRaceResultsScreen @ 0x00422480`
- `RunFrontendNetworkLobby @ 0x0041c330`

Then walked `UpdatePlayerVehicleControlState @ 0x00402E60` to decode the FULL bit-map consumed by the actor control interpreter, plus its companion `ResetPlayerVehicleControlBuffers @ 0x00402e40` / `ResetPlayerVehicleControlAccumulators @ 0x00402e30`. Pulled the force-feedback dispatch through `UpdateControllerForceFeedback @ 0x004288e0` and `CreateRaceForceFeedbackEffects @ 0x004285b0`. The controller-binding key-config UI at `ScreenControllerBindingPage @ 0x0040fe00` provided the per-binding-table semantics. Race-bootstrap `InitializeRaceSession @ 0x0042aa10` confirmed `DXInput::WriteOpen / ReadOpen` are the only callers (replay save/load).

Relevance gate: any global written or read during per-frame input polling, controller binding setup, force-feedback dispatch, or input recording / playback lifecycle.

## Proposals

### Bit-28 (manual transmission) writer cluster — **answers T3.14 open question**

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0048f378 | u8 | `g_player1ManualTransmission` | **high** | **THIS IS THE BIT-28 WRITER.** Read at `PollRaceSessionInput @ 0x0042c516`: `if (_DAT_0048f378 != 0) g_playerControlBits |= 0x10000000`. Written by `CarSelectionScreenStateMachine`'s Auto/Manual toggle button (`_DAT_0048f378 = DAT_0048f338` at 0x0040f5b1 + 0x0040f622). `DAT_0048f338` is initialized to `(uint)!(g_selectedGameType != 7)` — drag race defaults to manual=1, all others default to manual=0 (auto). Toggled in-place by `button==3` at 0x0040ea51. Persisted across races via `RestoreRaceStatusSnapshot @ 0x004114ca`. Zeroed by QuickRace init at 0x00421426. | (none — port currently lacks this UI; see TODO impact below) |
| 0x0048f37c | u32 | `g_player2ManualTransmission` | **high** | Symmetric to 0x0048f378 for player 2. Read at `PollRaceSessionInput @ 0x0042c52f`: `if (DAT_0048f37c != 0) DAT_00483000 |= 0x10000000` (where `DAT_00483000 = g_playerControlBits[1]`). Written by `CarSelectionScreenStateMachine`'s player-2 branch at 0x0040e1a6 and final commit at 0x0040f5f0. | (none — port currently lacks this UI) |
| 0x0048f338 | u32 | `g_carSelectManualTransmissionToggle` | high | Transient car-select-screen Auto/Manual button state. Initialized to `1` for drag (game_type==7), `0` for all others. Read by `CarSelectionScreenStateMachine` at 0x0040f59e, 0x0040f5e4, 0x0040f61d (commit to per-player flag). XOR'd at button-index 3 press (0x0040ea51). Final read in `RunFrontendNetworkLobby @ 0x0041c39f`. | (none — UI screen not ported yet) |

### Player control bits + 5-frame gear debounce (per-player input bit array)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00482ffc | u32 | `g_playerControlBits` *(already named)* | — | Player-1 packed input bit array (24 active bits documented in Key Discoveries below). Index pattern: `(&g_playerControlBits)[slotIndex]` where slotIndex is 0 or 1 for two-player. | td5_input.c:79 `s_control_bits[]` |
| 0x00483000 | u32 | `g_playerControlBits_player2_alias` | low | Stride-4 alias of `&g_playerControlBits[1]`. The pattern `(&DAT_00482ffc)[1]` is rewritten by Ghidra as a separate `DAT_00483000` symbol. **No new symbol needed** — comment only flag-up so future readers don't think this is a distinct global. | (port uses array indexing — no alias needed) |
| 0x00482fe4 | int[6] | `gPlayerControlBuffer` *(already named)* | — | 5-frame gear-change debounce counter array. Written by `ResetPlayerVehicleControlBuffers @ 0x00402e40`. Decremented and reset to 5 in shift-up/down branches of `UpdatePlayerVehicleControlState`. | td5_input.c:118 `s_gear_debounce[]` |

### Camera-cycle debounce + rear-view per-player array (per-player overlay state)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004aaf98 | int[2] | `g_cameraCyclePerPlayerCooldown` | high | Decremented every frame at `PollRaceSessionInput @ 0x0042c541` when nonzero. Reset to 10 when bit 24 (CHANGE_VIEW) fires AND `g_replayModeFlag==0` AND `g_cameraTransitionActive==0`. Also reset to 10 by F4/F5 keyboard fallback at 0x0042c8a9. Per-player (indexed by `iVar1=0..1`). | td5_input.c:127 `s_camera_cooldown[]` |
| 0x00466f88 | int[2] | `g_rearViewActiveFlag` | high | Per-player rear-view hold flag. WRITE: set to `1` when bit 25 held AND not in replay/transition; cleared to `0` otherwise. Read at 0x00401fce (camera lookup in `LoadCameraPresetForView` or similar). | td5_input.c:130 `s_rear_view[]` |
| 0x004aaf93 | u8 | `g_f3PauseInputHeldFlag` | high | F3-key edge-detect latch. Set to 1 when `DXInput::CheckKey(0x3d /*F3*/)` returns nonzero; on F3 release the rising-edge path writes `g_inputPollDeferFlag = 1` at 0x0042c728. Source confirms F3 = "pause input poll" toggle. Port mirror already labels this. | td5_input.c:186 `s_f3_held` |
| 0x004aadf0 | u32 | `g_inputPollDeferFlag` *(already named)* | — | Set on F3 release; consumed elsewhere to skip an input frame. | td5_input.c (no direct mirror; behaviour-only) |

### Manual transmission UI/cup-mode unlock gate

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0048f380 | u32 | `g_carSelectChampionshipReturnFlag` | med | Gate for car-select-screen re-entry from championship/cup flow. Set to 1 when game_type==7 (drag) and re-entering on second-pass (0x0040f74e). Cleared on UI navigation backout (0x0040f76b). READ by `CarSelectionScreenStateMachine` (0x0040e108, 0x0040e25b) to choose label-surface index (12 vs 13). Also gates `ManualButTxt` label vs `AutoButTxt`. | (none) |
| 0x0048f368 | u32 | `g_player1SelectedPaintScheme` | high | Persisted player-1 car paint variant. Written at 0x004114b8 (race restore), 0x00421a98 (QuickRace OK), 0x0040f5a6 (CarSelect commit), 0x0041c3b2 (NetLobby). Read at 0x0041b75e (frontend display), 0x0040dadc (CarSelect open), 0x0042251f (RaceResults). Paired with `DAT_0048f308` (transient car-select state). | (none — paint scheme not ported) |
| 0x0048f370 | u32 | `g_player1SelectedWheelScheme` | high | Persisted player-1 car wheel variant. Symmetric to 0x0048f368: written by same screens, read by same screens, transient version `DAT_0048f334`. | (none) |
| 0x0048f36c | u32 | `g_player2SelectedPaintScheme` | high | Player-2 mirror of 0x0048f368. Written at 0x0040f5d3 + 0x004233b3 (RaceResults). | (none) |
| 0x0048f374 | u32 | `g_player2SelectedWheelScheme` | high | Player-2 mirror of 0x0048f370. Written at 0x0040f5ea + 0x004233c2. | (none) |
| 0x0048f308 | u32 | `g_carSelectPaintSchemeTransient` | high | Transient paint scheme during car-select navigation. Cycled by arrow keys (0x0040e7a6..0x0040e85a, range 0..3). Committed to `g_player1SelectedPaintScheme` (or player 2 mirror) on OK button. | (none) |
| 0x0048f334 | u32 | `g_carSelectWheelSchemeTransient` | high | Twin of 0x0048f308; transient wheel scheme during car-select. Cycled by arrow keys (0x0040e75b..0x0040e860). | (none) |

### Force-feedback per-player state

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004a2cc4 | u32[6] | `g_ffControllerAssignment` | high | Per-player force-feedback controller index +1 (0 = no FF assigned, N+1 = joystick slot N). Initialized to 0 then copied from input-source array by `ConfigureForceFeedbackControllers @ 0x00428880`. Read by `UpdateControllerForceFeedback @ 0x004288e6` to gate FF effect playback. In network play, only `dpu_exref+0xbe8` (local slot index) gets assigned. | td5_input.c:15 `gFFControllerAssignment[6]` |
| 0x004a2cdc | u32[6] | `g_ffCollisionEffectActive` | high | Per-player collision FF "currently playing" gate. Cleared in `UpdateControllerForceFeedback` after first PlayEffect call; checked to choose between PlayEffect (initial) vs SetEffect (continuing). Set elsewhere by collision dispatch (`PlayVehicleCollisionForceFeedback @ 0x00428a60`). | td5_input.c (s_ff struct) |
| 0x00466afc | int[8] | `g_ffCollisionCoefficientTable` | high | Force-feedback collision-magnitude lookup. Read at 0x004289b0: `DAT_00466afc[actor.field_0x370]`. Verified contents: `{1, 200, 260, 180, 100, 600, 50, 240}` (indexed by collision-severity byte). Port mirror declares 13 elements but uses byte 0..7 in practice — extra slots may extend into adjacent constants. | td5_input.c:70 `g_terrain_ff_coefficients[]` (declares 13 but only 8 cited in orig) |

### Controller binding tables (key-config UI persistent state)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00463fc4 | u32[4][9] | `g_inputBindingTable` | high | Per-context (player+device-pair) action-binding table. 4 banks × 9 button-slots = 36 dwords = 144 bytes. Initial pattern `{1,4,5,6,7,9,8,4,5}` repeated 4×. Action codes: 1=accel, 2=brake_up, 3=brake_dn, 4=shift_dn?, 5=shift_up?, 6=horn, 7=handbrake, 8=changeview, 9=rearview. Modified by `ScreenControllerBindingPage @ 0x0040fc8b` (cycle-binding-on-press) and `WritePackedConfigTd5` (load from Config.td5). | td5_input.c:13 `gBindingTable[2][9]` (port labels 2×9 but orig is 4×9) |
| 0x00464054 | u8[16] | `g_keyboardScanCodeTable` | high | Default DirectInput scancode map for 10 action keys. Verified contents: `0xCB 0xCD 0xC8 0xD0 0x10 0x9D 0x1E 0x2C 0x14 0x2D 0x00 0x00 0x00 0x00 0x00 0x00` = `LEFT, RIGHT, UP, DOWN, Q, RCTRL, A, Z, T, X` + 6 unused. Modified by `ScreenControllerBindingPage`'s `case 0x1a` (key-rebind capture loop). Loaded/saved by `WritePackedConfigTd5 @ 0x0040f8d5`. | td5_input.c:14 `gKbScanCodes[16]` |
| 0x00465660 | u32[8] (stride 16) | `g_inputDeviceTypeTable` | high | Per-device descriptor: device-type-id at +0x0, `-1` sentinel at +0xc. `(0x##00) & 0xff00` discriminates wheel/pedal=0x600 from others. Stride 16, 8 entries (4 player × 2 devices). Read at 0x0040ffd5, 0x004270a0 (DirectInput-init bootstrap). | (none — port uses dxinput enumeration) |
| 0x00465680 | u32[8] (stride 16) | `g_inputDeviceButtonCountTable` | high | Companion to 0x00465660. Per-device button count (4..9 used in `ScreenControllerBindingPage` to clamp `DAT_00490ba4`). | (none) |
| 0x004974b8 | u32 | `g_controllerBindingActivePlayerSlot` | high | Active player index (0 or 1) being configured in the binding UI. Written at 0x0041e6c3 and 0x0041e71d (player-1/2 selector buttons). Read by `ScreenControllerBindingPage` to index into binding-table strides (`DAT_004974b8 * 9` + `DAT_004974b8 * 4`). | (none — port skips this UI) |
| 0x00490b94 | u32 | `g_controllerBindingActiveDeviceIndex` | high | Active joystick device index (+1) being configured. Set from `g_player1InputSource` or `g_player2InputSource` at 0x0040ff9a. Used as `DAT_00490b94 + -1` argument to `DXInput::GetJS`. | (none) |
| 0x00490b84 | u32 | `g_keyboardBindingProgressIndex` | high | Current key-being-bound index (0..9) during keyboard rebind UI. Incremented at 0x00410b17 each captured key; commits to byte `DAT_00464054[index]`. | (none) |
| 0x00490b88 | u32 | `g_controllerBindingEdgeMask` | high | Rising-edge mask for joystick-button cycle-binding. Computed as `prev & current` at 0x0040beforestriprefactor (in `case 10`). Drives the "press button N to advance binding[N] to next action" loop. | (none) |
| 0x00490b8c | u32 | `g_controllerBindingCurrentButtons` | high | `DXInput::GetJS(deviceIdx)` snapshot for the cycle-binding tick. Saved into `DAT_00490b88` next frame as the "previous" mask. | (none) |
| 0x00490b90 | u32 | `g_controllerBindingPrevButtons` | high | Previous-frame joystick mask (saved at top of `case 10` from `DAT_00490b88`). Used with current mask to compute the rising-edge cycle trigger. | (none) |
| 0x00490ba4 | u32 | `g_controllerBindingButtonCount` | high | Clamped count of buttons on the active device (2, or in [4,8], or 8 max). Read from `g_inputDeviceButtonCountTable[player*9]`. Loop bound for the cycle-binding pass. | (none) |

## Key discoveries

1. **Bit-28 of `g_playerControlBits` is the manual-transmission flag, and its WRITER is `DAT_0048f378` (player 1) / `DAT_0048f37c` (player 2).** Confirmed by reading `PollRaceSessionInput @ 0x0042c516` and `@ 0x0042c52f`. The default is **AUTO** because both flags are zeroed by the car-select screen init when `g_selectedGameType != 7` (drag-only forces manual). The reverse-latch failure in `todo_reverse_not_triggered_2026-05-19` is NOT caused by the bit-28 polarity in `UpdatePlayerVehicleControlState` (port mirror confirmed correct at td5_input.c:755 `((~(bits >> 28)) & 1)`) — it's caused by the **toggle UI being absent in the port**, leaving the actor's `field_0x378` perpetually at `~0 & 1 = 1` (auto). That should still allow the brake→reverse latch since the gate requires `auto_gearbox != 0` (which is 1). **Therefore the TODO root cause is downstream of bit-28**: investigate gate operands `speed < 10` (or 0x800 fixed-point) and `field_0x376 == 0` (surface-contact-bitmask=0 means **no ground contact** — that's the unusual gate); look at td5_input.c:683 for the port's gate ordering.

2. **Complete `g_playerControlBits` bit-map decoded from `UpdatePlayerVehicleControlState`:**
   - bit 0 (0x1) = LEFT steer
   - bit 1 (0x2) = RIGHT steer
   - bits 0-8 (0x1FF) low-9 = packed analog steer magnitude when bit 31 set
   - bit 9 (0x200) = horn / siren
   - bit 10 (0x400) = brake-or-reverse-engaged
   - bits 9-17 (0x3FE00 region) = packed analog throttle/brake when bit 27 set
   - bit 20 (0x100000) = handbrake
   - bit 21 (0x200000) = cheat-velocity-double / NOS trigger (gated by `g_cheatRemoteBraking != 0`)
   - bit 22 (0x400000) = shift-up
   - bit 23 (0x800000) = shift-down
   - bit 24 (0x1000000) = change-view (cycle camera)
   - bit 25 (0x2000000) = rear-view (hold)
   - bit 27 (0x8000000) = analog accel/brake override (low 18 bits hold value)
   - bit 28 (0x10000000) = manual-transmission flag (writes inverse to `field_0x378`)
   - bit 30 (0x40000000) = ESC / escape
   - bit 31 (0x80000000) = analog steer override (low 9 bits hold value)

3. **The replay-buffer write site is `DXInput::WriteOpen` in M2DX.DLL, called from `InitializeRaceSession @ 0x0042b29c`.** Only one writer in the binary. Companion `DXInput::ReadOpen` at 0x0042b28d (gated on `g_inputPlaybackActive != 0`). The recording itself happens inside M2DX (external DLL); the per-frame call is `DXInput::Write(&g_playerControlBits, 1)` at the bottom of `PollRaceSessionInput` (0x0042c6e9 area). **`todo_view_replay_restarts_race_2026-05-19` closure path:** the port must replicate the M2DX-internal recording buffer. Port's `td5_input.c:170 s_rec` (`TD5_InputRecordBuffer`) is the port-only struct that fills this role — verify it captures `g_playerControlBits` for both players every PollRaceSessionInput call, and persists across the "View Replay" restart via `g_inputPlaybackActive` flag.

4. **The car-select screen toggle button uses `DAT_0048f338` as a SHARED transient state.** When the player enters the screen, this is initialized based on game-type and the previous player's flag (`DAT_0048f378` for P1, `DAT_0048f37c` for P2). The XOR toggle at 0x0040ea51 (button-index 3) lives in `case 7` of the state machine. The commit (`_DAT_0048f378 = DAT_0048f338`) happens AT TRANSITION (case 0x18 final commit when leaving the screen). **Implication for the port:** if you re-implement the toggle UI later, copy this lifecycle: init from persistent flag, XOR on press, commit on exit.

5. **The input binding table is FOUR banks of 9 entries, not two.** `(&DAT_00463fc4)[player * 9]` indexes a player-relative bank (0 or 1 from `DAT_004974b8`), but the table itself stores 4 × 9 dwords because of the device-context split (one bank per player+device pair). Port's `gBindingTable[2][9]` (td5_input.c:13) under-declares; rebind to 4×9 if controller binding is ever fully ported.

6. **The default keyboard map at `0x00464054` decodes to `LEFT, RIGHT, UP, DOWN, Q, RCTRL, A, Z, T, X`** — 10 action keys. Port's `gKbScanCodes[16]` (td5_input.c:14) sizes correctly but the last 6 entries are unused padding in the original (verified by null-bytes in memory dump).

7. **`DXInput::SetConfiguration(g_player1InputSource, g_player2InputSource)`** is the once-per-race entry point that uploads the binding table from globals INTO the M2DX black box for per-frame use. Called at `InitializeRaceSession @ 0x0042b357`. So per-frame reads happen inside M2DX (not visible in TD5_d3d.exe) — the binding-table globals exist ONLY to populate this single call. **Implication for the port:** the port's per-frame mapping logic (in `td5_input.c` input poll) is doing the work that M2DX did in original; the port's binding-table globals are therefore both the source-of-truth AND the per-frame reference, unlike the original where they're snapshot-then-discarded each race.

8. **`g_cheatRemoteBraking` (already-named at 0x0049629c) is a global ENABLE, not a per-player flag.** Confirmed gating at `UpdatePlayerVehicleControlState`: cheat behaviour (zero-velocity blast on all OTHER actors when bit 21 held) only triggers when this is nonzero. The per-player accumulator at 0x00483014 (already named `g_cheatRemoteBrakingApplied` in batch_01) is the per-player one-shot latch.

9. **`g_inputPlaybackActive` distinguishes record (0) vs playback (1) modes for the M2DX serialization.** When 0, `DXInput::Write(&g_playerControlBits, 1)` is called every frame (record). When 1, `DXInput::Read(&g_playerControlBits)` overwrites the bits each frame (playback). The "View Replay" button at `RunRaceResultsScreen` (batch_05) toggles this flag to 1 and re-enters the race state.

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x00490b9c, 0x00490ba0, 0x0049628c | UI surface pointers (button lights, header overlay, body overlay) | T3 frontend rendering |
| 0x00490b88..0x00490b94 | Transient binding-page UI state cluster (already proposed above but UI-scoped) | T3 frontend |
| 0x004962cc, 0x004962c4, 0x004962c8 | Frontend animation tick + surface dims (already T1.5 noted) | T1.5 frontend (batch 05) |
| 0x00466b7c | `s_Failed_to_create_constant_force_e_...` string ref | (string only) |
| 0x0045ea28 | FF effect-template buffer (passed to `EnumerateEffects`) | T3 FF effect templates |
| `JoystickType_exref`, `js_exref`, `JoystickButtonC_exref`, `JoystickC_exref` | M2DX-imported pointer-locations for joystick capability tables — external | (external M2DX state) |
| `dpu_exref + 0xbcc, 0xbe8, 0xbf0, 0xc08, 0xc0c` | DXPlay/dpu network state offsets read by PollRaceSessionInput | T3 network/DXPlay batch |
| 0x004aaf94, 0x004aaf95, 0x004aaf96 (between 0x4aaf93 and 0x4aaf98) | Memory dump shows zeros — likely 3 reserved/padding bytes between `g_f3PauseInputHeldFlag` and `g_cameraCyclePerPlayerCooldown` | (padding) |
| `g_replayModeFlag` (label exists at 0x4aaf64? — referenced in `PollRaceSessionInput`) | Replay-vs-record discriminator (separate from `g_inputPlaybackActive`) | T3 replay batch (already touched in batch_05) |
| `g_cameraTransitionActive` | Camera-cycle gate companion to 0x004aaf98 | T3 camera batch |
| `g_raceEndFadeState`, `g_fadeDirection`, `g_fadeDirectionAlternator` | ESC-key fade-out cascade in PollRaceSessionInput | T3 race-end / fade batch |
| `g_raceViewportLayoutMode` | Fade-direction selector based on split-screen mode | T3 viewport/split-screen batch |
| 0x004a2cc4..0x004a2cf0 cluster | Force-feedback per-player state block (some named here, some neighbors unknown) | T3 FF state block |

## TODO impact

**todo_default_transmission_auto_2026-05-19:** **Investigation overturns the previous root-cause hypothesis from batch_01.** The default state is correctly AUTO (bit-28 default is 0, which makes `~(bit28>>28)&1 == 1 == auto_gearbox`). The bit-28 WRITER (`DAT_0048f378`) is ONLY set by the car-select Manual toggle, which the port currently doesn't expose. So port's `field_0x378` should permanently read 1 (auto). **If the user reports default-not-auto, the bug is downstream of bit-28** — most likely a port-side write to `field_0x378` that overrides the per-tick recompute, OR the per-tick recompute order (the original recomputes at the START of `UpdatePlayerVehicleControlState`; if port recomputes too late, a stale value leaks). Verify `td5_input.c:755` `((~(bits >> 28)) & 1)` runs BEFORE any consumer of `field_0x378` in the same frame. **No new naming closes this TODO directly** — but naming `g_player1ManualTransmission` makes the absent-UI gap visible.

**todo_reverse_not_triggered_2026-05-19:** Same family as above. The gate at port td5_input.c:683 is `auto_gearbox != 0 && throttle_st == 1 && speed < 10 && sflags == 0`. The `sflags == 0` (no-surface-contact) gate looks suspicious for a "stopped, want to reverse" scenario — when stopped on ground, sflags should be NONZERO (wheel touching ground). **Hypothesis:** the original's `field_0x376` is a **timer that COUNTS DOWN from a positive value while airborne** (not a "currently grounded" bitmask) — so `field_0x376 == 0` means "haven't been airborne recently", which is the correct gate for "allow reverse". Verify port's semantic for `field_0x376` (read at td5_input.c:678 `sflags = ab[0x376]`). If port treats it as a grounded-mask, the gate is inverted.

**todo_view_replay_restarts_race_2026-05-19:** Confirms batch_05's finding that the M2DX-internal recording buffer (`DXInput::WriteOpen` callback) is the missing piece in the port. The single WRITE site `0x0042b29c` is unambiguous. **Closing path for the port:** ensure `td5_input.c:170 s_rec` captures both players' `g_playerControlBits` snapshots per `PollRaceSessionInput` invocation, persists them across race restart via `g_inputPlaybackActive`, and replays via the per-frame override at port-input poll start (mirror of `DXInput::Read(&g_playerControlBits)` at `PollRaceSessionInput @ 0x0042c476`). No new globals needed beyond the port's own `s_rec` struct.

## Ghidra session notes

- Session `e9c4f1b5157f4c08bd09b21a98ce30c7` opened `TD5_pool0` read-only as required.
- Pool slot acquired via `bash scripts/ghidra_pool.sh acquire`; released via `bash scripts/ghidra_pool.sh cleanup` after analysis.
- No writes to Ghidra performed. Names listed here are PROPOSED only — the consolidation session will apply them.
- **Open question from T3.14 (where does bit-28 of `g_playerControlBits` get set) IS ANSWERED:** the writer is `DAT_0048f378` (player 1) / `DAT_0048f37c` (player 2), OR-ed in by `PollRaceSessionInput @ 0x0042c516/0x0042c52f`. The per-screen lifecycle (init → toggle → commit) of these flags lives entirely in `CarSelectionScreenStateMachine @ 0x0040dfc0` with `DAT_0048f338` as the transient state.
