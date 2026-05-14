# Pilot Audit — V2V Contact Pair (0x00408570 + 0x00408A60)

**Date:** 2026-05-14
**Pool slot:** TD5_pool15
**Worktree:** `.claude/worktrees/precise-v2v-contact` on branch `precise-v2v-contact` from `fpu-cw-fix`
**Tag:** `pool15_v2v_contact`
**Port-side functions:**
- `obb_corner_test` @ `td5_physics.c:2417` ↔ `CollectVehicleCollisionContacts` @ 0x00408570
- `collision_detect_full` @ `td5_physics.c:2974` ↔ `ResolveVehicleCollisionPair` @ 0x00408A60

**Pairing note:** Session 13 (pool14) owns `ApplyVehicleCollisionImpulse @ 0x004079C0`. Both 0x00408570 and 0x00408A60 are upstream callers of pool14's target.

## Function structure (from listing + decomp)

### 0x00408570 CollectVehicleCollisionContacts

Signature (cdecl): `uint(param_1=actorA*, param_2=actorB*, param_3=&{Ax,Ay,Az,Bx,By,Bz}, param_4=&{yawA_raw, yawB_raw}, param_5=short* contactData)`

Where `param_3/4` are STACK COPIES — see 0x408A60 setup `local_80..local_6c` for positions, `local_94`/`local_90` for raw yaw accumulators. All position/yaw inputs are int32 in 24.8 fixed-point; the function does `>> 8` on each to obtain int16 display units.

Prologue (0x00408570-0x004085D7):
```
iVar3   = (Bz - Az) >> 8                                ; delta_z in int16
iVar24  = (Bx - Ax) >> 8                                ; delta_x in int16
uVar25  = (yawA - yawB) >> 8 → MOVSX to int16           ; dheading (A frame rotation of B's box)
iVar4   = CosFixed12bit(dheading)
iVar5   = SinFixed12bit(dheading)
iVar6   = CosFixed12bit(yawA >> 8)
iVar7   = SinFixed12bit(yawA >> 8)
iVar8   = CosFixed12bit(yawB >> 8)
iVar9   = SinFixed12bit(yawB >> 8)
```

Box extents (0x00408607-0x004086B0):
- cardef pA: iVar16 = +0x04 (front_z), iVar21 = +0x08 (half_w), iVar17 = +0x14 (rear_z, negative)
- cardef pB: iVar18 = +0x04 (front_z), iVar14 = +0x08 (half_w), iVar22 = +0x14 (rear_z, negative)

Per-corner pre-products spilled to stack:
- iVar10 = half_w_pA  * cos_d >> 12          (used by 2nd half — A in B)
- iVar11 = -(half_w_pA  * sin_d) >> 12
- iVar12 = front_z_pA * cos_d >> 12
- iVar13 = -(-rear_z_pA  * sin_d) >> 12      ; rear_z_pA is negative → iVar13 = rear_z_pA*sin_d
- iVar19 = front_z_pB * cos_d >> 12
- iVar23 = half_w_pB  * cos_d >> 12
- iVar15 = half_w_pB  * sin_d >> 12
- iVar7  = -rear_z_pB  * sin_d >> 12  (rear_z negated then * sin_d)

World→local translations (0x004086D0-0x00408715):
```
iVar20 = (cos(yawA)*delta_x - sin(yawA)*delta_z) >> 12   ; B_center_in_A.x
iVar6  = (cos(yawA)*delta_z + sin(yawA)*delta_x) >> 12   ; B_center_in_A.z
```

NOTE: this is the **STANDARD world→local convention** (cos·dx − sin·dz, sin·dx + cos·dz), NOT (cos·dx + sin·dz, −sin·dx + cos·dz). See D1 below.

