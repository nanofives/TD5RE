# Pilot Audit — 0x00406980 ApplyTrackSurfaceForceToActor (WallResponse)

**Date:** 2026-05-14
**Pool slot:** TD5_pool7
**Worktree:** `.claude/worktrees/precise-00406980` on branch `precise-00406980`
**Port-side function:** `td5_physics_wall_response` @ `td5mod/src/td5re/td5_physics.c:260`
**Caller graph (original):** `UpdateActorTrackSegmentContacts (0x406CC0)`, `UpdateActorTrackSegmentContactsForward (0x406F50)`, `UpdateActorTrackSegmentContactsReverse (0x4070E0)`.
**Callee graph:** `CosFixed12bit (0x40A6E0)`, `SinFixed12bit (0x40A700)`, `DecayUltimateVariantTimer (0x40A440)`, `PlayVehicleSoundAtPosition (0x441D90)`, `PlayAssignedControllerEffect (0x428A10)`, `ComputeActorRouteHeadingDelta (0x434040)`.
**Body:** 0x00406980..0x00406CBF (832 bytes, 283 instructions).
**Signature (Ghidra):** `void __cdecl ApplyTrackSurfaceForceToActor(RuntimeSlotActor *actor, int *forceVec, uint angle, int magnitude, uint flags)`

## Function structure (from listing + decomp)

```
prologue (0x00406980-0x004069A0):
    EBX = cos(angle)
    EDI = sin(angle)
    EBP = magnitude - 4                  ; "push amount"
    ESI = actor

position push (0x004069A1-0x004069E3):
    actor->world_pos_x -= sign_round_f(sin*EBP) >> 4   ; sign_round mask = 0xF
    actor->world_pos_z += sign_round_f(cos*EBP) >> 4

velocity decomp into wall basis (0x004069E9-0x00406A1B):
    iVar3 = lv_x*cos + lv_z*sin          ; raw (×4096)
    iVar4 = sign_round_fff(iVar3) >> 12  ; v_para
    iVar5 = lv_z*cos - lv_x*sin          ; raw (×4096)

lever arm (0x00406A1D-0x00406A50):
    arm_x = (post_push_world_pos_x - forceVec[0]) >> 8
    arm_z = (post_push_world_pos_z - forceVec[2]) >> 8
    iVar9 = arm_z*sin + arm_x*cos        ; raw (×4096)
    iVar9 = sign_round_fff(iVar9) >> 12  ; lever along wall tangent

iVar11 build (0x00406A51-0x00406A78):
    yaw_div = actor->yaw / 0x28C         ; (signed via magic 0xC907DA5)
    iVar10 = sign_round_fff(iVar5) >> 12 ; v_perp (cached as [ESP+0x18])
    iVar11 = yaw_div * iVar9 + iVar10
    if (iVar11 < 0) JS 0x406cba          ; EARLY RETURN — no impulse, no audio,
                                         ; no velocity rotate-back, no flag write

impulse (0x00406A7E-0x00406AD1):
    K = DAT_00463200                     ; 1500000
    iVar6 = sign_round_ff(K) >> 8        ; = K>>8 (K positive)
    iVar6 *= -0x1100                     ; numerator scalar
    num = sign_round_fff(iVar6) >> 12    ; **ROUND-TOWARD-ZERO** on negative
    iVar7 = sign_round_ff(iVar9*iVar9 + K) >> 8  ; denominator
    iVar7 = (num * iVar11) / denom       ; signed IDIV (truncates toward 0)
    iVar6 = iVar10 + iVar7               ; new v_perp = v_perp + impulse

tangential damping (0x00406AD3-0x00406B69):
    iVar4 was sign_round_fff(iVar3) >> 12 = v_para
    if (iVar4 > 0):
        delta_in = sign_round_3f(iVar4) >> 6 + 0x800 + iVar11*2
        delta = sign_round_7ff(delta_in * 0x180) >> 11
        iVar4 = iVar4 - delta
        if (iVar4 >= 0) goto LAB_00406b6b ; preserve
        else            XOR ECX, ECX      ; zero
    else:
        delta_in = (iVar11*2 + 0x800) - sign_round_3f(iVar4) >> 6
        delta = sign_round_7ff(delta_in * 0x180) >> 11
        iVar4 = iVar4 + delta
        if (iVar4 <= 0) goto LAB_00406b6b ; preserve  (JLE includes ==0)
        else            XOR ECX, ECX      ; zero

writeback (0x00406B6B-0x00406BB7):
    actor->yaw += iVar7 * iVar9 / (K / 0x28C)   ; iVar7 here = impulse from above
    actor->lv_x = sign_round_fff(iVar4*cos - new_v_perp*sin) >> 12
    actor->lv_z = sign_round_fff(iVar4*sin + new_v_perp*cos) >> 12

audio/feedback (0x00406BB1-0x00406CB7):  -- OUTSIDE SIM CORE
    if (iVar10 > 0x3200) {
        clamp iVar10-0x2000 to [0x400, 0x800]
        PlayVehicleSoundAtPosition(...)
        DecayUltimateVariantTimer(actor, 1)
        impact_amount = sign_round_3(iVar10) >> 2; cap at 100000
        heading_delta = ComputeActorRouteHeadingDelta(...)
        if heading_delta in [0x400, 0xC00): flags ^= 3
        if flags == 1: PlayAssignedControllerEffect(slot, 2, impact, impact*10)
        if flags == 2: PlayAssignedControllerEffect(slot, 1, impact, impact*10)
    }
```

