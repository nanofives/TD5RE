# Pilot Audit — 0x004340C0 UpdateActorSteeringBias

**Date:** 2026-05-14
**Pool slot:** TD5_pool10
**Port-side function:** `td5_ai_update_steering_bias` @ `td5_ai.c:1013`
**Worktree:** `.claude/worktrees/precise-004340C0` on branch `precise-004340C0`
**Caller graph:** `UpdateActorTrackBehavior (0x00434FE0)`, `UpdateTrafficRoutePlan (0x00435E80)`, `AdvanceActorTrackScript (0x004370A0)`.
**Callee graph:** `SinFixed12bit (0x0040A700)` — sole callee.
**Body:** 0x004340C0..0x004342DA (0x21A bytes / 156 instructions / ~80 decompiled lines).

## Function structure (from listing)

```
prologue [0x004340C0-0x0043413B]:
    EBX = param_1 (route_state)
    EAX = param_1[0xD4] = rs[0x35] = RS_SLOT_INDEX
    ECX = slot * 0x71               ; via LEA*8/SUB/SHL/ADD
    EAX = *(int*)(0x4ab41c + ECX*8) ; actor[slot].LONGITUDINAL_SPEED @ +0x314
    EAX = lspd >> 8
    EDX = sign-extend(EAX)
    ESI = 0x4ab108 + ECX*8           ; &actor[slot]
    ECX = actor->REAR_AXLE_SLIP @ +0x320
    ECX = (rear_slip >> 8) ²         ; IMUL ECX,ECX
    EDI = abs(lspd>>8)               ; XOR/SUB with EDX
    EBP = ECX + 0x400                ; (rear_slip>>8)² + 0x400
    EAX = abs_lspd << 10
    EAX = EAX / EBP                  ; signed IDIV: (|lspd>>8|*0x400) / (rear² + 0x400)
    EBP = EAX
    EBP = EBP + 0x40
    EAX = 0xC0000 / EBP              ; iVar2 (per-tick rate cap)
    ECX = (rear_slip>>8)² + 0x10000
    EAX = abs_lspd << 16
    EAX = EAX / ECX                  ; (|lspd>>8|*0x10000) / (rear² + 0x10000)
    ECX = EAX + 0x100
    EAX = 0x1800000 / ECX            ; iVar3 (absolute steering cap)
    ECX = iVar3
    EBP = iVar2
    EBX = param_1[0x16] = rs[RS_LEFT_DEVIATION]    ; was [EBX+0x58]
    EAX = EBX (= LEFT_DEV)
    EBX = param_1[0x17] = rs[RS_RIGHT_DEVIATION]   ; was [EBX+0x5C] (clobbers param_1)
    JGE 0x004341FE if LEFT >= 0x800  ; → right-side branch

LEFT < 0x800 branch [0x0043414E-0x004341F9]:
    CMP LEFT,0x400; JLE 0x00434165 if LEFT <= 0x400 → fine_path
    LEFT > 0x400 (i.e. 0x401..0x7FF):
        EAX = actor[+0x30C] + 0x4000 ; STEER_CMD + 0x4000
        JMP 0x004342A3                ; → final_write+clamp

    fine_path [0x00434165]:
        EDI = param_2 (steer_weight)
        TEST EDI; JZ 0x004341AC if param_2 == 0 → script_path
        CMP LEFT,0x100; JGE 0x0043419F if LEFT >= 0x100 → mid_path
        fine_sin [LEFT < 0x100]:
            PUSH LEFT; CALL SinFixed12bit; EAX = sin12(LEFT)
            EAX = EAX * param_2
            ECX = STEER_CMD
            EAX = round-to-zero(EAX / 4096)  ; CDQ; AND EDX,0xFFF; ADD; SAR 12
            ECX = ECX + EAX
            STEER_CMD = ECX
            JMP 0x004342A9                    ; → clamp_end

        mid_path [LEFT in 0x100..0x400, param_2 != 0]:
            EAX = STEER_CMD + param_2
            JMP 0x004342A3                    ; → final_write+clamp

        script_path [0x004341AC, param_2 == 0]:
            XOR EAX; MOV AX,actor[+0x33A]    ; zero-ext load AX = ramp_accum
            CMP AX,0x100; JGE skip if (UNSIGNED) ramp >= 0x100
            ADD EAX,0x40; actor[+0x33A] = AX  ; ramp += 0x40
            (skip:) MOVSX EAX,actor[+0x33A]  ; sign-ext reload
            EAX = ramp * EBP                  ; = ramp * iVar2
            EAX = round-to-zero(EAX / 256)   ; CDQ; AND EDX,0xFF; ADD; SAR 8
            EDX = STEER_CMD + EAX
            EAX = EDX (new bias)
            STEER_CMD = EDX
            CMP EAX, ECX (= iVar3)
            JLE 0x004342A9 if bias <= iVar3   ; → clamp_end
            STEER_CMD = ECX (= iVar3)         ; clamp to +iVar3
            JMP 0x004342A9                    ; → clamp_end

LEFT >= 0x800 branch [0x004341FE-0x00434258]:
    CMP RIGHT,0x400; JLE 0x00434216 if RIGHT <= 0x400 → fine_path_right
    RIGHT > 0x400 (i.e. LEFT>=0x800 AND RIGHT>=0x401):
        EAX = STEER_CMD - 0x4000
        JMP 0x004342A3                        ; → final_write+clamp

    fine_path_right [0x00434216]:
        EDI = param_2
        TEST EDI; JZ 0x0043425A if param_2 == 0 → script_path_right
        CMP RIGHT,0x100; JGE 0x00434250 if RIGHT >= 0x100 → mid_path_right
        fine_sin_right [RIGHT < 0x100]:
            PUSH RIGHT; CALL SinFixed12bit
            EAX = sin12(RIGHT) * param_2
            ECX = STEER_CMD
            EAX = round-to-zero(EAX / 4096)
            EAX = -EAX                        ; NEG (subtract, not add)
            ECX = ECX + EAX
            STEER_CMD = ECX
            JMP 0x004342A9

        mid_path_right [RIGHT in 0x100..0x400, param_2 != 0]:
            EAX = STEER_CMD - param_2
            JMP 0x004342A3                    ; → final_write+clamp

        script_path_right [0x0043425A, param_2 == 0]:
            XOR EAX; MOV AX,actor[+0x33A]; ramp += 0x40 if (UNSIGNED) < 0x100
            MOVSX EAX,actor[+0x33A]
            EBX = STEER_CMD
            EAX = ramp * EBP                  ; = ramp * iVar2
            EAX = round-to-zero(EAX/256)
            EAX = -EAX                        ; NEG
            EBX = EBX + EAX                   ; new bias = STEER_CMD - sar8_rz(ramp*iVar2)
            EAX = ECX (= iVar3); EDX = EBX
            EAX = -EAX                         ; EAX = -iVar3
            CMP EDX, EAX                       ; new vs -iVar3
            STEER_CMD = EBX (= new bias)
            JGE 0x004342A9 if new >= -iVar3   ; → clamp_end
            (fall to 0x004342A3 with EAX=-iVar3): STEER_CMD = -iVar3

LAB_004342A3 (shared final write):
    STEER_CMD = EAX (the bias-or-clamp value)

LAB_004342A9 (clamp_end):
    EAX = STEER_CMD
    if (EAX > 0x18000):  STEER_CMD = 0x18000; RET
    if (EAX < -0x18000): STEER_CMD = -0x18000; RET
```

