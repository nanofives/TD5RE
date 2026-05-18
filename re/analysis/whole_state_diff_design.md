# Whole-State Per-Tick Diff — Design (V1)

## Why

Sixty sim-core functions are now byte-faithful ports of their original-binary
counterparts, yet AI races still diverge mid-track. The remaining bugs are not
in those ports — they're in something *upstream* of them: a non-ported caller
that mutates shared state, a global/DAT that's silently misinitialized, or a
field of `RuntimeSlotActor` that's modified by a function we haven't audited
yet.

The 79 existing Frida probes each dump only the fields *their* hooked function
touches, so a corrupted byte can drift through many ticks before showing up
in any narrow probe. **We need a single tick-aligned snapshot of the full
actor state on both binaries** so the first divergent byte localises the
upstream culprit precisely.

## Scope (V1)

- 6 actor slots × 0x388 bytes = 5424 bytes per tick.
- 128-byte globals blob per tick (RNG seeds, sim accumulators, race tables —
  see "Globals blob" below).
- One snapshot per sim tick.
- Same exact synchronisation point on both sides.
- Binary blob format, decoded post-hoc by a Python diff tool that prints
  `tick=T slot=S +0xNN (name) port=X original=Y` for the first byte that
  disagrees, using the offset map at `tools/td5_actor_offsets.py`.

