# Pilot Runbook — 0x00403720 RefreshVehicleWheelContactFrames

This is the operator-side procedure for running the pilot capture + diff cycle. The static port + harness work is already done; what's left needs the running game.

## Pre-flight

| Item | Status | Path |
|---|---|---|
| Worktree | ✅ | `.claude/worktrees/precise-00403720` (branch `precise-00403720`) |
| Pool slot | ✅ | `TD5_pool1` (pool0 had stuck channel lock) |
| Audit | ✅ | `re/analysis/pilot_00403720_audit.md` (7 divergences identified, D1 is the big one) |
| Frida probe | ✅ | `tools/frida_pool1_00403720.js` |
| Port trace module | ✅ | `td5_pilot_trace.{c,h}` in worktree |
| Port hook wired | ✅ | `td5_physics_refresh_wheel_contacts` calls enter/leave |
| Diff tool | ✅ | `tools/diff_func_trace.py` |
| Worktree build | 🚧 | In progress at session-end; verify with `cd .claude/worktrees/precise-00403720/td5mod/src/td5re && ./build_standalone.bat` |

## Step 1 — Verify the worktree build succeeded

```bash
cd /c/Users/maria/Desktop/Proyectos/TD5RE
ls -la .claude/worktrees/precise-00403720/td5re.exe
# expected: file exists, size > 1 MB
```

If the build failed, the most likely cause is the new `td5_pilot_trace.c` failing to find an offset. Check `build_standalone.log` and apply minimal fixes in the worktree (do NOT touch master).

## Step 2 — Capture the original-side trace

This is the **serial** part of the workflow — only one TD5_d3d.exe can run at a time. Reuse the existing `/diff-race` harness or attach Frida directly:

```bash
# In a terminal:
cd /c/Users/maria/Desktop/Proyectos/TD5RE
# Start the original (windowed via existing recipe)
./original/TD5_d3d.exe  # or use scripts/launch_original_windowed.sh

# In a second terminal — attach Frida with the pilot probe:
frida -p $(pgrep TD5_d3d.exe) -l tools/frida_pool1_00403720.js
```

Drive (or auto-drive) a deterministic test race:

- **Edinburgh, slot 0, span 0** (matches the existing audit baseline — see `reference_moscow_span175_degenerate.md` for why we avoid Moscow).
- ~60 sim_ticks of motion is plenty for the pilot; if the audit window is short, capture only ticks 0–30.

Expected output: `log/orig/pool1_00403720.csv` with ~240 rows (60 ticks × 2 callers × 4 wheels = 480 max).

## Step 3 — Capture the port-side trace

```bash
cd /c/Users/maria/Desktop/Proyectos/TD5RE/.claude/worktrees/precise-00403720
./td5re.exe
# drive the same test race; the trace appears at log/port/pool1_00403720.csv
```

Match the original capture window: same track, same starting span, same input sequence. The port's deterministic seed should produce identical sim_tick progression.

## Step 4 — Diff

```bash
cd /c/Users/maria/Desktop/Proyectos/TD5RE
python tools/diff_func_trace.py \
    .claude/worktrees/precise-00403720/log/port/pool1_00403720.csv \
    log/orig/pool1_00403720.csv \
    --max-tick=30
```

Reports per-column divergence count + first-diverge tick + first-row detailed dump.

## Step 5 — Iterate

Expected first divergences (predicted by audit):

1. **probe_w_x/y/z** (D1) — port has these aliased to wheel_contact_pos; original transforms from car_def[0..15]. **Fix:** implement the missing second loop in `td5_physics_refresh_wheel_contacts`.
2. **gap270_x/y/z** (D2) — port clamps at ±20000 fp8; original does not. **Fix:** remove the `CLAMP_DELTA` macro lines and use the raw delta.
3. **wcv_x/y/z** (D6) — port's `td5_isqrt` normalize vs original's `FUN_0042CD40`. **Fix:** port 0x0042CD40 byte-faithfully OR call into `ComputeActorTrackContactNormalExtended` instead of reimplementing.
4. Subtle FP/FISTP drift — pending audit of MinGW's `lrintf` vs MSVC's FISTP rounding mode under the original's FPU control word.

For each fix:

1. Edit `td5_physics.c` **in the worktree only**.
2. Rebuild via `./build_standalone.bat`.
3. Re-run port capture (Step 3).
4. Re-diff (Step 4).
5. Stop when the diff prints `*** ZERO DIVERGENCE ***`.

## Step 6 — Merge & release

Zero diff → commit to `precise-00403720` branch → merge to master → release the pool slot:

```bash
cd .claude/worktrees/precise-00403720
git add td5mod/src/td5re/
git commit -m "precise-port 0x00403720: byte-exact RefreshVehicleWheelContactFrames"
# back in main tree
git merge precise-00403720
git worktree remove .claude/worktrees/precise-00403720
rm ghidra_pool/TD5_pool1.assigned
bash scripts/ghidra_pool.sh release 1
```

## Measurement — cost-per-function

Record:
- **Wall time** from "pilot started" → "zero diff merged"
- **Number of iteration cycles** (Step 5 loops)
- **Lines of port code changed** in `td5_physics.c`
- **Surprises** — divergences not predicted by the audit

If wall-time < 8 hours: scale immediately to batch B (collision: 7 functions).
If 8–16 hours: reassess scope, possibly narrow harness coverage.
If > 16 hours: investigate the throughput bottleneck before continuing.

## Cleanup checklist if abandoning the pilot

```bash
# Release pool slot
rm ghidra_pool/TD5_pool1.assigned
bash scripts/ghidra_pool.sh release 1

# Remove worktree (does NOT delete the branch)
git worktree remove .claude/worktrees/precise-00403720
git branch -d precise-00403720

# The pilot artifacts (audit, runbook, probe, diff tool) stay — they're the
# reusable template for the next function in the precise-port queue.
```
