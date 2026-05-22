---
batch: 02
area: wanted_mode_police_chase
tier: T1
target_todos: [todo-police-chase-no-audio-2026-05-19]
ghidra_session: TD5_pool1 (released)
analyzed_addresses: 0x00410ca0, 0x0042aa10, 0x0043d2d0, 0x0043d4e0, 0x0043d690, 0x00434ba0, 0x00441c60
agent: a6699ec67624cd123 (Explore type)
date: 2026-05-20
status: SUMMARY ONLY — full per-global table missing (Explore agent could not Write); transcribed from agent completion summary. May need re-run with general-purpose agent to capture the full table.
---

# Globals enumeration — wanted_mode / police chase

## Summary

- Functions analyzed: 8
- Unnamed DAT_* globals proposed: 6 (counts per summary; specific addresses/sizes/names not captured in summary text — needs re-run for full table)
- Already-named globals noted: 10

## Functions analyzed

| address | name |
|---|---|
| 0x00410ca0 | `ConfigureGameTypeFlags` — game-type to flag dispatcher |
| 0x0042aa10 | `InitializeRaceSession` — race bootstrap; CRITICAL writer of `g_wantedModeEnabled` |
| 0x0043d2d0 | `InitializeWantedHudOverlays` — HUD + damage state init |
| 0x0043d4e0 | `UpdateWantedDamageIndicator` — damage indicator render |
| 0x0043d690 | `AwardWantedDamageScore` — damage tracker; updates multiple state globals |
| 0x00434ba0 | `UpdateSpecialEncounterControl` — encounter tracking referenced in wanted flow |
| 0x00441c60 | `LoadRaceAmbientSoundBuffers` — siren asset loading path |

## Proposals

**NOTE**: Specific per-global table not captured in agent summary. Re-run required if full evidence + addresses needed for consolidation. Six unnamed `DAT_*` globals were proposed but the addresses/names are only in the agent's transcript (not safely readable).

## Key discoveries

**WRITER-LOSS SITE FOUND** for the wanted-mode TODO:

- `g_wantedModeEnabled` is written in **`InitializeRaceSession` @ 0x0042aa10** at TWO locations:
  - **0x0042ab27** — cleared unconditionally (initializes to 0 at race-bootstrap)
  - **0x0042acf2** — SET to 1 when `g_selectedGameType == 8` (cops/chase game-type)

This is the exact missing-writer pattern predicted by the parent session's hypothesis. The port has the READERS (sound gate at td5_sound.c:552, HUD overlays, cop-actor lookup) but the WRITER inside the orig's race-init orchestrator path was lost when that function was rewritten as ARCH-DIVERGENCE.

## TODO impact

- **todo-police-chase-no-audio-2026-05-19**: ✅ ROOT CAUSE IDENTIFIED. Single fix:
  1. Verify port can select `game_type == 8` (cops-chase) via INI or UI
  2. In `td5_game.c` race-init equivalent of orig `InitializeRaceSession`, add: `if (g_td5.game_type == TD5_GAMETYPE_COPS_CHASE) g_td5.wanted_mode_enabled = 1;`
  3. That single write unlocks siren audio + cop behavior + damage HUD overlays

## Out-of-scope finds

Per summary, the agent noted "deferred to later batches" — specific items not enumerated in summary text.

## Reproducibility note

Agent ran for ~25 min, released pool slot TD5_pool1 cleanly. Returned a summary but couldn't write the file because the Explore agent type lacks the Write tool. Consolidation session will need either:
1. The full per-global table re-captured via `general-purpose` agent (~10 min)
2. OR accept the writer-loss-site finding as sufficient for closing the TODO without naming the 6 specific DAT_* globals at this batch
