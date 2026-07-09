# Pilot Audit — 0x00405E80 IntegrateVehiclePoseAndContacts

**Date:** 2026-05-14
**Pool slot:** TD5_pool2
**Port-side function:** `td5_physics_integrate_pose` @ `td5_physics.c:4553`
**Worktree:** `.claude/worktrees/precise-00405E80` on branch `precise-00405E80`
**Callers:** `ResetVehicleActorState (0x00405D70)`, `UpdateVehicleActor (0x00406650)`, `InitializeRaceVehicleRuntime (0x0042F140)`.
**Callees:**
- `BuildRotationMatrixFromAngles (0x0042E1E0)` — called TWICE per call (once before T2, once after)
- `UpdateActorTrackPosition (0x004440F0)`
- `ComputeActorHeadingFromTrackSegment (0x00445B90)`
- `RefreshVehicleWheelContactFrames (0x00403720)` — pilot pool1, already merged
- `TransformTrackVertexByMatrix (0x00446030)` — 4-wheel attitude full solver
- `TransformTrackVertexByMatrixB (0x00446140)` — roll-only variant
- `TransformTrackVertexByMatrixC (0x004461C0)` — pitch-only variant
- `LoadRenderRotationMatrix (0x0043DA80)`
- `ConvertFloatVec3ToShortAngles (0x0042E2E0)` — inside the per-wheel averaging loop
- `UpdateVehicleSuspensionResponse (0x004057F0)` — tail call, ALWAYS executed
**Body:** 0x00405E80 .. 0x00406362 (1250 bytes / 327 instructions / 145 decompiled lines).

## Function structure (from listing + decomp)

### Phase A — Pre-refresh integration (0x00405E80–0x00405FC9)

```
1. iVar4 = display_angles dword [+0x208]    ; OLD roll+yaw (pre-integration)
   iVar5 = display_angles.pitch  [+0x20C]   ; OLD pitch
   psVar10 = &display_angles.roll  (+0x208)

2. prev_frame_y_position [+0x2D8] = world_pos.y [+0x200]

3. linear_velocity_y [+0x1D0] -= gGravityConstant [0x00467380]

4. euler_accum.yaw   [+0x1F4] += angular_velocity_yaw   [+0x1C4]
   euler_accum.pitch [+0x1F8] += angular_velocity_pitch [+0x1C8]
   euler_accum.roll  [+0x1F0] += angular_velocity_roll  [+0x1C0]

5. world_pos.y += linear_velocity_y                     ; unconditional
   if (DAT_00483030 == 0) {
       world_pos.x [+0x1FC] += linear_velocity_x [+0x1CC]
       world_pos.z [+0x204] += linear_velocity_z [+0x1D4]
   }

6. display_angles.roll  = (uint)(euler_accum.roll  >> 8)   ; SAR EAX,8 — ARITHMETIC
   display_angles.yaw   = (uint)(euler_accum.yaw   >> 8)
   display_angles.pitch = (uint)(euler_accum.pitch >> 8)
   NO `& 0xFFF` mask — raw arithmetic shift, write as 16-bit truncation

7. render_pos.x = (float)world_pos.x * 1/256
   render_pos.y = (float)world_pos.y * 1/256
   render_pos.z = (float)world_pos.z * 1/256
   Implementation: FILD (S32 to F80) then FMUL by _DAT_0045d5e8 then FSTP F32

8. BuildRotationMatrixFromAngles(&rotation_matrix, &display_angles.roll)

9. UpdateActorTrackPosition(&track_span_raw [+0x80], &world_pos.x [+0x1FC])

10. ComputeActorHeadingFromTrackSegment(&track_span_raw,
                                         &world_pos.x,
                                         (undefined2*)&actor->field_0x290)

11. RefreshVehicleWheelContactFrames(actor)   ; updates wheel_contact_pos,
                                              ; wheel_contact_bitmask (+0x37C NEW),
                                              ; damage_lockout (+0x37D OLD)
```

