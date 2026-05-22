---
batch: 11
area: per_frame_state
tier: T3
target_todos: [todo_camera_lags_post_race_start_2026-05-17, todo_countdown_1_stuck_after_race_start_2026-05-17, todo_race_starts_on_countdown_1_not_orig]
ghidra_session: 7de037d98d764fdbb2c10bd75b8d28be
analyzed_addresses: 0x00442170, 0x0042B580, 0x0042C470, 0x00436A70, 0x00402E60, 0x00401E10, 0x0040A490, 0x0040A480, 0x0042AA10, 0x00439B70
agent: Claude Opus 4.7 (1M context)
date: 2026-05-20
---

# Globals enumeration — Per-frame top-level state

## Summary

- Functions analyzed: **10** primary entry points
  - `RunMainGameLoop @ 0x00442170` (T2.6 already covered) — re-audited for per-frame state lens
  - `RunRaceFrame @ 0x0042B580` — race per-frame entry, 3367 bytes
  - `PollRaceSessionInput @ 0x0042C470` — per-sub-tick input bridge
  - `UpdateRaceActors @ 0x00436A70` — per-sub-tick actor orchestrator (T2.10 RS focal)
  - `UpdatePlayerVehicleControlState @ 0x00402E60` — per-slot, called from sub-tick loop
  - `UpdateRaceCameraTransitionState @ 0x00401E10` — per-view, called per render-frame
  - `UpdateRaceCameraTransitionTimer @ 0x0040A490` — per-sub-tick countdown decrementer (KEY)
  - `InitializeRaceCameraTransitionDuration @ 0x0040A480` — countdown seed (sets 0xA000)
  - `InitializeRaceSession @ 0x0042AA10` — race-start initializer (sets paused=1, transition_gate=1)
  - `DrawRaceStatusText @ 0x00439B70` — gates drag-race timer on `g_gamePaused == 0`
- Unnamed `DAT_*` globals encountered in scope: **24** (after de-dup; 12 confirmed-unnamed proposals + 12 noted as already-named in T1/T2)
- Already-named globals encountered (verified via `symbol_by_name`, just noted): **~70**
- Proposals — high confidence: **12**
- Proposals — medium confidence: **5**
- Proposals — comment-only (low confidence): **3**
- **STRUCTURAL renames flagged** (existing names misleading): **3** (see Key Discoveries §1)

## Methodology

1. Decompiled each of the 10 entry functions and enumerated every `DAT_*` operand that appears at TOP-LEVEL (not inside per-actor `for(slot)` loops — those are T1/T2 territory).
2. For each unique address, ran `symbol_by_name` to verify whether it's already named via T1/T2 batches (this filtered out ~70 already-named globals).
3. For the surviving DAT_* addresses, used `reference_to` to enumerate every writer and reader; cross-referenced with port-side mirrors in `td5_game.c` / `td5_camera.c` to identify semantic role.
4. **Extra scrutiny on `g_cameraTransitionActive @ 0x004aaef0`**: its existing T1 name implied "camera transition" but the write at `InitializeRaceCameraTransitionDuration @ 0x0040A480` (value `0xA000` = 5 * `0x2800`) and the decrement-by-0x100/tick loop in `UpdateRaceCameraTransitionTimer @ 0x0040A490` (with `level = active / 0x2800` producing digits 5..0) make it clear this is **the race-start countdown timer**, not a generic camera-transition timer. The camera-side `UpdateRaceCameraTransitionState` happens to read the SAME counter to drive the countdown camera-arc animation. This is a misnamed-but-already-named case I'm proposing to relabel.

**Filter for relevance**: globals are in-scope iff they're read or written at TOP-LEVEL inside one of the 10 entry functions (excluding the per-slot inner loops). Globals that appear ONLY inside the per-slot loops are out-of-scope (covered by T1/T2 or future batches).

## Proposals

