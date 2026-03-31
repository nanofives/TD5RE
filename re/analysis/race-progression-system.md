# Race Progression System

Comprehensive analysis of how TD5 tracks race progress, lap counting, checkpoint timing, position ranking, scoring, unlock progression, and the post-race flow.

---

## 1. Track Position Tracking

### Core Function: `UpdateActorTrackPosition` (0x4440f0)

Every actor's position on the track is maintained as a triple:
```
param_1[0] = current_span_index    (short) — which STRIP.DAT span the actor is on
param_1[2] = accumulated_spans     (short) — monotonically increasing span counter (forward progress)
param_1[3] = max_accumulated_spans (short) — high-water mark of param_1[2]
param_1[6] = sub_lane_index        (byte)  — position within the current span's lanes
```

The function uses **cross-product boundary tests** against the track strip's left/right vertex pairs to determine which quadrilateral cell the actor occupies. It returns a bitmask indicating which edges were crossed. When the actor crosses the **forward boundary** (cases 2, 3, 6), `param_1[2]` is incremented; when crossing backward (cases 1, 8, 9, 0xC), it is decremented. Span type handlers (type 8/9 = branch, 10/11 = merge) handle track junctions.

### Span Normalization: `NormalizeActorTrackWrapState` (0x443fb0)

For circuit tracks, accumulated span position at offset +0x84 is divided by `DAT_004c3d90` (the total span ring length). The quotient is the **raw lap count**; the remainder becomes the normalized position at +0x82:
```c
normalized_span = accumulated_spans % total_span_count;
raw_lap_count   = accumulated_spans / total_span_count;
```

Called every sim tick in `RunRaceFrame` for all active actors.

---

## 2. Race Order / Position Tracking

### Core Function: `UpdateRaceOrder` (0x42f5b0)

Called every sim tick inside `RunRaceFrame`. Maintains the **race order array** at `DAT_004ae278` (6 bytes, one per slot, indexed 0-5) using a **bubble sort**:

1. Compares adjacent actors in the order array by their offset +0x86 value (normalized span position = forward progress).
2. If an unfinished actor (post_finish_metric_base == 0) has a lower span position than its neighbor, they are swapped.
3. Repeats until no swaps occur (stable sort).

After sorting, each actor's **display position** (0=1st, 1=2nd, ..., 5=6th) is written to `gap_0376[0xd]`.

**Time Trial special case:** When both slots 0 and 1 have finished (post_finish_metric_base != 0), the winner is determined by `finish_time * 256 - post_finish_metric_336`, providing millisecond-level tiebreaking. The positions are then hard-assigned (bypassing the span-based sort).

### HUD Display

`RenderRaceHudOverlays` (0x4388a0) displays position when bit 0x01 of the HUD flags is set:
```c
QueueRaceHudFormattedText(..., PositionStrings[actor.gap_0376[0xd]]);
```

---

## 3. Lap Detection (Circuit Tracks)

### In `CheckRaceCompletionState` (0x409e80) — circuit branch

For circuit tracks (`gTrackIsCircuit != 0` and `gTimeTrialModeEnabled == 0`):

The lap counter uses a **4-bit sector bitmask** system at `psVar6[0]` (the checkpoint progress field):

| Bitmask | Meaning |
|---------|---------|
| 0x00 | No sectors passed |
| 0x01 | Sector 0 passed |
| 0x03 | Sectors 0+1 passed |
| 0x07 | Sectors 0+1+2 passed |
| 0x0F | All 4 sectors passed = lap complete |

The track is divided into 4 **sectors** (not checkpoints — these are derived from `DAT_004c3d90` divided into equal segments). The function checks if the actor's current span falls within each sector's threshold range (`DAT_004aad68 +/- 1`), and progressively sets bits 0-3 in a strict order (0 -> 1 -> 3 -> 7 -> 0xF).

**When all 4 sectors are passed (bitmask == 0x0F):**
```c
checkpoint_index++;
checkpoint_bitmask = 0;  // reset for next lap
if (checkpoint_index == gCircuitLapCount) {
    // FINISH: record finish time, compute speed bonus, set state to 0x02
    post_finish_metric_base = cumulative_timer;
    average_speed = (sub_progress * 1500) / average_speed_raw;
    companion_state_1 = 1;  // "finished" flag
    state = 0x02;           // "completed" state
}
```

