# Drag Race Mode (Game Type 9) -- Full Analysis

## Overview

Drag Race is one of TD5's 11 game types (`g_selectedGameType == 9`). It is a **solo time-attack** mode on a straight (non-circuit) track, using the standard pre-race countdown system. There are **no AI opponents** -- all non-player slots are disabled. The mode shares the same race simulation loop (`RunRaceFrame`) and completion logic as other modes, differentiated by runtime flags and branch gates.

---

## Key Globals

| Address | Name | Value in Drag | Purpose |
|---------|------|--------------|---------|
| `0x0049635c` | `g_selectedGameType` | 9 | Selects drag race rules |
| `0x004aaf74` | `DAT_004aaf74` (mode family) | **1** | Drag-specific mode family (vs 2=cup, 3=TT, 4=cops) |
| `0x004aaf48` | `gDragRaceModeEnabled` | 1 | Master drag mode flag, 10 xrefs |
| `0x00494bac` | `gDragRaceUseCustomIntroMessage` | 1 | Bypasses encounter/series track override |
| `0x004aaf68` | `gWantedModeEnabled` | 0 | Cleared |
| `0x004aaf6c` | `gTimeTrialModeEnabled` | 0 | Cleared |
| `0x004b0fa8` | Checkpoint timer mode | 0 | Disabled -- no checkpoint countdown |
| `0x004aaf00` | `DAT_004aaf00` (actor count) | 6 | Still 6 slots allocated, but 5 are state 3 |

---

## Initialization (`ConfigureGameTypeFlags` at 0x410ca0)

When `g_selectedGameType == 9`:
```
gDragRaceModeEnabled = 1
gDragRaceUseCustomIntroMessage = 1
DAT_004b0fa8 = 0          // no checkpoint timers
DAT_004aaf74 = 1          // mode family "drag"
```

Notable: `gTrafficActorsEnabled`, `gSpecialEncounterEnabled`, `gCircuitLapCount`, `gRaceRuleVariant`, and `gRaceDifficultyTier` are **not explicitly set** for game type 9 -- they retain their default/cleared values (traffic=0, encounter=0).

The `gDragRaceUseCustomIntroMessage` flag **prevents** the series/encounter track lookup table from being consulted:
```c
if (gTimeTrialModeEnabled == 0 && gWantedModeEnabled == 0 && gDragRaceUseCustomIntroMessage == 0) {
    DAT_00490ba8 = encounter_track_table[...]; // SKIPPED for drag
}
```
The player's selected track is used directly.

---

## Session Setup (`InitializeRaceSession` at 0x42aa10)

### Slot State Assignment

1. Human player slot(s) get state `\x01` (player-controlled).
2. **All remaining slots (up to 6) get state `\x03` (INACTIVE).**
   - State `\x03` means: not simulated, not updated by UpdateVehicleActor, but IS rendered by the drag-mode render path.

This is the defining characteristic: **drag race has zero AI opponents**.

### Starting Grid (Actor Placement)

The primary positioning code stagers actors normally, then a drag-specific override repositions all non-player actors:

```c
// After standard placement, if mode family == 1 (drag):
InitializeActorTrackPose(1, 1, 0, 0);  // slot 1 -> span 1, lane 0
InitializeActorTrackPose(2, 1, 1, 0);  // slot 2 -> span 1, lane 1
InitializeActorTrackPose(3, 1, 2, 0);  // slot 3 -> span 1, lane 2
InitializeActorTrackPose(4, 1, 3, 0);  // slot 4 -> span 1, lane 3
InitializeActorTrackPose(5, 1, 4, 0);  // slot 5 -> span 1, lane 4
```

All 5 inactive actors are placed at track span 1, spread across lanes 0-4. Since they are state `\x03`, they **never move** -- they serve as static scenery (parked cars at the starting line).

The human player (slot 0) is positioned by the normal non-circuit stagger logic earlier.

### Other Session Differences

- `gTrafficActorsEnabled` is forced to 0 (also forced off for split-screen and benchmark modes).
- No encounter/special traffic actors.
- Standard track loading, 3D model parsing, and texture setup proceed identically.

---

## Pre-Race Countdown / Staged Start

TD5's drag race uses the **same countdown system as all other race modes** -- there is no drag-specific "Christmas tree" or staged light system.

### Countdown Mechanism

1. `InitializeRaceCameraTransitionDuration()` sets `g_cameraTransitionActive = 0xA000`.
2. Each simulation tick, `UpdateRaceCameraTransitionTimer()` decrements by `0x100`:
   ```
   g_cameraTransitionActive -= 0x100
   ```
