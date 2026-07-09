# Pilot Audit — 0x0042EBF0 ComputeVehicleSurfaceNormalAndGravity

**Date:** 2026-05-14
**Pool slot:** TD5_pool7
**Port-side function:** `td5_physics_compute_surface_gravity` @ `td5_physics.c:6324`
**Worktree:** `.claude/worktrees/precise-0042EBF0` on branch `precise-0042EBF0`
**Callers:** UpdatePlayerVehicleDynamics (0x00404030), UpdateAIVehicleDynamics (0x00404EC0).
**Callees:**
  - `StoreRoundedVector3Ints` @ 0x0042CCD0 (Ghidra mislabel — actually `NormalizeVec3iToConstantMagnitude4096`)
  - `CrossProduct3i_FixedPoint12` @ 0x0042EAC0
**Body:** 0x0042EBF0..0x0042ED47 (0x158 bytes / 102 instructions / ~30 decompiled lines).

## Function structure (from listing 0x0042EBF0–0x0042ED47)

```
// Phase 1 — Two diagonal-difference vectors of the 4 body probes (offsets +0x90..+0xBF).
// Each diagonal is the sum/difference of all 4 probes along one axis.

v1 = { (FL.x - FR.x - RR.x + RL.x) >> 8,           // SAR EBX,8 @ 0x42ec27 → [ESP+0x1c]
       (FL.y - FR.y - RR.y + RL.y) >> 8,           // SAR EBP,8 @ 0x42ec4c → [ESP+0x20]
       (FL.z - FR.z - RR.z + RL.z) >> 8 }          // SAR EBP,8 @ 0x42ec65 → [ESP+0x24]

v2 = { (FL.x - RR.x - RL.x + FR.x) >> 8,           // SAR EAX,8 @ 0x42ec86 → [ESP+0x10]
       (FL.y - RR.y - RL.y + FR.y) >> 8,           // SAR ECX,8 @ 0x42ec9d → [ESP+0x14]
       (FL.z - RR.z - RL.z + FR.z) >> 8 }          // SAR EDX,8 @ 0x42ecb7 → [ESP+0x18]

// Phase 2 — Normalize both diagonals to length 4096 (constant at 0x0046736C = 4096.0f).
// This is the call mislabelled "StoreRoundedVector3Ints" but actually performs:
//   - FILD each of the 3 ints → ST
//   - sum = x*x + y*y + z*z         (float)
//   - scale = 4096.0f / sqrt(sum)   (float, FSQRT then FDIVR)
//   - foreach component: store __ftol(scale * component) back as int
// The FPU sequence is at 0x0042CCD0 + uses MSVC __ftol (FLDCW with RC=11
// truncate-toward-zero, FISTP qword, restore CW).
StoreRoundedVector3Ints(&v1, &v1);                 // CALL @ 0x42ecbf, in-place
StoreRoundedVector3Ints(&v2, &v2);                 // CALL @ 0x42ecd0, in-place

// Phase 3 — Cross product of the two unit-length-4096 vectors:
//   cross[0] = (v2.z * v1.y - v1.z * v2.y) >> 12   // local_c[0]
//   cross[1] = (v1.z * v2.x - v1.x * v2.z) >> 12   // local_c[1]  (UNUSED)
//   cross[2] = (v1.x * v2.y - v2.x * v1.y) >> 12   // local_4
CrossProduct3i_FixedPoint12(&v1, &v2, &cross[0]);  // CALL @ 0x42ece6

// Phase 4 — Project g onto X and Z body axes.
//   IMUL with gGravityConstant (4-byte @ 0x00467380; static = 0x05DC = 1500).
//   CDQ; SUB EAX,EDX; SAR EAX,1   → round-toward-zero /2
//   CDQ; AND EDX,0xfff; ADD EAX,EDX; SAR EAX,0xc → round-toward-zero /4096
//   ADD to actor->linear_velocity_x (+0x1cc) and linear_velocity_z (+0x1d4).
//   (linear_velocity_y +0x1d0 is NOT touched here — handled by full-axis gravity.)

g = *(int *)0x00467380;
actor->linear_velocity_x += (((g * cross[0]) / 2) + sgn_bias) >> 12;
actor->linear_velocity_z += (((g * cross[2]) / 2) + sgn_bias) >> 12;
```

`sgn_bias = (negative ? 0xfff : 0)` — the same round-toward-zero idiom used in
`sar8_rz` / `sar12_rz` elsewhere in the engine, except here for SAR-12 the
bias is `0xfff` not `0xff`.

## Confirmed divergences from port (pre-fix)

### D1 — MISSING NORMALIZE step (HIGH IMPACT)
Port skips the two `NormalizeVec3iToConstantMagnitude4096` calls entirely.

Port at `td5_physics.c:6341-6348` computes raw integer diagonals `v1`, `v2`
(both at world-unit scale after `>> 8`), then immediately calls cross product
on the raw vectors with `>> 12`. The original normalizes both diagonals to a
length of 4096 first.

**Impact:**
- The original's cross product output is bounded by ~4096*4096/4096 = 4096 ×
  sin(angle between diagonals).  For a flat surface the angle stays close to
  90°, so the cross magnitude is ~4096 — a unit vector at 12-bit FP.
- The port's cross magnitude is `|v1| * |v2| * sin(theta) / 4096`. With raw
  diagonals at world-unit scale (typically hundreds to low thousands), this
  is `O(diag^2 / 4096)` — varies wildly with vehicle attitude.