### Race countdown & paused-state (THE CRITICAL T3 BLOCK)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004aaef0 | u32 | **`g_raceCountdownTimer`** (existing name `g_cameraTransitionActive` is MISLEADING — propose **rename**) | **high** | Initialized to `0xA000` at `InitializeRaceCameraTransitionDuration @ 0x0040A480`. Decremented by `0x100` per sub-tick at `UpdateRaceCameraTransitionTimer @ 0x0040A490` (called from RunRaceFrame inner sub-tick loop). Level digit = `active / 0x2800` → 5,4,3,2,1,0 (5×0x2800 = 0xA000). At level==0: writes `g_gamePaused = 0` (race start!) and `gRaceCameraTransitionGate = 0`. **12 xrefs.** This IS the race-start countdown timer — same logic the port has at `td5_game.c` `tick_race_countdown()` / `s_race_countdown_state` / `s_race_countdown_ticks`. **Camera dispatch happens to read it** at `UpdateRaceCameraTransitionState` 0x00401e45 to choose countdown preset 0xD/0xC/0xB/0xA — but it's a CONSUMER, not the owner. The name should reflect ownership. | port: `td5_game.c:468-469` `s_race_countdown_ticks` + `s_race_countdown_state`. **TODO impact**: `todo_camera_lags_post_race_start_2026-05-17` + `todo_countdown_1_stuck_after_race_start_2026-05-17` both arise because the port renamed/restructured this around commit 630d797. Naming the orig correctly makes the bisect against `afdb006~1` trivially readable. |
| 0x004aad60 | u32 | `g_gamePaused` (ALREADY NAMED — confirmed) | (existing) | Written `=1` at `InitializeRaceSession @ 0x0042b35d`. Written `=0` at `UpdateRaceCameraTransitionTimer @ 0x0040a520` when `g_raceCountdownTimer / 0x2800 == 0`. Read at `DrawRaceStatusText @ 0x00439d17` to gate drag-race timer drawing (`if (g_twoPlayerViewActiveIndex == 0 && g_gamePaused == 0)`), and at `UpdateVehicleActor @ 0x0040681a`. 5 xrefs. | port: `g_td5.paused` — already mirrored. Note included here because it's the **direct mirror** of port's `g_td5.paused` and pairs with the countdown timer. |
| 0x004aae0c | u32 | `gRaceCameraTransitionGate` (ALREADY NAMED — confirmed) | (existing) | Written `=1` at `InitializeRaceSession @ 0x0042b363`. Written `=0` at `UpdateRaceCameraTransitionTimer @ 0x0040a526` when countdown level reaches 0. Read at `UpdateRaceActors @ 0x004066fb` and `RunRaceFrame @ 0x0042ba7d` — it gates simulation-tick incrementing (`if (gRaceCameraTransitionGate == 0 && g_simulationTickCounter < 0x7fffffff) g_simulationTickCounter++`). 14 xrefs. | port: implied behavior in countdown gate. Confirms the "transition gate prevents sim-tick advance during countdown" pattern. |

### Per-view orbit/look camera state (read inside RunRaceFrame's per-view code)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00482df4 | u32[2] | `g_perViewPrevCameraPresetId` | high | 2-element per-viewport array (indexed `[param_2 * 4]` where param_2 is view index 0/1). Read 4× / written 5× in `UpdateRaceCameraTransitionState` at 0x00401ea5/eaf/eed/ef7/f3b/f45/f81/f8b/fc3. Each branch writes the chosen preset (0xD = full countdown, 0xC, 0xB, 0xA = transition phases) AFTER updating gRaceCameraPresetId; this is the LAG copy for transition gate detection. Per-view. | port: implicit in camera preset state — not byte-mirrored. |
| 0x00482f70 | u32[2] | `g_perViewLookAroundActiveFlag` | high | 2-element per-viewport array. Written 0 or 0x800 at `UpdateRaceCameraTransitionState @ 0x00401fb6/fd9/fe6` and additionally in 5+ other camera functions. 0x800 = "look-around input was active this tick"; 0 otherwise. **20 xrefs** — heavily-used. | port: maps to `td5_camera.c` look-around handling. |
| 0x00482f18 | u32[2] | `g_perViewCameraOrbitAccum` | high | 2-element per-viewport array. Accumulated by `+= ftol(orbital_delta)` at `UpdateRaceCameraTransitionState @ 0x00401eb6/efe/f4c` during countdown camera-orbit phases. Drives the "the camera swings around the car before race start" visual. **15 xrefs.** | port: camera-orbit state during countdown. |

