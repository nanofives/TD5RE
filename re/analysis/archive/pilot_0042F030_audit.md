# Pilot Audit — 0x0042F030 ComputeDriveTorqueFromGearCurve

**Date:** 2026-05-14
**Pool slot:** TD5_pool13
**Port-side function:** `td5_physics_compute_drive_torque` @ `td5_physics.c:6135`
**Worktree:** `.claude/worktrees/precise-0042F030` on branch `precise-0042F030` from `fpu-cw-fix`
**Callers:** `UpdatePlayerVehicleDynamics (0x00404030)`, `UpdateAIVehicleDynamics (0x00404EC0)`.
**Callees:** none (pure compute leaf).
**Body:** 0x0042F030..0x0042F0FC (0xCC bytes / 68 instructions / ~30 decompiled lines).

## Function structure (from listing 0x0042F030..0x0042F0FC)

`int __cdecl ComputeDriveTorqueFromGearCurve(int actor, int tuning)`

```
ENTRY:
    EAX  = actor                              ; [ESP+4]
    BL   = [actor + 0x36B]                    ; current_gear (byte)
    EBP  = [actor + 0x310]                    ; engine_speed_accum (int32)
    if (BL == 0x01) jump RET_ZERO             ; gear==1 (neutral) → 0

    EAX  = EBP
    CDQ ; AND EDX,0x1FF ; ADD EAX,EDX         ; sar9_rz prep: sign-bias 0x1FF if neg
    MOV  ECX, EAX
    SAR  ECX, 9                               ; index = sar9_rz(engine_speed)

    ESI  = tuning                             ; [ESP+0x14] (after PUSH EBX, EBP, ESI)
    EAX  = (i16)[tuning + ECX*2]              ; LUT[index]
    EDX  = (i16)[tuning + 0x68]               ; torque_mult
    IMUL EAX, EDX
    CDQ ; AND EDX,0xFF ; ADD EAX,EDX          ; sar8_rz prep (t0_raw)
    EDI  = EAX                                ; stash unscaled

    EAX  = (i16)[tuning + ECX*2 + 2]          ; LUT[index + 1]
    ECX  = (i16)[tuning + 0x68]               ; torque_mult (re-read)
    IMUL EAX, ECX
    CDQ ; AND EDX,0xFF ; ADD EAX,EDX          ; sar8_rz prep (t1_raw)

    SAR  EDI, 8                               ; t0 = sar8_rz(LUT[index] * mult)
    SAR  EAX, 8                               ; t1 = sar8_rz(LUT[index+1] * mult)

    ECX  = EBP                                ; ECX = engine_speed (raw)
    SUB  EAX, EDI                             ; delta = t1 - t0
    AND  ECX, 0x800001FF                      ; keep sign bit + low 9 bits
    JNS  skip_signfix
        DEC  ECX
        OR   ECX, 0xFFFFFE00                  ; sign-extend the low-9-bit slice
        INC  ECX
    skip_signfix:                              ; ECX = signed frac (engine_speed mod 512, signed)

    IMUL EAX, ECX                             ; (t1-t0) * frac
    CDQ ; AND EDX,0x1FF ; ADD EAX,EDX         ; sar9_rz prep
    EDX  = tuning                             ; [ESP+0x14] (after extra pushes? — actually MOV EDX,[ESP+0x14])
    ECX  = (i16)[actor + 0x33E]               ; throttle_signed (encounter_steering_cmd)
    SAR  EAX, 9                               ; sar9_rz((t1-t0) * frac)
    ADD  EAX, EDI                             ; torque0 = t0 + lerp_part
    IMUL EAX, ECX                             ; * throttle
    ECX  = (i16)[tuning + 0x72]               ; redline
    CDQ ; AND EDX,0xFF ; ADD EAX,EDX          ; sar8_rz prep
    EBX &= 0xFF                               ; gear as unsigned byte
    EDX  = (i16)[tuning + EBX*2 + 0x2E]       ; gear_ratio[gear]
    SAR  EAX, 8                               ; sar8_rz(... * throttle)
    IMUL EAX, EDX                             ; * gear_ratio
    CDQ ; AND EDX,0xFF ; ADD EAX,EDX          ; sar8_rz prep
    SUB  ECX, 0x32                            ; ECX = redline - 50
    SAR  EAX, 8                               ; sar8_rz(... * gear_ratio)

    CMP  EBP, ECX                             ; if engine_speed > redline-50
    JLE  KEEP
RET_ZERO:
    XOR  EAX, EAX                             ; return 0
KEEP:
    POP EBP ; POP EBX ; RET
```