The port's surface-normal cross result has the WRONG SCALE and the WRONG
DEPENDENCE on overall probe-square area. On a flat surface with all 4 probes
roughly in a plane, the magnitudes of v1, v2 (raw, pre-normalize) grow as the
vehicle moves over rougher terrain; the port's "gravity projection" effectively
grows with how rough the terrain is, which is the opposite of the desired
behaviour. On a perfectly flat surface, both diagonals can be near-zero
(probes nearly equidistant), and the port produces ~0 gravity projection
when the original produces a proper ±gravity slope component.

**Fix:** port the `NormalizeVec3iToConstantMagnitude4096` helper byte-faithfully
(at minimum: same FPU sum-of-squares, FSQRT, FDIVR pattern, with __ftol-style
truncate-toward-zero on each output component).

### D2 — Cross-product sign factoring (NO IMPACT, sanity check passed)
Original `CrossProduct3i_FixedPoint12` writes `cross[0] = (B.z * A.y - A.z * B.y) >> 12`.
Port computes `nx = (v1y * v2z - v1z * v2y) >> 12`. Since `B.z * A.y - A.z * B.y = A.y * B.z - A.z * B.y`,
these are arithmetically identical. Same goes for `nz`. ✓

### D3 — round-toward-zero on the /2 (NO IMPACT, identical result)
Original uses two-step round-toward-zero `CDQ; SUB EAX,EDX; SAR 1`.
Port uses C99 `/ 2`. Both round toward zero for signed ints, so the result is
identical bit-for-bit. ✓

### D4 — round-toward-zero on the /4096 (NO IMPACT, identical result)
Port has `(ax + ((ax >> 31) & 0xfff)) >> 12`. Original has the equivalent
`CDQ; AND EDX,0xfff; ADD EAX,EDX; SAR EAX,0xc`. Identical. ✓

### D5 — `gGravityConstant` binding upstream (BLOCKED, out of scope)
The static value at 0x00467380 in the binary image is 1500 (TD5_GRAVITY_EASY).
The port reads from `g_gravity_constant` which initializes to 1900 (TD5_GRAVITY_NORMAL)
and is changed to easy/normal/hard in `td5_physics_init_vehicle_runtime` based
on `g_difficulty_easy / g_difficulty_hard` flags. This binding is correct for
the port, but the static 1500 in the original suggests the original initializes
gGravityConstant from a similar configuration step. The actual runtime value
in the original is also set from difficulty per
`reference_edinburgh_floating_audit.md`. **Out of scope for this function** —
verify via Frida by reading `[0x00467380]` at the moment of the call.

## Capture schema

Per call (one row per call, no per-wheel sub-rows since this function is
chassis-scoped):

**Keys:** `sim_tick`, `slot`, `caller_ra`
**Inputs (snap at entry):**
- `actor_addr` (hex, sanity)
- `probe_FL_x/y/z`, `probe_FR_x/y/z`, `probe_RL_x/y/z`, `probe_RR_x/y/z`
- `g_grav` (read from [0x00467380] on the original side; `g_gravity_constant` on port)
- `lin_vel_x_in`, `lin_vel_z_in`

**Intermediates (snap on leave, computed from inputs by both sides):**
- `v1_x_pre, v1_y_pre, v1_z_pre`  (pre-normalize diagonal-1, port can emit; original needs an additional probe)
- `v1_x_post, v1_y_post, v1_z_post`  (post-normalize, length=4096 vector)
- `v2_x_pre, v2_y_pre, v2_z_pre`  (pre-normalize diagonal-2)
- `v2_x_post, v2_y_post, v2_z_post`
- `cross_x, cross_z` (cross.y ignored)

**Outputs:**
- `lin_vel_x_out`, `lin_vel_z_out`
- `dvel_x = lin_vel_x_out - lin_vel_x_in`
- `dvel_z = lin_vel_z_out - lin_vel_z_in`

Total: ~30 columns. For simplicity, capture only the inputs + outputs first;
add the v1/v2 intermediates only if the output diverges and we need to
localize which phase is wrong (Phase 1 sum, Phase 2 normalize, Phase 3 cross,
Phase 4 project).

## Initial fix plan

1. **Implement `td5_physics_normalize_vec3_to_4096`** — port `StoreRoundedVector3Ints`
   byte-faithfully. Use `float` intermediates + `lrintf` truncated toward zero
   (or explicit FPU sequence if `lrintf` rounding differs). Verify on a few
   sample inputs that the result matches the original within 1 LSB.

2. **Rewrite the port** to:
   ```c
   int32_t v1[3] = { ..., ..., ... };  // diagonal 1 >> 8
   int32_t v2[3] = { ..., ..., ... };  // diagonal 2 >> 8
   normalize_vec3_to_4096(v1);
   normalize_vec3_to_4096(v2);
   int32_t cross_x = (v1[1] * v2[2] - v1[2] * v2[1]) >> 12;
   int32_t cross_z = (v1[0] * v2[1] - v2[0] * v1[1]) >> 12;
   /* … gravity projection as before, already byte-correct … */
   ```

3. **Frida probe + port emitter** capturing inputs (4 probes + g) and outputs
   (linear_velocity delta), so the diff tool can confirm zero divergence.

## Next actions

1. Write `tools/frida_pool7_0042EBF0.js` to capture entry+exit on the original.
2. Write `td5mod/src/td5re/td5_pilot_trace_0042EBF0.{c,h}` (worktree only).
3. Implement the normalize helper byte-faithfully and rewrite the port body.
4. Add `td5_pilot_trace_0042EBF0.c` to `TD5RE_SRCS`.
5. Build worktree.
6. Capture original + port traces (Edinburgh slot 0).
7. Diff with `tools/diff_func_trace.py --min-tick=5 --max-tick=30`.
8. Iterate until zero divergence.
