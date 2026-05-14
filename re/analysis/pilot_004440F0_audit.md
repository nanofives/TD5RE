# Pilot Audit — 0x004440F0 UpdateActorTrackPosition

**Date:** 2026-05-14
**Pool slot:** TD5_pool10
**Port-side function:** `td5_track_update_actor_position` @ `td5_track.c:2635`
                       (chassis walker) + `td5_track_update_probe_position`
                       @ `td5_track.c:2681` (per-wheel) both delegate to
                       static `update_position_recursive` @ `td5_track.c:2227`.
**Worktree:** `.claude/worktrees/precise-004440F0` on branch `precise-004440F0`
              (forked from `fpu-cw-fix`).
**Callers (7):** UpdateChaseCamera (0x00401590),
                 RefreshVehicleWheelContactFrames (0x00403720),
                 IntegrateVehiclePoseAndContacts (0x00405E80),
                 UpdateVehiclePoseFromPhysicsState (0x004063A0),
                 RenderVehicleActorModel (0x004092D0),
                 IntegrateScriptedVehicleMotion (0x00409D20),
                 UpdateTrafficVehiclePose (0x00443CF0).
**Callees:** none (pure compute leaf).
**Body:** 0x004440F0..0x0044541C (~4892 bytes / large switch).

## Function structure (from listing + decomp)

The original is a single-pass-per-call 4-edge cross-product boundary walker:

```
1. Load strip record at g_trackStripRecords + span_index * 0x18
   - byte[0]  = span_type
   - byte[3]  = lane_count (low nibble) | h_offset (high nibble)
   - ushort[2]/[3] = left/right vertex_index
   - byte+0xc / +0x14 = origin_x / origin_z
2. Translate probe-supplied world XZ into span-local: (world>>8) - origin
   ** Listing 0x00444164/0x0044416a uses plain SAR (no round-to-zero idiom). **
3. Compute per-edge mask (s_edge_mask_first/last LUTs) based on sub_lane position
4. Test each of 4 edges (cross-product sign > 0 = "outside")
5. Switch on combined 4-bit result (bVar9):
   - 0          : within quad, return
   - 1 (FWD)    : secondary retest on prev-lane forward edge → advance or stay
   - 2 (RIGHT)  : forward step + retest on new span's edges
   - 3 (F+R)    : compound — forward step with two-cross retest
   - 4 (BACK)   : symmetric to case 1, backward
   - 6 (B+R)    : forward step with back-edge retests
   - 8 (LEFT)   : backward step + retest
   - 9 (F+L)    : compound — forward step with left-side retest
   - 12 (B+L)   : compound — backward step with left-side retest

Each case writes back: span_idx, sub_lane (post-step ±1), accum±1, hi=max.
```

## Confirmed divergence points (port vs original — static audit)

### D1 — Plain SAR — MATCHES ORIGINAL (no fix needed)
Listing 0x00444164 and 0x0044416a both use `SAR EAX, 0x8` directly with NO
prior CDQ/AND/ADD round-to-zero idiom. The port's `pos_x` / `pos_z` are
passed unmodified (already 24.8 FP) and the walker subtracts `origin_x<<8`
which is algebraically equivalent. **No port edit required for SAR.**

(Contrast with pool5/0x00403A20 which DID use the round-to-zero idiom on
multiple SAR-by-256 operations.)

### D2 — Case 1 (single FORWARD bit) — POTENTIAL DIVERGENCE
Original decomp at `case 1:` does NOT simply advance span+1. It performs a
SECONDARY cross-product test on the previous-lane's forward edge
(`psVar2[-3]` reads vertex 3 short-words before psVar2, i.e. previous lane's
left vertex). If that test passes, advance span+1 and decrement sub_lane.
Otherwise it tries a different transition (psVar5[-3] read for left edge of
next lane).

Port `case 1:` at `td5_track.c:2259` just calls `resolve_neighbor(0x01)` and
applies `sub_lane -= 1`. **No secondary retest.**

