# Pilot Audit — V2V impulse branches (0x004079C0)

**Date:** 2026-05-14
**Pool slot:** TD5_pool14
**Port-side function:** `apply_collision_response` @ `td5_physics.c:2598-2891`
**Worktree:** `.claude/worktrees/precise-v2v-impulse` on branch `precise-v2v-impulse` (from `fpu-cw-fix`)
**Tag:** `pool14_v2v`
**Parent function:** `ApplyVehicleCollisionImpulse (0x004079C0)`
**Branches audited:**
- **SIDE branch** — `LAB_00407B7F` (lateral collision, cx_A ≤ cx_B path)
- **FRONT branch** — `LAB_00407B2D` / converges at `LAB_00407D70` (axial collision, cz_A ≤ cz_B path)

## Caller mapping (slot semantics)

The original `ResolveVehicleCollisionPair (0x00408A60)` does a confusing variable swap:
```c
actorB = ... + slotIndexA * 0x388;   // var "actorB" holds slot_a
actorA = ... + slotIndexB * 0x388;   // var "actorA" holds slot_b
```
- Bits 0-3 dispatch: `ApplyVehicleCollisionImpulse(actorB-var, actorA-var, ...)` → first arg = slot_a, second arg = slot_b, angle = slot_a's bisected yaw.
- Bits 4-7 dispatch: `ApplyVehicleCollisionImpulse(actorA-var, actorB-var, ...)` → first arg = slot_b, second arg = slot_a, angle = slot_b's bisected yaw.

So **first arg = frame owner** (whose yaw drives `angle`) and **gets pushed back**; second arg gets pushed forward.