3. The HUD indicator value is computed as:
   ```
   indicator = g_cameraTransitionActive / 0x2800 + 1
   ```
   This produces values **5, 4, 3, 2, 1** during the fly-in, displayed via the "numbers" sprite atlas.

4. When `g_cameraTransitionActive / 0x2800 == 0`:
   - `DAT_004aad60 = 0` (pre-race gate opens)
   - `gRaceCameraTransitionGate = 0` (simulation tick counter begins)
   - The indicator value becomes 1, then 0 (no indicator drawn)

### Countdown Duration

- Total: `0xA000 / 0x100 = 160 ticks` (at 30fps fixed timestep = ~5.33 seconds)
- Each indicator step: `0x2800 / 0x100 = 40 ticks` (~1.33 seconds per number)
- Sequence: 5 -> 4 -> 3 -> 2 -> 1 -> GO (no indicator)

### HUD Indicator Display

The countdown sprite is rendered in `RenderRaceHudOverlays` when `DAT_004b11a8[view] != 0`:
- Positioned at `(pfVar9[2] - 8, pfVar9[3] - 12)` -- near center-top of viewport
- Sprite frame = `(indicator_value - 1)` into the "numbers" atlas (16x24 pixel glyphs, 5-column layout)

---

## Pre-Race Vehicle Behavior (Staging)

During the countdown (`DAT_004aad60 != 0`):

In `UpdateVehicleActor` (0x406650), the `DAT_004aad60` check gates vehicle behavior:

**When `DAT_004aad60 == 1` (countdown active):**
- The vehicle enters the **pre-start idle path**:
  - `UpdateVehicleEngineSpeedSmoothed()` is called -- the engine revs based on throttle input
  - The vehicle position/heading are maintained but NOT advanced (no dynamics integration through normal paths)
  - The car's engine RPM can be built up via throttle during staging
  - For AI: engine speed is set to `(maxRPM * 2) / 3`

**When `DAT_004aad60 == 0` (green light):**
- Normal dynamics path activates:
  - Player slot -> `UpdatePlayerVehicleDynamics()`
  - AI slot -> `UpdateAIVehicleDynamics()`
- The race timer (gap_0346+6) begins incrementing
- `g_simulationTickCounter` begins counting

### Engine Rev During Staging

`UpdateVehicleEngineSpeedSmoothed` (0x42ed50) allows pre-revving:
```c
targetRPM = (maxRPM - 400) * throttle_normalized / 256 + 400;
// Smoothly approach targetRPM with acceleration cap of 400/tick and decay of 200/tick
```
This means the player CAN rev the engine during the countdown, building RPM. However, there is **no launch control, boost mechanic, or optimal RPM window** -- the standard throttle-to-RPM mapping applies.

### No False Start Detection

There is **no false start penalty**. The vehicle simply cannot move during the countdown because the dynamics integration path is gated by `DAT_004aad60`. The player's throttle input is consumed for engine RPM only. Once `DAT_004aad60` goes to 0, whatever RPM was built up feeds directly into the first frame of dynamics.

### No Reaction Time Measurement

There is **no reaction time display or measurement**. The transition from countdown to racing is instantaneous at a fixed tick count. There is no per-player reaction time delta.

---

## Race Simulation

### AI Behavior

In `UpdateRaceActors` (0x436a70), when `gDragRaceModeEnabled == 1`:

```c
// Drag mode path: NO call to UpdateActorTrackBehavior()
for each slot:
    if (state == '\x02') { // finished
        apply post-finish braking
    }
    if (state != '\x03') { // active
        UpdateVehicleActor(slot);
    }
```

**Key difference:** `UpdateActorTrackBehavior` is completely skipped for all racers in drag mode. This function normally handles:
- AI steering via route scripts
- Lane changes and offset tracking
- Heading corrections

Since all AI slots are state `\x03` (inactive), this is moot -- no AI vehicles simulate at all. The drag race path exists for consistency but only processes the human player(s).

The `ComputeAIRubberBandThrottle()` call at the top of UpdateRaceActors still runs but produces no effect since there are no AI actors to apply it to.

### Traffic and Encounters

- `gTrafficActorsEnabled = 0` -- no traffic
- `gSpecialEncounterEnabled = 0` -- no police encounters
- The traffic actor loop at the bottom of UpdateRaceActors checks `gDragRaceModeEnabled` and handles traffic identically to normal mode, but since traffic is disabled, it's a no-op.

### Track Type

Drag race uses the **player-selected track** directly (non-circuit, point-to-point). The track must be a straight/non-circuit track. Circuit tracks would still work mechanically but the game presents non-circuit tracks for drag selection.

---

## Race Completion

### Win Condition