## Key arithmetic primitives

`sar8_rz(x)` — round-to-zero signed divide by 256:
```
CDQ; AND EDX,0xff; ADD EAX,EDX; SAR EAX,8
```
Equivalent C: `((x < 0) ? (x + 0xFF) : x) >> 8`. The port encodes this as
`(x + ((x >> 31) & 0xFF)) >> 8` which produces the same bits for all
inputs (`(x >> 31) & 0xFF` is `0xFF` if negative, `0` otherwise).

`sar12_rz(x)` — round-to-zero signed divide by 4096:
```
CDQ; AND EDX,0xfff; ADD EAX,EDX; SAR EAX,0xc
```
Port: `(x + ((x >> 31) & 0xFFF)) >> 12`. Same bits. Used in the fine-sin
branches for `sin12(dev) * param_2 / 4096`.

`(unsigned 16-bit) ramp_accum check`: the original loads `MOV AX,[ESI+0x33A]`
into the LOW 16 bits of EAX (zero-extending because EAX was cleared by
`XOR EAX,EAX`), then uses `CMP AX,0x100; JGE`. JGE compares the FULL 32-bit
EAX which is non-negative (high 16 bits zero), so the compare is effectively
**unsigned 0..0xFFFF >= 0x100**. After the conditional ADD it does
`MOVSX EAX,word ptr [ESI+0x33A]` — sign-extended reload for the IMUL.

The port reads `int16_t ramp = ACTOR_I16(actor, ACTOR_STEERING_RAMP_ACCUM);`
and compares `ramp < 0x100` (signed 16-bit). Equivalent for ramp values
0..0x7FFF; only diverges in the 0x8000..0xFFFF range which is unreachable
in practice (ramp is initialized to 0 and only incremented by 0x40 per
script tick until it reaches 0x100, then clamps).

## Confirmed divergences from port

After full line-by-line trace, **none of the divergence classes from prior
pilots (D1..D7 in `pilot_00403720_audit.md`, D1..D4 in
`pilot_00403A20_audit.md`) apply here**. The port already:

1. Uses the correct `sar8_rz`/`sar12_rz` round-to-zero idioms (lines 1063,
   1084, 1105, 1121).