### Phase B — T2 wheel-contact attitude block (0x00405FCA–0x004061B6)

```
12. switch (actor[+0x37C] & 0xFF):    ; NEW bitmask, not OLD!
      cases 0,1,2,4,6,8,9 → FULL solver (TransformTrackVertexByMatrix @ 0x00446030)
      cases 3, 12         → PITCH-only solver (...C @ 0x004461C0)
      cases 5, 10         → ROLL-only solver (...B @ 0x00446140)
      cases 7,11,13,14,15 → fallthrough (no T2 writeback)
      default (>=13)      → fallthrough

13. Solver arg signature: solver(&display_angles.roll, &wheel_contact_pos[0], &wheel_suspension_pos[0])
    Solver writes either or both of display_angles.roll (CASE B/full) and
    display_angles.pitch (CASE C/full).

14. STABILITY GATE: if ([+0x37C] == [+0x37D])  ; NEW == OLD (no wheel change this tick)
       d_roll  = wrap12((NEW_roll  - OLD_roll  - 0x800) & 0xFFF, -0x800) * 0x100
       d_pitch = wrap12((NEW_pitch - OLD_pitch - 0x800) & 0xFFF, -0x800) * 0x100
       angular_velocity_roll  = clamp(d_roll,  ±6000)
       angular_velocity_pitch = clamp(d_pitch, ±6000)
       NO clamp for yaw — yaw not touched in T2.
    (For B-only: only roll is written; for C-only: only pitch.)

15. After T2 block (regardless of gate hit):
       euler_accum.roll  = (int)display_angles.roll  << 8   ; FULL + B branch
       euler_accum.pitch = (int)display_angles.pitch << 8   ; FULL + C branch
    (Yaw left alone — not touched by T2.)

16. BuildRotationMatrixFromAngles(&rotation_matrix, &display_angles.roll)  ; SECOND CALL
    Note: ALWAYS runs (after JMP 0x004061B8 from any of the switch branches OR default)
```

### Phase C — Per-wheel ground-snap averaging (0x004061B9–0x004062E4)

```
17. cardef_0x82_arg = (cardef[+0x82] * 0xB5 + sign_byte) >> 8     ; SAR 8 round-toward-zero
    Implementation: LEA chain that computes EAX*45 (=0xB5 = 5*9*4+EAX),
    CDQ, AND EDX,0xFF, ADD EAX,EDX, SAR EAX,8
    Result stored in EBX (preload constant for inner loop)

18. LoadRenderRotationMatrix(&rotation_matrix)

19. local_18 = 0                  ; chassis Y accumulator (signed int)
    grounded_count = 0            ; signed int, "actor" reuse in decomp
    local_1c = &wheel_contact_pos[0].y   (+0xF4)
    local_20 = &wheel_suspension_pos[0]  (+0x2DC)
    puVar8  = &field_0x230   (per-wheel ConvertFloatVec3ToShortAngles output slot)
    psVar10 = &field_0x212   (per-wheel body_off.y stash slot)
    i = 0

    DO {
        IF ((+0x37C) & (1 << i)) == 0 {            ; wheel i is grounded
            preload_byte = (cardef_0x82 + sign>>0x1F & 0xff) >> 8 = EBX (already done)
            cwy = cardef[+0x40 + i*8 + 0x2]        ; signed short, stride 8 per wheel
            sp_div = (susp_pos[i] + sign_byte) >> 8 ; sar_rz_8 of susp_pos

            *psVar10 = (cwy - sp_div) - preload    ; write body_off.y (with preload subtracted)
                                                   ; psVar10[-1] is body_off.x stashed earlier
                                                   ; psVar10[+1] is body_off.z stashed earlier
            ConvertFloatVec3ToShortAngles(psVar10-1, puVar8)
                                                   ; pops 3 f32s from FPU stack and
                                                   ; writes 3 int16s to puVar8

            *psVar10 += preload                    ; ADD body_off.y back (restores preload)
            local_18 += wheel_contact_pos[i].y + puVar8[1] * -0x100
                                                   ; ADD: wheel_y, SUB: rotated_y * 256
            grounded_count++
        }
        local_20 += 4   ; next wheel_suspension_pos
        puVar8  += 8    ; next puVar8 slot
        i++
        local_1c += 12  ; next wheel_contact_pos.y
        psVar10 += 8    ; next body_off.y slot
    } WHILE (i < 4)

20. IF grounded_count <= 0:
        ++(uint16_t)[+0x360]    ; airborne_frame_counter++ — ONLY when fully airborne
    ELSE:
        new_y = local_18 / grounded_count        ; signed IDIV
        world_pos.y [+0x200] = new_y             ; UNCONDITIONAL absolute write
        IF DAT_0046318C[NEW_mask] != 0  AND
           (OLD_mask & (NEW_mask ^ 0xF)) == 0   :
            linear_velocity_y = new_y - prev_frame_y_position - gGravityConstant
                                                  ; velocity-from-snap gate
            UpdateVehicleSuspensionResponse(actor)
            return

21. UpdateVehicleSuspensionResponse(actor)        ; tail call ALWAYS reached
    return
```