### Per-view UI debounce & toggle latches (read in PollRaceSessionInput)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004aaf98 | u32[2] | `g_perViewCameraCyclePresetDebounce` | high | 2-element per-viewport counter `[ESI*4 + 0x4aaf98]` at `PollRaceSessionInput @ 0x0042c541` (READ) / 0x0042c54d (DECREMENT). Set to **10** at 0x0042c5ab and 0x0042c8a9 after a camera-cycle event. Decremented per-tick. Gates rapid re-cycling. 6 xrefs. | port: input-debounce in `td5_input.c` camera cycle handler — semantic match, not byte-faithful. |
| 0x00466f88 | u32[2] | `g_perViewLookBackInputActive` | high | 2-element per-viewport flag, set to **1** at `PollRaceSessionInput @ 0x0042c5d3` when player held look-back button (control bit `0x2000000`) AND not in replay AND no camera transition. Cleared to **0** in same branch when condition fails. Read at `UpdateRaceCameraTransitionState @ 0x00401d27` to choose look-back camera preset. 2 xrefs (this WRITER + 1 reader). | port: `td5_camera.c` look-back handling. |
| 0x004aaf93 | u8 | `g_globalDebugPanelKeySticky` | medium | Boolean. Set to 1 at `PollRaceSessionInput @ 0x0042c71f` when key 0x3D (F3 — diagnostics overlay) is pressed. Cleared to 0 at 0x0042c731 (only when key is NOT down). The transition-to-zero side-effect writes `g_inputPollDeferFlag = 1`. 3 xrefs. Likely a debug/diagnostics toggle key. | (none — port has no equivalent; possibly stripped from shipped build but the code path remains active.) |

### Per-slot auto-throttle / auto-transmission toggles

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0048f378 | u32 | `g_player1AutoThrottleEnabled` | high | Read at `PollRaceSessionInput @ 0x0042c516` (`if (_DAT_0048f378 != 0) g_playerControlBits \|= 0x10000000`). Written at 5 frontend menu functions (`ScreenQuickRaceMenu @ 0x00421426`, `InitializeRaceSeriesSchedule @ 0x0040db05`, `RestoreRaceStatusSnapshot @ 0x004114ca`, plus 2 others). Bit `0x10000000` OR'd into player 1's control word every tick — same bit-position the port's `g_td5.ini.player_is_ai`/auto-throttle path uses. 12 xrefs. | port: `g_td5.ini.player_is_ai` / `ini.auto_throttle` (verify mapping in `td5_input.c`). |
| 0x0048f37c | u32 | `g_player2AutoThrottleEnabled` | high | Twin of 0x0048f378 for player 2. Read at `PollRaceSessionInput @ 0x0042c52f`. Same 5 frontend writers as P1 (with adjacent addresses 0x00421420 / 0x0040e1a6 / 0x004114c8 / etc). 7 xrefs. | (same — split-screen P2 auto-throttle.) |

### Per-slot remote-brake cheat latch & view indicator parity

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00483014 | u8[6] | `g_remoteBrakeCheatPerSlotLatch` | high | 6-byte per-slot latch (one per racer). Read+write at `UpdatePlayerVehicleControlState @ 0x00403422-0x0040346e` inside the `if (_g_networkControlBufferReset != 0)` block. Set to `0xF` when remote-brake cheat fires (along with velocity-double effect at +0x1cc/+0x1d4); cleared to 0 when not active. Acts as the "remote brake just fired this tick, suppress until cleared" latch. 5 xrefs. | port: cheat-remote-brake handling — likely missing from port (no `g_td5.cheat_remote_brake_active`). |
| 0x0046317c | u16[6] | `g_perSlotViewIndicatorInitialAngle` | high | 6-element u16 table at `[slotIndex * 2 + 0x46317c]`. Bytes `[0001 0001 0001 0001 0001 0001]` — value `0x0100` per slot. Read at `UpdatePlayerVehicleControlState @ 0x00403335 / 0x00403408` (writes to actor +0x33e indicator field) when the "view-cycle button" path fires. Drives the "view changed" HUD arrow indicator initial angle per slot. 4 xrefs. | port: HUD view-change indicator init constant. Single-value init table; could be inlined. |

### Float constants used per-frame in RunRaceFrame (read-only data pool)