2. Uses the same nested cascade structure (`if LEFT < 0x800 / else if
   RIGHT < 0x401 / else -0x4000`) as the original.
3. Reads the same actor field offsets (0x30C, 0x314, 0x320, 0x33A) and
   route-state dword indices (0x16, 0x17, 0x35).
4. Uses `actor_ptr(rs[RS_SLOT_INDEX])` for actor reads/writes — the same
   indirection as the original's `[ECX*8 + 0x4ab108]` where `ECX` came
   from `param_1[0xD4] = RS_SLOT_INDEX`.
5. Applies the final ±0x18000 clamp as separate `if`/`else if`
   (algebraically equivalent to the original's two separate `if` blocks
   because the two ranges are disjoint).

The single semantic edge case is the unsigned-vs-signed treatment of
`ramp_accum` for values 0x8000..0xFFFF. This is unreachable in practice
because the field is initialized to 0 and grows monotonically by 0x40
until it reaches the 0x100 cap. **Flagged for future hardening (D-edge) but
not a runtime bug.**

### D-edge — `ramp_accum` unsigned-16 check (UNREACHABLE in practice)

Port:
```c
int16_t ramp = ACTOR_I16(actor, ACTOR_STEERING_RAMP_ACCUM);
if (ramp < 0x100) { ramp += 0x40; ACTOR_I16(..) = ramp; }
```
Original treats the loaded value as **unsigned 16-bit** for the compare.
Equivalent for ramp in [0, 0x7FFF]. For [0x8000, 0xFFFF] the port would
keep adding 0x40 while the original would skip. The field is monotonically
increasing from 0 with step 0x40, with a clamp at 0x100 — it cannot reach
0x8000 under normal play. No runtime impact predicted.

**Recommendation:** leave as-is; this is the kind of micro-correction that
risks introducing bugs if applied without runtime evidence (per
`feedback_pursue_fixes` — fix things to zero delta, but only where delta
exists).

## Capture schema for pilot

Per call (1 row per slot per call site):

**Keys:** `sim_tick`, `slot`, `call_site` (which of 3 callers)
**Inputs:**
- `param_2_steer_weight` (the function's second arg)
- `rs_left_deviation_in` (rs[0x16])
- `rs_right_deviation_in` (rs[0x17])
- `rs_slot_index` (rs[0x35])
- `actor_longitudinal_speed_in` (actor +0x314)
- `actor_rear_axle_slip_in` (actor +0x320)
- `actor_steering_cmd_in` (actor +0x30C)
- `actor_steering_ramp_accum_in` (actor +0x33A, int16)

**Outputs:**
- `actor_steering_cmd_out` (after potential ±0x18000 clamp)
- `actor_steering_ramp_accum_out`
- `iVar2_rate_cap` (recomputed in capture)
- `iVar3_abs_cap` (recomputed in capture)
- `branch_taken` (enum: left_emerg / left_mid / left_sin / left_script /
  right_emerg / right_mid / right_sin / right_script)

**Estimated rows per Edinburgh race**: slot 0 PlayerIsAI=1 → 1 call per tick
from `UpdateActorTrackBehavior` (state-0 calls
`UpdateActorSteeringBias(rs, 0x20000)` after threshold returns 0). May see
2-3 calls/tick during script-active windows (param_2=0 path from
`AdvanceActorTrackScript`).

## Fixes applied (this pilot)

`td5_ai.c` (worktree only): **NONE.**

The port is byte-faithful to the listing across all branches. The audit
identifies one unreachable edge case (D-edge) that does not warrant a
code change.

This concludes the static-port phase. The runtime trace probe captures
inputs/outputs to allow future runtime validation when upstream blockers
(carparam binding, spawn binding from pilot_00403A20_audit's "Blocked by
upstream") clear.

## Pilot reuse — algorithmic self-validator

`tools/validate_pool10_004340C0_math.py` (this pilot) feeds the original's
captured inputs through a Python reimplementation of the port's branch
logic and asserts that outputs match. This isolates the function-local
math from upstream divergences (carparam, spawn) that prevent direct
runtime diff.

## Reference

- Listing: 0x004340C0..0x004342DA (Ghidra TD5_pool10, 2026-05-14)
- Decompilation: same session, ~80 lines
- Port: `td5mod/src/td5re/td5_ai.c:1013-1145`
- Self-validator: `tools/validate_pool10_004340C0_math.py`
- Frida probe: `tools/frida_pool10_004340C0.js`
- Port trace emitter: `td5mod/src/td5re/td5_pilot_trace_004340C0.{c,h}` (worktree)
- Orig CSV: `log/orig/pool10_004340C0.csv` (when runtime capture runs)
- Port CSV: `log/port/pool10_004340C0.csv`
- Tag: `pool10_004340C0`