**Bit 0 (B FL corner ∈ A box)** — corner = B-local (-half_w_B, +front_z_B):
```
corner_x_in_A  = iVar20 - iVar18*sin_d - iVar14*cos_d   ; = -half_w_B*cos_d - front_z_B*sin_d + B_center_in_A.x
corner_z_in_A  = iVar6  + iVar19       - iVar15         ; = +front_z_B*cos_d - half_w_B*sin_d + B_center_in_A.z
```
After computing `iVar27 = corner_x_in_A` and `iVar28 = corner_z_in_A`:
```
if (-half_w_A <= iVar27 <= +half_w_A) && (rear_z_A <= iVar28 <= front_z_A) {
    local_48 |= 1
    psVar7[0] = (short)iVar27          ; contactData[0] = proj_x
    psVar7[1] = (short)iVar28          ; contactData[1] = proj_z
    psVar7[2] = (short)iVar27 - sVar1  ; contactData[2] = proj_x - B_center_in_A.x  (= rotated corner offset)
    psVar7[3] = (short)iVar28 - sVar2  ; contactData[3] = proj_z - B_center_in_A.z
}
```
where `sVar1 = (short)iVar20`, `sVar2 = (short)iVar6` (B center as int16 truncated).

Stride per record: 8 bytes (4 × int16). Record array `local_58 [4]; local_50 [4]; ... local_20 [4]` — 8 entries × 8 bytes = 64 bytes total. Caller reads via `psVar7 + 4` (i.e. +8 bytes per case-dispatch).

Subsequent 7 bits use the same rotated-corner addition pattern with sign permutations:
- Bit 1 (B FR = +half_w_B, +front_z_B): iVar26+iVar23, iVar6+iVar19+iVar15
- Bit 2 (B RR = +half_w_B, +rear_z_B):  iVar20-iVar23+iVar7, iVar6-iVar15+(-iVar22*cos_d>>12)
- Bit 3 (B RL = -half_w_B, +rear_z_B):  iVar23+iVar20+iVar7, iVar15+(iVar6+...)
- Bits 4-7 symmetric: A corners in B's frame using `(cos(yawB), sin(yawB))`; uses iVar10..iVar13 pre-products.

Both halves write a single int16 truncation per dim. **NO ROUND-TO-ZERO IDIOM here** — plain SAR EAX, 0xc. Negative non-multiples-of-4096 will round-toward-minus-infinity (one off zero) which **diverges from 0x00403A20's sar8_rz idiom** but is what the original does.

Return value: `local_48` = corner bitmask in low byte (bits 0-7). Bits 0-3 = B-corner-in-A hits, 4-7 = A-corner-in-B hits.

### 0x00408A60 ResolveVehicleCollisionPair

Signature (cdecl): `void(slotIndexA, slotIndexB)`.

Phase 1 — AABB broadphase (0x00408A60-0x00408AB7):
```
slotA_aabb_base = &gBroadphaseActorBoundsTable[slotA*5]   ; 0x483050 + 20*slotA
slotB_aabb_base = same with slotB
EBX  = gBroadphaseActorBoundsTable[slotB*5 + 2]    ; slotB.xMax
ESI  = &gBroadphaseActorBoundsTable[slotB*5]       ; slotB base

if slotA.xMin >= slotB.xMax  return    ; 0x00408a92: JGE skip
if slotA.xMax <= slotB.xMin  return    ; 0x00408a9d: JLE skip
if slotA.zMin >= slotB.zMax  return    ; 0x00408aa8: JGE skip
if slotA.zMax <= slotB.zMin  return    ; 0x00408ab4: JLE skip
```

Field offsets in the broadphase entry (verified by CMP layout): +0x0=xMin, +0x4=zMin, +0x8=xMax, +0xC=zMax, +0x10=unused/separator.

Phase 2 — Stack snapshot + seed test (0x00408ABD-0x00408B56):
```
local_80..local_78 = actorA->{world_pos.x, y, z}          ; +0x1fc, +0x200, +0x204
local_74..local_6c = actorB->{world_pos.x, y, z}
local_94           = actorA->euler_accum.yaw              ; +0x1f4
local_90           = actorB->euler_accum.yaw

local_88 = CollectVehicleCollisionContacts(actorA, actorB, &local_80, &local_94, local_58);
if (local_88 == 0) return;   ; 0x00408b56 JZ
```

Phase 3 — Bisection setup (0x00408B5C-0x00408BFD):