## Critical references to validate

- **`puVar8` aliases the per-wheel angle output array at +0x230, stride 8 bytes** — same stride as `wheel_display_angles[i][0..3]`. The `puVar8[1]` read in step 19 grabs the **second short** (=ROTATED Y angle for that wheel slot).
- **`psVar10` per-wheel body-offset slot at +0x212, stride 8 bytes** — `psVar10[-1]` is +0x210 = body_off.x, `psVar10[+1]` is +0x214 = body_off.z. These X/Z components are NOT written by this inner loop — they must be pre-populated by an earlier transform call (suspect: pre-set by the solver phase or by RefreshVehicleWheelContactFrames; needs verification).
- **`local_1c = +0xF4`** is wheel_contact_pos[0].y, advancing 12 bytes per iteration — accumulates `wheel_contact_pos[i].y` for grounded wheels.
- **`DAT_0046318C`** is the 16-entry "snap-velocity-gate" table: `{01,01,01,00,01,00,00,00,01,00,00,00,00,00,00,00}` — indices 0,1,2,4,8 enabled. Already verified in port.

## Confirmed divergences from port (pre-fix)

### D1 — DISPLAY-ANGLE MASK: port adds spurious `& 0xFFF`  **(POTENTIAL HIGH IMPACT)**
Original (listing 0x00405F54 / 0x00405F66 / 0x00405F6F):
```
SAR EAX, 8     ; signed-arith shift
MOV [EBX], AX  ; write low 16 bits
```
There is **no mask operation** — the truncation to 16 bits happens at the MOV, so negative values stay sign-extended in the low 16 bits but high bits of euler_accum can leak in if any are set above bit 23 (this is the desired wrap-around behavior).

Port (td5_physics.c:4622-4624):
```c
actor->display_angles.roll  = (int16_t)((actor->euler_accum.roll >> 8) & 0xFFF);
actor->display_angles.yaw   = (int16_t)((actor->euler_accum.yaw >> 8) & 0xFFF);
actor->display_angles.pitch = (int16_t)((actor->euler_accum.pitch >> 8) & 0xFFF);
```

The `& 0xFFF` mask wraps to [0, 4095] for any euler_accum sign, while the original keeps the bit-21..15 range intact as a signed 16-bit value. For typical euler_accum magnitudes < 2^23 the values are equivalent in low 12 bits but **the sign bit (bit 15) is wrong** when the original would produce a negative display angle.

