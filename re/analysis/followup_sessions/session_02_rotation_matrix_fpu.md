# Session 02 — FPU-faithful rotation matrix rebuild (root of countdown cascade)

## Goal
Close the root chain: `ang_vel residual → euler drift → rotation matrix recompute → wheel_pos.y drift → contact_delta`. Match orig's countdown contact_delta magnitude (~±8 fp world units) so the pragmatic countdown gate at `td5_physics.c:7055` becomes unnecessary.

## Context — read these first

- `memory/reference_root_cause_chain_2026-05-22.md` — full diagnostic chain confirmed
- `memory/reference_fwd_track_comp_sqrt_divisor_2026-05-21.md` — recent FPU work pattern
- `td5mod/src/td5re/td5_physics.c:5896` — where rotation matrix is rebuilt
- Comment at td5_physics.c:5905 — "2026-05-15 PRECISION FIX" mentions a precision fix already shipped (`CosFloat12bit` direct), but the drift still happens

## Observable symptom (port countdown diagnostic, slot 0 wheel 0)

```
Frame 0: rot[1]=-0.0330, rot[5]=-0.0353, rot[7]=-0.0127, rp.y=2570.57, world[1]=2327
Frame 1: rot[1]=-0.0325, rot[5]=-0.0338, rot[7]=-0.0093, rp.y=2571.57, world[1]=2330
Frame 2: rot[1]=-0.0321, rot[5]=-0.0338, rot[7]=-0.0107, rp.y=2571.07, world[1]=2329
```

Body offset stable: (-255,-227,435). Suspension stable: 0. But rotation matrix drifts ±0.0005 per frame and render_pos.y drifts ±0.5-1.0 per frame.

## Approach

1. **Extract orig's exact rotation matrix build sequence** from Ghidra:
   - Open `TD5_d3d.exe` in `TD5_pool11` (read-only)
   - Decompile `0x0042E1E0` (rotation matrix build site, Ry*Rx*Rz with YXZ order)
   - Document EXACT instruction sequence: FILD/FMUL/FSTP order, register allocation, FPU stack usage

2. **Diff against port** at `td5_physics.c:5896-5980` (the rotation build block). Look for:
   - Different operation order
   - Different precision (float vs double)
   - Missing FPU control word setup
   - Q12-truncate sites that orig avoids

3. **Run extended_physics_probe** against orig to capture orig's rotation matrix values across the SAME countdown frames. Compare against port's diagnostic dump.

4. **Apply fix** as a byte-faithful rewrite of the rotation build. Verify by:
   - Re-running diagnostic; rot[1/5/7] drift should match orig (<0.0001 per frame)
   - Removing the countdown gate at `td5_physics.c:7055`
   - Confirming port contact_delta at countdown is within ±8 fp world units (matches orig)

5. **Cascade verification**: with rotation fixed, items 6/7 (Edinburgh span launches) should also reduce in magnitude since the same precision improvement applies to non-countdown frames.

## Tools

- Ghidra MCP for orig disasm of `0x0042E1E0`
- Add a temporary Frida probe to orig that hooks `0x0042E1E0` onLeave and dumps the 12-float rotation matrix
- Port-side TD5_LOG_I dump of `rot[0..8]` per refresh_wheel_contacts call

## Success criteria

1. Countdown contact_delta in port within ±8 fp (matches orig).
2. Remove the pragmatic gate at `td5_physics.c:7055` and tests still pass.
3. Port AI in PlayerIsAI=1 reaches further than span 147 in 2000-tick race (combined with session #1 fix).

## Files likely touched

- `td5mod/src/td5re/td5_physics.c:5896-5980` (rotation matrix build)
- Possibly `td5_render.c` or wherever CosFloat12bit lives
- Remove pragmatic gate at `td5_physics.c:7055-7068`

## Risk
MEDIUM. FPU work has cascade effects everywhere. Changes should be tested with paired diff (`tools/diff_pool13_csv.py`) to confirm no regressions.

## Estimated time
4-8 hours (multi-session work). Can be split into "extract orig sequence" (2h) + "port and validate" (2-6h).