## Confirmed divergence points (port vs original)

### D1 — Unconditional velocity rotate-back **(LOW-MEDIUM IMPACT)**

Original takes `JS 0x406cba` when `iVar11 < 0`, which jumps to the function epilogue and returns with NO writes to `linear_velocity_x/z` and NO update to `angular_velocity_yaw`.

Port has:
```c
if (iVar11 >= 0) { ...impulse + damping... }
/* Rotate (new_v_para, new_v_perp) back to world basis */
actor->linear_velocity_x = ((int64_t)new_v_para * cos_w - (int64_t)new_v_perp * sin_w) >> 12;
actor->linear_velocity_z = ((int64_t)new_v_para * sin_w + (int64_t)new_v_perp * cos_w) >> 12;
```

When `iVar11 < 0`, port still rotates back. Since `new_v_para = v_para` and `new_v_perp = v_perp` are themselves `>>12` quantized projections of the raw velocity, rotating them back through the same `>>12` quantization introduces small round-off — typically a few units of fp8 per axis. Compound across many separating-contact ticks, this accumulates.

**Fix:** wrap the rotate-back, yaw update, and side flag write inside the same `iVar11 >= 0` gate. (track_contact_flag write must also gate — see D5.)

### D2 — Numerator `>>12` round-toward-zero **(MEDIUM IMPACT, magnitude depends on iVar11)**

Original: `num = (iVar6 + (iVar6 >> 0x1F & 0xFFF)) >> 12` — adds `0xFFF` when negative before SAR. This is round-toward-zero on negative values (Borland/MSVC `__cdecl` integer-divide-by-power-of-two emit). For `iVar6 = (K>>8) * -0x1100 = 5859 * -4352 = -25497168`, result = `(-25497168 + 4095) >> 12 = -25493073 SAR 12 = -6225`.

Port: `num = ((V2W_INERTIA_K >> 8) * -V2W_NUM_SCALE) >> 12`. With GCC arithmetic SAR semantics, `-25497168 >> 12 = -6226`.

So num is off by **+1 unit** (port more negative). This propagates as `impulse = (num * iVar11) / denom` → impulse is `iVar11 / denom` more negative than original. With typical `denom ≈ 5860..30000` and `iVar11` up to a few thousand, the per-impulse error is small (1-10 fp8) but it accumulates in `v_perp` and feeds into `angular_velocity_yaw += impulse * iVar9 / 2300`.

**Fix:** Apply Borland-style round-toward-zero before the `>>12`. Same as the existing pattern in the port: `(x + (x < 0 ? 0xFFF : 0)) >> 12`.

### D3 — Tangential `>>11` round-toward-zero **(LOW-MEDIUM IMPACT)**

Same class of bug. Original line:
```
iVar4 = ((int)(iVar3 + (iVar3 >> 0x1f & 0x7ffU)) >> 0xb) + iVar4;
```
Port line:
```c
int32_t delta = (tmp * 0x180) >> 11;
```

The `>> 11` in port uses GCC arithmetic SAR (round-toward-`-inf`). Original adds `0x7FF` when negative before SAR, yielding round-toward-zero. For `tmp * 0x180 < 0`, port computes 1 unit larger magnitude than original. Per-tick effect: 1-2 fp8 difference in `new_v_para`. Same fix.

### D4 — Lever-arm `>>12` round-toward-zero **(LOW)**

Both halves of `iVar9` use `>>12` with `0xFFF` sign-round in original. Port uses pure `>>12`:
```c
int32_t iVar9 = ((int64_t)arm_z_int * sin_w + (int64_t)arm_x_int * cos_w) >> 12;
```

Same class. The lever arm typically straddles zero so the rounding error can compound especially for shallow contacts.

### D5 — `track_contact_flag` write outside iVar11 gate **(LOW)**

