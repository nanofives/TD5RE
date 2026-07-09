# Pilot Audit — 0x004370A0 AdvanceActorTrackScript

**Date:** 2026-05-14
**Pool slot:** TD5_pool11
**Port-side function:** `td5_ai_advance_track_script` @ `td5_ai.c:1541`
**Worktree:** `.claude/worktrees/precise-004370A0` on branch `precise-004370A0`
**Caller graph:** `UpdateActorTrackBehavior (0x00434FE0)` — single caller, per-tick when
the actor has an active script.
**Callee graph:** `UpdateActorSteeringBias (0x004340C0)` — invoked in the aligned-finalize
path from LAB_004372cf with weight `0x4000`.
**Body:** 0x004370A0..0x0043777E (0x6DE bytes / 503 instructions / 192 decompiled lines).

## Function structure (from listing)

The function has five sequential phases plus a final 12-case switch:

```
[0x004370A0-D2]  Prologue + countdown decrement.
                 If countdown wraps below 0:
                    - Rotate base_ptr: A<->B (programs 0x473CD4 <-> 0x473CEC),
                                       C<->D (programs 0x473D00 <-> 0x473D18).
                    - Reset countdown to 0x96.
                    - **IMPORTANT**: BOTH rs[0x3A] and rs[0x3B] receive the SAME
                      pointer value (the new program base). This is the
                      same listing idiom seen in 0x00434FE0 ("ip aliases base_ptr").

[0x00437136-204] Flag-4 branch (yaw aligned with route → steering ramps up):
                 Compute hdelta_mirror = -((yaw>>8 - sar8_rz(route_byte*0x102c)) - 0x800) - 0x800
                                         (each step masked to 0xFFF).
                 If hdelta_mirror <= 0x200 (i.e., < 0x201):
                    Fall to LAB_004372cf (aligned-finalize).
                 Else:
                    steering_cmd += 0x4000 (clamp +0x19000); skip to flag-0x10 test.

[0x00437209-2cd] Flag-8 branch (yaw misaligned → steering ramps down):
                 Compute the same hdelta_mirror.
                 If hdelta_mirror >= 0xE00:
                    Fall to LAB_004372cf.
                 Else:
                    steering_cmd -= 0x4000 (clamp -0x19000); skip to flag-0x10 test.

[0x004372CF-32C] LAB_004372cf aligned-finalize (BOTH flag-4 and flag-8 paths):
                 combined_h = (steering_cmd + yaw_accum) >> 8     (plain SAR)
                 dev_full   = -(((combined_h - sar8_rz(route_byte*0x102c)) - 0x800) - 0x800)
                              & 0xFFF
                 rs[LEFT_DEVIATION]  = dev_full
                 rs[RIGHT_DEVIATION] = 0xFFF - dev_full
                 UpdateActorSteeringBias(rs, 0x4000)              [returns void]

[0x0043732F-381] Flag-0x10 stop-and-wait:
                 if |longitudinal_speed| < 0x100:
                    flags ^= 0x10; brake_flag=0; encounter_steer=0; RET 0
                 else:
                    brake_flag=1; encounter_steer=0xFF00;          RET 0

[0x00437382-404] Flag-0x02 throttle to target-speed:
                 brake_flag = 0
                 thr = sar8_rz(rs[OFFSET_PARAM] * 0x40308)
                 if (OFFSET_PARAM > 0):
                    encounter_steer = 0x00FF
                    if lspd > thr: encounter_steer = 0
                 else (OFFSET_PARAM <= 0):
                    encounter_steer = 0xFF00
                    if lspd < thr: encounter_steer = 0

[0x00437405-410] Opcode dispatch:
                 ip = (uint32 *) rs[RS_SCRIPT_IP]   [raw pointer]
                 opcode = ip[0]
                 if (opcode > 11) return 0   [unknown opcode]
                 switch on jump-table @ 0x00437780.

[switch 0..11]   (jump-table addresses listed below)
```

## Opcode dispatch table (verbatim from 0x00437780)

| Op | Target    | Action |
|---:|-----------|--------|
|  0 | 0x0043741D | Terminate if hdelta_mirror NOT in [0x40..0xFC0]; otherwise block (return 0). On terminate: zeroes base_ptr/flags/3E/43, conditionally zeroes steering_cmd (if op<0) then unconditionally zeroes steering_cmd + angular_velocity_yaw. RET 1. |
|  1 | 0x004374EB | rs[SPEED_PARAM] = ip[1]; flags \|= 0x01; ip += 2 dw. RET 0. |
|  2 | 0x0043765C | flags \|= 0x02; rs[OFFSET_PARAM] = ip[1]; ip += 2. RET 0. |
|  3 | 0x0043773B | flags \|= ip[1]; ip += 2. RET 0. |
|  4 | 0x0043775C | flags &= ~ip[1]; ip += 2. RET 0. |
|  5 | 0x004376BA | flags \|= 0x04; ip += 1. RET 0. |
|  6 | 0x004376D9 | flags \|= 0x08; ip += 1. RET 0. |
|  7 | 0x00437717 | encounter_steer = 0xFF00; brake_flag = 1; ip += 1. RET 0. |
|  8 | 0x004376F8 | flags \|= 0x10; ip += 1. RET 0. |
|  9 | 0x00437502 | Auto-select program by hd_mir + sub_lane vs strip_half. Sets countdown = 0xFA. Writes SAME pointer to base + ip. RET 0. |
| 10 | 0x00437679 | flags \|= 0x40; ip += 1. RET 0. |
| 11 | 0x00437698 | flags \|= 0x80; ip += 1. RET 0. |