Lap split times are stored at `pcVar7 + 0xc` onwards (per-lap array of uint16 values), computed as deltas from cumulative time.

### gCircuitLapCount

Set per game type in `ConfigureGameTypeFlags`:
- **Era Cup (type 2):** hardcoded to 4 laps
- **All other cup types:** from frontend options (DAT_004aaf3c)
- **Time Trial:** N/A (point-to-point logic used even on circuits)

---

## 4. Checkpoint Timing (Point-to-Point Tracks)

### Checkpoint Table: `DAT_004aed88` -> `DAT_004aed98`

Loaded from hardcoded `.rdata` per-track tables by `LoadTrackRuntimeData` (0x42fb90):
```c
// DAT_0046cf6c is a pointer table (1 + 18 entries), first dword is padding (0xC30)
// DAT_0046cf70..DAT_0046cfA0 are 18 pointers to 24-byte checkpoint timing records
// at addresses 0x46CBB0 through 0x46CD48 (each 24 bytes)
puVar4 = *(ptr *)(0x46cf6c + track_pool_index * 4);
memcpy(DAT_004aed98, puVar4, 24);  // copy 6 dwords
DAT_004aed88 = &DAT_004aed98;      // active checkpoint pointer
```

### Checkpoint Record Format (24 bytes)
```c
struct CheckpointTimingRecord {
    uint8_t  checkpoint_count;   // +0x00: number of checkpoints (always 5)
    uint8_t  pad;                // +0x01: zero padding
    uint16_t initial_time;       // +0x02: starting timer value (in 1/30s ticks)
    // For each checkpoint (checkpoint_count entries):
    struct {
        uint16_t span_threshold; // +0x04+i*4: span index that triggers this checkpoint
        uint16_t time_bonus;     // +0x06+i*4: time added when checkpoint is reached
    } checkpoints[5];
    uint16_t pad_end;            // +0x18: zero terminator
};
```

### Decoded Checkpoint Table (18 tracks)

| Track | Init Time | CP1 span/bonus | CP2 span/bonus | CP3 span/bonus | CP4 span/bonus | CP5 span/bonus |
|-------|-----------|----------------|----------------|----------------|----------------|----------------|
| 1 | 0x3B=59 | 0x0365/0x3C | 0x05E7/0x2D | 0x080D/0x3C | 0x0A3A/0x28 | 0x0C02/0x00 |
| 2 | 0x3B=59 | 0x033A/0x2D | 0x0595/0x14 | 0x0674/0x1E | 0x0786/0x3C | 0x09D4/0x00 |
| 3 | 0x3B=59 | 0x0300/0x46 | 0x0563/0x41 | 0x082A/0x41 | 0x0AD8/0x2D | 0x0C95/0x00 |
| 4 | 0x3B=59 | 0x026F/0x32 | 0x0497/0x3C | 0x06D7/0x23 | 0x0885/0x23 | 0x09F8/0x00 |
| 5 | 0x3B=59 | 0x02EB/0x1E | 0x03EE/0x32 | 0x05FD/0x37 | 0x07BA/0x46 | 0x0AC2/0x00 |
| 6 | 0x3B=59 | 0x0261/0x28 | 0x0405/0x28 | 0x0618/0x32 | 0x085C/0x41 | 0x0A07/0x00 |
| 7 | 0x3B=59 | 0x022C/0x46 | 0x0459/0x37 | 0x067F/0x37 | 0x0901/0x5A | 0x0BF4/0x00 |
| 8 | 0x3B=59 | 0x02CB/0x23 | 0x03DD/0x23 | 0x04BC/0x37 | 0x0717/0x32 | 0x09CC/0x00 |
| 9 | 0x3B=60 | 0x0249/0x4B | 0x04F7/0x50 | 0x07BE/0x46 | 0x0A21/0x46 | 0x0CD2/0x00 |
| 10 | 0x3B=60 | 0x01D2/0x23 | 0x0380/0x32 | 0x05C0/0x37 | 0x07E8/0x32 | 0x09E0/0x00 |
| 11 | 0x3B=59 | 0x0385/0x28 | 0x0542/0x2D | 0x0751/0x1E | 0x0854/0x37 | 0x0AC3/0x00 |
| 12 | 0x3B=60 | 0x0207/0x3C | 0x044B/0x2D | 0x065E/0x1E | 0x0802/0x23 | 0x09DB/0x00 |
| 13 | 0x3B=59 | 0x028B/0x37 | 0x0468/0x32 | 0x063F/0x37 | 0x0843/0x2D | 0x0A0E/0x00 |
| 14 | 0x3B=59 | 0x01E6/0x28 | 0x0421/0x2D | 0x0677/0x23 | 0x0817/0x2D | 0x0A62/0x00 |
| 15 | 0x3B=59 | 0x0294/0x3C | 0x0511/0x37 | 0x0730/0x28 | 0x0891/0x32 | 0x0A60/0x00 |
| 16 | 0x3B=90 | 0x0275/0x28 | 0x049E/0x2D | 0x0648/0x32 | 0x08A3/0x37 | 0x0A54/0x00 |
| 17 | 0x3B=59 | 0x02AD/0x41 | 0x05A6/0x2D | 0x0732/0x32 | 0x08E9/0x46 | 0x0BAC/0x00 |
| 18 | 0x3B=59 | 0x025E/0x3C | 0x0462/0x32 | 0x0639/0x41 | 0x0816/0x3C | 0x0A32/0x00 |