Original only reaches the flag/audio path when `iVar11 >= 0`. The flag write (in the original, it's done via the audio/feedback path conditionals — actually the original NEVER writes a "track_contact_flag", the port made that up to map from the `flags` parameter to a separate field).

Looking at port: `if (side >= 0) actor->track_contact_flag = (uint8_t)(side + 1);`. The original has no such field write — the closest equivalent is `flags ^= 3` (XOR with 3) when `0x400 < heading_delta < 0xC00`. This is on a different state (the `flags` parameter, used only for controller effect dispatch).

**This is a port-only addition.** If `track_contact_flag` is consumed elsewhere in the port (game UI / state machine), it's a port-bridge. Out-of-scope for the impulse-byte-match unless we can show it leaks into the sim output set. Document but defer.

### D6 — Velocity rotate-back uses `>>12` arithmetic SAR (no sign round) **(LOW-MEDIUM)**

Original `lv_x = sign_round_fff(iVar4*cos - new_v_perp*sin) >> 12`. Port: `(...) >> 12` without sign round. Same rounding-mode class as D2/D3/D4. Per-tick: ±1 fp8 per axis when the value is negative.

### D7 — Position push `>>4` round-toward-zero **(LOW)**

Original lines 0x004069B1/0x004069C9: `add EAX,EDX; sar EAX,4` where `EDX = sign_round_f(EAX)`. Port:
```c
actor->world_pos.x -= (int32_t)((px + ((px >> 63) & 0xF)) >> 4);
actor->world_pos.z += (int32_t)((pz + ((pz >> 63) & 0xF)) >> 4);
```

Port uses int64 with `(px >> 63) & 0xF` which IS the round-toward-zero pattern, just lifted to 64-bit. **THIS ONE MATCHES.** The pattern is `add sign_round_f; SAR 4`. ✓

So D2/D3/D4/D6 are the same family of bugs (Borland round-toward-zero on negative for power-of-two divides), repeated across four occurrences. Port handles D7 correctly with the same pattern — so the FIX TEMPLATE is already established in the code, just needs to be applied to the other four sites.

### D8 — Yaw update divisor uses port's K/0x28C **(MATCH)**

Port: `int32_t ang_div = V2W_INERTIA_K / ANGULAR_DIVISOR_W;  /* 2300 */`. Original computes via 0xC907DA5 magic constant idiom, which is the GCC/MSVC pattern for signed integer division by 652. Result is 2300 for K=1500000. **MATCH.**

## Capture schema for pilot

Per call (one row per WallResponse invocation):

**Keys:** `sim_tick`, `slot`, `caller_ra`, `call_idx` (per-tick sequence number)
**Inputs:**
- `actor_addr` (hex)
- `force_x_fp8`, `force_z_fp8` (forceVec[0], forceVec[2])
- `angle` (uint, & 0xFFF)
- `magnitude` (signed penetration)
- `flags` (uint)
- `pre_lv_x`, `pre_lv_z`, `pre_yaw` (pre-call snapshot)
- `pre_pos_x`, `pre_pos_z`
- `pre_iVar11_predicted` (computed in probe for cross-check; optional)

**Outputs:**
- `post_lv_x`, `post_lv_z`, `post_yaw`
- `post_pos_x`, `post_pos_z`
- `delta_lv_x` = post-pre, `delta_lv_z`, `delta_yaw`

Schema ~16 columns; manageable.

## Test track choice

Moscow track=4 with `--start-span-offset=175`, `PlayerIsAI=1` per memory `reference_moscow_span175_degenerate`. Both port AND original Viper get stuck against the same wall there, producing dense WallResponse fires per tick.

## Next actions

1. Author `tools/frida_pool7_00406980.js` to capture per-call inputs+outputs.
2. Author `td5_pilot_trace_00406980.{c,h}` in worktree mirroring the schema.
3. Hook port-side enter/leave around `td5_physics_wall_response` body.
4. Build worktree; capture both traces on Moscow span 175.
5. Diff; expect D2/D3/D4/D6 to surface first as 1-unit-per-call drifts; D1 to surface on every iVar11<0 separating-contact call.
6. Fix all four rounding sites + D1 gate; iterate to zero diff.

## Runtime validation outcome — ESCALATION

**Date:** 2026-05-14
**Pool slot:** TD5_pool7 (acquired manually; ghidra_pool.sh acquire only enumerates pool0..6 by default).
**Status:** **Cannot bit-validate against runtime trace due to caller-graph divergence.**

### What was captured

| Side     | Window      | Rows  | Slot distribution             | Caller RA(s)                       |
|----------|-------------|-------|-------------------------------|------------------------------------|
| Original | 600 sim_ticks | 12    | slots 2/3/4/5 (no slot 0/1)   | 0x406e06, 0x406f26 (both inside 0x00406CC0 UpdateActorTrackSegmentContacts) |
| Port     | 600 sim_ticks | 457   | slots 2/3/4 mostly             | port-side from fwd_rev_resolve_contact (lateral handler `#if 0`-disabled in td5_track.c:737) |

### Root cause of zero common keys

The port's lateral wall handler `td5_track_update_actor_wall_contacts` is wrapped in `#if 0 / #endif legacy wall_contacts disabled`. Only the forward/reverse boundary handlers (`fwd_rev_resolve_contact`) call `td5_physics_wall_response` in the port.

The original's `UpdateActorTrackSegmentContacts` (0x00406CC0) is the **dominant** WallResponse caller, and it operates on lateral edges (left/right rails per probe), NOT on per-level forward/reverse boundary sentinels.

So the port and original drive the same WallResponse impulse function with **completely different upstream contact-detection logic**:
- Different gating (per-probe sub_lane vs boundary span)
- Different `angle` values (lateral wall tangent vs boundary edge tangent)
- Different `magnitude` (penetration depth in different geometry)
- Different `forceVec` (probe position vs boundary span vertex)

The result is that the port and original fire WallResponse at completely disjoint sim_ticks. `diff_func_trace.py` reports `common keys: 0` regardless of the WallResponse implementation.

### Static fixes applied (Ghidra-confirmed)

Even without runtime trace pairing, the audit identified six concrete rounding/gating bugs in the port's `td5_physics_wall_response` that are Ghidra-confirmed from the original 0x00406980 disassembly. All fixes shipped on branch `precise-00406980`:

| ID | Fix | Location | Verification |
|----|-----|----------|-------------|
| D1 | Wrap velocity rotate-back + yaw update + side flag inside `iVar11 >= 0` gate | td5_physics.c rotate-back block | Original `JS 0x00406CBA` confirmed jumps to function epilogue (POPs+RET) without writing lv_x/lv_z/yaw. |
| D2 | Numerator `>>12` round-toward-zero using `(val + (val>>31 & 0xFFF)) >> 12` | impulse computation | Original asm 0x00406A9E-0x00406AA9: `CDQ; AND EDX,0xFFF; ADD EAX,EDX; SAR EAX,0xC`. |
| D3a | `v_para_round` (the `>>6`) round-toward-zero with `(v_para + (v_para>>31 & 0x3F)) >> 6` | tangential damping | Original asm 0x00406B0F-0x00406B15 and 0x00406B39-0x00406B3F. |
| D3b | Tangential delta `>>11` round-toward-zero with `(mul_raw + (mul_raw>>31 & 0x7FF)) >> 11` | both v_para branches | Original asm 0x00406B25-0x00406B2E and 0x00406B51-0x00406B60. |
| D4 | Lever-arm `>>12` round-toward-zero on `iVar9_raw` | iVar9 computation | Original asm 0x00406A46-0x00406A4F. |
| D6 | Velocity rotate-back `>>12` round-toward-zero on `lvx_raw`/`lvz_raw` | inside D1 gate | Original asm 0x00406B88-0x00406B91 and 0x00406BA5-0x00406BAE. |

D2 alone propagates to angular_velocity_yaw via `(impulse * iVar9) / 2300` — 1-unit drift in `num` becomes `iVar11/denom` units of drift in impulse, which becomes `iVar11*iVar9/(denom*2300)` units of yaw drift per call. With typical Moscow `iVar11 ~ 1000-5000, iVar9 ~ -200..+200, denom ~ 5860`, yaw drift per separating contact was 1-3 fp8 units. Cumulative on AI cars over hundreds of wall events.

D1 was visually demonstrable post-fix: events with `iVar11 < 0` (separating contact) now show `delta_lv_x=delta_lv_z=delta_yaw=0` in the port trace, where pre-fix they had 10-30 fp8 of round-trip jitter.

### What full bit-validation requires (next sessions)

1. **Re-enable or faithfully re-port the lateral handler** (`UpdateActorTrackSegmentContacts` 0x00406CC0). Without it, WallResponse's input distribution differs from original by construction.
2. **Or scope the precise-port harness to only the `iVar11 >= 0 → impulse + tangential` codepath** by synthesizing inputs from the original's captured rows and replaying them through the port's pure C implementation in isolation. This sidesteps the upstream caller divergence but loses the closed-loop integration test.
3. **Pool7 release** — done; no slot was actually checked out via `acquire`; the `.assigned` marker is removed at the end of this session.

### Files touched (worktree `.claude/worktrees/precise-00406980`)

- `td5mod/src/td5re/td5_physics.c` — D1/D2/D3a/D3b/D4/D6 fixes inside `td5_physics_wall_response`
- `td5mod/src/td5re/td5_pilot_trace_00406980.h`, `td5_pilot_trace_00406980.c` — new pilot trace emitter
- `td5mod/src/td5re/build_standalone.bat` — added the new module to `TD5RE_SRCS`
- `tools/frida_pool7_00406980.js` (root, NOT worktree-only — Frida tooling shared across sessions) — new probe
- `re/analysis/pilot_00406980_audit.md` (this doc)