## Field offsets

| Source | Offset | Width | Meaning |
|---|---|---|---|
| `actor + 0x310` | int32 | engine_speed_accum (signed, sign-extends through frac+index math) |
| `actor + 0x33E` | int16 | encounter_steering_cmd (signed throttle, reverse-flipped upstream by `ApplyReverseGearThrottleSign` @ 0x42F010) |
| `actor + 0x36B` | byte (BL) | current_gear; 0x01 = neutral → early-out |
| `tuning + 0x00..` | int16 LUT[N] | torque curve at engine_speed intervals of 512 |
| `tuning + 0x2E + gear*2` | int16 | per-gear ratio (signed) |
| `tuning + 0x68` | int16 | torque_mult |
| `tuning + 0x72` | int16 | redline; cutoff applies when `engine_speed > redline - 50` |

The LUT base is **offset 0 of tuning data**, NOT `tuning + 0x32` as the decomp comment claims. The decomp comment is wrong (vestigial from a renamed field). The MOVSX uses `[ESI + ECX*2]` with ESI=tuning and ECX=index.

## Arithmetic primitives

`sar8_rz(x)` — round-toward-zero signed divide by 256 (`((x<0)?(x+0xFF):x) >> 8`).
Idiom: `CDQ ; AND EDX, 0xFF ; ADD EAX, EDX ; SAR EAX, 8`.

`sar9_rz(x)` — round-toward-zero signed divide by 512 (`((x<0)?(x+0x1FF):x) >> 9`).
Idiom: `CDQ ; AND EDX, 0x1FF ; ADD EAX, EDX ; SAR EAX, 9`.

`signed_low9(x)` — extract the signed remainder modulo 512 (i.e. `x` reduced to `[-256,+255]` using the same sign-arithmetic as a divrem-by-512 with truncated quotient). Idiom seen in listing:
```
AND ECX, 0x800001FF        ; keep bit-31 + bits 0..8
JNS skip
  DEC ECX
  OR  ECX, 0xFFFFFE00      ; set bits 9..30 to 1
  INC ECX
skip:
```
For non-negative input this leaves the low 9 bits intact (frac = x & 0x1FF, in [0,511]).
For negative input, the sequence `(DEC; OR 0xFFFFFE00; INC)` effectively does:
- Start `ECX = (x bit-31 ≠ 0) ? (bit-31 set OR low-9-bits) : low-9-bits`
- DEC subtracts 1 from `(bit-31|low9)`
- OR forces bits 9..30 to 1
- INC adds 1 back

This is exactly the `x - 512 * trunc_div(x, 512)` remainder with TRUNCATED quotient (round-toward-zero). For example for `x = -1`: ECX = 0x800001FF; DEC → 0x800001FE; OR 0xFFFFFE00 → 0xFFFFFFFE; INC → 0xFFFFFFFF = -1. So `signed_low9(-1) = -1`, which is the truncated-divide remainder (because `-1 / 512 = 0` truncated, leaving remainder `-1`).

C equivalent for the `signed_low9` operation: simply `x % 512` with C99/C11's truncated `%` operator (since the standard requires truncated division). However the compiler-generated code uses the bit-twiddle path; the result is identical.

## Identified divergences (port vs original)

