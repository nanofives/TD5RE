# Pilot Audit — 0x004057F0 UpdateVehicleSuspensionResponse

**Date:** 2026-05-14
**Pool slot:** TD5_pool6
**Port-side function:** `td5_physics_update_suspension_response` @ `td5_physics.c:3618`
**Worktree:** `.claude/worktrees/precise-004057F0` on branch `precise-004057F0`
**Caller graph:** `IntegrateVehiclePoseAndContacts (0x00405E80)` — single caller. Return-address tagging shows the call site at 0x00406340.
**Callee graph:** TransposeMatrix3x3 (0x42E710), LoadRenderRotationMatrix (0x43DA80), ConvertFloatVec3ToShortAngles (0x42E2E0), PlayVehicleSoundAtPosition (0x441D90).
**Body:** 783 bytes / 241 instructions / ~120 decompiled lines.

## Function structure (from listing + decomp)

```
prologue:
    bVar1 = actor->wheel_contact_bitmask     (= +0x37C, CURRENT-frame airborne)
    bVar2 = actor->damage_lockout            (= +0x37D, PREVIOUS-frame airborne, written by 0x00403720)
    if bVar1 == 0xF:  return    (all wheels airborne, gravity NOT restored)

    TransposeMatrix3x3(actor + 0x120, local_30)    // local_30 = body→world^T = world→body
    LoadRenderRotationMatrix(local_30)             // makes the global render matrix point at local_30

    locals: loni_grav=0, lat_grav=0, loni_spr=0, lat_spr=0, bounce=0,
            cnt_active=0, cnt_grounded=0

    LOOP 4 wheels (psVar11 walks +0x254 stride 8 bytes):
        if (bVar1 & (1<<i)) skip wheel       // airborne this tick
        lat   = arms[i*4+0]                  // = wheel_display_angles[i][0] @ +0x210+i*8
        loni  = arms[i*4+2]                  // = wheel_display_angles[i][2] @ +0x214+i*8
        ConvertFloatVec3ToShortAngles(wcv[i], &local_38)   // reads ROW 1 of transpose = COL 1 of body matrix
        y_view = local_36                                  // Y component, int16 truncated toward 0 (FPU __ftol RC=11)
        g_scaled = (y_view * gravity + ((y_view*gravity)>>31 & 0xFFF)) >> 12
        loni_grav += g_scaled * loni
        lat_grav  -= g_scaled * lat
        ++cnt_active

        if (bVar2 & (1<<i)):                 // wheel was AIRBORNE prev tick → just landed
            dot = wfdh[i].x*wcv[i].x + wfdh[i].y*wcv[i].y + wfdh[i].z*wcv[i].z
            dot = (dot + (dot>>31 & 0xFFF)) >> 12        // signed >>12 with round bias
            loni_spr += (dot * loni) * -0x100
            lat_spr  += (dot * lat ) *  0x100
            bounce   += dot / 2                          // C-style signed div toward zero
            ++cnt_grounded

    loni_grav /= cnt_active        (IDIV — cnt_active > 0 guaranteed once bVar1 != 0xF)
    lat_grav  /= cnt_active
    if cnt_grounded > 0:
        loni_spr /= cnt_grounded
        lat_spr  /= cnt_grounded
        bounce   /= cnt_grounded
    if bounce > 0x14: PlayVehicleSoundAtPosition(0x17, bounce*50, 0x5622, &world_pos, 4)

    actor->angular_velocity_roll  += (loni_spr + loni_grav) / 0x4B0
    actor->angular_velocity_pitch += (lat_spr  + lat_grav)  / 0x226
    actor->linear_velocity_y      += bounce + gravity

    if cnt_grounded > 0:
        switch (bVar2):                  // pattern-clamp keyed on prev_air
            case 0,1,2,4,6,8,9: clamp roll AND pitch to ±4000
            case 3,12:          clamp roll only to ±4000
            case 5,10:          clamp pitch only to ±4000
            default (7,11,13,14,15): no clamp
    return
```

## Capture infrastructure

**Frida probe:** `tools/frida_pool6_004057F0.js` — captures one row per call (slot 0 only) with:
- Inputs: lock, prev_air, gravity, 9 floats of rotation_matrix, per-wheel (lat, loni, wcv.{x,y,z}, wfdh.{x,y,z})
- Outputs: pre/post angular_velocity_{roll,pitch} + linear_velocity_y, plus deltas

**Port-side trace:** `td5_pilot_trace_004057F0.{c,h}` — same schema, hooks `td5_physics_update_suspension_response` entry/exit.

**Output:** `log/orig/pool6_004057F0.csv` and `.claude/worktrees/precise-004057F0/log/port/pool6_004057F0.csv` (~150 rows each, 30 sim_ticks of Edinburgh, slot 0, PlayerIsAI=1, Viper).

## Static port audit — single algorithmic divergence found and SHIPPED

### D1 — `bounce += dot >> 1` vs `dot / 2` (SHIPPED)

**Original** at 0x00405938-0x0040593F:
```
CDQ                  ; EDX = sign_ext(EAX) — -1 if EAX neg, 0 if pos
SUB EAX, EDX         ; round-bias: pos unchanged, neg += 1
...
SAR EAX, 1           ; arith shift right by 1
```
This idiom implements **C-style signed div-by-2 with truncation toward zero** (-5/2 = -2, not -3).

**Port (pre-fix)** at `td5_physics.c:3768`:
```c
bounce += dot >> 1;       /* signed SAR 1 → toward -infinity, -5 >> 1 = -3 */
```

