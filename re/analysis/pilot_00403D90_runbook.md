# Pilot Runbook — 0x00403D90 UpdateVehicleState0fDamping

This is the operator-side procedure for running the pool6 capture + diff cycle. The static port + harness work is already done; what remains needs the running game.

## Pre-flight

| Item | Status | Path |
|---|---|---|
| Worktree | DONE | `.claude/worktrees/precise-00403D90` (branch `precise-00403D90`) |
| Pool slot | DONE | `TD5_pool6` (read-only) |
| Audit | DONE | `re/analysis/pilot_00403D90_audit.md` (7 divergences identified, D1 is the critical gate-inversion) |
| Frida probe | DONE | `tools/frida_pool6_00403D90.js` |
| Port trace module | DONE | `td5_pilot_trace_00403D90.{c,h}` in worktree |
| Port hook wired | DONE | `td5_physics_state0f_damping` calls enter/leave under `-DTD5_PILOT_TRACE_00403D90` |
| Port byte-faithful rewrite | DONE | `td5_physics.c:5843..` — D1-D7 all addressed |
| Build | DONE | `.claude/worktrees/precise-00403D90/td5re.exe` (1741450 bytes) |
| Diff tool | EXISTING | `tools/diff_func_trace.py` (use `--key=sim_tick,slot`) |

## Capture difficulty (READ FIRST)

State 0x0F fires only when the actor state byte (+0x376) is forced to 0x0F by `UpdatePlayerVehicleControlState (0x00402E60)` on input bit `0x200000`. Per memory `todo_state0f_overfire_skips_player.md`, this is essentially the chassis-launch / airborne event.

Edinburgh (track=1, span 0) typically does NOT trigger state-0f under normal driving. If the original capture yields zero rows, retry with:

- Honolulu (track=11) — known rollover scenarios per `todo_chassis_launch_track_walker.md`
- Moscow with jump features
- Force state-0f via Frida by writing 0x0F to actor+0x376 at a known tick

## Step 1 — Verify the worktree build succeeded

```bash
cd /c/Users/maria/Desktop/Proyectos/TD5RE
ls -la .claude/worktrees/precise-00403D90/td5re.exe
# expected: file exists, size ~1.74 MB
```

## Step 2 — Capture the original-side trace

Serial step — only one TD5_d3d.exe at a time. Attach with the pool6 probe:

```bash
cd /c/Users/maria/Desktop/Proyectos/TD5RE
# Start the original (windowed)
./original/TD5_d3d.exe  # or scripts/launch_original_windowed.sh

# In a second terminal — attach Frida with the pilot probe
frida -p $(pgrep TD5_d3d.exe) -l tools/frida_pool6_00403D90.js
```

Drive a deterministic test race:

- **Honolulu, slot 0**, drive until a chassis launch hits (or to ~60 sim_ticks if no event yet).
- If no state-0f events captured, try Moscow + Viper at full throttle into the tunnel exit ramp.

Expected output: `log/orig/pool6_00403D90.csv` with the header + N rows (one per state-0f tick).

If the capture is empty (no state-0f triggered):
1. Verify the actor state byte (+0x376) hits 0x0F at any point.
2. Add a Frida write-watcher on +0x376 in `frida_pool6_00403D90.js` to confirm transition.
3. Alternative: force the state via `Memory.writeU8(actor.add(0x376), 0x0F)` on a known tick.

## Step 3 — Capture the port-side trace

```bash
cd /c/Users/maria/Desktop/Proyectos/TD5RE/.claude/worktrees/precise-00403D90
./td5re.exe
# drive the same test race; the trace appears at log/port/pool6_00403D90.csv
```

Match the original capture window exactly.

## Step 4 — Diff

```bash
cd /c/Users/maria/Desktop/Proyectos/TD5RE
python tools/diff_func_trace.py \
    .claude/worktrees/precise-00403D90/log/port/pool6_00403D90.csv \
    log/orig/pool6_00403D90.csv \
    --key=sim_tick,slot --min-tick=5 --max-tick=60
```

Reports per-column divergence count + first-divergence tick + first-row column dump.

## Step 5 — Iterate

Expected first divergences after the pool6 fix (all D1-D7 from audit should resolve):

1. **out_angvel_roll** — the gate inversion was the headline bug; expect this to converge now.
2. **out_slip_acc_x / out_slip_acc_z** — D6: tire-slip accum should now match (sources +0x318/+0x314).
3. **Trig table mismatches** — port uses `math.h sin/cos`, original uses LUT @ DAT_00483984. May show ±1 LSB drift on `out_angvel_roll` under high-magnitude proj. NOT a pool6-specific bug; gated by pool3 (trig LUT) pilot.

For each fix:
1. Edit `td5_physics.c` **in the worktree only**.
2. Rebuild via `./build_standalone.bat`.
3. Re-run port capture (Step 3).
4. Re-diff (Step 4).
5. Stop when the diff prints `*** ZERO DIVERGENCE ***`.

## Step 6 — Merge & release

NO merge per session instructions. Commit on the branch only:

```bash
cd .claude/worktrees/precise-00403D90
git add td5mod/src/td5re/
git add ../../../tools/frida_pool6_00403D90.js
git add ../../../re/analysis/pilot_00403D90_audit.md
git add ../../../re/analysis/pilot_00403D90_runbook.md
git commit -m "precise-port 0x00403D90: invert gate polarity + RZ rounding + slip accum source"
```

## Pool slot release

After the pilot completes (or is paused for runtime capture):

```bash
bash scripts/ghidra_pool.sh cleanup
```

## What the static fix changes (summary of D1-D7 resolutions)

| ID | Status | Change |
|---|---|---|
| D1 | FIXED | Gate condition inverted: was `abs_v < 0x21 && abs_r < 0x7F`, now `(sVar2 > 0x20 && roll12 < 0x80) OR (sVar2 < -0x20 && roll12 > -0x80)` matching the listing's two-branch `JLE/JGE` tree. |
| D2 | FIXED | Asymmetric roll gate per sign of sVar2, was symmetric. |
| D3 | FIXED | Yaw source now `display_angles.yaw` (+0x20A int16) NEG'd, was `(euler_accum.yaw>>8) & 0xFFF` directly. |
| D4 | FIXED | `state0f_sar_rz` helper for round-to-zero divides at proj/4096, sVar2/4, av_roll/16, av_pitch/16. |
| D5 | FIXED | int16 truncation of `vx>>8`, `vz>>8` BEFORE IMUL with cos/sin (matches MOVSX CX, AX in listing). |
| D6 | FIXED | Tire-slip accumulator reads `actor->lateral_speed` / `actor->longitudinal_speed` directly, NOT recomputed from heading. |
| D7 | FIXED | Roll source `display_angles.roll` (+0x208 int16) folded via `((uint16-0x800) & 0xfff) - 0x800`. |

## Measurement — cost-per-function

Record when runtime diff lands:
- Wall time from "pilot started" → "zero diff merged"
- Number of iteration cycles (Step 5 loops)
- Lines of port code changed in `td5_physics.c` (~50 net lines)
- Surprises (any divergences not predicted by audit)

## Cleanup checklist if abandoning the pilot

```bash
# Release pool slot (idempotent)
bash scripts/ghidra_pool.sh cleanup

# Remove worktree (does NOT delete the branch)
git worktree remove .claude/worktrees/precise-00403D90
git branch -d precise-00403D90

# The pilot artifacts (audit, runbook, probe, diff tool) stay — reusable
# template for the next function in the precise-port queue.
```