`CheckRaceCompletionState` (0x409e80) uses the standard non-circuit finish detection:
- Each actor's checkpoint progress is tracked via the checkpoint byte array
- When an actor reaches the final checkpoint (`bVar3 == *pbVar2`), their finish time is recorded
- The `post_finish_metric_base` field stores the raw finish time
- `companion_state_1` and `companion_state_2` are set to mark completion

Since only the human player is active, the race ends when **the player reaches the finish line** (the sole checkpoint boundary). There is no opponent to beat on time.

### Post-Finish Cooldown

After all active actors complete:
- `DAT_00483980` counts up via `g_simTimeAccumulator`
- At `0x3FFFFF` accumulated time (~268M ticks), `BuildRaceResultsTable()` is called
- The fade-out transition begins

### Results Table

`BuildRaceResultsTable` (0x40a8c0) processes all 6 slots:
- For actors with no finish time (`post_finish_metric_base == 0`), a synthetic time is generated from distance and a random component
- Lap split times are distributed across checkpoints
- For drag mode specifically, the results screen at `RunRaceResultsScreen` (0x422480):
  - **Skips rows at y=0x90 and y=0xA8** in the column headers (position and "best finish" rows)
  - Uses `SNK_ResultsTxt` for headers, same as single race mode
  - **No left/right arrow navigation** between racers (`InitializeFrontendDisplayModeArrows` is skipped)
  - Shows elapsed time and checkpoint splits

---

## Rendering Differences

### Actor Rendering

`RenderRaceActorsForView` (0x40bd20) has a drag-specific path:
```c
if (gDragRaceModeEnabled == 0) {
    // Normal: render all slots 0-5 unconditionally
} else {
    // Drag: only render if slot state != '\x00'
    for each slot 0-5:
        if (state != '\x00')
            RenderRaceActorForView(slot, view);
}
```

Since drag mode sets inactive AI to state `\x03` (not `\x00`), they ARE rendered as static models. This creates the visual effect of parked cars at the starting grid.

### HUD Configuration

`InitializeRaceOverlayResources` (0x4377b0) for `DAT_004aaf74 == 1` (drag):
- Sets HUD flags: `0x01 | 0x02 | 0x10 | 0x20 | 0x40 | 0x08` (rank, lap/split times, minimap, speedometer, metric display, wrong-way indicator)
- The `| 0x02` flag explicitly enables split time display
- No `| 0x80` (points display) or `| 0x100` (wanted meter)
- No `| 0x200` (circuit lap counter)

### Fade-Out

`BeginRaceFadeOutTransition` (0x42cc20) disables the radial pulse "1ST PLACE" win overlay for drag mode (same as network/replay modes).

---

## Frontend Menu Flow

### Race Type Menu

In `RaceTypeCategoryMenuStateMachine` (0x4168b0):
- Button index 3 = "Drag Race" (uses `SNK_DragRaceButTxt` from LANGUAGE.DLL)
- Maps to `g_selectedGameType = 9`
- After selection, `ConfigureGameTypeFlags()` is called, then flows to track selection (screen 0x14)

### Track Selection

The drag race proceeds to the standard track selection screen. The player picks any available track (point-to-point tracks from the pool). No restrictions beyond the standard track pool.

---

## Summary of What Drag Race IS and ISN'T

### What it IS:
- A solo time-attack/sprint mode on a point-to-point track
- Uses the standard 5-4-3-2-1 countdown (same as all modes)
- Allows engine revving during countdown (throttle builds RPM)
- Player races alone to the finish line
- Records elapsed time and checkpoint splits
- Static parked cars at the starting line for visual dressing
- Standard physics, standard vehicle dynamics

### What it ISN'T:
- No head-to-head AI competition (all AI slots are disabled)
- No Christmas tree / staged lights (uses standard number countdown)
- No reaction time measurement
- No false start penalty (vehicle is locked in place during countdown)
- No launch control or boost mechanic
- No speed trap at the finish
- No drag-specific RPM/launch window
- No quarter-mile/eighth-mile distance markers
- No automatic vs manual transmission launch difference (standard gearbox behavior)

### Architectural Notes:
- The `gDragRaceModeEnabled` flag (10 xrefs) controls branching in: UpdateRaceActors, RenderRaceActorsForView, RunRaceFrame, BeginRaceFadeOutTransition, InitializeRaceSession
- Mode family `DAT_004aaf74 = 1` controls: InitializeRaceSession (grid placement, slot states), InitializeRaceOverlayResources (HUD flags), InitializeRaceActorRuntime (no traffic), BuildRaceResultsTable (scoring), RunRaceResultsScreen (display layout)
- The mode is essentially "Time Trial minus one player" -- same solo formula but without the two-player TT variant