Initial times are ~59-90 ticks (about 2-3 seconds at 30fps). Time bonuses range from 0x14 (20 ticks = 0.67s) to 0x5A (90 ticks = 3s). Final checkpoint always has 0 bonus.

### Difficulty Scaling: `AdjustCheckpointTimersByDifficulty` (0x40a530)

Applied once per actor during `InitializeRaceVehicleRuntime`:
```c
if (is_human_player) {
    if (gRaceDifficultyTier == 0) {     // Easy
        initial_time *= 12/10;           // +20% time
        each bonus *= 12/10;
    } else if (gRaceDifficultyTier == 1) { // Medium
        initial_time *= 11/10;           // +10% time
        each bonus *= 11/10;
    }
    // Hard (tier 2): no adjustment (base values)
}
actor.timer = initial_time;  // at offset +0x344
actor.checkpoint_index = 0;  // at offset +0x37E
actor.finish_time = 0;       // at offset +0x328
```

### Checkpoint Crossing (Point-to-Point Path in `CheckRaceCompletionState`)

For P2P tracks (`gTrackIsCircuit == 0` or `gTimeTrialModeEnabled != 0`):

1. Record cumulative time at the current checkpoint slot.
2. Compute per-checkpoint split times as deltas.
3. Check if actor's current span progress >= checkpoint `span_threshold`.
4. If yes: add `time_bonus` to the countdown timer and advance to next checkpoint.
5. When `checkpoint_index == checkpoint_count`:
   - Record finish time: `post_finish_metric_base = cumulative_timer`
   - Compute average speed: `avg_speed = (sub_progress * 1500) / time_in_256ths`
   - Compute speed bonus: `speed_bonus = (avg_speed * 1000) - (some_factor * 1000 / 256)`
   - Set state = 0x02 (completed).

### Checkpoint Order Remapping: `RemapCheckpointOrderForTrackDirection` (0x42fd70)

When the track is run in reverse (`gReverseTrackDirection != 0`), the checkpoint order table at `DAT_004aedb0` (loaded from `CHECKPT.NUM` inside the level ZIP) is remapped by swapping indexed pairs using `SwapIndexedRuntimeEntries`. Two flavors exist depending on whether the track has 3 or 5 permutable segments.

---

## 5. Race Completion Detection

### `CheckRaceCompletionState` (0x409e80) — Two-Phase Architecture

**Phase 1** (DAT_00483980 == NULL): Per-actor finish detection
- Runs the appropriate logic (circuit sector bitmask or P2P checkpoint) for each active actor.
- When all required actors have finished, sets `DAT_00483980 = 1` (cooldown latch).
- **Time Trial special case:** only requires slots 0 and 1 (human players) to finish.
- **Normal race:** requires all 6 slots with state 0x01 or 0x02 to finish, OR slot 0 to be finished and `DAT_004aaf78 != 0` (spectator/replay mode).

**Phase 2** (DAT_00483980 != NULL): Post-finish cooldown
- Accumulates `param_1` (the sim time delta) into `DAT_00483980`.
- When the accumulator exceeds `0x3FFFFF` (~4.2 million, about 2 seconds of sim time):
  - Resets the latch to NULL.
  - Calls `BuildRaceResultsTable()`.
  - Returns 1 (signals RunRaceFrame to begin fade-out).