The downstream `cos_fixed12 / sin_fixed12` table uses index = `angle & 0xFFF` (verified in port via the precise-trig pilot), so this mask is harmless THERE. But the T2 delta computation at step 14 does:
```
((NEW_disp_roll - OLD_disp_roll - 0x800) & 0xFFF) - 0x800
```
With port's masked display_angles, OLD captured at step 1 is `(int)(int16_t)((int32_t >> 8) & 0xFFF)` whereas original captures the **raw 16-bit signed truncation**. For values straddling 0xFFF/0x800 boundaries (~ roll near ±π/2) the delta arithmetic produces different magnitudes.

**Fix:** drop the `& 0xFFF` mask — write raw `(int16_t)(euler_accum.* >> 8)`.

### D2 — RENDER POSITION uses `* 1/256` literal instead of FILD+FMUL `_DAT_0045d5e8`  **(LOW IMPACT, FPU PRECISION ONLY)**
Original (0x00405F48 / 0x00405F5A / etc.):
```
FILD dword [ESI+0x1FC]            ; S32 → F80
FMUL dword [0x0045d5e8]           ; F80 *= _DAT_0045d5e8 (1.0/256.0)
FSTP dword [ESI+0x144]            ; F32 store with current FPU CW rounding
```
The constant `_DAT_0045d5e8` is `1/256.0f` stored as float; the FMUL upcasts to float80 internally.

Port (td5_physics.c:4659-4661):
```c
actor->render_pos.x = (float)actor->world_pos.x * (1.0f / 256.0f);
actor->render_pos.y = (float)actor->world_pos.y * (1.0f / 256.0f);
actor->render_pos.z = (float)actor->world_pos.z * (1.0f / 256.0f);
```

`1.0f/256.0f = 0x3B800000` exactly (a power of 2 reciprocal), so this multiplication is **bit-exact** with the original under both 53-bit and 64-bit precision. Master has FPU CW RC=01 (toward zero); MinGW's compiler converts int→float using FILD-like semantics. Functionally identical → mark as NO-OP divergence.

### D3 — PHASE A INTEGRATION ORDER  **(LIKELY BENIGN — needs verification)**
Original integrates Y velocity into world_pos.y BEFORE updating display_angles, then computes render_pos from the post-integration world_pos. Port does the same order. **MATCH**.

Original also unconditionally adds `linear_velocity_x/z` to `world_pos.x/z` ONLY when `DAT_00483030 == 0`. Port has no such gate (just always adds). **POTENTIAL DIVERGENCE** — but DAT_00483030 has 1 ref_count and is never written (memory dump shows zeros), so effectively always-zero → port's unconditional path matches runtime behavior. Mark as NO-OP.

### D4 — T2 SOLVER STRUCTURE MISMATCH  **(MEDIUM IMPACT — needs runtime verification)**
Port re-implements the T2 attitude solver in `td5_physics_attitude_from_wheels` and dispatches based on `actor->wheel_contact_bitmask`, matching the original switch cases. **BUT**:

1. Port reads OLD display angles into `t2_old_disp_roll`/`t2_old_disp_pitch` at line 4562-4563. Original captures `iVar4 = display_angles dword`, `iVar5 = display_angles.pitch dword` at function entry too. **MATCH**.
2. Port uses `td5_physics_attitude_from_wheels(actor, &new_roll, &new_pitch)` for ALL 3 solver variants (full, B-only, C-only) — only differing in which output it writes back. Original calls **THREE DIFFERENT FUNCTIONS** (`TransformTrackVertexByMatrix` vs `B` vs `C`) which compute different formulas. Looking at decomp:
   - FULL solver (0x00446030): uses 4 contact_pos.y reads AND all 4 susp positions for BOTH roll and pitch outputs
   - B-only solver (0x00446140): uses 4 contact_pos.y reads AND all 4 susp positions, ONLY writes ROLL output (one AngleFromVector12 call)
   - C-only solver (0x004461C0): uses 4 contact_pos.y reads AND all 4 susp positions, ONLY writes PITCH output

   The formulas inside the B/C variants use **DIFFERENT field combinations** than the corresponding output channel of the full solver. Specifically B's roll formula uses operand layout `iVar8 = +0x1C` (RL.y), while the full solver's roll formula uses `iVar8 = +0x10` (FR.y). **THESE ARE NOT THE SAME FORMULA**.

   **Port likely produces wrong magnitudes** when wheel_contact_bitmask is 3/5/10/12 (one wheel airborne). The port's audit at line 4754 acknowledges this but claims "byte-faithful" — needs verification by capturing rotated-y outputs and comparing to a known-good reference.