This may matter at:
- Lane-boundary onsets where the actor is barely inside the FWD edge but
  the previous-lane geometry also wants to test forward.
- The original's secondary test is what selects between (span+1, sub_lane-1)
  and (link_next, sub_lane+Δ) transitions in some junction-adjacent cases.

### D3 — Case 4 (single BACKWARD bit) — POTENTIAL DIVERGENCE
Symmetric to D2: original case 4 performs secondary cross on `psVar5[+3]`
(next-lane forward edge backward orientation). Port case 4 just advances
backward.

### D4 — `s_loaded_tuning` / s_jump_entries fallback (port-only safety net)
Port lines 2127-2163: extensive "branch-return safety net" that fires when
new_span goes out of bounds. Original has no such safety net — it walks
through links naturally via type-8/9/10/11 dispatch. The port-only logic
adds branches and may produce different end-of-branch transitions.

### D5 — TRACK_MAX_RECURSION loop + saved-state rollback (port-only)
Original is single-pass-per-call — every case path returns immediately. The
port wraps `compute_boundary_bits` + switch in a `for (iter = 0; iter <
TRACK_MAX_RECURSION; iter++)` loop with optional `single_step` exit. The
chassis and per-wheel wrappers BOTH pass `single_step=1`, which means the
loop exits after one iteration, matching the original. Plus a saved-state
memcpy rollback if iter overflows — never reached when `single_step=1`.
**Effectively a no-op at runtime given current `single_step=1`** but adds
~32 bytes of code on every call.

### D6 — Camera-probe stub at `td5_track.c:4911` — APIfaithful, no real walk
The port's `UpdateActorTrackPosition` (camera variant) is a stub that
clamps span_idx and sub_lane to valid range. The original 0x004440F0 is
called from `UpdateChaseCamera` 0x00401590 with a stack-local probe and a
camera-supplied world XZ. The port's camera doesn't actually walk; it
relies on the chassis walker's last result. Not in the pilot's critical
path but a divergence.

### D7 — `s_jump_entries` heuristic branch-return remap (port-only)
Lines 2127-2161: scans `s_jump_entries` for entries matching
`branch_lo <= span_idx <= branch_hi`, remaps `new_span` to
`main_target ± offset`. Original has no such heuristic — it follows
`link_next` / `link_prev` directly. The port-only heuristic was added to
handle out-of-bounds branch returns; if upstream binds correctly it should
be a no-op.

## BLOCKED BY UPSTREAM — runtime row-by-row diff cannot be performed

A Frida-captured run on Edinburgh slot 0 PlayerIsAI=1 with the pilot probe
yielded `log/orig/pool10_004440F0.csv` with 1521 rows / sim_tick 0..30
(working capture).

The matching port run instrumented `td5_track_update_actor_position` with
a static debug log at the top of the function — **before the `s_span_array
== NULL` early-bail guard** — and captured:

```
entry=1    actor=001317A0  s_span_array=00000000  s_span_count=0
entry=2    actor=00131B28  s_span_array=00000000  s_span_count=0
entry=3    actor=00131EB0  s_span_array=00000000  s_span_count=0
entry=4    actor=00132238  s_span_array=00000000  s_span_count=0
entry=256  actor=00132238  s_span_array=00000000  s_span_count=0
entry=512  actor=00133058  s_span_array=00000000  s_span_count=0
... (every entry through tick 30: s_span_array=NULL)
```

The port's chassis walker is invoked every tick but **bails on the
`s_span_array == NULL` early-return on every single call**. The STRIP.DAT
track data is NEVER loaded in the port's `AutoRace=1` + `PlayerIsAI=1` +
`DefaultTrack=1` (Edinburgh) launch path.

Concurrently `race_trace_pose.csv` shows the actor's `world_x` stuck at 0
for the full 30 sim_ticks, while `world_z` ticks up via the velocity
integrator. This is the same upstream blocker documented by pool5's
00403A20 audit (carparam + spawn-bind chain failure in AutoRace path).

