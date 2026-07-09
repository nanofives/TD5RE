# Session 05 — F-R suspension asymmetry (car tilted right)

## Goal
Find why the player car visually tilts to one side on flat ground. Per Phase
4c diagnostic memory, the cause is suspension positions sp[0..3] all being
written for racers (orig may only write 2), with F-R asymmetry producing the
roll appearance.

## Context — read these first

- `memory/reference_phase4_results_2026-05-20.md` — Phase 4c F-R asymmetry diagnosis
- `memory/todo_car_tilted_right_flat_surface_2026-05-19.md` — original TODO
- `td5mod/src/td5re/td5_physics.c:4258+` — `td5_physics_integrate_suspension` (player, per-wheel 4-DOF)
- `td5mod/src/td5re/td5_physics.c:2857+` — `apply_damped_suspension_force` (traffic, 2-DOF)

## Observable symptom

Player car on flat-ground straight section visibly leans to one side. Behavior
persists across cars (not cardef-specific).

## Approach

1. **Static audit first**: read `td5_physics_integrate_suspension` end-to-end against orig disasm at `0x00403A20`. Specifically check:
   - Does each of 4 wheels get an INDEPENDENT integration, or do orig wheels 2/3 (rear) share state with wheels 0/1 (front)?
   - The arm_x/arm_z (lever arm) for each wheel — are F and R wheels signs correct?
   - Wheel ordering: is port using {FL, FR, RL, RR} the same as orig? Or {FR, FL, RR, RL}?

2. **Frida probe orig** at `0x00403A20` (`IntegrateWheelSuspensionTravel`) onLeave. Capture per-wheel:
   - `wheel_suspension_pos[i]` (4 wheels)
   - `wheel_spring_dv[i]`
   - Roll/pitch produced (for downstream display)

3. **Port-side TD5_LOG_I** same fields.

4. **Diff** — likely findings:
   - F-R asymmetry: port may write all 4 sp[i] but orig only updates 2 of them, leaving the other 2 at 0 → orig's roll has no F-R contribution
   - Or sign-flip on a specific wheel index

5. **Apply fix**: gate or zero specific sp[i] writes to match orig.

## Tools

- Ghidra MCP for orig `0x00403A20` disasm
- New Frida probe: `tools/_probes/orig_suspension_probe.js`
- Port-side TD5_LOG_I in `td5_physics_integrate_suspension`

## Success criteria

- Port player car visibly sits level on flat straight Edinburgh section
- Roll display angle stable near 0 when car is stationary on flat ground

## Files likely touched

- `td5mod/src/td5re/td5_physics.c` (`td5_physics_integrate_suspension` body)
- `tools/_probes/orig_suspension_probe.js` (new)

## Risk
LOW. Targeted fix in a well-understood function.

## Estimated time
1-2 hours.
