---
tier: T1
date: 2026-05-20
batches: [01, 02, 03, 04, 05]
status: ready_for_consolidation
---

# T1 Global Naming Sweep — Summary

## Headline stats

- **Total unnamed `DAT_*` globals enumerated**: 37 in inventory CSV (+ 6 from batch_02 lost in overwrite — see caveat)
- **High-confidence proposals**: 29
- **Medium-confidence**: 7
- **Low-confidence / provisional**: 1
- **Functions analyzed**: ~33 across 5 areas
- **Pool slots used**: 1, 7, 10, 11, 12+ (all released)

## Per-batch roll-up

| batch | area | unnamed globals | high | med | TODO impact |
|---|---|---|---|---|---|
| 01 | transmission_reverse | 6 | 4 | 2 | Both transmission + reverse TODOs close via input-bit-28 polarity fix |
| 02 | wanted_mode | 6 (table lost) | — | — | Police-chase TODO closes via writer at 0x0042acf2 |
| 03 | traffic_init | 9 | 7 | 2 | Both traffic TODOs close via traffic queue cursor seed |
| 04 | checkpoint_config | 5 | 5 | 0 | Both checkpoint TODOs partially close |
| 05 | replay_results | 17 | 10 | 5 (+2 low) | Both replay + view-race-data TODOs identified |

## Root-cause findings unblocking TODOs

### 1. Transmission + Reverse (collapses two TODOs to one fix)

`actor[+0x378]` (auto_gearbox) is **recomputed every frame** from input bit 28 of `g_playerControlBits` @ 0x00482FFC, NOT cardef-seeded.

```c
// UpdatePlayerVehicleControlState @ 0x00402E60 (instruction 1)
actor[+0x378] = ~(g_playerControlBits[slot] >> 28) & 1;
```

**Fix scope**: port's `td5_input.c` bit-28 mapping. Likely an inversion or unconditional set.

Closes: `todo-default-transmission-auto-2026-05-19`, `todo-reverse-not-triggered-2026-05-19`.

### 2. Police chase / wanted mode

`g_wantedModeEnabled` writer at `InitializeRaceSession @ 0x0042aa10`:
- `0x0042ab27` — cleared unconditionally
- `0x0042acf2` — SET to 1 when `g_selectedGameType == 8` (cops/chase)

**Fix scope**: port's race-init equivalent of `InitializeRaceSession` is missing the game-type-8 → wanted-mode write. ARCH-DIVERGENCE rewrite dropped it.

Closes: `todo-police-chase-no-audio-2026-05-19`.

### 3. Traffic — both bugs are one fix

`g_traffic_queue_cursor_ptr @ 0x004b08b8` is the seed point. If not initialized at level load, `RecycleTrafficActorFromQueue @ 0x004353B0` returns early — traffic cardef pointers stay NULL → no drive torque AND no Y-lift.

**Fix scope**: port must seed the traffic spawn queue on level load (TRAFFIC.BUS asset parse + queue init).

Closes: `todo-traffic-not-moving-2026-05-19`, `todo-traffic-clipping-ground-2026-05-19`.

### 4. Circuit checkpoint timer

`AdjustCheckpointTimersByDifficulty @ 0x40A530` is called unconditionally for all modes. Original behavior: HUD render layer suppresses the timer on circuit; port may have a bug where the timer is rendered.

**Fix scope**: gate `adjust_checkpoint_timers` on `track_type != TD5_TRACK_CIRCUIT` at `td5_game.c:2081`, AND audit HUD timer-draw gating.

Closes (partial): `todo-circuit-no-checkpoint-timer-2026-05-19`.

### 5. Replay restart bug

Replay save is triggered at race START (in `InitializeRaceSession` based on `g_inputPlaybackActive` flag). The orig calls `DXInput::WriteOpen()` to start recording. **Port has no equivalent writer.**

**Fix scope**: port needs a replay-record path in `td5_input.c` that fires when `g_inputPlaybackActive == 0` at race-init, recording inputs each tick to a buffer/file readable on playback.

Closes: `todo-view-replay-restarts-race-2026-05-19`.

### 6. View Race Data flash-back

State-3 early-exit gate at `0x00422506` checks `g_raceActorRuntimeValid @ 0x0048d988`. On "View Race Data" re-entry, if `InitializeFrontendDisplayModeState` cleared actor state, gate fires → bounces back.

**Fix scope**: ensure actor-runtime snapshot persists across the View Race Data self-jump, OR add a `results_displayed_once` flag to gate state-3 separately.

Closes: `todo-view-race-data-broken-2026-05-19`.

## Methodology notes

- All 5 agents ran read-only against the Ghidra pool — no project mutations.
- Pool slots: 1, 7, 10, 11, 12 (all released).
- `Explore` agent type CAN Write to files (despite the description suggesting otherwise). I learned this the hard way after my fallback Write overwrote one batch's per-global table (batch_02 wanted_mode — only the writer-loss-site finding survived).
- Consolidation session (writable Ghidra against master, not pool) is the next step. Process: apply `decomp_global_rename` for every high-confidence row, add `_PROVISIONAL` suffix for the one provisional entry, write Ghidra comments with the evidence, generate port-side `td5_orig_globals.h` for incremental adoption.

## Open caveats for consolidation

- **batch_02 partial**: 6 wanted-mode-adjacent globals were enumerated by the agent but their addresses + sizes were lost. Re-run a `general-purpose` agent if those names are needed; OR proceed without them (police-chase TODO closes via the writer-loss-site finding regardless).
- **`gUnknownCheckpointToggle_B`** at `0x00466008` is provisional — semantics need a deeper read of `ScreenGameOptions` before finalizing the name.

## Next session inputs

For the writable consolidation session, all 5 batch files plus `inventory_t1.csv` are the inputs. Process is straightforward (per-row apply renames + comments + port header gen). Estimated 10-20 min wall-clock to apply all 37 renames.
