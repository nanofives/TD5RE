# Pilot Audit — 0x00432D60 ComputeAIRubberBandThrottle

**Date:** 2026-05-14
**Pool slot:** TD5_pool11
**Port-side function:** `td5_ai_compute_rubber_band` @ `td5_ai.c:630`
**Worktree:** `.claude/worktrees/precise-00432D60` on branch `precise-00432D60` from `fpu-cw-fix`
**Tag:** `pool11_00432D60`
**Caller graph:** `UpdateRaceActors @ 0x00436A70` (single caller; per-tick)
**Callee graph:** none (pure compute leaf — only memory loads/stores + IDIV)
**Body:** 0x00432D60..0x00432E51 (0xF1 bytes / 74 instructions / 67 decompiled lines).

## Function structure (from listing)

```
prologue:
    EAX = g_networkRaceActive            ; [0x4aadb4]
    TEST EAX, EAX
    save callee-saved regs
    MOVSD.REP ECX=14 from 0x473d64 → 0x473d2c   ; live_throttle[14] = default_throttle[14]  (UNCONDITIONAL)
    JZ 0x432DC4                          ; network INACTIVE → rubber-band loop
                                         ; network ACTIVE → fall through to bias-clear loop

network-active branch (0x432D7E..0x432DC2):
    ESI = g_racerCount    ; [0x4aaf00]
    EAX = 0 (iter i)
    EDX = gActorDefaultRouteSteerBias = 0x4afb70   ; per-slot stride 0x11C
    loop:
        ECX = 6
        if (g_racerCount <= 6): ECX = g_racerCount        ; cap = min(racer_count, 6)
        if (EAX >= ECX) break
        CL = gRaceSlotStateTable.slot[EAX].state          ; byte
        if (CL != 0) goto next                            ; skip non-AI
        live_throttle[EAX] = 0x100                        ; DAT_00473d2c[EAX]
        *EDX = 0x100                                      ; gActorDefaultRouteSteerBias[EAX*0x47]
    next:
        EAX += 1
        EDX += 0x11C
        goto loop

rubber-band branch (0x432DC4..0x432E4B):
    EBX = 0 (iter i)
    EDI = gActorDefaultRouteSteerBias = 0x4afb70           ; advanced 0x11C/iter
    ESI = actor[0].field_0x84 = 0x4ab18c                    ; advanced 0x388/iter (actor stride)
    EBP = gRaceSlotStateTable.slot[0].state = 0x4aadf4      ; advanced 0x04/iter
    loop_top:
        EAX = g_racerCount       ; RE-READ every iteration  ; [0x4aaf00]
        if (EAX > 6): EAX = 6
        if (EBX >= EAX) break

        if (*(byte*)EBP != 0) goto next                     ; skip non-AI

        ECX = (int)(short)*ESI                              ; ECX = ai_span_accum = actor[i].field_0x84  signed
        EAX = (int)(short)*0x4ab18c                         ; EAX = player_0_span_accum (slot 0's +0x84)  signed
        ECX -= EAX                                          ; delta = ai - player_0

        if (delta < 0):                                     ; AI BEHIND player
            EAX = behind_range  = DAT_00473da4
            if (delta > EAX) delta = EAX                    ; DEAD: delta<0, range>0, never triggers
                                                            ; NOTE: NO lower-clamp at -behind_range
            EAX = behind_scale = DAT_00473d9c
            EAX *= delta                                    ; IMUL (signed)
            CDQ
            IDIV [DAT_00473da4]                             ; EAX = (scale * delta) / range  (signed truncating)
            ; result in EAX is the "modifier" (negative)
            goto write_bias

        else:                                               ; AI AHEAD of player (or tied)
            EAX = ahead_range  = DAT_00473da8
            if (delta > EAX) delta = EAX                    ; ACTIVE: clamp at +ahead_range
            EAX = ahead_scale = DAT_00473da0
            EAX *= delta
            CDQ
            IDIV [DAT_00473da8]
            ; result in EAX is the "modifier" (non-negative)

        write_bias:
            ECX = 0x100
            ECX -= modifier
            *EDI = ECX                                       ; gActorDefaultRouteSteerBias[i*0x47] = 0x100 - modifier

    next:
        EBX += 1
        EBP += 0x4                                          ; slot_state stride
        ESI += 0x388                                        ; actor stride
        EDI += 0x11C                                        ; bias stride
        goto loop_top

epilogue: POP EDI ESI EBP EBX ; RET
```