```
LoadVehicleAssets        → td5_physics_load_carparam(slot, …)   ← FAILS
                         → s_carparam_loaded[slot] = 1
TD5_StartRace            → SetupPlayerVehicleState
                         → actor->world_pos = Edinburgh span 62 origin   ← FAILS
LoadTrackRuntimeData     → STRIP.DAT parse + s_span_array binding         ← FAILS
0x004440F0 walker        ← THIS pilot — blocked, bails on NULL span array
```

Until the **track load chain** (LoadTrackRuntimeData → s_span_array binding)
fires for AutoRace launches, no per-tick CSV diff will reflect 0x004440F0
quality.

## Static-port deliverable (this branch)

Even without a clean runtime diff:

- Frida probe `tools/frida_pool10_004440F0.js` captures 28 columns per
  call (sim_tick, slot, call_idx, caller tag, probe/wpos offsets, full
  ProbeSnap before+after, retval). Slot-resolution by probe-pointer offset
  within the actor block (offset 0x00..0x8F covers body+wheel+chassis
  probes; camera/traffic stack-locals are filtered).
- Port-side emitter `td5_pilot_trace_004440F0.{c,h}` mirrors the schema.
  Wired at the chassis (`td5_track_update_actor_position`) and per-wheel
  (`td5_track_update_probe_position`) wrappers, capturing entry+exit.
- Original capture committed at `log/orig/pool10_004440F0.csv` (1521 rows
  across 30 sim_ticks, 7 callsites observed via `caller_tag` column).
- Static audit of port `update_position_recursive` vs listing/decomp
  identifies D2/D3/D4/D5/D7 as candidate divergences for future work.

## Static algorithmic-byte-exact validation — possible but not yet wired

A Python reimplementation of the port's `update_position_recursive` could
be fed the original-captured inputs (`log/orig/pool10_004440F0.csv`
`*_in` columns + Edinburgh's `s_span_array` / `s_vertex_table`) and assert
output columns match. This is the same pattern as pool5's
`tools/validate_pool5_00403A20_math.py` and would let us validate the
walker algorithmically without needing the port runtime.

**Deferred:** requires extracting Edinburgh's STRIP.DAT span / vertex /
jump tables into a Python-readable form, plus reimplementing all 7 case
handlers + `compute_boundary_bits` + `resolve_neighbor` in Python — ~400
lines. If/when upstream binds correctly, runtime diff will subsume this.

## What NOT to do

- **Do not "fix" port case 1/4 by adding a secondary cross test** until
  the runtime trace confirms the divergence affects the captured ticks.
  The original's secondary test may fire only in geometry-edge cases
  (lane transitions, junction approaches) that the Edinburgh start
  doesn't exercise.
- **Do not remove the TRACK_MAX_RECURSION loop / saved-state rollback**
  without a runtime check — it's gated by `single_step=1` so it's
  effectively dead code at runtime, and a defence-in-depth measure for
  configurations that allow multi-step walks.
- **Do not touch the `s_jump_entries` safety net** until upstream
  branch-link binding is verified. Removing it may break wrap-around at
  the Edinburgh main → branch transitions.

## Reference

- Listing: 0x004440F0..0x0044541C (Ghidra TD5_pool10, 2026-05-14)
- Decompilation: same session, ~250 lines
- Port walker: `td5mod/src/td5re/td5_track.c:1845-2625` (compute_boundary_bits,
  resolve_neighbor, update_position_recursive)
- Frida probe: `tools/frida_pool10_004440F0.js`
- Port trace emitter: `td5mod/src/td5re/td5_pilot_trace_004440F0.{c,h}` (worktree)
- Orig CSV: `log/orig/pool10_004440F0.csv` (1521 rows / 30 sim_ticks)
- Port CSV: blocked — `log/port/pool10_004440F0.csv` never created because
  walker bails on `s_span_array==NULL` (upstream track load not run).