### Fade-Out in RunRaceFrame (0x42b580)

When `CheckRaceCompletionState` returns 1:
1. Sets `g_raceEndFadeState = 1`.
2. **Replay timeout:** If in replay mode and `g_raceEndTimerStart` has been active for > 45 seconds, forces fade-out.
3. `BeginRaceFadeOutTransition` (0x42cc20) selects fade direction based on viewport layout (alternating horizontal/vertical for single player, fixed for split-screen).
4. The fade accumulates per-tick until reaching 255.0.
5. At 255.0: releases all race resources (sound, input, render, textures) and returns 1.

### RunRaceFrame Returns 1 -> Game State Dispatcher

In `RunMainGameLoop` (0x442170), when `RunRaceFrame()` returns non-zero:
```c
DAT_004c3ce8 = (benchmark_mode ? 3 : 1);  // state 1 = frontend
DXPlay::UnSync();
```
This returns to the **frontend display loop**, which picks up at `RunRaceResultsScreen` (screen index 0x18 = 24, which is Entry 24 in g_frontendScreenFnTable).

---

## 6. Results Table & Scoring

### Results Table: `DAT_0048d988` (0x78 bytes = 6 entries x 0x14 bytes)

```c
struct RaceResultEntry {  // 20 bytes per entry
    /* +0x00 */ uint8_t  slot_flags;        // bit 0 = active
    /* +0x01 */ uint8_t  slot_index;        // racer slot (0-5)
    /* +0x02 */ int16_t  final_position;    // 0=1st, 1=2nd, ..., 5=6th
    /* +0x04 */ uint16_t pad;
    /* +0x06 */ uint16_t pad2;
    /* +0x08 */ int32_t  primary_metric;    // finish time (for sort: lower = better)
    /* +0x0C */ int32_t  secondary_metric;  // accumulated points (for sort: higher = better)
    /* +0x10 */ uint8_t  wanted_kills;      // cop chase busts
    /* +0x11 */ int16_t  speed_bonus;       // accumulated speed bonus
    /* +0x13 */ int16_t  top_speed;         // max speed achieved
};
```

### `ResetRaceResultsTable` (0x40a880)

Clears all 6 entries to zero, sets `slot_flags` of entry 0 to 1 (marks it active).

### `BuildRaceResultsTable` (0x40a8c0)

Called by `CheckRaceCompletionState` when the cooldown expires. For each of 6 actor slots:

1. **AI finish time estimation:** If `post_finish_metric_base == 0` (AI didn't actually finish), synthesizes a finish time:
   ```c
   estimated_time = rand() & 0x1F  // random 0-31 ticks
       + (cumulative_timer * reference_constant) / normalized_span
       + checkpoint_progress_byte * 0x20;
   ```
   Then distributes this time into per-lap splits (with random +/- 150 jitter per lap).

2. **Points accumulation:** Based on position and race rule variant:
   - `gRaceRuleVariant == 0` (Championship/basic, with `DAT_004aaf74 == 2`):
     Points from position table at `DAT_00463a18`: **{15, 12, 10, 5, 4, 3}**
   - `gRaceRuleVariant == 4` (Ultimate, with `DAT_004aaf74 == 2`):
     Points from table at `DAT_00463a20`: **{1000, 500, 250, 0, 0, 0}**
   - Other variants: no position points awarded.

3. **Metric folding:** For each slot, the entry accumulates:
   - `primary_metric += post_finish_metric_base` (cumulative finish time)
   - `secondary_metric += accumulated_points_at_0x2C8` (points/score)
   - `wanted_kills` from cop chase mode
   - `speed_bonus += current_race_speed_bonus`
   - `top_speed = max(top_speed, current_race_top_speed)`

### Sort Functions

**`SortRaceResultsByPrimaryMetricAsc` (0x40aad0):**
- Bubble sorts `DAT_004ae278` order array by `primary_metric * 100 / 30` (ascending = fastest wins).
- Used for game types 2-5 (Era, Challenge, Pitbull, Masters).
- Writes `final_position` into each result entry.

**`SortRaceResultsBySecondaryMetricDesc` (0x40ab80):**
- Bubble sorts by `secondary_metric` (descending = most points wins).
- Used for game types 1, 6 (Championship, Ultimate).
- Writes `final_position` into each result entry.

---

## 7. Speed Bonus Scoring

### `AccumulateVehicleSpeedBonusScore` (0x40a3d0)

Called periodically during the race. Accumulates a speed-based bonus at actor offset +0x2C8:
```c
if (!finished && forward_speed > 0 && (tick_counter & 3) == 0) {
    bonus = (speed >> 15) - (skid_factor >> 1);
    if (contact_count > 15 || bonus < 0) bonus = 0;
    if (is_human && current_span < target_span) bonus = 0;  // behind checkpoint
    accumulated_bonus += bonus;
}
```

### `DecayUltimateVariantTimer` (0x40a440)

In Ultimate mode (gRaceRuleVariant == 4), the speed bonus decays over time:
```c
if (!finished && gRaceRuleVariant == 4) {
    accumulated_bonus -= decay_amount;
    if (accumulated_bonus < 0) accumulated_bonus = 0;
}
```

### `AwardWantedDamageScore` (0x43d690)

In Cop Chase mode: when the player vehicle collides with an NPC, the NPC's health at `DAT_004bead4[npc*2]` is decremented by 0x200 (light hit) or 0x400 (heavy hit, if damage > 20000). Each "kill" (health <= 0) increments `DAT_004bead0` and awards +50 score to the player's accumulator.

---

## 8. Cup Series & Race Scheduling

### `InitializeRaceSeriesSchedule` (0x40dac0)

Sets up the 6-race schedule when a cup begins:

**For Era Cup (type 2):**
- Randomly selects tracks from pool, avoiding duplicates. Tracks 0-7 for standard difficulty, 8-15 for advanced.
- Random direction per track.
- Hardcodes 4 laps.

**For Masters Cup (type 5):**
- Uses a pre-shuffled 15-entry random permutation at `DAT_0048f30c`.
- 5 races are marked "pending" (`DAT_0048f324[i] = 1`), rest are 0.
- `MarkMastersRaceSlotCompleted` (0x40df80) marks each completed slot.

**For all other cups (types 1, 3, 4, 6):**
- Reads track indices from a difficulty-tiered lookup table at `0x463e10 + gRaceDifficultyTier * 6`.
- Randomly assigns direction.

### Series State at `DAT_00497250`

```
DAT_0049725c[0..5] : per-race status bytes (0=empty, 1=active, 2=complete)
DAT_00497268[0..5] : track pool index for each race
DAT_00497280[0..5] : track direction (0-3)
DAT_00497298[0..5] : accumulated time score
DAT_004972b0[0..5] : accumulated point score
```

### `ConfigureGameTypeFlags` (0x410ca0)

Maps `g_selectedGameType` into runtime flags before each race:

| Type | Variant | Difficulty | Traffic | Encounters | Laps | Points |
|------|---------|------------|---------|------------|------|--------|
| 1 (Championship) | 0 | Easy | Yes | Yes | Options | Position (15/12/10/5/4/3) |
| 2 (Era) | 5 | Easy | Yes | Yes | 4 | Time-based |
| 3 (Challenge) | 1 | Medium | Yes | Yes | Options | Time-based |
| 4 (Pitbull) | 2 | Hard | Yes | Yes | Options | Time-based |
| 5 (Masters) | 3 | Medium | Yes | Yes | Options | Time-based |
| 6 (Ultimate) | 4 | Hard | Yes | Yes | Options | Position (1000/500/250) |
| 7 (Time Trial) | - | Hard | No | No | - | Time + speed bonus |
| 8 (Cop Chase) | - | - | Yes | No | - | Wanted/damage |
| 9 (Drag Race) | - | - | No | No | - | Time-based |

The **NPC group index** for cup races is read from a 2D table:
```c
DAT_00490ba8 = lookup_table[g_selectedGameType * 16 + g_raceWithinSeriesIndex + 0x14];
```
This maps (game type, race index within series) to one of the 26 NPC racer groups.

---

## 9. Special Encounter System

### `UpdateSpecialTrafficEncounter` (0x434da0)

The "encounter" is a special NPC (actor slot 9 in the traffic system) that challenges the player mid-race. Controlled by `gSpecialEncounterEnabled`.

**Trigger conditions:**
- Player is exactly 2 spans ahead of encounter start position
- Player speed > 0x15639 (~87k internal units)
- Player is moving forward (gActorForwardTrackComponent > 0)
- Route table matches (same route variant)
- Encounter cooldown `DAT_004b064c` has expired (starts at 300 ticks = 10 seconds)

**Active encounter:** `UpdateSpecialEncounterControl` (0x434ba0)
- Overrides the encounter actor's steering to chase the player
- Tests heading alignment (must be within +/- 1024 of 4096 range)
- Tests forward progress (must exceed minimum threshold)
- If alignment/spacing fails: tears down encounter, increments `special_encounter_state`, resets 300-tick cooldown

**Teardown on distance:** If encounter actor falls more than 0x40 spans behind the player, it is despawned.

---

## 10. Cup Completion Unlocks

### `AwardCupCompletionUnlocks` (0x421da0)

Called from `RunRaceResultsScreen` when the player presses "Quit" after a cup series. Only fires if:
- `gRaceSlotStateTable.slot[0].companion_state_2 == 1` (player completed)
- `gRuntimeSlotActorTable.slot[0].gap_0376[0xd] == 0` (player finished 1st)

The NPC group header byte at `gNpcRacerGroupTable[group * 0x29]` determines unlock behavior:

**Byte == 0 (track-based unlock):**
```c
DAT_004a2c9c[current_group] = 1;  // marks track as unlocked
```

**Byte == 1 (car-based unlock):**
Hardcoded switch on group index:
| Group | Unlocks Car Index |
|-------|-------------------|
| 4 | 0x15 (21) |
| 5 | 0x11 (17) |
| 6 | 0x18 (24) |
| 7 | 0x19 (25) |
| 16 | 0x17 (23) |
| 17 | 0x1A (26) |
| 18 | 0x20 (32) — or sets `DAT_004962b0 = 1` if already at max |

The unlock clears the "locked" flag: `DAT_00463e4c[car_index] = 0` and advances the available car count `DAT_00463e0c` if needed.

---

## 11. High Score Table

### NPC Racer Group Table: `gNpcRacerGroupTable` (0x4643b8)

26 groups, 0x29 (41) bytes per group header + 5 entries of 0x20 (32) bytes = 0xA4 per full group.

**Header byte [0]** determines scoring mode:
- **0 = time scoring** (lower is better) — displays MM:SS:ms
- **1 = lap-time scoring** — displays lap splits
- **2 = points scoring** (higher is better) — displays integer points
- **4 = precision time** — displays MM:SS:mmm

Each entry (32 bytes):
```
+0x00: char name[16]     — racer name
+0x10: uint16_t score    — time (ticks) or points
+0x14: uint8_t car_id    — car index
+0x18: uint16_t avg_speed
+0x1C: uint16_t top_speed
```

### `DrawPostRaceHighScoreEntry` (0x413010)

Renders the high-score board with columns: Name, Best (time/laps/points), Car, Avg Speed, Top Speed. Speed unit toggled by `DAT_00466028` (0=MPH, 1=KPH).

---

## 12. Post-Race Flow

### State Machine in `RunRaceResultsScreen` (0x422480)

The results screen is a 22-state (`g_frontendInnerState` 0-0x15) frontend state machine:

**State 0 — Initialization:**
1. Saves pre-race state as backup (for "race again").
2. Calls `RestoreRaceStatusSnapshot()` to reload the series state.
3. **Sorts results:**
   - Types 1, 6 (Championship, Ultimate): `SortRaceResultsBySecondaryMetricDesc` (most points wins)
   - Types 2-5 (Era, Challenge, Pitbull, Masters): `SortRaceResultsByPrimaryMetricAsc` (fastest wins)
4. Checks for **disqualification** (state 0x02 = DNF, or finish time == 0):
   - Cup modes: if disqualified, jumps to "Cup Failed" screen (0x1A).
   - Drag Race (type 4): if player lost, also goes to failure.
5. Draws column headers per game type (Result/Time/Position etc.).
6. Creates OK button and data display surface.

**States 1-5 — Slide-in animation.**

**State 6 — Interactive:**
- User can press OK to proceed, or use left/right to browse other racers' data.
- `DrawRaceDataSummaryPanel` shows: position, finish time, avg/top speed, per-lap splits.

**States 7-10 — Slide animation for racer data panel.**

**States 11 — Slide-out.**

**State 12 — Resource cleanup.**

**State 13 — Post-race menu:**
- **Single race / Time Trial (types < 1, == 7):** Shows buttons: Race Again, View Replay, View Race Data, Select New Car, Quit.
- **Cup modes (types 1-6):**
  - Advances `g_raceWithinSeriesIndex`.
  - Calls `ConfigureGameTypeFlags()` — if it returns 0, the cup is finished (group index == 99).
  - Cup finished: buttons include "Next Cup Race" (preview), View Replay, View Race Data, Save Race Status, OK.
  - Cup in progress: same buttons but "Next Cup Race" is active.
- **Cop Chase / Drag Race (types 8-9):** Returns to main menu (screen 5).

**State 15 — Button dispatch:**
| Button | Action |
|--------|--------|
| 0 (Next/Race Again) | Sets `DAT_004a2c8c = 2`, restores pre-race state if needed |
| 1 (View Replay) | Sets `g_inputPlaybackActive = 1` |
| 2 (View Race Data) | Goes to high-score screen (0x18) |
| 3 (Save Race Status) | Goes to save state (state 0x11) |
| 4 (Quit) | Calls `AwardCupCompletionUnlocks()`, goes to screen 0x19 |

**State 0x11 — Save:**
- Calls `WriteCupData()` (XOR-encrypted mid-cup snapshot to CupData.td5).
- Displays "Block Saved OK" or "Failed to Save".

**State 0x15 — New car selection:**
- Sets `DAT_00497a78 = 1` (signals car select needed).
- Navigates to car selection screen (0x14).

---

## 13. Race Slot State Machine

Each of the 6 actor slots has a 4-byte state entry in `gRaceSlotStateTable`:

```c
struct RuntimeSlotStateEntry {
    char state;              // +0: 0=AI-inactive, 1=active/player, 2=completed, 3=disabled
    char companion_state_1;  // +1: 0=racing, 1=finished
    char companion_state_2;  // +2: 0=ok, 1=completed-ok, 2=DNF/disqualified
    char reserved;           // +3: unused
};
```

State transitions:
```
0x00 (inactive AI) -> runs AI control, no input polling
0x01 (active player) -> player input polled, physics active
0x02 (completed) -> actor still rendered but no control
0x03 (disabled) -> actor zeroed, not simulated or rendered
```

---

## 14. Serialization / Save-Load

### `SerializeRaceStatusSnapshot` (0x411120)

Serializes the entire race state into a contiguous buffer at `DAT_00490bac` (0x32A6 = 12966 bytes):
- Game type, race index, difficulty, rule variant, mode flags
- Series schedule (6 tracks, directions, accumulated scores)
- Results table (6 entries)
- Full RuntimeSlotActorTable (6 x 0x388 = all actor state)
- Race slot state table (6 x 4 bytes)
- CRC-32 checksum (polynomial table at `DAT_00475160`)

### `RestoreRaceStatusSnapshot` (0x4112c0)

Reverses the serialization. Validates CRC-32 before restoring. Used at the start of `RunRaceResultsScreen` and when loading a saved cup.

### `WriteCupData` / `LoadContinueCupData` (0x4114f0 / 0x411590)

XOR-encrypts the serialized buffer with key "Steve Snake says - No Cheating!" and writes to `CupData.td5`. CRC-32 validation on load prevents tampering.

---

## Key Addresses Summary

| Address | Type | Description |
|---------|------|-------------|
| 0x46CF70-0x46CFA0 | .rdata | 18 pointers to per-track checkpoint timing records |
| 0x46CBB0-0x46CD48 | .rdata | 18 checkpoint timing records (24B each) |
| 0x463A18 | .data | Position points table: {15,12,10,5,4,3} (Championship) |
| 0x463A20 | .data | Position points table: {1000,500,250,0,0,0} (Ultimate) |
| 0x4AE278 | .bss | Race order array (6 bytes, sorted by position) |
| 0x48D988 | .bss | Race results table (6 x 20 bytes) |
| 0x4AED88 | .bss | Active checkpoint record pointer |
| 0x4AED98 | .bss | Active checkpoint record copy (24 bytes) |
| 0x4AEDB0 | .bss | CHECKPT.NUM data (96 bytes, loaded from ZIP) |
| 0x483980 | .bss | Post-finish cooldown accumulator/latch |
| 0x4C3D90 | .data | Total span ring length (for wrap/normalize) |
| 0x4AAF00 | .bss | Active racer count |
| 0x490BAC | .bss | Serialized race snapshot (12966 bytes) |
| 0x4643B8 | .rdata | gNpcRacerGroupTable (26 groups x 0xA4 bytes) |
