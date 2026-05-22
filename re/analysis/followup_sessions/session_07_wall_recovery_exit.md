# Session 07 — AI wall-collision recovery exit conditions

## Goal
Ensure port AI exits "recovery" mode after handling a wall collision, matching
orig behavior. Symptom: AI slots 1/3/4/5 enter recovery (per log) but may not
exit cleanly, leading to repeated heading misalignment.

**Depends on session 01** (AI stuck-at-137) — that session may already
characterize whether slot 0 has this issue too. Run #1 first.

## Context — read these first

- `memory/todo_ai_recovery_360_overcorrection_2026-05-16.md` (partial close)
- `memory/todo_ai_early_turn_into_wall_2026-05-16.md` (partial close)
- `td5mod/src/td5re/td5_ai.c:3370+` — recovery trigger at heading-mismatch
- Orig `0x00434FE0` — UpdateActorTrackBehavior with recovery script dispatch

## Observable symptom

Port log shows `recovery: slot=N hdelta=0x...` entries for slots 1/3/4/5 with
hdelta in (0x320, 0xCE0) range. Once entered, the recovery script may complete
or may need explicit termination logic.

## Approach

1. **Capture orig recovery exit conditions** via Frida hook on `0x00434FE0` onEnter+onLeave for any slot in recovery state. Log per-tick: actor->script_flags, script_countdown, hdelta.

2. **Capture same fields in port** via TD5_LOG_I.

3. **Compare**:
   - When does orig clear recovery state? (script_countdown=0? heading aligned?)
   - When does port clear?
   - Difference → fix.

4. **Likely findings**:
   - Port may not clear `RS[0x3D]` (script_flags) when script completes
   - Or `RS[0x45]` (script_countdown) doesn't decrement to 0
   - Or hdelta check is permanently in recovery range due to upstream divergence

## Tools

- Ghidra MCP for orig `0x00434FE0`
- Frida probe + port TD5_LOG_I

## Success criteria

- AI slots that enter recovery exit within reasonable tick count (matching orig)
- No infinite recovery loops in 2000-tick AI race

## Files likely touched

- `td5mod/src/td5re/td5_ai.c` (recovery exit logic)

## Risk
LOW-MEDIUM. Tightly scoped but may overlap with session 01 findings.

## Estimated time
2-3 hours (independent) OR cascade-close if session 01 already fixes the trigger.
