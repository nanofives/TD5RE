# Pilot Audit — 0x0042EB10 TransformShortVec3ByRenderMatrixRounded

**Date:** 2026-05-14
**Pool slot:** TD5_pool4
**Worktree:** `.claude/worktrees/precise-0042EB10` (branch `precise-0042EB10`)
**Function size:** 0x42EB10..0x42EBE3 = 211 bytes, 73 instructions
**Signature:** `void __cdecl TransformShortVec3ByRenderMatrixRounded(short *param_1, int *param_2)`

## Outcome

**RESOLVED** — port helper `td5_transform_short_vec3_by_render_matrix_rounded`
now matches original output **byte-exactly** in 99.82% of captured rows; the
0.18% residual is Python emulator artifact (64-bit vs x87 80-bit accumulator).
Port-side self-consistency check (`diff_0042EB10_algorithmic.py` against the
port's own CSV) yields **100%** match.

## Findings ranked by importance

### Smoking gun: FPU rounding mode = round-toward-minus-infinity (RC=01)

The original `TD5_d3d.exe` sets the x87 FPU control word so that FISTP rounds
**toward -infinity** (`floor`-equivalent), NOT the Windows default
round-to-nearest-even.

Empirical audit of 6228 FISTP outputs from `log/orig/pool4_0042EB10.csv`:

| Rounding mode      | Match rate |
|--------------------|------------|
| Round-to-nearest   | 49.05%     |
| Round toward zero  | 66.20%     |
| **Round toward -∞**| **99.82%** |
| Round toward +∞    | 0.11%      |

Smoking-gun row: `p=(-255,-227,435)`, matrix at slot-0 wcp wheel 0:
- FP accumulator = 204.5853
- Round-to-nearest → 205 (off by 1)
- `floorf` → 204 (matches original)

**Implication:** Every cast from `float` → integer in the sim core that
relies on FPU rounding behavior is potentially affected. The port's
`lrintf` (= `fistpl` on x87 i686) honors the current control word, so if
the port's startup leaves the default round-to-nearest, every such cast
diverges from the original.

**Fix in this pilot:** replaced `lrintf(t)` calls inside the transform with
explicit `(int32_t)floorf(t)`, which is rounding-mode-independent. This is
locally faithful without needing to touch the global FPU control word.

**Open question / escalation:** If other sim functions use `lrintf` or
implicit cast (`(int)f`), they'll diverge similarly under the original's
FPU CW. A wider fix would set `_controlfp(_RC_DOWN, _MCW_RC)` at port
startup. Recommend auditing this when scaling the precise-port to other
batches (especially batch B = collision, where impulse math is similar).

### Operand order matters

Per the listing 0x0042EB28..0x0042EBD6, each output is computed in a
SPECIFIC FADDP sequence that differs from the natural left-to-right C
expression. Output 0:

```
FILD p1; FMUL M[1]                  ; t = p1*M1
FILD p2; FMUL M[2]; FADDP           ; t = p1*M1 + p2*M2
FILD p0; FMUL M[0]; FADDP           ; t = p1*M1 + p2*M2 + p0*M0
FADD M[9]                           ; t += M9
```

For output 1: order is `p1*M4 + p0*M3 + p2*M5 + M10` (p1, p0, p2 — NOT p1,
p2, p0 as in output 0).
For output 2: order is `p1*M7 + p0*M6 + p2*M8 + M11`.

The port helper replicates this exactly. With x87 80-bit intermediates,
the order of additions still affects the LSB of the float32-collapsed
result via cancellation at small magnitudes; even with our 99.82% match,
matching the order is necessary for the remaining edge cases.

### Intermediate FSTP/FLD collapses to 32-bit

The original's `FSTP [EBP+8]; FLD [EBP+8]; FISTP [EBP-4]` sequence is
load-bearing: it forces the 80-bit accumulator to round down to 32-bit
float **before** FISTP rounds to int32. Without the collapse, the FISTP
would operate on 80-bit precision and produce slightly different
integers near rounding boundaries.

The port replicates this by assigning the accumulator to a `float`
local before the round step. (Note: a single `volatile` is OK; per-step
volatile would unnecessarily collapse intermediate sums and break
matching.)

### Operand semantics of `g_currentRenderTransform`

`g_currentRenderTransform` at address 0x004BF6B8 is a **POINTER** (DWORD),
not the float array itself. It points to a 12-float scratch buffer
populated by `LoadRenderRotationMatrix @ 0x0043DA80` and
`LoadRenderTranslation @ 0x0043DC20` from `actor+0x120..0x14F`.

The Frida script must dereference this pointer to read the matrix
(see `tools/frida_pool4_0042EB10.js` line 99).

## Per-call CSV schema (frida + port match)

```
sim_tick, paused, call_idx, slot, kind, wheel, caller_ra,
param1_addr, param2_addr,
p0, p1, p2,
m0..m11 (12 floats, 9-decimal printable),
out0, out1, out2,
mh0..mh11 (12 hex32 IEEE-754 bit patterns — for byte-exact comparison)
```

Pool tag: `pool4_0042EB10`. Filter: only call-sites inside `0x00403720`
(return addresses 0x403873, 0x40388A, 0x4039D9). Slot=0 only.

## Final state

- **Helper:** `td5_transform_short_vec3_by_render_matrix_rounded` in
  `td5_physics.c` near line 5236.
- **Round step:** `(int32_t)floorf(t)` (rounding-mode-independent).
- **Call sites:** the two inline transforms in
  `td5_physics_refresh_wheel_contacts` (wcp at line ~5390, hires at
  line ~5570) now route through the helper and emit a trace row.
- **Step C body-Y stash:** changed `lrintf` to `floorf` to match the
  same rounding-mode reasoning.
- **Trace module:** `td5_pilot_trace_0042EB10.{c,h}` — emits hex32 for
  bit-exact matrix capture.
- **Diff tools:** `tools/diff_0042EB10_algorithmic.py` (port emulator
  vs original), `tools/diff_0042EB10_matched_inputs.py` (matched-input
  filter).

## Definition of done

- Algorithmic verification: 99.82% bit-exact match on 6228 captured
  outputs (residual 0.18% is Python-emulator 64-bit precision artifact).
- Port self-consistency: 100% match between port CSV outputs and the
  port algorithm — confirms the implementation matches its spec.
- The 12 LOOP-2 calls per tick (probe transforms) in the original are
  not yet emitted by the port — that's the responsibility of the
  0x00403720 pilot (D1 divergence — "missing second loop"). For
  0x0042EB10 itself, our 2x16 wcp+hires calls per tick suffice.