### D5 — GROUND-SNAP LOOP REWRITTEN  **(KEY DIVERGENCE)**
The original's per-wheel averaging loop (Phase C step 19) writes through THREE distinct slots:
- `psVar10` (+0x212 + i*8, body_off.y)
- `puVar8` (+0x230 + i*8, ConvertFloatVec3ToShortAngles output)
- `local_18` (chassis-Y accumulator)

It calls `ConvertFloatVec3ToShortAngles(&body_off, &out_short3)` which **pops 3 floats from the x87 FPU stack** that were pushed by the preceding solver/matrix sequence. The function does NOT compute new transforms — it just FISTPs three pre-staged floats.

**Port's implementation (lines 4902-5037)** computes the rotated Y locally using `rotate_body_to_world_y(actor, src)` which reads the rotation_matrix.m[] and does a fresh 3-coefficient multiply-add in float space (`m[1]*v[0] + m[4]*v[1] + m[7]*v[2]`). This is **mathematically equivalent ONLY IF**:
- The rotation_matrix is exactly the matrix loaded by `LoadRenderRotationMatrix` at 0x004061EA (the original's per-wheel transform uses the LOADED rotation matrix's row-1, not a directly-multiplied row).
- The body_off vector matches the original's `(cwx, body_wy_no_preload, cwz)` exactly. Port computes `body_wy = cwy - sp_div - susp_offset` (no preload-add-back), but original's outer pre-call sets body_off.y = cwy - sp_div - preload **THEN** restores body_off.y += preload AFTER the ConvertFloatVec3ToShortAngles call. The output `puVar8[1]` is computed with body_off.y = (cwy - sp_div - preload). After the convert, body_off.y is restored, but `puVar8[1]` already contains the rotated Y from the pre-add-back vector.

**Port matches this**: it uses `body_wy = cwy - sp_div - susp_offset` in the rotation call. But the X and Z used are RAW `cwx` and `cwz` — original has `psVar10[-1]` and `psVar10[+1]` which are the **stashed body_off X/Z slots** at +0x210 and +0x214. These were pre-populated by an upstream caller (almost certainly `RefreshVehicleWheelContactFrames` or the T2 solver call that pushed FPU values for ConvertFloatVec3ToShortAngles to pop).

**HYPOTHESIS:** the +0x210..+0x22F per-wheel `wheel_display_angles` array is being repurposed by 0x00405E80 as a temporary stash for body-offset shorts to feed into ConvertFloatVec3ToShortAngles. Port's `rotate_body_to_world_y` uses raw cardef `cwx/cwz`, not stashed +0x210..+0x214 values — these may DIFFER if the prior T2 call wrote to them.

This is the most likely place for runtime divergence and is what the pilot's runtime diff will reveal.

### D6 — VELOCITY-FROM-SNAP GATE  **(WELL UNDERSTOOD, ALREADY PORTED)**
Port at lines 5009-5036 ports the `DAT_0046318C` table gate + the `(OLD & (NEW^0xF)) == 0` mask check, with prev_mask reconstructed from `s_prev_grounded_mask`. Original directly reads +0x37D (which is correctly OLD post-refresh). **Verify byte-equality** in the runtime CSV.