`local_84 = 0x80` (impactForce accumulator), `local_8c = 0x80` (yaw step pair B / dword-spilled). All velocity reads use `CDQ; SUB EAX,EDX; SAR EAX,1` (round-to-zero divide by 2):
- iVar12 = sar1_rz(actorA->linear_velocity_x)   ; +0x1cc, "rdz halve"
- iVar13 = sar1_rz(actorA->linear_velocity_z)   ; +0x1d4
- iVar9  = sar1_rz(actorA->angular_velocity_yaw); +0x1c4
- iVar11 = sar1_rz(actorB->linear_velocity_x)   ; +0x1cc
- local_4 = sar1_rz(actorB->linear_velocity_z)
- local_5c = sar1_rz(actorB->angular_velocity_yaw); +0x1c4

Pre-loop rollback:
```
local_80 -= iVar12   ; A.world_pos.x  -= vel_ax/2
local_78 -= iVar13   ; A.world_pos.z  -= vel_az/2
local_94 -= iVar9    ; A.yaw_accum    -= omega_a/2
local_74 -= iVar11   ; B.world_pos.x  -= vel_bx/2
local_6c -= local_4  ; B.world_pos.z  -= vel_bz/2
local_90 -= local_5c ; B.yaw_accum    -= omega_b/2
local_68 = 7         ; iteration count
```

Phase 4 — 7-iteration bisection (0x00408C07-0x00408D23):

Each iteration halves all six step variables with the same `CDQ; SUB EAX,EDX; SAR EAX,1` idiom:
```
iVar12 = sar1_rz(iVar12)
iVar13 = sar1_rz(iVar13)
iVar9  = sar1_rz(iVar9)
iVar11 = sar1_rz(iVar11)
local_4 = sar1_rz(local_4)
local_5c = sar1_rz(local_5c)
local_8c = sar1_rz(local_8c)   ; impactForce step

uVar5 = CollectVehicleCollisionContacts(actorA, actorB, &local_80, &local_94, local_58)
if (uVar5 != 0) {                       ; bitmask non-empty → step backward in time
    local_88 = uVar5                    ; cache last-hit bitmask
    local_80 -= iVar12; local_78 -= iVar13; local_94 -= iVar9
    local_74 -= iVar11; local_6c -= local_4; local_90 -= local_5c
    local_84 -= local_8c                ; impactForce -= step
} else {                                 ; separated → step forward
    local_80 += iVar12; local_78 += iVar13; local_94 += iVar9
    local_74 += iVar11; local_6c += local_4; local_90 += local_5c
    local_84 += local_8c
}
local_68 -= 1
```

Phase 5 — Dispatch (0x00408D29-0x00408EA0):
```
impactForce = local_84 - 0x10           ; range [-0x10, 0xF0] after bisection
                                         ; (NOT clamped — passed verbatim)
iVar9 = impactForce

if (local_88 - 1 > 0x7F) goto loop_dispatch    ; 8-bit-mask path: bits beyond 0x80 → full loop
else {
    uVar10 = (local_94 >> 8)            ; A's bisected yaw → 12-bit angle for B-in-A cases
    uVar5  = (local_90 >> 8)            ; B's bisected yaw → 12-bit angle for A-in-B cases
    switch (local_88) {
        case 0x01: ApplyVehicleCollisionImpulse(actorA, actorB, local_58, uVar10, iVar9); break;
        case 0x02: ... local_50, uVar10
        case 0x04: ... local_48, uVar10
        case 0x08: ... local_40, uVar10
        case 0x10: ApplyVehicleCollisionImpulse(actorB, actorA, local_38, uVar5,  iVar9); break;
        case 0x20: ... local_30, uVar5
        case 0x40: ... local_28, uVar5
        case 0x80: ... local_20, uVar5
        default:   goto loop_dispatch;
    }
}

loop_dispatch:
    /* When multiple bits set, iterate. Each loop iteration shifts a 1 bit. */
    local_8c = 1; psVar7 = local_58; iVar12 = 4
    do {
        if (local_88 & local_8c) ApplyVehicleCollisionImpulse(actorA, actorB, psVar7, uVar10, iVar9)
        local_8c <<= 1; psVar7 += 4; iVar12 -= 1
    } while (iVar12 != 0)
    /* Now uVar10 here is local_8c << 1 after 4 iters = bit 4. Switch to A-in-B half. */
    psVar7 = local_38; iVar6 = 4
    do {
        if (local_88 & uVar10) ApplyVehicleCollisionImpulse(actorB, actorA, psVar7, uVar5, iVar9)
        psVar7 += 4; uVar10 <<= 1; iVar6 -= 1
    } while (iVar6 != 0)
```