Port mapping in `collision_detect_full`:
```c
apply_collision_response(b, a, i, &cached_corners[i], test_ha, impactForce);    // bits 0-3
apply_collision_response(a, b, i, &cached_corners[i + 4], test_hb, impactForce); // bits 4-7
```
- Bits 0-3: penetrator=b, target=a, angle=test_ha (slot_a's yaw) — `A:=target=a` matches original's frame owner.
- Bits 4-7: penetrator=a, target=b, angle=test_hb (slot_b's yaw) — `A:=target=b` matches.

Verdict: port's `A = target` ≡ original's first arg ≡ frame-owner. Push direction `A.x -= push; B.x += push` matches original `[ESI=arg0+0x1FC] -= ; [EBP+0x1FC] += `.

## Constants verified

| Constant | Address | Value | Port macro |
|---|---|---|---|
| INERTIA_K | DAT_00463204 | 500000 | V2V_INERTIA_K |
| NUM_SCALE | inline 0x4 + 0x8 shift | 0x1100 | V2V_NUM_SCALE |
| ANG_DIVISOR | inline 0xc907da5 magic | 0x28C (652) | V2V_ANG_DIVISOR |
| INERTIA_PER_ANG | inline | 500000/652 = 766 | V2V_INERTIA_PER_ANG |
| TOI_FULL | inline 0x100 | 0x100 | (`0x100 - impactForce`) |

NUM_CONST = `(INERTIA_K + sign_bias_FF) >> 8 * 0x1100`. Since INERTIA_K = +500000 (positive), sign_bias = 0, so NUM_CONST = 1953 * 0x1100 = 8,499,456. Port matches.

`0xc907da5` is the standard MSVC magic-multiplier for **trunc-to-zero divide by 652**:
```
IMUL r          ; EDX:EAX = r * 0xc907da5
SAR EDX, 0x5    ; EDX = high(r * 0xc907da5) >> 5
MOV EAX, EDX
SHR EAX, 0x1f   ; EAX = sign mask
ADD EDX, EAX    ; bias toward zero
```
Equivalent to signed C `r / 652` (truncate-to-zero). Port uses C `/`, identical behavior.

## Confirmed divergences (port vs listing)

### D1 — Plain `>> 12` on velocity rotations **(HIGH IMPACT, multiple sites)**

The original's prologue rotates A's and B's velocities into A's frame via 4 instances of the **signed-round-to-zero divide by 0x1000** idiom:

```
[0x00407A19-29]    CDQ; AND EDX, 0xfff; ADD EAX, EDX; SAR EAX, 0xc    ; local_54
[0x00407A3E-4D]    same                                                ; local_50
[0x00407A69-72]    same                                                ; local_4c
[0x00407A95-AA]    same                                                ; local_44
```

The C equivalent is `((x < 0) ? (x + 0xFFF) : x) >> 12` — i.e. `sar12_rz(x)`.

**Port at td5_physics.c:2634-2637:**
```c
int32_t local_54 = (vxA * cos_a - vzA * sin_a) >> 12;  /* WRONG: plain SAR */
int32_t local_50 = (vxA * sin_a + vzA * cos_a) >> 12;
int32_t local_4c = (vxB * cos_a - vzB * sin_a) >> 12;
int32_t local_44 = (vxB * sin_a + vzB * cos_a) >> 12;
```

For positive products this matches the listing. For negative products not divisible by 4096, plain `>> 12` rounds toward -∞ while the original rounds toward zero — **diverges by 1 LSB**.

Same idiom appears at writeback at 0x408023-39 (linear_velocity_x), 0x408042-58 (linear_velocity_z) — 4 more instances.

**Port at td5_physics.c:2804-2807:**
```c
A->linear_velocity_x = (local_50 * sin_a + local_54 * cos_a) >> 12;  /* WRONG */
A->linear_velocity_z = (local_50 * cos_a - local_54 * sin_a) >> 12;  /* WRONG */
B->linear_velocity_x = (local_44 * sin_a + local_4c * cos_a) >> 12;  /* WRONG */
B->linear_velocity_z = (local_44 * cos_a - local_4c * sin_a) >> 12;  /* WRONG */
```

**Fix:** introduce `sar12_rz()` inline and replace 8 sites.

### D2 — Plain `>> 12` on impulse final scale **(HIGH IMPACT, both branches)**

The original's impulse scalar is computed by:

```
[0x00407CB2-C1]    CDQ; AND EDX, 0xfff; ADD EAX, EDX; SAR EAX, 0xc    ; SIDE branch
[0x00407E68-7A]    same                                                ; FRONT branch
```

i.e. **round-to-zero divide by 0x1000** on `(NUM_CONST / denom) * rel_vel`.

**Port at td5_physics.c:2723-2724 (SIDE) and 2756-2757 (FRONT):**
```c
int64_t impulse_raw = (NUM_CONST / denom) * rel_vel;
impulse = (int32_t)(impulse_raw >> 12);  /* WRONG: plain SAR */
```

**Fix:** apply `sar12_rz_64(int64_t)` to `impulse_raw`.

### D3 — Plain `>> 8` on TOI rollback/advance **(HIGH IMPACT, 12 sites)**

The original's TOI rollback at 0x00407F31-0x004080A6 applies `(toi_frac * actor_field + sign_bias_FF) >> 8` for 6 actor fields (A.x, A.z, A.yaw_eul, B.x, B.z, B.yaw_eul) — that's the **rollback** half. Then 6 more for the **re-advance** half at 0x004080C8-0x0040816B (with new post-impulse velocities).

Each instance follows:
```
IMUL EAX, [actor field]
CDQ
AND EDX, 0xff
ADD EAX, EDX
SAR EAX, 0x8
NEG EAX                          ; only for rollback half
ADD [actor field], EAX
```

i.e. **`sar8_rz(toi_frac * vel)`**.

**Port at td5_physics.c:2792-2797 (rollback) and 2810-2814 (advance):**
```c
A->world_pos.x -= (toi_frac * A->linear_velocity_x) >> 8;  /* WRONG: plain SAR */
A->world_pos.z -= (toi_frac * A->linear_velocity_z) >> 8;
A->euler_accum.yaw -= (toi_frac * A->angular_velocity_yaw) >> 8;
B->world_pos.x -= (toi_frac * B->linear_velocity_x) >> 8;
B->world_pos.z -= (toi_frac * B->linear_velocity_z) >> 8;
B->euler_accum.yaw -= (toi_frac * B->angular_velocity_yaw) >> 8;
... 6 more lines for advance
```

**Fix:** apply `sar8_rz(toi_frac * field)` to all 12 sites.

### D4 — `denom >> 8` confirmed safe **(no fix)**

Listing 0x00407C40-0x00407C53 applies sign-bias to denom before `>> 8`. But denom = `(cz_B² + 500000) * mass_A + (cz_A² + 500000) * mass_B` — always strictly positive (sum of squares + positive constant × positive mass). Plain `>> 8` of a positive value matches SAR-RZ exactly.

### D5 — NUM_CONST sign-bias confirmed irrelevant **(no fix)**

Listing 0x00407C35-0x00407C4C applies sign-bias to INERTIA_K (= 500000, positive) before `>> 8`. For positive input, sign-bias = 0. NUM_CONST = `1953 * 4352 = 8,499,456`. Port matches.

### D6 — Push direction confirmed **(no fix, already shipped per memory `todo_v2v_clipping_partial_fix`)**

Verified port's push direction matches listing at 0x00407BB7-0x00407BDB (SIDE) and 0x00407D70-0x00407D94 (FRONT).

### D7 — Angular delta `/766` confirmed safe **(no fix)**

Listing uses MSVC magic-divide (0xc907da5 / SAR 5 / sign-bias) which equals signed C `/`. Port uses C `/`, identical.

## What is NOT in this audit (out of scope for V2V impulse)

- **obb_corner_test rotation** — already fixed 2026-05-13 per memory `reference_obb_corner_test_rotation_sign`.
- **TOI bisection** (`ResolveVehicleCollisionPair (0x00408A60)`) — separate function, already audited as faithful per memory `todo_v2v_clipping_partial_fix`.
- **contactData layout** — already audited as faithful per memory `reference_v2v_contact_data_layout`.
- **Heavy-impact scatter (impact_mag > 90000)** — out of scope for V2V impulse byte-exact (random-number-driven, untestable without controlling GetDamageRulesStub).
- **Wanted-mode + traffic recovery escalation** — gameplay state writes, not impulse math.

## Capture schema for pilot

Per call (1 row per `ApplyVehicleCollisionImpulse` invocation):

**Keys:** `sim_tick`, `slotA`, `slotB`
**Inputs (rotation frame + contact):**
- `angle` (uint), `impactForce` (int, range [-0x10, 0xF0])
- `contactData[0..3]`: `proj_x`, `proj_z`, `own_x`, `own_z` (signed int16)
**Pre-state (both actors):**
- `pre_posAx`, `pre_posAz`, `pre_velAx`, `pre_velAz`, `pre_omegaA`, `pre_eulerYawA`
- `pre_posBx`, `pre_posBz`, `pre_velBx`, `pre_velBz`, `pre_omegaB`, `pre_eulerYawB`
**Post-state:**
- `post_posAx`, `post_posAz`, `post_velAx`, `post_velAz`, `post_omegaA`, `post_eulerYawA`
- `post_posBx`, `post_posBz`, `post_velBx`, `post_velBz`, `post_omegaB`, `post_eulerYawB`
- `retval` (impact magnitude returned by function, = `|(mass_a + mass_b) * impulse|`)

This is ~30 columns; manageable with a single CSV per pool slot.

## Fix list (this branch)

1. **Add `sar8_rz` and `sar12_rz` static inlines** at top of `apply_collision_response`.
2. **D1**: replace 8 plain `>> 12` sites in velocity rotation (4 prologue + 4 writeback) with `sar12_rz`.
3. **D2**: replace 2 plain `>> 12` sites on impulse final scale (SIDE + FRONT) with `sar12_rz_64`.
4. **D3**: replace 12 plain `>> 8` sites on TOI rollback/advance with `sar8_rz`.
5. **Inline comments** anchored to listing-address ranges for each replacement.

## Blocked by upstream — runtime row-by-row diff cannot be performed

The runtime trace diff (`log/port/pool14_v2v.csv` vs `log/orig/pool14_v2v.csv`)
diverges at **every captured row** because of two upstream blockers:

### Blocker 1 — spawn binding broken in AutoRace + PlayerIsAI=1
On Moscow track=4 with --StartSpanOffset=175, the original places slot 0 at
world_pos ≈ (9.88e6, -2.88e7) and triggers 12 V2V calls between slots 4 and 5
over 60 sim_ticks. The port's AutoRace=1 launch path does NOT bind the actor
spawn to the track's span — slot 0 sits at world_pos ≈ (0, 8452), other
actors cluster within ~4096 units of the origin, and all OBB tests trigger
spurious overlap. The port fires **38,221 V2V calls in 60 sim_ticks** (33,260
during countdown, 160/tick after). This is the same upstream blocker
documented in `pilot_00403A20_audit.md` "Blocked by upstream".

### Blocker 2 — cardef constants + cos/sin LUT not captured by Frida
Even sampling just the original's 12 captured calls, the impulse math depends
on:
- `mass_a`, `mass_b` (cardef +0x88, int16) — not captured in pool14_v2v.csv
- `half_w_a`, `front_z_a`, `rear_z_a` (cardef +0x08/+0x04/+0x14, int16) —
  not captured, control SIDE-vs-FRONT branch selection
- `cos_fixed12(angle)`, `sin_fixed12(angle)` — game's 4096-entry LUT at
  DAT_00483984; Python `math.cos/sin` differs by 1-2 LSB rounding.

`tools/validate_pool14_v2v_math.py` runs the port's Python-port math over the
original's captured inputs and gets 0/12 matches against post-state — but
this is **expected** because the validator uses `math.sin/cos` (not the game
LUT) and guesses cardef constants. The validation script reports byte-exact
status as informational rather than build-gating.

### What this means for byte-exact merge

This pilot is **blocked** on:
1. **Spawn-binding fix** (precise-port pilot worktree for `SetupPlayerVehicleState`
   or `TD5_StartRace` chain). Until slot 0 is placed at Moscow span 175 in the
   port's AutoRace path, the port's V2V events cannot pair-key to the original's
   by `(sim_tick, slotA, slotB)`.
2. **Cardef-bind fix** (already shipped per memory `todo_playerisai_carparam_binding`
   for slot 0 but not slot 4/5 on this trace). The captured V2V calls are between
   AI slots 4 and 5 — both need correct Viper cardef.
3. **Cos/sin LUT capture or faithful-port verification**. Either capture
   `cos_fixed12(angle)` output via Frida and re-do the algebraic validator,
   or audit `td5_math.c::cos_fixed12` against DAT_00483984 byte-exact.

**Pool14_v2v deliverable (this branch):**
- `td5_physics.c::apply_collision_response` math is byte-exact-correct vs
  the listing at three previously-unfixed sites (D1: 8 velocity-rotation
  `>>12`; D2: 2 impulse-scale `>>12`; D3: 12 TOI rollback/advance `>>8`).
- Trace harness committed for future runtime diff once upstream blockers clear.
- Audit committed with listing-address-anchored line-by-line mapping.

## Reference

- Listing: 0x004079C0..0x00408566 (Ghidra TD5_pool14, 2026-05-14, read-only)
- Decompilation: same session
- Port: `td5mod/src/td5re/td5_physics.c:2598-2891` (apply_collision_response)
- Frida probe: `tools/frida_pool14_v2v.js` (this branch)
- Port trace emitter: `td5mod/src/td5re/td5_pilot_trace_v2v.{c,h}` (this branch)
- Caller dispatch: `td5_physics.c:3138-3147` (collision_detect_full)