**Fix applied** in commit on `precise-004057F0`:
```c
bounce += dot / 2;        /* signed div toward zero */
```

For negative odd `dot`, the difference is 1 LSB of `bounce`. Effect propagates through `bounce /= cnt_grounded` then `linear_velocity_y += bounce + gravity`. Magnitude small but compounding over many ticks of light landing impulses; matches the project's "pursue fixes to zero delta" stance.

All other algorithmic patterns audited match line-for-line vs disasm:
- Sign convention `loni_grav += g_scaled * loni`, `lat_grav -= g_scaled * lat` (port has both correctly).
- Spring scaling `(dot * loni) * -0x100` for loni_spr, `(dot * lat) * +0x100` for lat_spr (matches).
- Axis assignment to ang_vel_roll/pitch (port commented "axis-swap fix" agrees with disasm at 0x00405A3D / 0x00405A43).
- Divisors `/ 0x4B0` (roll) and `/ 0x226` (pitch) confirmed via IMUL constants `0x1B4E81B5 SAR 7` and `0x77280773 SAR 8`.
- Linear velocity Y update gated on cnt_active > 0 (original is unconditional once past lock==0xF, but cnt_active > 0 is implied by that gate). Equivalent.
- Pattern clamp switch on `bVar2 = prev_air` (port keys on `prev_air` — matches).

## Runtime diff — BLOCKED BY UPSTREAM POOL1

Captured 31 sim_ticks (ticks 0-30) on Edinburgh slot 0 PlayerIsAI=1 Viper. Port and original share keys for 31 common sim_ticks.

**Every input column diverges across 153/153 captured calls.** Detail:

| Column | First-tick port → orig | Cause | Source |
|---|---|---|---|
| rotation_matrix (rot0..8) | port = identity → orig = full pitch+yaw matrix | Port's slot 0 actor rotation_matrix not updated by pose integrator during PlayerIsAI=1 path | UPSTREAM — `td5_physics_integrate_pose` / `td5_physics_update_ai_pose` |
| wcv_y (4 wheels) | port = +4096 → orig = -4095 | Sign convention flip on surface normal Y | UPSTREAM pool1 — `0x00403720 RefreshVehicleWheelContactFrames` |
| wcv_x, wcv_z | port = 0 → orig = 51, 22 | Surface normal X/Z components — flat surface in port, actual road grade in orig | UPSTREAM pool1 |
| wfdh_x, wfdh_y, wfdh_z (gap_270) | port = ~0 → orig = ±200..500 | gap_270 frame-to-frame wheel-position delta — port shows no wheel motion | UPSTREAM pool1 |
| w0..3_lat, w0..3_loni (arms = wheel_display_angles) | port = 0 → orig = ±255 / ±435 | Per-vehicle wheel body-offsets — port's cardef field not populated for slot 0 PlayerIsAI=1 | UPSTREAM — cardef loading path (separate from pool1) |
| pre_av_roll | port = 0 → orig = 256 (then 6000+) | Port slot 0 angular velocity never accumulates | UPSTREAM — pose integrator |
| gravity | tick 0: 1900 vs 1500 (only divergent row) | Original starts with easy-gravity 1500, port uses normal 1900 at very first call before lock_difficulty fires | UPSTREAM — game init |

Because **all four input categories diverge before the function reaches its body**, outputs (d_av_roll, d_vy) also diverge by inevitable downstream propagation. The function's *internal* arithmetic cannot be validated against the original until inputs match.

## Status

- **Pilot probe + port-side trace + diff infrastructure**: complete and producing well-formed CSVs at expected row counts.
- **Static port audit**: complete; one real bug (`dot >> 1` vs `dot / 2`) identified and fixed on branch `precise-004057F0`. No other algorithmic divergences found.
- **Runtime byte-equality validation**: **BLOCKED BY UPSTREAM POOL1** (precise-00403720) + a side-channel cardef loading bug in the port's PlayerIsAI=1 slot 0 path. Cannot proceed until the precise-00403720 worktree's fixes (hires shift removal + gap_270 clamp removal + possibly wcv sign restoration) are merged to master AND the port's slot-0-PlayerIsAI cardef loading is verified.

## Next actions (after upstream unblocks)

1. Wait for pool1 (`precise-00403720`) merge to master.
2. Verify cardef-loading path: `wheel_display_angles[w]` should be populated by `td5_physics_init_runtime` (called from race spawn) regardless of PlayerIsAI. If port's PlayerIsAI=1 skips the cardef copy for slot 0, that needs a separate fix outside this pilot.
3. Rebase precise-004057F0 onto the updated master.
4. Re-capture original-side `tools/frida_pool6_004057F0.js` and port-side `td5_pilot_trace_004057F0.csv` for 30 ticks.
5. Re-run `python tools/diff_func_trace.py ... --max-tick=30 --key=sim_tick,slot`.
6. Expect zero diff on (d_av_roll, d_av_pitch, d_vy) at sim_tick ≥ 5 once inputs are aligned. If non-zero, iterate on output-line algorithmic deltas (none currently suspected).

## Definition of done

Zero diff on (d_av_roll, d_av_pitch, d_vy) at sim_tick ≥ 5 in `pool6_004057F0.csv` once upstream inputs are aligned.

Current state: STATIC PORT FIX SHIPPED + INPUT DIVERGENCE TRACED TO UPSTREAM (pool1). PAUSED.