### D7 — `airborne_frame_counter` INCREMENT CONDITION  **(SUBTLE)**
Original (0x0040634B): `INC word ptr [ESI+0x360]` is reached ONLY when `grounded_count <= 0` (test follows IDIV). The branch is `JLE 0x0040634B` after `TEST EAX,EAX`.

Port (line 4686-4702):
```c
if (actor->wheel_contact_bitmask == 0x0F && !g_game_paused) {
    actor->airborne_frame_counter++;
    ...
}
```

This fires **before** the ground-snap loop runs, gated on the simple equality `wcb == 0x0F` plus a pause guard. The original's gate is **after** the loop and based on the loop's running `grounded_count` (which can be 0 even if wcb != 0xF if all wheels happened to have their bits set — but in practice grounded_count==0 iff wcb==0x0F).

Functionally **equivalent for wcb==0x0F**, but the position-in-flow differs: port increments BEFORE the ground-snap loop (which then sees count=0 and skips), original increments AFTER. **No state diverges** because no other writes happen between the loop and the increment.

However, the port's `!g_game_paused` gate is **NOT in the original**. The original increments unconditionally during the airborne branch. Port comments acknowledge this as a port-only guard against countdown afc inflation. **Acceptable scope — leave as-is for now**, document.

### D8 — TAIL CALL `UpdateVehicleSuspensionResponse` UNCONDITIONAL  **(MATCH)**
Both port and original always call UpdateVehicleSuspensionResponse at the tail. Port line 5064: `td5_physics_update_suspension_response(actor)`. **MATCH**.

### D9 — TRACK-SPAN GUARD IS PORT-ONLY  **(SHOULD BE REMOVED FOR PARITY)**
Port (lines 4602-4618) clamps `track_span_raw` and per-wheel `wheel_probes[wi].span_index` against `td5_track_get_span_count()`. **No such clamp exists in the original** — `UpdateActorTrackPosition` is trusted to return a valid span. This guard masks upstream walker bugs but doesn't introduce divergence on Edinburgh (walker is correct there). Document as scope deferred.

### D10 — PORT LOG / DIAGNOSTIC BRANCHES  **(NO DIVERGENCE, JUST OVERHEAD)**
Multiple `TD5_LOG_I` / `TD5_LOG_D` calls at lines 4699, 4825-4836, 5072-5089, 5092-5096, 5126-5139. All read-only state. Negligible for output diff.

## Capture schema for the pilot

Per call (1 row per call; T2 + ground-snap state captured as columns):

**Keys:** `sim_tick`, `slot`, `caller_ra` (one of 0x405D70/0x406650/0x42F140), `paused`
**Inputs (function-entry snapshot):**
- `world_pos_x/y/z`            (+0x1FC/+0x200/+0x204)
- `linear_velocity_x/y/z`      (+0x1CC/+0x1D0/+0x1D4)
- `angular_velocity_roll/yaw/pitch`  (+0x1C0/+0x1C4/+0x1C8)
- `euler_accum_roll/yaw/pitch` (+0x1F0/+0x1F4/+0x1F8)
- `display_angles_roll/yaw/pitch` (+0x208/+0x20A/+0x20C, int16)
- `wheel_contact_bitmask`      (+0x37C, uint8 — NEW from prior tick)
- `damage_lockout`             (+0x37D, uint8 — OLD entering this call)
- `airborne_frame_counter`     (+0x360, uint16)
- `prev_frame_y_position`      (+0x2D8, int32)
- `track_span_raw`             (+0x80, int16)
- `cardef_0x82`                ([+0x1B8]+0x82, int16)
- `gravity_constant`           ([0x00467380], int32 GLOBAL — capture for sanity)

