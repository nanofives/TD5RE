# Pilot Audit — 0x0040A720 AngleFromVector12

**Date:** 2026-05-14
**Pool slot:** TD5_pool2
**Port-side function:** `AngleFromVector12` @ `td5_render.c:4343`
**Worktree:** `.claude/worktrees/precise-0040A720` on branch `precise-0040A720`
**Caller graph:** 21 callers across track/AI/physics/render/vfx — pure leaf math function called every tick from many sites.
**Callee graph:** none (leaf).
**Body:** 341 bytes / 142 instructions / 47 decompiled lines.

## Function summary

12-bit-angle atan2 from a 2D signed vector `(param_1, param_2)`. Convention:
- `param_1` = "horizontal" component (the one growing east in track-heading callers, i.e. **dx**)
- `param_2` = "vertical" component (the one growing north, i.e. **dz**)
- Return = 12-bit angle (0..0xFFF) measured **clockwise from +param_2 axis** (so 0 = +Z/north, 0x400 = +X/east, 0x800 = -Z/south, 0xC00 = -X/west) — matches the game's "yaw" convention.
- `(0, 0)` returns 0.

This is the foundational atan2 used by `AngleFromVector12Full` (0x00433FC0) and 20+ direct callers. **Pure function, no global state.**

## Algorithm — LUT-based octant dispatch

The function uses a 1024-entry int16 LUT at `DAT_00463214` (verified: dumped via `mcp__ghidra__memory_read`):
- `LUT[i] = round( atan(i / 1024) * 2048 / pi )` for `i in [0, 1023]`, range `[0, 511]`.
- `LUT[1024] = 0x200` (silently used by the octant-2 diagonal case when `param_1 == param_2`).

**Index formula (octant 0, both positive, param_1 < param_2):**
```
idx = (param_1 * 1024 + (param_2 >> 1)) / param_2     ; signed div, trunc toward zero
return LUT[idx]
```
That's `round(param_1 / param_2 * 1024)` via "add half-divisor then truncate". (Note: when param_2 is negative this rounds toward zero rather than away — a non-symmetric quirk that's preserved literally below.)

**8 octants total**, dispatching by sign of param_1, param_2 and whichever absolute value is larger:

| Quadrant (p1, p2 signs) | Inner test | Index numerator | Divisor | Sign of LUT lookup | Bias |
|--|--|--|--|--|--|
| (+, +, p1 <  p2) | octant 0 | p1*1024 + p2/2 | p2 | + | 0 |
| (+, +, p1 >= p2) | octant 1 | p2*1024 + p1/2 | p1 | + | 0x400 (subtract LUT) |
| (+, -, -p2 <= p1) | octant 2 | p2*1024 - p1/2 | p1 | - (negated idx) | 0x400 (add LUT) |
| (+, -, -p2 >  p1) | octant 3 | p1*1024 - p2/2 | p2 | - (negated idx) | 0x800 (subtract LUT) |
| (-, -, -p1 <= -p2) | octant 4 | p1*1024 + p2/2 | p2 | + | 0x800 (add LUT) |
| (-, -, -p1 >  -p2 OR p1==p2) | octant 5 | p2*1024 + p1/2 | p1 | - (negated idx via *-2) | 0xC00 (subtract LUT) |
| (-, +, p2 <= -p1) | octant 6 | p2*1024 - p1/2 | p1 | - (negated idx) | 0xC00 (add LUT) |
| (-, +, p2 >  -p1) | octant 7 | p1*1024 - p2/2 | p2 | - (negated idx) | 0x1000 (subtract LUT) |

The "negated idx" trick in the assembly is:
```
SHL EAX, 1            ; 2*quotient (may be negative)
MOV ECX, 0x463214
SUB ECX, EAX          ; LUT_base - 2*quotient = LUT_base + 2*|quotient| if quotient<0
MOVSX EDX, word[ECX]
```
which is equivalent to indexing `LUT[-quotient]` when the quotient is negative. The decomp expresses this as `&DAT_00463214 + idx * -2`.

## Confirmed divergence points (port vs listing)

### D1 — atan2/lround approximation **(HIGH IMPACT)**
Port: `td5_render.c:4343-4357`:
```c
int AngleFromVector12(int x, int z) {
    double rad = atan2((double)x, (double)z);
    long angle = lround(rad * (4096.0 / (2.0 * M_PI)));
    return (int)(angle & 0xFFF);
}
```

This is a floating-point approximation that diverges from the LUT for many inputs by ±1. Documented in the port's own comment block at td5_render.c:4344-4353: confirmed gap of `disp_yaw=3824 vs 3823` at sim_tick=1 post_ai.

**Root cause:** the original LUT applies a per-octant rounding via integer division (which truncates the quotient toward zero, with the `+ (divisor/2)` term implementing round-half-up — but only when the divisor is positive; negative-divisor branches truncate toward zero without rounding). The fp atan2 + lround can never reproduce this exactly because the truncation point doesn't coincide with the fp midpoint.

**Fix:** port the literal LUT and the 8-branch dispatch. The LUT is 2050 bytes (1025 int16 entries — 1024 from the table plus the silent `LUT[1024] = 0x200` that the diagonal case (param_1==param_2>0) reads).

### D2 — LUT[1024] over-read **(LOW IMPACT, edge case)**
The original's octant 1 (p1>=p2>0) computes `idx = (p2*1024 + p1/2)/p1`. When `p1==p2`: `idx = (p1*1024 + p1/2)/p1 = 1024` exactly. The original reads LUT[1024] from memory past the declared 1024-entry table — at 0x00463A14 the byte is `0x0200`, which atan2 says should be `round(atan(1)*2048/pi)=512=0x200`. So **LUT[1024]=0x200 is mathematically correct** and the original silently exploits this. Port must include 1025 entries to match.

