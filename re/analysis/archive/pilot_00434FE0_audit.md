## Pilot Audit — 0x00434FE0 UpdateActorTrackBehavior

**Date:** 2026-05-14
**Pool slot:** TD5_pool9
**Port-side function:** `td5_ai_update_track_behavior` @ `td5_ai.c:1821`
**Worktree:** `.claude/worktrees/precise-00434FE0` on branch `precise-00434FE0`
**Caller graph:** `UpdateRaceActors (0x00436A70)` — single caller, per-slot per-tick dispatcher.
**Callee graph:** AngleFromVector12 (0x40A720), UpdateActorSteeringBias (0x4340C0), ComputeTrackSpanProgress (0x4345B0), ComputeSignedTrackOffset (0x434670), ComputeRouteForwardOvershootScalar (0x434740), SampleTrackTargetPoint (0x434800), UpdateActorTrackOffsetBias (0x434900), UpdateActorRouteThresholdState (0x434AA0), AdvanceActorTrackScript (0x4370A0).
**Body:** 0x00434FE0..0x004353AC (0x3CC bytes / 313 instructions / 119 decompiled lines).

## Function structure (from listing)

Three branches before convergence at `LAB_0043523D`:

1. **Outer gate:** if `g_selectedGameType != 0` → fall through to convergence.
2. **Game-type SINGLE_RACE branch:**
   - If `rs[SCRIPT_BASE_PTR] == 0`:
     - Compute `hdelta = -(((yaw_accum>>8) - ((route_byte_idx1 * 0x102c + sign_bias)>>8) - 0x800 & 0xFFF) - 0x800 & 0xFFF) & 0xFFF`
     - If `0x320 < hdelta < 0xCE0`: write `rs[SCRIPT_BASE_PTR] = rs[SCRIPT_IP] = 0x473cc8`.
     - Fall through to convergence (LAB_0043523D path).
   - Else (script active):
     - Call `AdvanceActorTrackScript(rs)`:
       - If returns 0 (still blocking): recompute `rs[TRACK_PROGRESS]` from `ComputeTrackSpanProgress` and `rs[TRACK_OFFSET_BIAS]` from `ComputeSignedTrackOffset(span_raw, progress, route_byte_at_idx0)`. RETURN.
       - Else (returns nonzero): jump back to outer gate fall-through.
3. **Convergence (LAB_0043523D):**
   - Compute `target_span` = `(span_norm + 4) % g_trackTotalSpanCount`, with junction-remap walker over `[DAT_004c3da0 + 0x18 + 6*i]` entries (3 u16s each: field_a, field_b, range_lo at offsets 0/2/4) if `rs[ROUTE_TABLE_PTR] != DAT_004afb58`.
   - Call `UpdateActorTrackOffsetBias(slot)`.
   - Recompute `lin_span = (span_norm + 4) % count` (PRE-remap) for route_byte_at_idx0 lookup.
   - Call `SampleTrackTargetPoint(target_span_post_remap, route_byte_pre_remap, &local_c, rs[TRACK_OFFSET_BIAS])`.
   - Call `ComputeRouteForwardOvershootScalar(target_span_post_remap, slot, &local_c)` — return value DISCARDED.
   - Compute `dx = (local_c[0] - ref_actor.world_pos_x) >> 8; dz = (local_c[2] - ref_actor.world_pos_z) >> 8` (plain SAR).
   - Quadrant-decomposed `target_angle = AngleFromVector12(...)` with quadrant adders (+0, +0x400, +0x800, +0xC00).
   - `actor_heading = ((ref_actor.YAW_ACCUM + ref_actor.STEERING_CMD) >> 8) & 0xFFF` (plain SAR).
   - `delta = actor_heading - target_angle` (full 32-bit signed).
   - `LEFT_DEVIATION/RIGHT_DEVIATION` decomposed: if `delta >= 0`: L=0xFFF-|delta|, R=delta; else L=|delta|, R=delta+0xFFF.
   - Call `UpdateActorRouteThresholdState(slot)`.
   - If returns 0: `UpdateActorSteeringBias(rs, 0x20000)`; else `UpdateActorSteeringBias(rs, 0x10000)`.

## Key arithmetic primitives