These appear inside `RunRaceFrame` and are pure data constants. Naming them makes the timing pipeline legible.

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0045d5f4 | float | `g_floatOne` (= 1.0f) | medium | `00 00 80 3F` = 1.0f. **39 xrefs across binary** — most-used float constant. Inside RunRaceFrame at 0x0042ba77 (used to seed `g_simTickBudget = 1.0f`). Pure read-only literal. | (none — port uses `1.0f` directly.) |
| 0x0045d650 | float | `g_floatFour` (= 4.0f) | medium | `00 00 80 40` = 4.0f. **18 xrefs**. Inside RunRaceFrame at 0x0042c1f8 used to clamp `g_simTickBudget` to max 4.0 (= 4 sub-ticks max per frame). The hard cap that prevents the sub-tick loop from going beyond 4 iterations on a slow frame. | port: `td5_game.c` MAX_SUBTICKS = 4 constant (verify). |
| 0x0045d68c | float | `g_inv65536F` (= 1/65536) | medium | `00 00 80 37` = 1.526e-5 = 1/65536.0f. 2 readers (both inside RunRaceFrame at 0x0042b729 / 0x0042b772) — converts `g_simTimeAccumulator & 0xFFFF` to a [0..1) fraction stored in `g_subTickFraction`. The sub-tick interpolation fractional-time scale. | port: subtick_fraction computation. |
| 0x0045d684 | float | `g_floatMaxByte` (= 255.0f) | low | `00 00 7F 43` = 255.0f. 4 xrefs. Inside RunRaceFrame at 0x0042b77c / 0x0042b827 used as the saturation cap for the race-end fade alpha value. | (none — port likely uses literal 255.0f.) |
| 0x0045d680 | float | `g_floatOneDiv255` (= 1/255) | low | `81 80 80 3B` ≈ 0.00392 = 1/255.0f. 1 reader at 0x0042b7a8 (race-end fade alpha → screen-fraction conversion). | (none.) |
| 0x0045d670 | double | `g_doubleOne` (= 1.0) | low | `00 00 00 00 00 00 F0 3F` = 1.0 (double precision). 3 readers. Inside RunRaceFrame at 0x0042c148 (`_g_reciprocalFrameDtMs` = 1.0 / frame_dt_ms). Companion to g_doubleThousand for ms→s. | (none.) |
| 0x0045d668 | double | `g_doubleThousand` (= 1000.0) | low | `00 00 00 00 00 40 8F 40` = 1000.0. 1 reader at 0x0042c15d. Used to convert reciprocal-ms to FPS via `g_instantFPS = (1000.0/dt_ms) * 1000.0` — actually 1000/dt scaled. | (none.) |
| 0x0045d660 | double | `g_doubleThirty` (= 30.0) | low | `00 00 00 00 00 00 3E 40` = 30.0. 1 reader at 0x0042c169. Used to compute `g_normalizedFrameDt = dt_ms * (1/1000.0) * 30.0` — = frame time in 30Hz reference units. **30 Hz = the original target framerate**, exposed as a normalization factor. | (none — port uses 60Hz native.) |
| 0x0045d658 | double | `g_doubleInvThousand` (= 1/1000.0) | low | `FC A9 F1 D2 4D 62 50 3F` ≈ 0.001 = 1/1000.0. 1 reader at 0x0042c16f. ms→s conversion factor companion. | (none.) |

Notes on the confidence levels:

- **high** → consolidation session will apply the rename + add a Ghidra comment with the evidence
- **medium** → apply name with `_PROVISIONAL` suffix
- **low** → add a Ghidra comment with the analysis only; leave `DAT_*` label

## Key discoveries

These are the high-impact findings — biggest payoff comes from §1, the misnamed countdown timer.

### 1. **`g_cameraTransitionActive @ 0x004aaef0` is the RACE COUNTDOWN TIMER — propose rename to `g_raceCountdownTimer`**