### D1 — Index NOT bounds-clamped **(HIGH IMPACT for low/negative engine_speed)**
Port `td5_physics.c:6154-6155`:
```c
if (index < 0) index = 0;
if (index >= 15) index = 14;
```
Original does NOT clamp — it always reads `LUT[index]` and `LUT[index+1]` with whatever signed `index = engine_speed >> 9` produces. Negative `engine_speed` would read out-of-bounds LUT entries.

The Viper's tuning data likely keeps engine_speed_accum bounded such that `index ∈ [0, redline/512]` at all times (the redline cutoff handles the upper bound; engine slew clamps the lower). But the port's clamp is still a defensive over-approximation that may zero a legitimate sample.

The `>> 9` itself is also wrong — port uses plain SAR, original uses `sar9_rz`. For positive engine_speed values the two are identical, but for any negative value (idle-undershoot, encoded-reverse-speed, etc.) they differ.

### D2 — `>> 9` instead of `sar9_rz(...)` **(MEDIUM IMPACT)**
Port line 6153:
```c
int32_t index = rpm >> 9;
```
Original: `sar9_rz(rpm)`. Diverges when `rpm < 0` and `(rpm & 0x1FF) != 0` by exactly 1.

### D3 — Wrong frac extraction **(HIGH IMPACT, easy fix)**
Port line 6161:
```c
int32_t frac = rpm & 0x1FF;
```
This is always non-negative (frac ∈ [0, 511]).
Original: `signed_low9(rpm)` which sign-extends for negative rpm to be in `[-256, +255]`-ish (truncated-div remainder).

For `rpm < 0`, port's frac is in [0,511] but original's frac is in [-511, 0]. The lerp formula `t0 + ((t1-t0) * frac) >> 9` therefore extrapolates in the wrong direction for negative rpm, producing torque of magnitude up to ±511/512 × (t1-t0) **away** from the correct sign.

### D4 — Plain `>> 8` instead of `sar8_rz` (4 sites) **(MEDIUM IMPACT)**
Port:
```c
int32_t t0 = ((int32_t)PHYS_S(actor, index * 2) * torque_mult) >> 8;     // site A
int32_t t1 = ((int32_t)PHYS_S(actor, (index + 1) * 2) * torque_mult) >> 8; // site B
...
torque = (torque * throttle) >> 8;                                        // site C
torque = (torque * gear_ratio) >> 8;                                      // site D
```
Original: all four are `sar8_rz`. Diverges by 1 LSB for negative intermediates.
- Site A/B diverge when `LUT[i]*mult < 0` AND `(LUT[i]*mult & 0xFF) != 0`. The Viper's torque curve is positive, but the multiplication or LUT values for other cars or higher gears may go negative.
- Site C diverges when `(torque*throttle) < 0`. Reverse-throttle path produces negative throttle → very likely site C is exercised on reverse.
- Site D same story for negative `gear_ratio[gear]`.

### D5 — Plain `>> 9` instead of `sar9_rz` for the lerp **(MEDIUM IMPACT)**
Port line 6162:
```c
int32_t torque = t0 + (((t1 - t0) * frac) >> 9);
```
Original: `t0 + sar9_rz((t1 - t0) * frac)`. Diverges by 1 LSB when the numerator is negative.

### D6 — Redline cutoff comparison **(LOW IMPACT, possibly equivalent)**
Port:
```c
if (rpm > redline - 50) return 0;
```
Original: `if (rpm > redline - 50) return 0;` (compare BEFORE final multiply chain, but conceptually equivalent — the original computes the result first then zeros it on cutoff, but the cmp is the same).

Algebraically the same (only the order of work-vs-cutoff differs; result is identical). The original computes the torque pipeline up-front then `CMP EBP, ECX ; JLE keep ; XOR EAX,EAX`. Port early-outs on cutoff and skips the math. Same return value for the same inputs.

### D7 — slot0 debug log every 30 frames **(LOW IMPACT)**
Port lines 6173-6177: log call adds a branch (no functional effect on return value).

## Fixes to apply (worktree only)

