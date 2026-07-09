# Pilot Runbook — 0x004370A0 AdvanceActorTrackScript

This is the operator-side procedure for running the pilot capture + diff cycle
once a port + harness are committed. The static port + harness work is done in
`pilot_004370A0_audit.md`; what remains needs the running game.

## Pre-flight

| Item | Status | Path |
|---|---|---|
| Worktree | DONE | `.claude/worktrees/precise-004370A0` (branch `precise-004370A0`) |
| Pool slot | DONE | `TD5_pool11` |
| Audit | DONE | `re/analysis/pilot_004370A0_audit.md` |
| Frida probe | DONE | `tools/frida_pool11_004370A0.js` |
| Port trace module | DONE | `td5_pilot_trace_004370A0.{c,h}` in worktree |
| Port hook wired | DONE | `td5_ai_advance_track_script` calls enter/exit at every return site |
| Worktree build | DONE | `1,744,478` bytes |

## Step 1 — Verify the worktree build artifact

```bash
ls -la .claude/worktrees/precise-004370A0/td5re.exe
# expected: file present, ~1.7 MB
```

## Step 2 — Capture the original-side trace

Serial step (one TD5_d3d.exe at a time):

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE
./original/TD5_d3d.exe  # via existing windowed launch recipe
# In a second terminal:
frida -p $(pgrep TD5_d3d.exe) -l tools/frida_pool11_004370A0.js
```

Drive a deterministic test race:
- **Edinburgh, slot 0, span 0, PlayerIsAI=1** (matches the 0x00434FE0 + 0x004340C0 chain baselines).
- ~30 sim_ticks of motion is plenty (script VM is per-tick when active).

Expected output: `log/orig/pool11_004370A0.csv` (one row per call; one call per tick when the script is active = ~30 rows).

## Step 3 — Capture the port-side trace

```bash
cd .claude/worktrees/precise-004370A0
./td5re.exe
# drive the same test race; trace lands at log/port/pool11_004370A0.csv
```

The port deterministic seed must reproduce the same input progression. Match
race length to original.

## Step 4 — Diff

The recommended key is `(sim_tick, slot)` because `call_idx` increments
independently between port and original. Within a tick the function fires once
per slot, so keying by (tick, slot) pairs rows cleanly.

```bash
python tools/diff_func_trace.py \
    .claude/worktrees/precise-004370A0/log/port/pool11_004370A0.csv \
    log/orig/pool11_004370A0.csv \
    --min-tick=5 --max-tick=30 \
    --key=sim_tick,slot \
    --ignore=rs_addr,script_base_ptr_in,script_ip_in,script_base_ptr_out,script_ip_out,route_table_ptr,call_idx
```

The `--ignore` list excludes the raw-pointer fields (port and original have
different address spaces; only the *_index columns are comparable).

## Step 5 — Iterate

Predicted diff outcomes for this pilot (given the audit):

1. `script_flags_out` may differ during transitions between flag-4 and aligned
   states because of the **D1** fix (flag-4 aligned test now using
   `hdelta_mirror`). This is expected; verify the new value matches the
   original where the previous port did not.
2. `script_ip_index_in/out` should match exactly when computed (port now stores
   index; original is converted by the Frida probe via
   `(ip_ptr - base_ptr) >> 2`).
3. `actor_steering_cmd_out` and `rs_left_deviation_out` /
   `rs_right_deviation_out` should match in aligned-finalize ticks since the
   port-side math is byte-faithful.
4. `actor_angular_velocity_yaw_out` should match zero only on case-0 terminate
   ticks (op<0 branch).

If any of these diverges, the cause is **upstream** of 0x004370A0 — most
likely:
- `actor_yaw_accum` carryover (drift in the bicycle solve from 0x00404EC0)
- `actor_long_speed` carryover (also from 0x00404EC0 or 0x00404030)
- `actor_steering_cmd` carryover (zero-on-tick reset hypothesized in
  pilot_00434FE0_audit.md; this is the steer_in==0 puzzle that pool9 left
  open)

Record the divergence in the audit's "Blocked by upstream" section and hand
off to the next pilot in the chain (UpdateAIVehicleDynamics 0x00404EC0).

## Step 6 — Merge & release (only when zero diff)

```bash
cd .claude/worktrees/precise-004370A0
git push origin precise-004370A0  # optional
# Merging is the parent agent's responsibility — DO NOT auto-merge from here.

# Release pool slot:
rm ghidra_pool/TD5_pool11.assigned
bash scripts/ghidra_pool.sh release 11
```

## Cleanup checklist if abandoning

```bash
rm ghidra_pool/TD5_pool11.assigned
bash scripts/ghidra_pool.sh release 11
git worktree remove .claude/worktrees/precise-004370A0
git branch -D precise-004370A0
```

## Reference

- Audit: `re/analysis/pilot_004370A0_audit.md`
- Listing: 0x004370A0..0x0043777E (Ghidra TD5_pool11, 2026-05-14)
- Port: `.claude/worktrees/precise-004370A0/td5mod/src/td5re/td5_ai.c:1541-` (post-fix)
- Frida probe: `tools/frida_pool11_004370A0.js`
- Port trace emitter: `td5_pilot_trace_004370A0.{c,h}` (worktree only)
- Tag: `pool11_004370A0`