## Key arithmetic primitives

**sar8_rz(x)** — round-to-zero signed divide by 256 (used on `route_byte * 0x102c`):
```
CDQ            ; EDX = -1 if sign(EAX) negative, else 0
AND EDX, 0xFF  ; rounding bias byte
ADD EAX, EDX
SAR EAX, 8
```
Since `route_byte ∈ [0,255]`, `route_byte * 0x102c ∈ [0, 0x101FF4]` is always non-negative,
so for THIS usage `sar8_rz(x) == x >> 8`. Port's plain `>> 8` is byte-faithful.

**Mirror-fold of heading delta** — used in flag-4 / flag-8 / case-0 / case-9 paths:
```
delta_pre = (yaw >> 8) - sar8_rz(route_byte * 0x102c)
tmp1      = (delta_pre - 0x800) & 0xFFF
tmp2      = (tmp1 - 0x800)  & 0xFFF      ; = delta_pre & 0xFFF = hdelta_raw
tmp3      = -tmp2 & 0xFFF                 ; = (0x1000 - hdelta_raw) & 0xFFF = hdelta_mirror
```
The mirror is essentially `(-hdelta_raw) mod 0x1000`. For hdelta_raw == 0, mirror == 0.

## Confirmed divergences from port

### D1 — Flag-4 aligned test uses `hdelta_raw` instead of `hdelta_mirror` **(HIGH IMPACT)**

`td5_ai.c:1609`:
```c
if (hdelta_raw < 0x201) {
```

Listing 0x004371a0-a5 tests `EAX = hd_mir` (i.e., `hdelta_mirror`):
```
CMP EAX, 0x200
JLE 0x004371d5    ; aligned-finalize path
```

**Symptom:** for `hdelta_raw ∈ [1, 0x200]` (small +bias) the port enters aligned-finalize
spuriously; for `hdelta_raw ∈ [0xE00, 0xFFF]` (small -bias = small mirror) the port stays
in the steer-up branch when it should be in aligned-finalize.

Note: the flag-8 test at `td5_ai.c:1595` correctly uses `hdelta_mirror > 0xDFF`, matching
listing 0x0043726b-70.

**Fix:** change line 1609 from `hdelta_raw < 0x201` to `hdelta_mirror < 0x201`.

### D2 — Countdown rotation writes `RS_SCRIPT_IP = 0` instead of `RS_SCRIPT_IP = next_base_ptr` **(MEDIUM IMPACT — trace divergence)**

`td5_ai.c:1577-1580`:
```c
if (next != cur) {
    rs[RS_SCRIPT_BASE_PTR] = (int32_t)next;
    rs[RS_SCRIPT_IP]       = 0;
}
```

Listing 0x004370F2/F8 (A→B branch) and 0x0043712A/30 (B→A or D→C):
```
MOV [ESI + 0xe8], EAX/EDX   ; rs[0x3A] = next_program_ptr
MOV [ESI + 0xec], EAX/EDX   ; rs[0x3B] = SAME next_program_ptr
```

Original stores IP as a raw pointer (not an index). Port stores IP as an index. The two are
**functionally equivalent** as long as all reads/writes are internally consistent — port
correctly decodes `base[ip]` with ip=0 as "first opcode of new program". But the LITERAL
value in `rs[RS_SCRIPT_IP]` diverges from original (port: 0; original: e.g. 0x00473CEC).

**For runtime trace parity**, we either:
- (a) Switch port to store IP as a pointer (refactor every IP read/write in the function).
- (b) Translate port-side IP-index → pointer at the trace emit point.

Option (b) is lower-impact. Selected for this pilot.

### D3 — Case 9 writes `RS_SCRIPT_IP = 0` instead of `RS_SCRIPT_IP = selected_program_ptr` **(MEDIUM IMPACT — trace divergence)**

`td5_ai.c:1796`:
```c
rs[RS_SCRIPT_IP] = 0;
```

Listing 0x004375AB-B7, 0x004375E7-F3, 0x0043762C-38, 0x00437644-50:
```
MOV EAX, 0x473cd4 (or 0x473cec / 0x473d00 / 0x473d18)
MOV [ESI + 0xe8], EAX     ; base_ptr = selected
MOV [ESI + 0xec], EAX     ; ip = SAME pointer
```

Same trace-divergence class as D2. Resolved via translation at the trace emit point.

### D4 — Recovery-script seed in caller writes `RS_SCRIPT_IP = 0` instead of pointer **(LOW IMPACT — already audited in 0x00434FE0 pilot)**