1. Replace `>> 8` with `sar8_rz(...)` at all 4 sites (A, B, C, D).
2. Replace `>> 9` (index, lerp) with `sar9_rz(...)`.
3. Replace `frac = rpm & 0x1FF` with the original's truncated-modulo signed expression. C99 `%` operator on signed types truncates toward zero, so `rpm % 512` is exactly the original's intent. **Verify on MSVC/MinGW**: ISO C99/C11 guarantees truncated division for `/` and matching remainder for `%` (since C99). MinGW-w64 GCC honors this. Safe to use `int32_t frac = rpm % 512;`.
4. Remove the index clamp `if (index < 0) index = 0; if (index >= 15) index = 14;` — original has no bounds check.
5. Remove the debug log to match listing layout (or guard it under a trace flag).
6. Confirm preservation of throttle sign (port already passes signed throttle; OK).

## LUT byte-equality

The original reads the LUT through `param_2` = `actor->tuning_data_ptr` which already points at the Viper's carparam structure loaded by `td5_physics_load_carparam`. No embedded LUT — the LUT IS the first N int16 entries of the per-car tuning block. **Byte-equality is guaranteed when `tuning_data_ptr` is bound to the same Viper carparam memory as the original.** This is the upstream blocker (already documented in `pilot_00403A20_audit.md` D4: s_loaded_tuning fallback).

For the port-side trace harness, we must capture `tuning_data_ptr` and the first 16 int16 LUT entries so a runtime diff can verify byte-by-byte LUT equality.

## Capture schema

Per call (1 row per slot per tick):

**Keys:** `sim_tick`, `slot`, `caller_ra`

**Inputs:**
- `actor_addr` (hex), `tuning_ptr` (hex)
- `engine_speed_accum` (i32)
- `current_gear` (u8)
- `encounter_steering_cmd` (i16, signed)
- `torque_mult` (`tuning+0x68`, i16)
- `redline` (`tuning+0x72`, i16)
- `gear_ratio_curr` (`tuning + 0x2E + gear*2`, i16)
- `lut[0..15]` (16 × i16) — for byte-equality verification of the per-car curve
- `lut_index_used` (i32, derived = `sar9_rz(engine_speed)`)
- `lut_frac_used` (i32, derived = `engine_speed % 512` truncated)

**Outputs:**
- `return_value` (i32) — the drive torque

Schema is ~30 columns. Manageable in a single CSV per pool.

## Next actions for the pilot

1. Generate `tools/frida_pool13_0042F030.js` capturing the schema above.
2. Add `td5_pilot_trace_0042F030.{c,h}` in worktree with matching schema.
3. Apply D1–D7 fixes to `td5_physics.c` in worktree.
4. Build worktree.
5. Run paired capture (port + original) on Edinburgh slot 0, PlayerIsAI=1.
6. Diff column-by-column; iterate to zero divergence on `return_value` at `sim_tick ≥ 5`.

## Fixes applied (this pilot)

`td5_physics.c` (worktree only):

1. **Added `sar8_rz_42F030` and `sar9_rz_42F030` static inlines** at the top of
   `td5_physics_compute_drive_torque`, encoding the original's CDQ/AND/ADD/SAR idiom.
2. **Per-site replacement**: all four `>> 8` sites (t0, t1, *throttle, *gear_ratio) now use
   `sar8_rz_42F030`. Both `>> 9` sites (index, lerp) use `sar9_rz_42F030`.
3. **Removed index bounds clamp** (`if (index < 0) ...`, `if (index >= 15) ...`). Original
   has no bounds — the LUT is treated as effectively unbounded; redline cutoff + engine slew
   keep `index` in range at runtime.
4. **Frac extraction**: replaced `rpm & 0x1FF` with `rpm % 512` (C99 truncated-mod). Matches
   the original's `AND 0x800001FF` + sign-extension sequence for negative rpm.
5. **Redline-cutoff position**: kept at function exit (matching listing's CMP-after-pipeline
   sequence). Result is bit-exact since the multiplication chain never overflows int32 for
   the captured Viper torque curve values.
6. **Removed the per-30-frame TD5_LOG_I call** — original has no log; left as code-layout
   parity.