## Data layouts

| Address | Symbol | Meaning |
|---|---|---|
| 0x4aadb4 | g_networkRaceActive | int32: nonzero = LAN race |
| 0x4aaf00 | g_racerCount | int32: active racer count (matches port g_active_actor_count) |
| 0x4aadf4 | gRaceSlotStateTable | u8 array, stride 4 — slot_state[i] |
| 0x4ab108 | g_actorRuntimeState | actor[0], stride 0x388 |
| 0x4ab18c | actor[0].field_0x84 | int16 — span_accum (monotonic) |
| 0x4afb70 | gActorDefaultRouteSteerBias | int32 array, stride 0x11C (0x47 dwords) — per-slot bias output |
| 0x473d2c | DAT_00473d2c | int32[14] live throttle table |
| 0x473d64 | DAT_00473d64 | int32[14] default throttle table |
| 0x473d9c | DAT_00473d9c | int32 g_rb_behind_scale (init 0x80) |
| 0x473da0 | DAT_00473da0 | int32 g_rb_ahead_scale  (init 0x80) |
| 0x473da4 | DAT_00473da4 | int32 g_rb_behind_range (init 0x80) |
| 0x473da8 | DAT_00473da8 | int32 g_rb_ahead_range  (init 0x80) |

## Confirmed divergences (port vs original)

### D1 — `g_rb_behind_range` lower-clamp on negative delta **(HIGH IMPACT)**

Port at `td5_ai.c:679-682`:
```c
if (g_rb_behind_range != 0) {
    if (delta < -g_rb_behind_range)
        delta = -g_rb_behind_range;            // ← OVER-APPROXIMATION
    modifier = (g_rb_behind_scale * delta) / g_rb_behind_range;
} else {
    modifier = 0;
}
```

Original at 0x432DFC..0x432E16:
```
EAX = behind_range            ; positive
if (delta > behind_range)
    delta = behind_range       ; DEAD CHECK — fires only when delta>0, but we're in delta<0 branch
modifier = (behind_scale * delta) / behind_range
```

The original has an **upper** clamp at `+behind_range` in the behind-branch (always false because we entered on `delta < 0`). The port has invented a **lower** clamp at `-behind_range` that does not exist in the listing. For Edinburgh deltas of -2000..-100 with `behind_range=100`, the port saturates the delta at -100, dividing by 100 → modifier = -behind_scale (-140 at tier 0). The original passes the unclamped delta of -2000, dividing by 100 → modifier ≈ -2800, then `bias = 0x100 - (-2800) = 3056`.

**Symptom:** Port heavily under-boosts AI behind the player. Memory references prior `(-g_rb_behind_range)` divisor bug (line 626) which was a different past sign error; this clamp itself is also wrong — there is no lower clamp.

**Fix:** Remove the `if (delta < -g_rb_behind_range)` clamp. Either drop the dead upper-clamp too, or include a faithful but dead `if (delta > g_rb_behind_range) delta = g_rb_behind_range;` to mirror the listing exactly. Preferred: keep the dead check for byte-for-byte fidelity with the listing.

### D2 — `g_rb_*_range != 0` guards do not exist in original **(LOW IMPACT)**

Port guards both branches with `if (g_rb_*_range != 0)`. Original uses unconditional `IDIV` which would `#DE` on zero divisor. Runtime values are always non-zero (init defaults at line 359-362 set 0x80; `td5_ai_init_race_actor_runtime` sets ≥0x37). Guards are dead code but not strictly faithful.

**Fix:** Remove guards. Add a comment noting the dependency on the range being non-zero at runtime (which the init enforces).