Final call (0x00408EA2-0x00408EBA):
```
PlayVehicleCollisionForceFeedback(local_88, actorA, actorB, last_impulse_return_value)
```

## Port-side equivalents (current state)

### `obb_corner_test` @ td5_physics.c:2417

Mirrors the original closely but with these issues (see D-points below):

```c
int32_t local_dx = (delta_x * cos_a + delta_z * sin_a) >> 12;  // PORT
int32_t local_dz = (-delta_x * sin_a + delta_z * cos_a) >> 12; // PORT
```

vs original:
```c
local_dx = cos(yawA)*delta_x - sin(yawA)*delta_z
local_dz = cos(yawA)*delta_z + sin(yawA)*delta_x
```

Corner rotation lines 2471-2472 use the world→local form `(cos·lx − sin·lz, sin·lx + cos·lz)` for both halves, which is correct ASSUMING the world→local delta translation in lines 2466-2467 / 2512-2513 is fixed.

### `collision_detect_full` @ td5_physics.c:2974

Port uses an absolute-frac interpolation formula:
```c
int32_t frac = 0x80; int32_t bisect_step = 0x40;
for iter 0..6:
    rollback = 0x100 - frac
    test_ax = ax - ((vel_ax * rollback) >> 8)
    ...
    if bitmask: frac -= bisect_step  else frac += bisect_step
    bisect_step >>= 1; if < 1 set to 1
```

The original uses INCREMENTAL accumulator math (subtract or add the per-iteration step) on STACK COPIES that are also passed-by-pointer to the corner test. **Both formulations visit the same test points** at every iteration regardless of hit/miss history — verified by enumerating all 128 paths through 7 iterations.

Issues are NOT in the bisection logic. They are in:
- Velocity halving: port uses `>> 8` once at entry → `vel_ax = lin_vel_x >> 8` and divides "rollback" by 0x100 afterward, while original uses `sar1_rz` (round-to-zero halve) on EACH iteration. Combined effect: port may differ by ±1 LSB on negative velocities through bisection.
- Heading halving: same idiom mismatch — port pre-divides `omega_h = ang_vel_yaw >> 8` then accumulates; original keeps raw accumulator and divides at the end.

## Confirmed divergence points (port vs original)

### D1 — World→local rotation sign on outer translation **(HIGH IMPACT)**

Port `obb_corner_test` lines 2466-2467 and 2512-2513:

```c
int32_t local_dx = (delta_x * cos_a + delta_z * sin_a) >> 12;       /* B-in-A half */
int32_t local_dz = (-delta_x * sin_a + delta_z * cos_a) >> 12;
...
int32_t local2_dx = (delta2_x * cos_b + delta2_z * sin_b) >> 12;    /* A-in-B half */
int32_t local2_dz = (-delta2_x * sin_b + delta2_z * cos_b) >> 12;
```

Original disasm at 0x004086DC..0x004086FE (and symmetric for 2nd half):
```
iVar20 = iVar6 * iVar24 − iVar7 * iVar3 >> 0xc      ; cos·delta_x − sin·delta_z
iVar6  = iVar6 * iVar3  + iVar7 * iVar24 >> 0xc     ; cos·delta_z + sin·delta_x
```

Port has BOTH sin terms negated relative to original. This is equivalent to rotating the world delta by **−yaw** instead of **+yaw** when projecting world → actor-local frame. (Mathematically: world→local for the "CW from +Z" convention is `lx = cos·dx − sin·dz, lz = sin·dx + cos·dz`.)