`td5_ai.c:1936` writes `rs[RS_SCRIPT_IP] = 0`. The original at 0x00435094-9F writes the same
pointer to BOTH rs[0x3A] and rs[0x3B] (the listing for `UpdateActorTrackBehavior`). See
`pilot_00434FE0_audit.md` D1 — pilot fix already restricted writes to the two pointer
assignments; the value-0 vs value-pointer divergence remains for trace parity only.

Out of scope for this pilot — owned by precise-00434FE0.

### D5 — Case 0 reuses prologue `hdelta_mirror` instead of recomputing from yaw_accum **(NEGLIGIBLE — algebraically identical)**

Port case 0 (lines 1696-1698) reads `hdelta_raw` from the prologue (line 1588) and
re-derives `hd_mir` via the same triple-fold idiom. Listing 0x0043741D-91 reads
yaw_accum + span_normalized FRESHLY from the actor. Since neither yaw_accum nor
span_normalized changes between prologue and case 0 (flag-4/8 paths only mutate
steering_cmd; flag-0x10 / flag-0x02 only mutate encounter_steer/brake), the values are
identical. The port could simplify by using the prologue `hdelta_mirror` directly, but the
current double-compute is harmless.

### D6 — IP semantics: index vs pointer **(SHIPPED PORT-WIDE — not a fixable divergence)**

Port stores `RS_SCRIPT_IP` as a dword INDEX into the program (incremented by 1 or 2 per
opcode). Original stores it as a RAW POINTER (incremented by 4 or 8 bytes per opcode). All
reads/writes within `td5_ai_advance_track_script` are internally consistent under the
index convention, but absolute IP values diverge between port and original. Captured at
trace level via index→pointer translation.

## Fixes to apply (this pilot, in worktree)

1. **D1**: Change `td5_ai.c:1609` from `if (hdelta_raw < 0x201)` to `if (hdelta_mirror < 0x201)`.
   Mirrors flag-8 sibling at line 1595 which already uses `hdelta_mirror`.

2. **D2/D3 trace harness**: Add a port-side trace emitter at function entry + exit
   capturing both port (index) and translated (pointer) IP, plus the input/output state.
   The harness lives in the worktree only. Diff converts both sides to the same
   canonical form (translate index→pointer using base_ptr + index*4).

3. **D5**: Leave as-is. Pursue only if Frida row-diff shows residual at case 0 dispatch
   (it shouldn't, given the algebraic identity).

4. **D4**: Out of scope (owned by 0x00434FE0 pilot).

## Capture schema for Frida + port

**Keys:** `sim_tick`, `slot`, `phase` (enter/exit), `caller_addr` (UpdateActorTrackBehavior @ 0x00435133)

**Inputs (per call entry):**
- `rs_addr` (hex sanity)
- `slot_index` (rs[0x35])
- `route_table_ptr` (rs[0])
- `script_base_ptr_in` (rs[0x3A])
- `script_ip_in` (rs[0x3B])
- `script_flags_in` (rs[0x3D])
- `script_countdown_in` (rs[0x45])
- `script_offset_param` (rs[0x1B])
- `script_speed_param` (rs[0x3C])
- `actor_yaw_accum` (actor +0x1F4)
- `actor_steering_cmd` (actor +0x30C)
- `actor_long_speed` (actor +0x314)
- `actor_span_norm` (actor +0x82)
- `actor_span_raw` (actor +0x80)
- `actor_sub_lane` (actor +0x8C)
- `actor_encounter_steer_in` (actor +0x33E)
- `actor_brake_flag_in` (actor +0x36D)

**Outputs (per call exit):**
- `opcode` (opcode hit this tick, or sentinel -1 if early-return via flag-0x10 / flag-2)
- `branch_taken` (enum: countdown_wrap / flag4_aligned / flag4_nonaligned / flag8_aligned /
   flag8_nonaligned / flag10 / flag2 / opcode_case_N)
- `script_base_ptr_out`
- `script_ip_out`
- `script_flags_out`
- `script_countdown_out`
- `script_offset_param_out`
- `script_speed_param_out`
- `rs_left_deviation_out`
- `rs_right_deviation_out`
- `actor_steering_cmd_out`
- `actor_encounter_steer_out`
- `actor_brake_flag_out`
- `actor_angular_velocity_yaw_out` (zeroed only on case 0 terminate)
- `return_value` (0 = still running, 1 = terminated)

**Estimated row count:** countdown rotation @ 0x96 ticks; during racing AdvanceActorTrackScript
fires once per script-active tick. For Edinburgh AI-only with slot 0 PlayerIsAI=1, expect
~30 rows captured across 30 sim_ticks.

## Reference

- Listing: 0x004370A0..0x0043777E (Ghidra TD5_pool11, 2026-05-14)
- Decompilation: same session, 192 lines
- Port: `td5mod/src/td5re/td5_ai.c:1541-1813`
- Jump table: 0x00437780 (12 dwords)
- Tag: `pool11_004370A0`
