# Session 06 — Edinburgh span 350 + Newcastle span 216 invisible walls

## Goal
Close the two known wall-synth holes where port detects a wall collision but
no visible wall exists in geometry:
- Edinburgh span 350 (reported by user)
- Newcastle span 216 (reported earlier; per memory deferred)

## Context — read these first

- `memory/todo_newcastle_span_216_invisible_wall_2026-05-19.md`
- `memory/todo_wall_response_clamp_fix_2026-05-16.md` — fork-adjacent gate shipped 2c7c7e8 closed Sydney+BlueRidge but not Edinburgh/Newcastle
- `td5mod/src/td5re/td5_track.c` — search for `fork_adjacent` or wall-synth gating code

## Observable symptom

Driving on Edinburgh span 350 (and Newcastle 216) hits an invisible wall —
visual mesh shows no wall, but collision triggers wall_response.

## Approach

1. **Static audit of wall-synth gate**:
   - Find `fork_adjacent` or equivalent gate (memory references shipped 2c7c7e8)
   - Check the data table that drives which spans are flagged
   - Compare against orig table at corresponding RVA

2. **Runtime drive-test**: 
   - `td5re.exe --DefaultTrack=1 --PlayerIsAI=1 --RaceTraceMaxSimTicks=2000` (Edinburgh)
   - Look at `log/race.log` for `wall_response` triggers near span 350
   - Repeat for `--DefaultTrack=5` (Newcastle)

3. **Two possible fixes** depending on findings:
   - (a) Extend the fork-adjacent table to include Edinburgh 350 + Newcastle 216
   - (b) The MODELS.DAT remap via `rebuild_span_display_list_mapping` nearest-neighbor heuristic mis-identifies these spans — fix the heuristic for those specific tracks

4. **Validate**: drive through the patched spans, confirm no wall hit.

## Tools

- Ghidra MCP for orig wall-synth dispatch RVA
- Port instrumented with TD5_LOG_I on wall_response

## Success criteria

- Drive through Edinburgh span 350 with no wall_response trigger
- Drive through Newcastle span 216 with no wall_response trigger

## Files likely touched

- `td5mod/src/td5re/td5_track.c` (wall-synth gate or table)

## Risk
LOW. Localized to specific data table.

## Estimated time
1-2 hours.