**Outputs (function-exit snapshot):**
- `world_pos_x/y/z`
- `linear_velocity_x/y/z`      (vy may have been overwritten by snap-gate)
- `euler_accum_roll/yaw/pitch`
- `display_angles_roll/yaw/pitch`
- `angular_velocity_roll/pitch` (yaw NOT touched)
- `wheel_contact_bitmask`, `damage_lockout`
- `rotation_matrix_m[0..8]` (9 floats — captures both BuildRotationMatrixFromAngles outcomes via post-second-call state)
- `render_pos_x/y/z` (3 floats)
- `airborne_frame_counter`
- `prev_frame_y_position` (read-only this call but capture)
- `track_span_raw` (may have moved if UpdateActorTrackPosition triggered)

Schema is ~45 columns; one row per call. Capture only slot 0 to match prior pilots.

## Next actions

1. **Generate** `tools/frida_pool2_00405E80.js` — entry+exit hooks, slot-0 only, output to `log/orig/pool2_00405E80.csv`.
2. **Add** port-side `td5_pilot_trace_00405E80.{c,h}` mirroring the schema column-for-column.
3. **Add** the new TU to `build_standalone.bat` TD5RE_SRCS list.
4. **Wire** enter/leave calls at the top + bottom of `td5_physics_integrate_pose`.
5. **Build** the worktree.
6. **Capture** original-side via `td5_quickrace.py` with `--extra-script tools/frida_pool2_00405E80.js`, then port-side via worktree td5re.exe.
7. **Diff** using `tools/diff_func_trace.py --key=sim_tick,slot` over sim_tick 5..30.
8. **Iterate**: predicted first-diverge columns (priority order):
   - D5 (ground-snap loop): `world_pos_y`, `linear_velocity_y` post-snap-gate
   - D1 (display-angle mask): `display_angles_roll/yaw/pitch` straddling sign boundary
   - D4 (T2 solver mismatch on mask 3/5/10/12): `display_angles_roll`, `display_angles_pitch`, `angular_velocity_roll/pitch`
9. **Commit** to `precise-00405E80` branch. **DO NOT MERGE** — leave for batch review.

## Initial runtime diff result (2026-05-14)

Captured 164 orig rows + 193 port rows on Edinburgh (track=1, slot 0, span 0,
PlayerIsAI=1, 30 sim_ticks). Diff via `tools/diff_func_trace.py --key=sim_tick,slot`:

**Tick 0 (first call):**
- Both ports start with `in_world_pos=(0,0,0)` and `in_*_velocity=0`.
- Port has `in_euler_yaw=0` vs orig `944640` — spawn yaw not bound before
  the very first integrate_pose call in port. Original sets it inside
  `InitializeRaceVehicleRuntime` (the only caller for sim_tick=0 row).
- Outputs at tick 0 diverge wildly (rot0..rot8, world_y, all eul/disp angles)
  because the port's first-call state has the wrong euler_accum.yaw seed.

**Tick 1 (first post-spawn call):**
- `in_world_pos`, `in_euler_*`, `in_display_*` all MATCH. Only AI-dynamics
  outputs (`in_lvx=-621 vs -667`, `in_lvz=-207 vs -141`, `in_avy=0 vs 57`)
  show small divergences from the upstream `UpdateAIVehicleDynamics`
  (pool12 pilot, separately tracked).
- `out_world_y = 672384 (port) vs 660096 (orig)` — port chassis +12288 fp8
  ABOVE ground after the ground-snap loop. Likely D5 (different X/Z source
  for the body-offset rotation — port uses raw cardef cwx/cwz while
  original uses stashed +0x210/+0x214 slots written by T2/refresh chain).
- `out_lvy = 12288 (port) vs 0 (orig)` — port's velocity-snap gate fired
  with a +12288 delta where orig's didn't fire (or fired with 0 net).
- `out_avr = -32 (port) vs -3360 (orig)` — T2 block produced wildly
  different angular_velocity_roll. Likely D1+D4 (display-angle mask
  affects T2 delta + T2 solver formula mismatch).