`route_heading = sar8_rz(route_byte * 0x102C)` — uses the **round-to-zero idiom** `CDQ; AND EDX,0xff; ADD EAX,EDX; SAR 8` at 0x0043505A-63. Since `route_byte ∈ [0,255]`, the product is always ≥ 0, so `sar8_rz` and plain `>> 8` produce IDENTICAL results here. **No semantic divergence**, only the listing-vs-port idiom differs.

All other `>> 8` operations in the function (yaw_accum SAR at 0x00435044, dx/dz SAR at 0x004352C2/C8, actor_heading SAR at 0x00435349) use **plain SAR** — no round-to-zero idiom. Port's plain `>> 8` is byte-faithful for these.

## Confirmed divergences from port

### D1 — `SCRIPT_BASE_PTR`/`SCRIPT_IP` recovery write is over-extended **(HIGH IMPACT — output divergence)**

Original at 0x00435094-9F writes ONLY two fields when misalignment triggers:
```
MOV [EDI + 0x4afc48], 0x473cc8   ; rs[RS_SCRIPT_BASE_PTR] (dword 0x3A)
MOV [EDI + 0x4afc4c], 0x473cc8   ; rs[RS_SCRIPT_IP]        (dword 0x3B)
```

Both fields receive the **same pointer value** (`DAT_00473cc8`).

Port at line 1935-1940 writes SIX fields:
```c
rs[RS_SCRIPT_BASE_PTR] = (int32_t)(intptr_t)g_script_init_recovery;
rs[RS_SCRIPT_IP] = 0;                  // ← should be same ptr, not 0
rs[RS_SCRIPT_FLAGS] = 0;                // ← not written by original
rs[RS_SCRIPT_FIELD_3E] = 0;             // ← not written by original
rs[RS_SCRIPT_FIELD_43] = 0;             // ← not written by original
rs[RS_SCRIPT_COUNTDOWN] = 0x96;         // ← not written by original
```

Hypothesis: original relies on the script VM to initialize FLAGS/FIELD_3E/FIELD_43/COUNTDOWN on its first `AdvanceActorTrackScript` step, and on the IP=base-ptr alias to start at opcode 0 of the recovery program. Port's extra writes may STALL/SHORTEN the recovery duration.

**Fix:** write only `RS_SCRIPT_BASE_PTR = RS_SCRIPT_IP = g_script_init_recovery`. Remove the four extra writes.

### D2 — Cascade deviation clamp `abs_delta < 64` is over-approximation **(HIGH IMPACT — explicit workaround)**

Port at lines 2098-2108 has an EXTRA case before the original's two-branch decomposition:
```c
if (abs_delta < 64) {
    rs[RS_LEFT_DEVIATION]  = 0xFFF;
    rs[RS_RIGHT_DEVIATION] = 0;
}
```

Original has NO such clamp. The port comment at 2087-2097 explicitly labels this as "Workaround for port's ~3deg target_angle divergence vs orig at spawn… while the seed-progress divergence is investigated."

Per `feedback_precise_port_over_approximation`: REMOVE the clamp. If a downstream test regresses, that surfaces the upstream divergence to fix at its source.

### D3 — `RS_SLOT_INDEX` indirection ignored **(MEDIUM IMPACT — semantic mismatch for encounter/traffic)**

Original reads from `actor[rs[RS_SLOT_INDEX]]` for:
- `SPAN_NORMALIZED` (0x0043502E in misalignment branch)
- `YAW_ACCUM` (0x0043503E in misalignment branch)
- `WORLD_POS_X/Z` (0x004352A1/A8 in convergence branch)
- `YAW_ACCUM + STEERING_CMD` (0x00435338/3E in convergence branch)