**Memory `reference_obb_corner_test_rotation_sign` (22 days old) claims this was fixed 2026-05-13 — but the fix is NOT in the current `fpu-cw-fix` branch tip. The memory is stale.**

Verification convention check: Jarash test case from memory yields delta `(827, -1252)` with yaw=0xA83 (both actors). The original's expected B-in-A delta is `(-1500, ~0)`; port's wrong-sign produces `(+588, +1380)`. This makes contact bit 0x28 fire on stationary spawn — exactly the visible-slide symptom that memory describes.

**Fix:** flip the sign of `sin_a*delta_z` in `local_dx`, AND flip the sign of `sin_a*delta_x` in `local_dz`. Same for `local2_*`.

```c
int32_t local_dx = (delta_x * cos_a - delta_z * sin_a) >> 12;
int32_t local_dz = (delta_x * sin_a + delta_z * cos_a) >> 12;
```

### D2 — AABB broadphase strict vs non-strict comparison **(LOW IMPACT)**

Port:
```c
if (g_actor_aabb[idx_a][2] < g_actor_aabb[idx_b][0] || ...) return;   /* a.xMax < b.xMin */
```

Original 0x00408A92-0x00408AB7 uses JGE/JLE — i.e. skip on **`>=` / `<=`**. So at exact equality (`a.xMax == b.xMin`), original skips, port enters. Port over-approximates by one LSB at every edge.

This only fires on integer-exact equality, which for raw `world_pos>>8 ± cardef.radius` coordinates is rare but not impossible.

**Fix:** change all four predicates from `<` to `<=`.

### D3 — Bisection halving idiom (round-to-zero vs round-to-minus-infinity) **(LOW-MEDIUM IMPACT)**

Original at 0x00408BB5/BC1/CD/D9/E5/F1 + each iteration 0x00408C13/19/25/31/3D/49/54:
```
CDQ              ; sign-extend EAX
SUB EAX, EDX     ; EAX += (EAX<0 ? 1 : 0)
SAR EAX, 1       ; signed divide-by-2 with round-toward-zero
```

This is **round-to-zero** signed halving. For positive x = identical to `x >> 1`. For negative x not divisible by 2, returns one closer to zero than `>> 1` (e.g. `-3 >> 1 == -2`, but `sar1_rz(-3) == -1`).

Port:
```c
int32_t vel_ax = a->linear_velocity_x >> 8;    /* arithmetic shift, no rounding */
...
test_ax = ax - ((vel_ax * rollback) >> 8);     /* arithmetic shift */
```

Port uses plain `>> 8` (round-to-minus-infinity for negatives). For negative velocities the per-tick step value will be exactly 1 LSB lower in port at every halving step.

Practically the divergence stays in the int16 noise floor for typical race speeds (vel ~10000 fp8 → halved 7 times → 78 fp8 → diff at most 1 LSB ≈ 1/256 wu per axis). Could accumulate to ~1 wu test-point drift but not change the bitmask trajectory.

**Fix:** introduce `sar1_rz()` helper (same as in `td5_physics_integrate_suspension`); call it at each bisection halving step.

### D4 — Heading halving point — accumulator scale **(LOW IMPACT, verify)**

Original keeps `local_94`/`local_90` as raw euler_accum.yaw (24.8 FP) throughout the bisection and only divides `>> 8` when pushing to `ApplyVehicleCollisionImpulse` at 0x00408D58 (`SAR ECX, 8`). Halving step is also in 24.8 units.

Port computes `omega_a_h = ang_vel_yaw >> 8` (12-bit display units) up-front then bisects in those units. Algebraically equivalent for integer arithmetic when the raw value happens to be a multiple of 256. For non-multiples, port loses up to 7 LSBs of yaw-step precision at the corner test.

This pairs with D3 — same fix pattern: bisect in raw accumulator units, convert at dispatch.

### D5 — impactForce range expectation **(NO-OP, documented for caller pool14)**