- `out_eup = 0 (port) vs 256 (orig)` — port's T2 didn't write pitch
  euler_accum where orig did. Looks like the solver-dispatch case selected
  differs (port and orig may pick different switch case based on
  `in_wcb`/`in_dlk` orderings at function entry).
- `out_rot_*` matrix entries diverge ~0.02..0.06 — downstream of the
  display_angle divergences.

**Tick 5..30 (cumulative drift):**
- By tick 5 EVERY input column has diverged. `in_afc=3 (port) vs 0 (orig)`
  means port's airborne_frame_counter has counted up 3 ticks of full
  airborne where orig stayed grounded. World_y delta = +38456 fp8
  (port floating ~150 world units above road).
- This is the same **upstream-blocker** pattern observed in pilot_00403A20:
  even though the integrate_pose math may be correct in isolation, the
  runtime trace cannot validate that until the upstream chain
  (RefreshVehicleWheelContactFrames downstream effects, T2 solver,
  spawn-yaw binding) produces matching state.

## Blocked by upstream — cannot byte-validate at runtime

The static port has the correct overall structure (gravity → euler integ
→ pos integ → matrix build → track-pos refresh → wheel refresh → T2 →
ground-snap → suspension response). But the **internal details** of the
T2 block (D4 solver mismatch) and the **ground-snap body-offset source**
(D5) produce real divergences from tick 1 onward.

Without fixing the upstream pollution (spawn-yaw binding for tick 0, AI
dynamics deltas at tick 1) AND aligning the T2 + ground-snap
implementations to byte-faithful pipelines, the runtime trace will
continue showing cascading divergences.

**Recommended follow-up (separate pilots):**
1. **Spawn-yaw binding** — find where original writes `euler_accum.yaw`
   on the very first integrate_pose entry (likely inside
   InitializeRaceVehicleRuntime / 0x42F140 → 0x405D70). Port the
   pre-call binding.
2. **D4 — port the B/C solver branches as separate functions** mirroring
   `TransformTrackVertexByMatrixB/C` byte-faithfully. Replace the
   shared `attitude_from_wheels` dispatch for masks 3/5/10/12.
3. **D5 — locate the +0x210/+0x214 stash source** — likely the per-wheel
   loop of `RefreshVehicleWheelContactFrames` writes there. Port-side
   ground-snap loop must consume those stashed values, not raw cardef.
4. **D1 — drop the `& 0xFFF` mask on display_angle writes** at line
   4622-4624. Confirm no downstream consumer relies on the unsigned
   wrap.

## Static-port deliverable (this branch)

Pilot harness fully wired and committed:
- `re/analysis/pilot_00405E80_audit.md` — this document.
- `tools/frida_pool2_00405E80.js` — original-side input/output capture.
- `td5_pilot_trace_00405E80.{c,h}` (worktree) — port-side mirror.
- `td5_physics.c` integrate_pose entry+leave hooks (worktree).
- `build_standalone.bat` TD5RE_SRCS updated (worktree).
- `log/orig/pool2_00405E80.csv` (164 rows) — orig capture committed.
- `log/port/pool2_00405E80.csv` (193 rows) — port capture committed.

The harness is reusable for future runtime validation once the upstream
blockers are addressed.

## Reference

- Listing:    0x00405E80 .. 0x00406362 (Ghidra TD5_pool2, 2026-05-14)
- Decomp:     same session, 145 lines
- Port:       `td5mod/src/td5re/td5_physics.c:4554-5147`
- Frida probe: `tools/frida_pool2_00405E80.js`
- Port emitter: `td5_pilot_trace_00405E80.{c,h}` (worktree)
- Orig CSV:   `log/orig/pool2_00405E80.csv` (164 rows / ~30 sim_ticks)
- Port CSV:   `log/port/pool2_00405E80.csv` (193 rows / ~30 sim_ticks)
- Diff tool:  `tools/diff_func_trace.py --key=sim_tick,slot`