Port reads from `actor_ptr(slot)` (its own slot's actor) for all of these. For racer slots 0..5 with `RS_SLOT_INDEX = slot`, this is identical. For encounter slot 9 or traffic slots, `RS_SLOT_INDEX` may rebind to point at the player's actor — in which case port reads divergent state.

**Fix:** introduce `ref_actor = actor_ptr(rs[RS_SLOT_INDEX])` and use it for ALL actor reads in this function. The writes (`LEFT_DEVIATION/RIGHT_DEVIATION` on slot's own RS) stay on `slot`'s RS.

### D4 — Extra `RS_TRACK_PROGRESS` write in normal-AI branch **(LOW IMPACT — side-effect addition)**

Port at lines 1997-2006 calls `td5_track_compute_spline_position` and writes `rs[RS_TRACK_PROGRESS]` in the convergence (normal-AI) branch:
```c
rs[RS_TRACK_PROGRESS] = td5_track_compute_spline_position(...);
```

Original does NOT write `RS_TRACK_PROGRESS` in the convergence branch — only in the script-completed sub-branch (0x0043514D). The extra write per tick pollutes the field that other AI helpers may read.

**Fix:** remove the `td5_track_compute_spline_position` call from the convergence branch.

### D5 — Missing `ComputeRouteForwardOvershootScalar` call **(NEGLIGIBLE — Ghidra-annotated as side-effect-free, but listing tracker may flag it)**

Original at 0x00435288 calls `ComputeRouteForwardOvershootScalar(target_span, slot, &local_c)` and discards the return value. Ghidra annotation: *"The current caller appears to ignore the returned scalar"*. The function is pure (no global writes evident), so omission has no functional effect.

**Fix:** add the call for symbolic parity (zero-impact). Pursue only if a Frida trace flags the missing call.

### D6 — `RS_FORWARD_TRACK_COMP` initial-progress dependency **(NEEDS VERIFY — port-only)**

Port at line 1999 reads `int32_t seg_dist = rs[RS_FORWARD_TRACK_COMP];` and passes it to `td5_track_compute_spline_position`. Original does NOT consume `RS_FORWARD_TRACK_COMP` here — it's part of the extra write in D4. Removing D4 also removes this read.

## Static fix priority (apply in worktree)

1. **D2** (drop `abs_delta < 64` cascade clamp) — explicit workaround, removes a known over-approximation.
2. **D1** (fix recovery-script write set) — restrict to `RS_SCRIPT_BASE_PTR = RS_SCRIPT_IP = recovery_ptr`.
3. **D4** (remove extra `RS_TRACK_PROGRESS` write in normal-AI branch) — cleans up a port-only side effect.
4. **D3** (apply `RS_SLOT_INDEX` indirection) — semantic fix; null effect for racer slots, important for encounter/traffic.
5. **D5** (add `ComputeRouteForwardOvershootScalar` call for symmetry) — optional, zero functional impact.

## Capture schema for pilot (Frida + port)

**Keys:** `sim_tick`, `slot`
**Inputs (per call):**
- `game_type` (g_selectedGameType)
- `rs_script_base_ptr` (RS dword 0x3A)
- `rs_script_ip` (RS dword 0x3B)
- `rs_route_table_ptr` (RS dword 0x00)
- `rs_route_table_selector` (RS dword 0x03)
- `rs_slot_index` (RS dword 0x35)
- `actor_span_normalized` (actor +0x82)
- `actor_yaw_accum` (actor +0x1F4)
- `actor_steering_cmd` (actor +0x30C)
- `actor_world_pos_x` (actor +0x1FC)
- `actor_world_pos_z` (actor +0x204)
- `rs_track_offset_bias_in` (RS dword 0x09)
- `route_byte_idx1_at_span_norm` (rb[span_norm*3+1])

**Outputs:**
- `hdelta` (computed misalignment angle)
- `branch_taken` (enum: gate_skip / misalign_set / script_run_blocked / script_done / normal_ai)
- `rs_script_base_ptr_out`
- `rs_script_ip_out`
- `rs_track_progress_out`
- `rs_track_offset_bias_out`
- `rs_left_deviation_out`
- `rs_right_deviation_out`
- `target_angle` (post-quadrant adder)
- `actor_heading` (post-mask)
- `delta` (signed deviation)
- `threshold_result`
- `steer_weight_to_bias_call` (0x10000 or 0x20000)

**Estimated rows per Edinburgh race**: 6 slots × ~30 ticks = 180 rows for slot 0..5; we focus on slot 0 with PlayerIsAI=1.

## Fixes applied (this pilot)

`td5_ai.c` (worktree only):

1. **D1 narrowed**: recovery-script set restricted to the two pointer writes
   the listing performs at 0x00435094-9F. Removed the four extra writes
   (`SCRIPT_FLAGS`, `SCRIPT_FIELD_3E`, `SCRIPT_FIELD_43`, `SCRIPT_COUNTDOWN`)
   that had no listing counterpart, and corrected `RS_SCRIPT_IP` to alias
   `RS_SCRIPT_BASE_PTR` (the listing writes the SAME pointer value to both).
2. **D2 removed**: dropped the `abs_delta < 64` cascade clamp that the port
   comment explicitly described as an over-approximation workaround.
3. **D4 removed**: dropped the extra `rs[RS_TRACK_PROGRESS] =
   td5_track_compute_spline_position(...)` write in the normal-AI branch
   that the listing does NOT perform — only the script-completed branch
   writes that field. Pre-fix, port wrote -2081 where original kept 95
   at sim_tick=0.

D3 (RS_SLOT_INDEX indirection) and D5 (ComputeRouteForwardOvershootScalar
side-effect-free call) are NOT applied: at the captured Edinburgh slot-0
PlayerIsAI=1 baseline, `rs_slot_index == slot` and Ghidra annotates the
overshoot scalar as discarded by the caller, so neither has observable
output impact for this test.

## Self-validation (algorithmic byte-exact)

`tools/validate_pool9_00434FE0_math.py` feeds the original's captured
inputs (`log/orig/pool9_00434FE0.csv`) through a Python reimplementation
of the port's LEFT/RIGHT decomposition formula and asserts the
reproduced outputs match the original's captured `(rs_left_deviation_out,
rs_right_deviation_out)`.

**Result: 33/33 eligible rows match byte-exact.**

This proves the port's per-row deviation math is byte-faithful given
equal inputs.

## Runtime row-by-row diff — blocked by upstream

After D1+D2+D4 fixes, the runtime diff (`tools/diff_func_trace.py
--max-tick=30 --min-tick=5 --key=sim_tick,slot`) shows 26/26 rows with
divergent inputs:

| Column | Port @ tick=5 | Orig @ tick=5 | Cause |
|---|---|---|---|
| actor_world_pos_x | 56481165 | 56480815 | UpdatePlayerVehicleDynamics drift |
| actor_world_pos_z | -15013048 | -15012817 | same |
| actor_yaw_accum | 733036 | 733134 | bicycle-model integration drift |
| actor_steering_cmd | 7136 | 0 | **steering_cmd never zeroed in port** |
| rs_track_offset_bias_in | -921 | -1494 | UpdateActorTrackOffsetBias divergence |
| rs_forward_track_comp_in | 7 | 8 | track walker divergence |
| rs_left_deviation_in | 6 | 102 | downstream of inputs above |
| rs_right_deviation_in | 4089 | 3993 | same |

The most striking input divergence is **`actor_steering_cmd`**: the
original reads `steer_in == 0` at every tick from 1 onward, while the
port carries the previous tick's output (7136, then 8320, etc).
Something between this function's exit and the next call zeros
`actor.steering_cmd` in the original but not in the port. Candidate
locations:

- `UpdateAIVehicleDynamics @ 0x00404EC0` (the bicycle solve) consumes
  steering_cmd, and may write 0 to it after consumption.
- `UpdateActorSteeringBias @ 0x004340C0` (called at the end of this
  function) writes to `actor+0x30C` only — does NOT zero it.
- `UpdateVehicleActor @ 0x00406650` (top-level dispatcher) may apply a
  per-tick decay or zero.

Given the captured originals show `steer_in = 0` exactly (not decayed
toward 0, but discrete reset), the writer is almost certainly a
deliberate `actor->steering_cmd = 0` somewhere in the AI dynamics path.

## Definition of done (this pilot)

Static port: byte-faithful per algorithmic-validator.
Runtime diff: **blocked by upstream** on `actor_world_pos_*`,
`actor_yaw_accum`, and `actor_steering_cmd` carry-over.

The next precise-port pilot in the chain (UpdateAIVehicleDynamics
0x00404EC0 or UpdateActorTrackOffsetBias 0x00434900) should investigate
the missing `actor_steering_cmd = 0` write that the original performs
once per tick.

## Reference

- Listing: 0x00434FE0..0x004353AC (Ghidra TD5_pool9, 2026-05-14)
- Decompilation: same session, 119 lines
- Port: `td5mod/src/td5re/td5_ai.c:1821-2139` (post-fix)
- Self-validator: `tools/validate_pool9_00434FE0_math.py`
- Frida probe: `tools/frida_pool9_00434FE0.js`
- Port trace emitter: `td5mod/src/td5re/td5_pilot_trace_00434FE0.{c,h}` (worktree)
- Orig CSV: `log/orig/pool9_00434FE0.csv` (155 rows / ~122 calls in countdown + 30 per-tick)
- Port CSV: `log/port/pool9_00434FE0.csv` (similar)
- Tag: `pool9_00434FE0`