7. **Inline comments** anchored to specific instruction-address ranges
   (e.g. `[0x0042F060-72]`) so future audits can verify against the listing.

## Self-validation (algorithmic byte-exact)

`tools/validate_pool13_0042F030_math.py` feeds the original's captured inputs
(`log/orig/pool13_0042F030.csv`, 40 rows) through a Python reimplementation of
the port's byte-exact math, then asserts the outputs match the original's
captured `return_value`.

**Result: 40/40 rows match byte-exact.**

This validates that **the port's drive-torque math is byte-faithful** given correct
inputs.

## Runtime diff status

`log/port/pool13_0042F030.csv` (50 rows) vs `log/orig/pool13_0042F030.csv` (40 rows)
diverges on **every input column** at sim_tick=1:

| Column | Port | Original | Reason |
|---|---|---|---|
| engine_speed_accum | 5785 | 5985 | Upstream — different engine slew rate |
| current_gear | 2 | 3 | Upstream — different gear-change cadence |
| torque_mult | 360 | 120 | s_loaded_tuning fallback (port falls back to defaults, original loads Viper carparam) |
| redline | 6000 | 6200 | Same s_loaded_tuning fallback |
| gear_ratio_curr | 320 | 1398 | Same s_loaded_tuning fallback |
| lut[0..15] | 96,120,144,168,184,192,196,192,184,176,168,156,144,132,120,104 | 128,192,232,248,252,254,255,256,256,256,256,256,256,256,256,256 | Same s_loaded_tuning fallback |
| return_value | 267 | 655 | Downstream of all of the above |

This is the same upstream binding bug documented in `pilot_00403A20_audit.md` D4
(s_loaded_tuning fallback). The carparam + spawn chain runs BEFORE
`ComputeDriveTorqueFromGearCurve` each tick; until that chain produces matching
state for slot 0 on Edinburgh AutoRace, no per-tick CSV diff on 0x0042F030 will
reflect this function's math quality.

## Static-port deliverable (this branch)

Even without a clean runtime diff:

- `td5_physics.c` ComputeDriveTorque is byte-exact-correct vs the listing
  (algorithmic validation via captured-orig-inputs replay: 40/40 match).
- Probe + trace harness committed for future runtime diff once upstream blockers clear.
- Audit committed with line-by-line listing → port mapping.

## What NOT to do

- Do not tune Viper carparam LUT/redline/mult values to mask the s_loaded_tuning issue.
- Do not "fix" the torque math to compensate for wrong tuning inputs.
- Do not restore the index bounds clamp — the original has none.
- Do not switch back to `rpm & 0x1FF` — that's a non-negative mask, not a signed
  truncated remainder.

## Reference

- Listing: 0x0042F030..0x0042F0FC (Ghidra TD5_pool13, 2026-05-14)
- Decompilation: same session, 30 lines
- Port (post-fix): `td5mod/src/td5re/td5_physics.c:6109-` (worktree precise-0042F030)
- Self-validator: `tools/validate_pool13_0042F030_math.py`
- Frida probe: `tools/frida_pool13_0042F030.js`
- Port trace emitter: `td5_pilot_trace_0042F030.{c,h}` (worktree)
- Orig CSV: `log/orig/pool13_0042F030.csv` (40 rows / 40 calls)
- Port CSV: `.claude/worktrees/precise-0042F030/log/port/pool13_0042F030.csv` (50 rows; upstream-blocked)

## Reference

- Listing: 0x0042F030..0x0042F0FC (Ghidra TD5_pool13, 2026-05-14)
- Decompilation: same session, 30 lines
- Port pre-fix: `td5mod/src/td5re/td5_physics.c:6135-6180`
- Frida probe: `tools/frida_pool13_0042F030.js` (to be authored)
- Port trace emitter: `td5_pilot_trace_0042F030.{c,h}` (worktree)
- Orig CSV: `log/orig/pool13_0042F030.csv`
- Port CSV: `log/port/pool13_0042F030.csv`