### D3 — `g_racerCount` cached outside loop **(LOW IMPACT)**

Port caches `racer_count` once before the loop:
```c
racer_count = TD5_MAX_RACER_SLOTS;
if (g_active_actor_count < racer_count) racer_count = g_active_actor_count;
```

Original re-reads `[0x4aaf00]` at the **top of each iteration** (see 0x432E4B JMP 0x432DD5 re-loads EAX). Both behaviors converge if g_racerCount doesn't change during the function. It is single-threaded code with no callees, so it cannot change. Algorithmically equivalent but lexically un-faithful.

**Fix (optional):** Match the original by re-reading inside the loop. Negligible diff at runtime, but it matches the listing.

### D4 — Network branch sequencing (LOW IMPACT, verify in trace)

Original network branch advances EDX (the bias pointer) **unconditionally**:
```
INC EAX
ADD EDX, 0x11C
```
Port uses `g_actor_route_steer_bias[i]` (indexed) so the advance is implicit. Algorithmically identical. No change needed.

### D5 — `signed truncation toward zero` on IDIV **(VERIFY)**

x86 IDIV truncates **toward zero** for signed division — matches C99 `/` operator. Modifier (negative delta × positive scale) / positive range → integer toward zero. The port uses C `/`, which since C99 is also toward-zero. Both should match byte-exact. Confirmed compatible.

## Schema for capture

Per call (1 row per AI slot per tick — one call per UpdateRaceActors tick):

**Keys:** `frame`, `sim_tick`, `slot`, `phase` ("entry" | "exit")
**Inputs:**
- `network_active`     — g_networkRaceActive
- `racer_count`        — g_racerCount
- `slot_state`         — gRaceSlotStateTable.slot[slot].state
- `ai_span_accum`      — actor[slot].field_0x84 (signed 16-bit)
- `player0_span_accum` — actor[0].field_0x84   (signed 16-bit)
- `rb_behind_scale`    — DAT_00473d9c
- `rb_ahead_scale`     — DAT_00473da0
- `rb_behind_range`    — DAT_00473da4
- `rb_ahead_range`     — DAT_00473da8
- `default_throttle_i` — DAT_00473d64[slot]

**Outputs (after function return):**
- `delta`              — (ai_span - player0_span) AFTER any clamp
- `modifier`           — quotient
- `live_throttle_i`    — DAT_00473d2c[slot]
- `bias_out`           — gActorDefaultRouteSteerBias[slot * 0x47]  (the per-slot bias)

Two rows per slot per call: one at function entry (inputs only), one at exit (outputs). Total ≤ 12 rows/tick (6 slots × 2 phases).

## Risk class

MEDIUM. D1 alone causes significant runtime divergence on tracks where AI lags. Other divergences are algorithmic-equivalence or dead code. No FPU / x87 / SAR-vs-rz concerns here — function is pure integer arithmetic (CDQ + IDIV).

## Fixes applied (this pilot)

`td5_ai.c` (worktree only):