### D3 — Signed integer division semantics **(MEDIUM IMPACT)**
The original uses x86 `IDIV` which truncates toward zero. C99+ also truncates toward zero (`-7 / 2 == -3`), so no fix needed — but the literal C must use plain `/` operator on `int32_t`, not `>>` or `floor()`-style ops.

### D4 — Signed right shift `param_X >> 1` **(LOW IMPACT)**
`SAR EDX, 1` is an arithmetic shift: -7 >> 1 == -4 (truncates toward -inf). Port must use `>> 1` on `int32_t` (implementation-defined in C99 but universally arithmetic shift on x86 GCC). Direct compile of `int32_t >> 1` produces `SAR` instruction. No issue.

## Capture schema for pilot

Per call:
- **Keys:** `(p1, p2)` — the input pair, since this is a pure function.
- **Inputs:** `p1`, `p2` (int32)
- **Outputs:** `ret` (int32, low 12 bits significant)
- **Context:** `sim_tick`, `caller_ra` (informational), `call_idx_in_tick` (informational, helps catch call-order parity gaps but not used as a join key)

Schema is 6 columns. Diff keys by `(p1, p2)` so call-order divergence doesn't pollute the diff — for any input pair, port and original should return the same answer regardless of when/where it's called.

## Next actions for the pilot

1. **Generate** `tools/frida_pool2_0040A720.js` to capture `(p1, p2, ret)` on every call to 0x0040A720.
2. **Add** port-side trace emit `td5_pilot_trace_0040A720_enter(p1, p2, ret)` invoked from every call to `AngleFromVector12`.
3. **Port** the literal LUT-based implementation in `td5_render.c`.
4. **Build** the worktree.
5. **Run** one Edinburgh race (track=1, slot 0, span 0) capturing both traces with `PlayerIsAI=1`.
6. **Diff** by `(p1, p2)`.
7. **Iterate** until zero diff.

## Acceptance gate

For the union of `(p1, p2)` pairs observed across both port and original captures, **port `ret` must equal original `ret` exactly** (no ±1 tolerance).

## Result (2026-05-14)

**SHIPPED — zero diff on captured (p1, p2) pairs.**

Edinburgh AI race, sim_tick 0-30:
- Original captures: **3540 calls**, all match `angle_decomp` (Python translation of Ghidra decomp) exactly.
- Port captures: **2296 calls**, all match `angle_decomp` exactly.
- Common (p1, p2) pairs at any tick: **73** — zero mismatches.
- Common (p1, p2) pairs at sim_tick ≥ 5: **49** — zero mismatches.

Per-input verification (more robust than overlap-driven diff since this is a pure
function): both sides individually agree with the Ghidra-faithful decomp on every
captured input pair (5836 total calls across the two captures).

Old port (atan2 + lround) had `disp_yaw=3824 vs 3823` divergence at sim_tick=1 post_ai.
The literal LUT port closes this. Across a 37507-pair Python sweep, the old atan2
implementation diverged from the LUT on **4052 pairs (10.8%)** with the majority being
±1 jitter and two ±512 outliers from the (-1,-1) / (-1,+1) past-end-LUT degenerate
cases.

## Fixes applied (this branch)

1. **`td5_render.c`** — replaced the `atan2()/lround()` implementation of
   `AngleFromVector12` with a literal LUT-based port mirroring the 0x0040A720
   listing. Embedded the full 1024-entry `DAT_00463214` table plus two silent
   past-end entries (indices 1024 → 512 and 1025 → 0) verified against
   `mcp__ghidra__memory_read` at 0x00463A14.

2. **`td5_render.c`** — added 8-octant dispatch matching the assembly's branch
   structure exactly, including the JZ-on-zero edge cases and the
   "negative-index trick" (`&LUT[0] + idx * -2` → `LUT[-idx]`) for the four
   octants where the quotient is intentionally negative.

3. **`td5_pilot_trace_0040A720.{c,h}`** — new module emitting per-call
   `(sim_tick, call_idx, caller_ra, p1, p2, ret)` rows to
   `log/port/pool2_0040A720.csv` when `TD5_PILOT_TRACE_0040A720` is defined.

4. **`build_standalone.bat`** — added `-DTD5_PILOT_TRACE_0040A720` and registered
   the new module in TD5RE_SRCS.

5. **`tools/frida_pool2_0040A720.js`** — original-side capture hook on 0x0040A720.

6. **`tools/verify_0040A720.py`** — offline Python validator with both
   `angle_decomp` (literal Ghidra translation) and `angle_listing` (literal
   translation of my C port) implementations plus a parameterized sweep. Catches
   port-vs-decomp drift without a build/run round-trip.

## Residual risks

- **Past-end LUT reads:** the original silently reads two bytes past the
  declared 1024-entry table for two degenerate inputs (-1, ±1). The port
  preserves this by storing those bytes at LUT[1024]=512 and LUT[1025]=0. If
  the original's binary layout changes (e.g., a recompile of TD5_d3d.exe with
  a different segment-data ordering), the past-end reads could shift. The
  port pins the values to the Ghidra-verified ones, so this is one-way
  faithful only.
- **Idx > 1025:** no input encountered in this 5836-call capture window
  triggers idx > 1025. A static-sweep over |inputs| ≤ 10000 confirms no
  additional overflow cases exist.