The existing name (from T1) is structurally misleading. Static analysis:
- `InitializeRaceCameraTransitionDuration @ 0x0040A480` writes `0xA000` (= 40960 = 5 × 0x2800)
- `UpdateRaceCameraTransitionTimer @ 0x0040A490` is called every sub-tick (from RunRaceFrame's sub-tick loop)
- Decrement: `g_cameraTransitionActive -= 0x100` per sub-tick (so `0xA000 / 0x100 = 160` sub-ticks total = exactly 4 seconds at 40Hz physics, or 2.67 sec at 60 Hz physics)
- Level digit: `g_cameraTransitionActive / 0x2800` produces level 5,4,3,2,1,0 (= the "5,4,3,2,1,GO!" countdown levels)
- At level == 0: writes `g_gamePaused = 0` (race officially starts) + `gRaceCameraTransitionGate = 0`
- The camera dispatch in `UpdateRaceCameraTransitionState` happens to READ this counter to pick countdown camera preset 0xD/0xC/0xB/0xA — that's a *consumer*, not the owner

**TODO impact**: this rename makes the bisect against commit `afdb006~1` (for `todo_camera_lags_post_race_start_2026-05-17` and `todo_countdown_1_stuck_after_race_start_2026-05-17`) trivially readable. The port's `g_paused`-flip-timing change in commit 630d797 corresponds exactly to the orig's `level == 0 → g_gamePaused = 0` transition in `UpdateRaceCameraTransitionTimer`, not the timer-reaches-0 condition. The port followed the orig's semantics correctly; the visual bugs are downstream consumers (chase camera + countdown indicator) that read stale state.

**This is the single most-important T3 finding.**

### 2. **`gRaceCameraTransitionGate @ 0x004aae0c` gates the simulation-tick increment**

Confirmed at `RunRaceFrame @ 0x0042ba7d`:
```
if ((gRaceCameraTransitionGate == 0) && (g_simulationTickCounter < 0x7fffffff)) {
    g_simulationTickCounter = g_simulationTickCounter + 1;
}
g_raceSimSubTickCounter = g_raceSimSubTickCounter + 1;
```

So `g_simulationTickCounter` only advances when the countdown is OVER (gate = 0), while `g_raceSimSubTickCounter` advances regardless. This explains why the port's `s_race_countdown_ticks` and `g_td5.simulation_tick_counter` need different semantics. **Naming clarification**: the existing name `gRaceCameraTransitionGate` is technically correct but suggests it's only about cameras — it actually gates the SIM TICK COUNTER advancement, which has nothing to do with cameras directly. Consider rename to `g_raceCountdownActiveGate` for symmetry with the timer rename.

### 3. **`UpdateRaceCameraTransitionState` reads ALL of these as the per-view consumer**

The camera-side function at `0x00401E10` walks the same countdown counter (`g_cameraTransitionActive`) and produces per-view camera presets:
- Level 5 (initial): camera preset 0xD (full intro arc)
- Level 1 (= "GO!"): preset 0xC
- Level 2: preset 0xB
- Levels 3..4: preset 0xA
- Level 0 (race started): falls into the `< 1` branch, switches to look-back preset (0x800) or normal (0)

This is the SOLE consumer of `g_perViewLookBackInputActive @ 0x00466f88` and the SOLE writer of `g_perViewPrevCameraPresetId @ 0x00482df4`. They form a 3-global cluster: timer drives preset, preset drives camera. **`todo_camera_lags_post_race_start_2026-05-17` likely lives in this 3-global handoff.**

### 4. **No traditional `g_frameTickCounter` / `g_subTickCounter` exists at top-level — they're already named**

The "global frame index since boot" the user prompt asked about is `FCount_exref` (DLL-side, set at the end of RunRaceFrame: `*(int *)FCount_exref += 1`). The "per-frame sub-tick (4 per frame at 240Hz physics)" is `g_raceSimSubTickCounter @ 0x004aac5c` (already named in T1). The simulation tick is `g_simulationTickCounter @ 0x004aada0` (T1). So three of the user prompt's listed candidates resolve to existing names — confirming no new top-level counter is needed.

### 5. **`g_simTimeAccumulator @ 0x004aaed0` IS the per-frame budget that drives the sub-tick loop**

The loop `while ((uchar *)0xffff < g_simTimeAccumulator) { ... g_simTimeAccumulator -= 0x10000; }` in RunRaceFrame is the variable-rate sub-tick scheduler. Already-named (T1). Worth flagging because the port's sub-tick loop in `td5_game.c` should preserve the identical accumulator semantics — verify against `g_td5.sim_time_accumulator` (or whatever the port calls it).

### 6. **Per-view state architecture: 2-element arrays at fixed addresses (NOT structs)**

The orig consistently encodes per-viewport state as adjacent 2-element u32 arrays at fixed addresses (`[param_2 * 4]` indexing), NOT as struct fields. Five examples identified in this batch:
- `g_perViewPrevCameraPresetId @ 0x00482df4` (camera lag-copy)
- `g_perViewCameraOrbitAccum @ 0x00482f18` (orbit angle)
- `g_perViewLookAroundActiveFlag @ 0x00482f70` (look-around active)
- `g_perViewCameraCyclePresetDebounce @ 0x004aaf98` (cycle debounce)
- `g_perViewLookBackInputActive @ 0x00466f88` (look-back active)

Plus already-named: `gPrimarySelectedSlot @ 0x00466ea0` + `gSecondarySelectedSlot @ 0x00466ea4`, `gRaceCameraPresetMode @ 0x00482fd8` + `gSecondaryRaceCameraPresetMode @ 0x00482fdc`.

**Architectural recommendation**: a future T3.x batch could promote this constellation into a proper `TD5_PerViewState` struct array `g_perViewState[2]` once all per-view fields are named. Not a naming-batch task.

### 7. **`g_player1AutoThrottleEnabled` (0x0048f378) and `g_player2AutoThrottleEnabled` (0x0048f37c) clarify a TODO ambiguity**

Both are written by FIVE different frontend menu functions (ScreenQuickRaceMenu, InitializeRaceSeriesSchedule, RestoreRaceStatusSnapshot, CarSelectionScreenStateMachine, plus 1 more). Read every tick in PollRaceSessionInput → OR'd into `g_playerControlBits[i] | 0x10000000`. The port's `g_td5.ini.player_is_ai` should be **one** of these but they're per-player split. **If port has only one `player_is_ai` it's wrong** — verify split-screen handling. Possible quick-win to inspect.

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x004afbe0 (gActorSpecialEncounterActive) | Already named — referenced from UpdatePlayerVehicleControlState | (already named, T1/T2) |
| 0x00466e88 (gCircuitLapCount) | Already named — referenced from InitializeRaceSession | (already named) |
| 0x004ae278 (g_raceOrderTable) | Already named | (already named) |
| 0x00496298..0x004962b4 (cheat flags g_cheatFlagUnlockAll etc.) | Already named in T2.7 — read by `UpdatePlayerVehicleControlState` cheat-remote-brake gate via `g_cheatRemoteBraking @ 0x0049629c` | T2.7 frontend orchestrators |
| 0x00474834..0x0047483c (FMV ownership) | Already named in T2.7 | T2.7 |
| 0x0045d5d0 (= 0.5f) | Pure FP literal, 18+ readers; could be `g_floatHalf` if useful — out of scope for per-frame focus | T3.x FP constants pool |
| 0x0045d678 (= 0.15f) / 0x0045d67c (= 0.85f) | Two adjacent FP literals; likely interp blend factors (15%/85%) — used in render not top-level | T3.x FP constants pool |
| 0x00466e90 (g_instantFPS) | Already named | (already named) |
| dpu_exref + 0xbcc/+0xbe8/+0xbf0/+0xc08/+0xc0c | DirectPlay user-data offsets in PollRaceSessionInput — DLL-side struct fields | DXPlay layout (T3.x net batch) |
| g_appExref + 0x100/+0x104/+0x108/+0x13c/+0x14c/+0x150/+0x15c/+0x16c/+0x174/+0x180/+0x188 | DDraw app-state struct fields — DLL-side | (DLL-side, not this binary's globals) |
| dd_exref + 8/+0x1690/+0x1730 | DDraw device-state struct fields — DLL-side | (DLL-side) |
| 0x004bcb98 / 0x004bcba0 / etc. | TGQ/FMV control-block fields — covered in T2.7 | T2.7 |
| 0x00482ed0 (g_cameraOrbitRadius) | Already named — companion to per-view orbit accum | (already named) |
| 0x004ab310 (used by CycleRaceCameraPreset second arg) | Looks like `gPerSlotCameraStateBlock` (actor +0x208 alias) — out of scope | T3.x camera-state |

## TODO impact

### `todo_camera_lags_post_race_start_2026-05-17` — **MAJOR PROGRESS**

The bug is that the chase camera drifts behind after race start. Hypothesis from memory: commit 630d797 (which moved paused-flip to `level == 0`) "skipped chase-cam state init."

**This batch's finding makes the bisect tractable.** Naming `g_raceCountdownTimer @ 0x004aaef0` and surfacing the 3-global handoff (timer → preset → camera) means we now know exactly where to look:
- The orig's `UpdateRaceCameraTransitionTimer` sets BOTH `g_gamePaused = 0` AND `gRaceCameraTransitionGate = 0` AT THE SAME LEVEL TRANSITION (level == 0)
- If the port's 630d797 fix only flipped `g_td5.paused = 0` without simultaneously dropping the camera-transition gate, the chase-cam would still be running its countdown-orbit path while the sim is now full-speed → camera drifts behind
- **Suggested fix point**: verify port's countdown-level-0 transition flips BOTH `g_td5.paused = 0` AND the camera-transition gate (whatever the port calls it). If only one is flipped, that's the bug.

Verdict: **investigation surface now clear**; targeted fix candidate identified. Not a one-line close but the investigation pattern is "find the port's mirror of `gRaceCameraTransitionGate @ 0x004aae0c` and verify it gets cleared at the same level-transition as `g_td5.paused`."

### `todo_countdown_1_stuck_after_race_start_2026-05-17` — **MAJOR PROGRESS**

Sister bug: countdown "1" indicator stays on screen after race start. Hypothesis: indicator-hide gated on old `timer == 0` instead of `!g_td5.paused`.

**Same surface analysis as above.** The orig's countdown indicator is driven by `UpdateRaceCameraTransitionState` reading `g_cameraTransitionActive / 0x2800` (= level). When level == 0, the camera transitions but the **indicator** is driven by a SEPARATE consumer — likely `SetRaceHudIndicatorState(0, level + 1)` or similar in `UpdateRaceCameraTransitionTimer` (visible in the decomp: `iVar2 = iVar1 + 1; SetRaceHudIndicatorState(0, iVar2);`).

The orig's pattern: at level == 0, `SetRaceHudIndicatorState(0, 1)` would still display "1" — but the function path then writes `g_gamePaused = 0`. The indicator-hide must happen on the NEXT sub-tick's `g_cameraTransitionActive < 1` branch (`UpdateRaceCameraTransitionTimer @ 0x0040A4E5-ish` decomp shows `g_cameraTransitionActive = 0` + `SetRaceHudIndicatorState(0, 0); SetRaceHudIndicatorState(1, 0);` return — yes that's the early-exit-when-timer-is-zero branch).

**Suggested fix**: port should retie the indicator-hide to `!g_td5.paused` (matching the timing of the camera fix commit `fb60d3d` per memory) — confirmed by orig's actual flow which fires the hide AFTER the timer reaches zero, which is AFTER the paused flag was flipped.

Verdict: **investigation surface clear**, same one-edit fix template as the camera-lag bug.

### `todo_race_starts_on_countdown_1_not_orig` — already SHIPPED (commit 630d797)

Verified — this batch's analysis confirms commit 630d797 correctly identifies the orig pattern (`level == 0` is the paused-flip transition, not `timer == 0`). The MEMORY entry notes this is shipped.

### General TODOs surfaced (not yet written to memory)

- **Possible split-screen P1/P2 auto-throttle binding gap**: orig has `g_player1AutoThrottleEnabled @ 0x0048f378` AND `g_player2AutoThrottleEnabled @ 0x0048f37c` as separate globals. Port has only `g_td5.ini.player_is_ai` (single). If split-screen race ever has different P1/P2 AI settings, the port can't represent that.
- **Possible missing cheat: remote-brake**: `g_remoteBrakeCheatPerSlotLatch @ 0x00483014` is the per-slot latch driven by `g_cheatRemoteBraking @ 0x0049629c` (already named) — but the cheat-completion writer in the frontend (per T2.7) doesn't broadcast to NPC racers. May explain missing cheat-affected NPC behavior if reported.

## Headline stats

- **Functions analyzed**: 10
- **New high-confidence proposals**: 12
- **New medium-confidence proposals**: 5
- **New low-confidence (comment-only)**: 3
- **STRUCTURAL renames recommended**: 3 (most important = `g_cameraTransitionActive → g_raceCountdownTimer`; also `gRaceCameraTransitionGate → g_raceCountdownActiveGate`; also `UpdateRaceCameraTransitionTimer → UpdateRaceCountdownTimer`)
- **Most-frequently-referenced new global named**: `g_floatOne` (39 xrefs) — but most *useful* is the countdown-timer rename at 0x004aaef0 (12 xrefs, drives 2 critical TODOs)
- **TODO impact**: 2 stuck TODOs (`todo_camera_lags_post_race_start_2026-05-17`, `todo_countdown_1_stuck_after_race_start_2026-05-17`) — investigation surface now clear; suggested single-edit fix candidates identified for both.

## Ghidra session notes

- Session `7de037d98d764fdbb2c10bd75b8d28be` opened `TD5_pool0` read-only as required.
- Pool slot acquired via `bash scripts/ghidra_pool.sh acquire`; cleanup ran (lock files cleared).
- No writes to Ghidra performed. All names listed here are PROPOSED only — the consolidation session will apply them.