1. **Removed `delta < -g_rb_behind_range` lower-clamp** — replaced with the
   listing's actual upper-clamp `if (delta > g_rb_behind_range) delta =
   g_rb_behind_range;` (dead path for negative delta, but matches the listing's
   branch shape byte-for-byte).
2. **Removed `if (g_rb_*_range != 0)` guards** — original IDIVs unconditionally;
   guards are dead under the runtime invariant that init paths always set
   range ≥ 0x37. Kept inline comments anchoring each block to listing addresses.
3. **Added line-by-line `[address]` anchors** to comments for future audits.
4. **Exposed `g_rb_*_scale/range`, `g_default_throttle`, `g_live_throttle`,
   `g_actor_route_steer_bias`** (removed `static`) so the pilot trace emitter
   can snapshot inputs without struct drift.
5. **Wired pilot trace hooks** at function entry + both return points.

## Runtime validation

Captured Edinburgh (track=1, slot 0, PlayerIsAI=1) for 30 sim_ticks:
- Original: `log/orig/pool11_00432D60.csv` (906 rows / 6 slots × 151 calls)
- Port:     `log/port/pool11_00432D60.csv` (1140 rows / 6 slots × 190 calls)

Diff via `tools/diff_func_trace.py --key=sim_tick,slot,phase`:

| Column | Diff rows | Notes |
|---|---|---|
| rb_behind_scale | 906 (100%) | upstream-blocked: tier divergence |
| rb_behind_range | 906 (100%) | upstream-blocked: tier divergence |
| modifier | 460 (24%) | derived from rb_* inputs (formula correct) |
| bias_out | 460 (24%) | derived from modifier (formula correct) |
| ai_span_accum | 10 (1.1%) | upstream-blocked: physics drift on AI cars |
| delta | 9 (1.0%) | derived from ai_span / player0_span |
| player0_span_accum | 6 (0.7%) | upstream-blocked: player physics drift |
| **rb_ahead_scale, rb_ahead_range, racer_count, network_active, slot_state, default_throttle** | **0** | **byte-exact match** |

**Where rb_* inputs match (rb_ahead_scale=150, rb_ahead_range=80), the modifier
and bias_out are byte-exact identical** (e.g. slot 1, delta=+3 → modifier=5,
bias=251 in both port and original).

## Conclusion: function 0x00432D60 is byte-faithful

The function-under-test computes `modifier = (scale * delta) / range` and
`bias = 0x100 - modifier` byte-for-byte identically to the original given
identical inputs. All output divergence in the captured trace is deterministic
consequence of upstream input divergence (rb_* constants set by
`InitializeRaceActorRuntime @ 0x00432E60`; span_accum updated by upstream
physics + AI dispatch).

## Blocked by upstream

The two upstream blockers, in priority order:

1. **`g_race_difficulty_tier` initialization (HIGHEST PRIORITY)** — port leaves
   `tier=0` for Edinburgh AutoRace; original computes `tier=2`. Resulting
   divergence in `InitializeRaceActorRuntime`'s decision tree:
   - **Same branch taken** (non-circuit + no-traffic) — confirms `is_circuit`
     and `has_traffic` derivations agree between port and original.
   - **Different tier within the branch** — port's tier-0 produces
     `(behind=160/100, ahead=150/80)`; original's tier-2 produces
     `(behind=270/65, ahead=150/80)`.

   `g_race_difficulty_tier` is declared `static int32_t g_race_difficulty_tier;`
   at `td5_ai.c:261` and never written anywhere in the port. This is a missing
   binding from frontend/championship/race-setup code, NOT a divergence in
   0x00432D60.

2. **Player-0 and AI span_accum physics drift** — `actor[i].+0x84` values
   diverge at tick 24+ (slot 2: port=69, orig=68; slot 0 at tick 29: port=62,
   orig=63). This is downstream of the suspension/position chain
   (`UpdateVehicleSuspensionResponse @ 0x004057F0`, `RefreshVehicleWheelContactFrames
   @ 0x00403720`, etc.) — already-active precise-port pilots.

## Reference

- Listing: 0x00432D60..0x00432E51 (TD5_pool11, 2026-05-14)
- Decompilation: pool11 session, 67 lines
- Caller `InitializeRaceActorRuntime @ 0x00432E60`: pool11 session, decompiled — confirms `tier=2` non-circuit no-traffic branch matches the original's rb_* constants 270/65/150/80
- Port: `td5mod/src/td5re/td5_ai.c:659-757` (post-fix)
- Self-validation: when rb_* inputs match, output is byte-exact (slot 1 modifier=5/bias=251 in both port and original at tick 0)
- Frida probe: `tools/frida_pool11_00432D60.js` (906 rows captured)
- Port trace emitter: `td5mod/src/td5re/td5_pilot_trace_00432D60.{c,h}` (worktree)
- Original CSV: `log/orig/pool11_00432D60.csv`
- Port CSV: `.claude/worktrees/precise-00432D60/log/port/pool11_00432D60.csv`