Original `impactForce = local_84 − 0x10` after 7 iterations from 0x80. Range = `[0x80 − 7×0x40 + 6×0x01 − 0x10, 0x80 + 7×0x40 − 0x10]`... actually the step sequence is `0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01`. Sum = 0x7F. So `local_84 ∈ [0x80 − 0x7F − 0x40 = 0x01, 0x80 + 0x7F + 0x40 = 0xFF]` worst case. After `−0x10`: range `[-0xF, 0xEF]`.

Port: identical `frac − 0x10` after 7-iter bisection from 0x80 with same step sequence. Range identical. **Pool14 (V2V impulse) should expect this range, not the older `[0x10, 0xF0]` claim from a stale memory entry.**

### D6 — Dispatch yaw conversion uses BISECTED yaw, not pre-bisect **(VERIFY)**

Original 0x00408D58 `SAR ECX, 8` — but ECX was loaded at 0x00408D4F `MOV ECX, [ESP+0x10]` (= local_94, the BISECTED A yaw). Similar for the A-in-B switch arm using local_90.

Port: passes `test_ha` / `test_hb` (= heading − accumulated bisection deltas) to `apply_collision_response`. Same source, but the port pre-converts to 12-bit display units (the `& 0xFFF` lives inside `cos_fixed12`/`sin_fixed12`). Algebraically equivalent.

### D7 — pre-loop test bitmask cached as `local_88` seed **(MATCH)**

Original 0x00408B52: `MOV [ESP+0x24], EAX` immediately after the first CollectVehicleCollisionContacts. ESP+0x24 = local_88. So `local_88` starts as the pre-loop bitmask and is only updated in-loop when a non-zero bitmask appears. After the 7 iterations, dispatch reads `local_88` straight — there is NO post-loop re-test.

Port matches this: `cached_bitmask = bitmask` at the post-seed point and only updates on hit inside the loop, then dispatches directly. **Confirmed faithful** [td5_physics.c:3071, 3100, 3138-3148].

### D8 — Corner record struct size on the wire **(NO-OP)**

Original record is **8 bytes** (4 × int16): `{proj_x, proj_z, own_x, own_z}`. Records are addressed by `psVar7 + 4` (stride 8 bytes).

Port `OBB_CornerData` is **12 bytes** with extra `pen_x, pen_z` fields used only by port-side logic (not consumed in the dispatch path the original takes). This is a port-side enrichment, not a divergence — does not affect observable inputs/outputs of either function.

The Frida trace must read original records via `psVar7 + 4` stride, capture only the 4 active int16 fields, and the port-side trace must emit the same 4 fields ignoring `pen_*`.

### D9 — `cardef[+0x14]` rear_z stored negative; original consistently treats as signed **(MATCH)**

Both port and original treat `cardef + 0x14` as a **signed negative int16** (rear-Z extent stored as a negative value). Port mapping at lines 2427-2430 matches.

## Sanity-check: signs of corner extents in dispatch

Bit 0 case (B FL = `(-half_w_B, +front_z_B)`): manual derivation from the cos/sin convention gives:
```
corner_x_in_A = B_center_x − half_w_B*cos_d − front_z_B*sin_d
corner_z_in_A = B_center_z − half_w_B*sin_d + front_z_B*cos_d
```
Matches original `iVar27 = iVar20 − iVar18*sin_d − iVar14*cos_d` and `iVar28 = (iVar6 − iVar15) + iVar19` exactly.

Port lines 2471-2476 produce the same corner offset relative to the world→local-translated B-center, **IF** D1 is fixed. The corner-rotation math itself (lines 2471-2472) does **NOT** carry the D1 bug — it's only the outer delta translation.

## Capture schema for pilot

### `obb_corner_test` rows (which_fn = "contact")

Keys: `sim_tick`, `slot_a`, `slot_b`, `call_idx` (1 = pre-loop seed, 2..8 = bisection iters)