Out of scope for V1 (added later if needed):
- DAT-table snapshots (mutated tables only — read-only tables don't drift).
- Traffic actor slots 6-11.

## Globals blob (128 bytes per record)

Mutable per-tick state outside the actor array. Fixed offsets inside the blob
so both sides write the same layout. Reserved tail padding lets us add fields
without breaking format compatibility (just bump `version`).

| Blob off | Size | Original addr | Field                       |
|---------:|-----:|--------------:|-----------------------------|
|   0x00   | 4    | 0x4C3CE8      | g_gameState                 |
|   0x04   | 4    | 0x4AAD60      | g_gamePaused                |
|   0x08   | 4    | 0x4C3D80      | g_raceEndFadeState          |
|   0x0C   | 4    | 0x4AAED0      | g_simTimeAccumulator        |
|   0x10   | 4    | 0x466E88      | g_simTickBudget (f32)       |
|   0x14   | 4    | 0x4AADA0      | g_simulationTickCounter     |
|   0x18   | 4    | 0x4AAD70      | g_normalizedFrameDt (f32)   |
|   0x1C   | 4    | 0x466E90      | g_instantFPS (f32)          |
|   0x20   | 4    | 0x4B1134      | g_viewCount                 |
|   0x24   | 4    | 0x4C3D44      | g_splitscreenCount          |
|   0x28   | 4    | 0x4969D4      | g_randomSeedForRace         |
|   0x2C   | 4    | 0x4AAD64      | g_raceSessionRandomSeed     |
|   0x30   | 4    | 0x4AADBC      | g_raceRandomSeedTable[0]    |
|   0x34   | 8    | 0x482FFC      | g_playerControlBits[2]      |
|   0x3C   | 24   | 0x4AADF4      | g_raceSlotStateTable (6×u32)|
|   0x54   | 6    | 0x4AE272      | g_raceSlotPlayerFlags (6×u8)|
|   0x5A   | 6    | 0x4AE278      | g_raceOrderArray (6×u8)     |
|   0x60   | 32   | —             | reserved (zero-filled)      |

`g_simulationTickCounter` is captured as a sanity check against the
record's `sim_tick` field — they must equal.

## Sync point — end of sim tick

Both sides hook **the same logical instant**: just before
`g_simulationTickCounter` is incremented at the end of one sim tick, after
every per-tick callee has run.

- **Original side:** Frida `Interceptor.attach(0x40A490, { onEnter })`. This
  is `UpdateRaceCameraTransitionTimer`, the last per-tick callee. Its 5-byte
  prologue is known-safe (used by `frida_race_trace.js` for `post_progress`).
- **Port side:** equivalent of `STG_POST_PROGRESS` — emit the snapshot from
  `td5_game.c` at the same call site where the existing per-module trace
  already emits `post_progress` rows.

## Binary format

Single file per side, append-only, fixed-size records:

```
file header (16 bytes):
  uint32  magic       = 'TD5W' (0x57354454 LE)
  uint16  version     = 1
  uint16  actor_count = 6
  uint16  actor_stride= 0x0388
  uint16  reserved    = 0
  uint32  record_count (rewritten on close)

record (5560 bytes):
  uint32  frame
  uint32  sim_tick
  uint8   actors[6][0x388]   // raw verbatim copy, no decoding
  uint8   globals[128]       // see "Globals blob" below
```

At 60 sim Hz × 10 s = 600 records ≈ **3.18 MiB** per side. Negligible.

Files:
- Original-side: `tools/frida_csv/whole_state_original.bin`
- Port-side: `log/whole_state_port.bin`

## Alignment

Both sides need to start from the same race conditions. Default baseline:

| Knob              | Value                              |
|-------------------|------------------------------------|
| Track             | 8 (Honolulu — open rollover case)  |
| Slot 0            | AI (`PlayerIsAI=1`)                |
| Start span offset | 0                                  |
| Cars              | Viper × 6                          |
| Tick cap          | 600                                |

Rationale: Honolulu has an unresolved chassis-launch from AI-dynamics force
overshoot. Forty-six percent excess vx by tick 1 (per memory:
`todo_state0f_overfire_skips_player.md`) means the divergence root cause is
visible within the first few ticks — ideal for a first-divergence diff.

The `sim_tick` field in each record is the authoritative join key. The diff
tool walks records by `sim_tick` and aborts at the first mismatched byte,
reporting (tick, slot, offset, semantic).

## Diff modes

- **First-divergence** (default): stop at the first byte that disagrees and
  report it.
- **All-divergences-per-tick** (`--all`): list every mismatched offset in the
  first divergent tick (often a single root-cause field corrupts several
  derived fields downstream).
- **Field-summary** (`--summary`): across the whole capture, rank fields by
  total ticks divergent. Useful when V1 finds a divergence early on and you
  want to know what *else* is wrong before fixing.

## V1 limitations (documented, not blockers)

1. **Offset map coverage is ~37%** of semantic fields (~16% of bytes); the
   rest report as `+0xNN (?)`. The canonical struct at
   `re/include/td5_actor_struct.h` is the source of truth — future work can
   expand the offset map from it. Unlabeled bytes still produce actionable
   diffs.
2. **RNG seed parity is not enforced, only observed.** Both binaries seed
   RNG from `g_randomSeedForRace` etc. at race start; if those globals differ
   in the first captured record, the diff tool flags it and aborts (no point
   diffing further). The runner script sets `RaceTraceSeed` to a fixed value
   on both sides to remove the variable.
3. **DAT tables are not snapshotted.** If actor + globals diff is clean but
   races still diverge, the bug is in a DAT mutated mid-race (rare). Phase 2.

## Acceptance gate

V1 ships when:
1. Both sides emit a record at every sim tick of an Edinburgh AI race.
2. `tools/diff_whole_state.py whole_state_port.bin whole_state_original.bin`
   on a clean build produces zero divergences within tick 0 (spawn parity)
   and exactly one identifiable first-divergent (tick, offset) pair
   thereafter — which is then the next bug to fix.
3. Run is automated by `tools/run_whole_state_diff.sh edinburgh-ai`.

## File checklist

- [x] `re/analysis/whole_state_diff_design.md` — this file
- [x] `tools/td5_actor_offsets.py` — offset → semantic map
- [ ] `tools/frida_whole_state_snapshot.js` — original-side capture
- [ ] `td5mod/src/td5re/td5_trace_whole_state.{c,h}` — port-side capture
- [ ] `tools/diff_whole_state.py` — diff + semantic decoder
- [ ] `td5re.ini` `[Trace] WholeState=` / `WholeStateMaxTicks=` knobs
- [ ] `tools/run_whole_state_diff.sh` — end-to-end runner