Inputs (per call, scalar):
- `actor_a_addr`, `actor_b_addr` (hex)
- `ax`, `ay`, `az` (actor A world pos, 24.8 fp — the pos PASSED at call time, post-rollback)
- `bx`, `by`, `bz` (actor B world pos)
- `yaw_a`, `yaw_b` (raw euler_accum.yaw values passed at call time)
- `cardef_a_off04`, `cardef_a_off08`, `cardef_a_off14` (front_z, half_w, rear_z)
- `cardef_b_off04`, `cardef_b_off08`, `cardef_b_off14`

Outputs (per call):
- `bitmask` (uint8)
- 8 × `{proj_x, proj_z, own_x, own_z}` (32 int16) — even unfired records report 0 since the buffer is on stack but original doesn't memset; port memsets first. Diff must ignore corner records whose bit is not set in bitmask.

### `collision_detect_full` rows (which_fn = "toi")

Keys: `sim_tick`, `slot_a`, `slot_b`

Inputs (one row per pair invocation):
- Pre-loop seed `world_pos` and `yaw_accum` (same as contact's first call)
- Per-iter step values (iVar12/iVar13/iVar9/iVar11/local_4/local_5c/local_8c) → 7 sub-rows ×7 cols

Outputs:
- `impactForce_final` (= local_84 − 0x10)
- `dispatched_bitmask` (= local_88 final)
- `final_test_ha`, `final_test_hb` (= bisected yaw_accumulators >> 8)

## Trigger scenario

Memory `reference_obb_corner_test_rotation_sign` cites **Jarash (track=4) PlayerIsAI=1 spawn** as the loudest fail-case for D1. The visible slide pattern emerges at sim_tick 1 from stationary V2V tests, with two slots side-by-side.

Recommended `--DefaultTrack=4 --PlayerIsAI=1 --DefaultCar=0 --StartSpanOffset=0 --AutoRace=1 --RaceTraceMaxSimTicks=60 --RaceTraceMaxFrames=1200`.

Session 13 (pool14) needs the SAME scenario (V2V impulse on the contact data that this pilot generates). Coordinate capture order — only one TD5_d3d.exe at a time per the standard precise-port serial-capture protocol.

## Next actions

1. Build pilot trace `td5_pilot_trace_v2v_contact.{c,h}`. Hook `obb_corner_test` at entry/exit (8 calls per ResolveVehicleCollisionPair). Hook `collision_detect_full` at entry and just-before-dispatch.
2. Author `tools/frida_pool15_v2v_contact.js`. Attach at 0x00408570 (capture call_idx by call sequence within a frame) and 0x00408A60.
3. Add to `TD5RE_SRCS`. Build worktree.
4. Wait for pool14 (session 13) capture to finish OR coordinate to share a single capture window (both scripts attached to one race).
5. Original-side: Jarash PlayerIsAI=1 trace with `--extra-script tools/frida_pool15_v2v_contact.js`.
6. Port-side: same scenario, post-build worktree exe.
7. Diff with `--key=sim_tick,which_fn,event_idx`.

## Definition of done

Zero diff on the 8 × 4 corner record output of `obb_corner_test` + the `impactForce_final + dispatched_bitmask + final_test_ha/hb` outputs of `collision_detect_full` at sim_tick ≥ 5 once V2V fires.

Expected fixes that close the diff:
- D1 (rotation sign) — single-line flip × 2 — clears the bitmask divergence on every contact.
- D2 (AABB ≤ vs <) — 4 predicate flips — closes the edge-equality divergence.
- D3+D4 (round-to-zero idiom in bisection halving) — `sar1_rz` helper — closes 1-LSB drift on negative velocities/yaw rates.

D5/D6/D7/D8/D9 are confirmed-faithful or no-op — no port edits required.

## Reference

- Listing 0x00408570..0x00408A5D — TD5_pool15, 2026-05-14
- Listing 0x00408A60..0x00408EC4 — same session
- Decompilation: same session
- Port: `.claude/worktrees/precise-v2v-contact/td5mod/src/td5re/td5_physics.c:2417` and `:2974`
- Memory: `reference_v2v_contact_data_layout` (close-to-current), `reference_obb_corner_test_rotation_sign` (claims fixed, stale)
- Memory: `todo_v2v_clipping_partial_fix` (TOI bisection + contactData layout re-verified 2026-05-02)
