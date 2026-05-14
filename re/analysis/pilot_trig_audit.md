# Pilot Audit — 0x0040A6A0 CosFloat12bit + 0x0040A6C0 SinFloat12bit

**Date:** 2026-05-14
**Pool slot:** TD5_pool3
**Worktree:** `.claude/worktrees/precise-trig` on branch `precise-trig`
**File tag:** `pool3_trig`
**Port-side function:** `CosFloat12bit`/`SinFloat12bit` @ `td5_render.c:4327-4333`
**Body:** 16 bytes / 4 instructions (cos), 21 bytes / 5 instructions (sin) — pure LUT lookups.

## Function structure (from listing)

### 0x0040A6A0 CosFloat12bit (signature `float10 __cdecl CosFloat12bit(uint)`)

```asm
0040a6a0  MOV  EAX,[ESP + 0x4]              ; angle arg (cdecl)
0040a6a4  AND  EAX,0xfff                    ; mask to 12-bit
0040a6a9  FLD  float ptr [EAX*4 + 0x488984] ; load LUT[arg & 0xFFF] as float10
0040a6b0  RET
```

### 0x0040A6C0 SinFloat12bit (signature `float10 __cdecl SinFloat12bit(int)`)

```asm
0040a6c0  MOV  EAX,[ESP + 0x4]              ; angle arg
0040a6c4  ADD  EAX,0xfffffc00               ; EAX -= 0x400 (shift cos -> sin by π/2)
0040a6c9  AND  EAX,0xfff                    ; mask to 12-bit
0040a6ce  FLD  float ptr [EAX*4 + 0x488984] ; load LUT[(arg - 0x400) & 0xFFF]
0040a6d5  RET
```

Both functions return on the x87 FPU stack (ST0) as the standard cdecl float10 convention. Caller does `FSTP dword/qword/tbyte` and the 80-bit ST0 gets rounded to the destination width.

## LUT: g_sinCosFloatTable @ 0x00488984

Runtime-initialized by `BuildSinCosLookupTables @ 0x0040A650` once at startup. Disk-image bytes are all zero. The init body is:

```asm
0040a650  PUSH EBP
0040a651  MOV  EBP,ESP
0040a653  SUB  ESP,0x10
0040a656  XOR  ECX,ECX
0040a658  XOR  EAX,EAX
0040a65a  MOV  [EBP-0x10],EAX               ; lo64 = 0 (FILD source low)
0040a65d  MOV  [EBP-0xc],ECX                ; hi64 = 0 (FILD source high)
0040a660  FILD qword [EBP-0x10]             ; ST0 = (double)i (treated as int64)
0040a663  FMUL double [0x0045d610]          ; ST0 *= 2π    (double: 0x401921fb54442d18 → 6.283185307179586)
0040a669  FMUL double [0x0045d608]          ; ST0 *= 1/4096 (double: 0x3f30000000000000)
0040a66f  FCOS                              ; ST0 = cos(ST0)
0040a671  FST  float [EAX*4 + 0x488984]    ; store *32-bit float* — non-popping
0040a678  FMUL float [0x0045d604]           ; ST0 *= 4096.0 (float)
0040a67e  FSTP float [EBP-0x4]              ; pop to temp
0040a681  FLD  float [EBP-0x4]              ; reload
0040a684  FISTP dword [EBP-0x8]             ; round-to-int per current FPU rounding mode
0040a687  MOV  EDX,[EBP-0x8]
0040a68a  MOV  [EAX*4 + 0x483984],EDX       ; store to *int* LUT DAT_00483984
0040a691  INC  EAX
0040a692  CMP  EAX,0x1400                  ; 5120 entries (extra ¼ circle past wrap)
0040a697  JL   0x0040a65a
0040a699  MOV  ESP,EBP
0040a69b  POP  EBP
0040a69c  RET
```

**Constants (read live from `.data`):**

| Addr | Width | Bytes (LE) | Value |
|------|-------|-----------|-------|
| 0x0045d604 | float (4)  | `00 00 80 45`             | `4096.0f` |
| 0x0045d608 | double (8) | `00 00 00 00 00 00 30 3f` | `0x3f30000000000000` = `1.0 / 4096.0` (≈2.4414e-4) |
| 0x0045d610 | double (8) | `18 2d 44 54 fb 21 09 40` | `0x400921fb54442d18` = **2π** ≈ 6.283185307179586 — wait, actual bytes from memory read: `1c 77 42 54 fb 21 19 40` = `0x401921fb54427711` |

Re-decoding 0x0045d610: 8 bytes little-endian = `0x40 19 21 fb 54 42 77 1c` → IEEE 754 double sign=0, exp=0x401, mantissa=0x921fb54427711 → 4 × (1 + 0x921fb54427711/2^52) ≈ 4 × 1.5707963267948965 = **6.283185307179586** = 2π. Confirmed.

So `BuildSinCosLookupTables` runs: for `i in 0..0x1400`:
```
angle_rad = (i * 2π) / 4096       (double precision)
cos_val   = fcos(angle_rad)       (x87 FCOS, 80-bit)
float_lut[i] = (float)cos_val     (FST truncates to 32-bit)
int_lut[i]   = round(cos_val * 4096.0)  (FISTP, current rounding mode)
```

The size is `0x1400 = 5120` — that's the full 4096-step circle plus an extra `0x400` (quarter turn) past the wrap, so that `SinFloat12bit` (which reads `cos[(angle - 0x400) & 0xFFF]`) and any in-bounds read with `& 0xFFF` is safe; the extra room is only ever touched if someone forgets to mask. Since the implementations DO mask, only `LUT[0..0xFFF]` matter functionally — the rest is just padding the init touches but the lookup never reads.

## Port-side equivalents

`td5mod/src/td5re/td5_render.c:4327-4333`:

```c
float CosFloat12bit(unsigned int angle) {
    return (float)cos((double)(angle & 0xFFF) * (2.0 * M_PI / 4096.0));
}

float SinFloat12bit(int angle) {
    return (float)sin((double)(angle & 0xFFF) * (2.0 * M_PI / 4096.0));
}
```

## Confirmed divergence points (port vs original)

### D1 — SinFloat12bit argument-shift mismatch **(HIGH IMPACT)**

Original: `cos[(angle - 0x400) & 0xFFF]`, i.e. cos shifted RIGHT by 0x400 in 12-bit angle space (=π/2). Since `cos(θ - π/2) = sin(θ)`, this is sin(θ).

Port: `sin(angle & 0xFFF * 2π/4096)`. Mathematically same function, but the input is masked **before** the shift, where the original shifts **before** mask. For negative `angle`:
- Original: `angle - 0x400` (signed int math, wraps in 32-bit), then `& 0xFFF` — modular shift, well-defined for any int.
- Port: `sin((angle & 0xFFF) * 2π/4096)` — masks first, then computes sin. The mask result is always in [0, 0xFFF], so `sin(masked)` is fine for *one* circle but **shifted by π/2 vs original.**

Wait — let me re-check. Port's `SinFloat12bit(angle)` returns `sin(masked_angle * 2π/4096)`. Original returns `cos((angle - 0x400) & 0xFFF * 2π/4096)`. For all valid angles (any signed int), `cos(θ - π/2) = sin(θ)` so the mathematical result is identical. **NOT a divergence** — verify with test cases:
- angle=0: original = cos(-0x400 & 0xFFF * 2π/4096) = cos((0xFFF - 0x3FF) × ... wait, `(0 - 0x400) = 0xFFFFFC00 in 32-bit unsigned → & 0xFFF = 0xC00`. cos(0xC00 / 4096 × 2π) = cos(3π/2) = 0. Port: sin(0) = 0. Match.
- angle=0x400: original = cos((0x400-0x400) & 0xFFF) = cos(0) = 1. Port: sin(0x400 × 2π/4096) = sin(π/2) = 1. Match.
- angle=0x800: original = cos(0x400) = 0. Port: sin(π) = 0. Match.

So D1 is NOT a divergence in result — both compute sin(angle * 2π/4096). Demoted.

### D2 — LUT vs live-cos **(MAIN DIVERGENCE, LSB level)**

Original stores `(float)fcos((double)(i * 2π / 4096))` in a static LUT and reads it back via `FLD float [LUT]`.

Port computes `(float)cos((double)(angle & 0xFFF) * (2.0 * M_PI / 4096.0))` live.

If MinGW's libm `cos()` produces the same double-precision result as x87 FCOS for all 4096 inputs (i × 2π/4096), then `(float)` truncation gives bit-identical results. **fdlibm and FCOS agree for almost all but a small number of inputs at ULP level.** The well-known "FCOS imprecision" for large arguments doesn't apply here (max input ≈ 2π).

The bigger issue: `(float)cos((double)angle * (2.0 * M_PI / 4096.0))` — the constant `2.0 * M_PI / 4096.0` is folded at compile time by the compiler. The original computes it in **two FMULs at runtime** using x87 80-bit: `FILD i → FMUL 2π → FMUL (1/4096)`. The two-step multiply can give a different LSB than a single combined multiply due to intermediate rounding. Specifically, `i × 2π × (1/4096)` vs `i × (2π / 4096)` — these can differ by 1 ULP in the double-precision intermediate, which can flip the float result.

**Likely symptom:** for some subset of angles (especially near zeros of cos), the LSB of the returned float differs. Downstream effects: tiny drift in rendered orientation, camera position, AI heading. Most won't cause visible divergence, but accumulating physics state may.

### D3 — Compile-time constant precision **(LSB level, related to D2)**

Port uses `(2.0 * M_PI / 4096.0)`. `M_PI` is `3.141592653589793` (double). Compile-time folding gives:
- `2.0 * M_PI` = `6.283185307179586` (exact representable double)
- `/ 4096.0` = `0.0015339807878856412` (rounded once)

Original at runtime:
- ST0 = `i × 6.283185307179586` (one FMUL, 80-bit intermediate)
- ST0 = ST0 × `0.000244140625` (1/4096, exact in binary) — one FMUL, 80-bit intermediate

The 80-bit intermediate vs the 64-bit folded constant give different paths. For most i, the final cos() value rounds to the same double, but for some i where the result is near a representable boundary, they diverge.

### D4 — Calling convention return type **(LOW IMPACT, verify)**

Original signature is `float10 __cdecl`. The ABI for x87 cdecl returns float in ST0. MinGW's `float`-returning `__cdecl` function on i686 also returns via ST0. So the return mechanism matches; the LSB rounding when callers do `FSTP float [...]` will be byte-identical IF the ST0 value is byte-identical.

## Capture schema

Per call (one row per invocation of either function):

**Keys:** `sim_tick`, `which_fn` ("cos" or "sin"), `call_idx` (0-based monotonic within a tick × which_fn)

**Inputs:**
- `arg_raw` (the 32-bit input as signed int)
- `arg_masked` (`(arg - 0x400 if sin else 0) & 0xFFF` — the LUT index actually used)

**Outputs (the bit-exact thing):**
- `ret_float_bits` (uint32 reinterpret of the returned float, captured via the LUT entry directly so we're bit-exact for the original)
- `ret_float_val` (printable repr for human eyes — not joined-on)

A single CSV per session, multiplexed by `which_fn`. Cap to ~5000 rows total.

## Strategy for bit-exact match

The original IS a LUT lookup. To match bit-exactly, the port must also be a LUT lookup over the **same float values**. Two options:

**Option A — Replicate the build:** in `td5_render.c`, allocate `static float g_sinCosFloatTable[0x1400]` and init it at startup with `(float)cos((double)i * 6.283185307179586 / 4096.0)`. Then change `CosFloat12bit`/`SinFloat12bit` to `return g_sinCosFloatTable[(angle [- 0x400]) & 0xFFF];`. This eliminates the live-cos cost and stabilizes the LSB across multiple calls with the same arg. But the bit pattern may still differ from the original's LUT (FCOS in x87 vs MinGW libm cos in double).

**Option B — Embed the original's LUT:** dump 0x488984 from a running TD5_d3d.exe, embed `float g_sinCosFloatTable[0x1400] = { 0x3f800000, ... };` directly. Guaranteed bit-exact. Cost: one-time dump + 20KB of source.

**Pilot plan:** start with A. Capture both, diff. If LSB diffs persist, escalate to B.

## Frida capture approach

Don't try to read ST0 from Frida — fragile. Instead read the LUT memory `[0x488984 + ((arg - 0x400 if sin else 0) & 0xFFF) * 4]` as a uint32 on each call. That IS the value the function will return (modulo x87 80-bit pass-through to caller's FSTP, which is byte-identical for in-range float32 values). Cap probe rows.

## Next actions

1. Author `tools/frida_pool3_trig.js` — attach both addresses, emit row per call with `which_fn`, `arg_raw`, `arg_masked`, `ret_float_bits` (read from LUT).
2. Author `td5_pilot_trace_trig.{c,h}` — port-side equivalent. Hook both port-side functions.
3. Build worktree.
4. Run original-side via quickrace harness with `--extra-script tools/frida_pool3_trig.js`.
5. Run port-side `td5re.exe` with matching CLI overrides.
6. Diff. Iterate.

Definition of done: zero diff on `ret_float_bits` column for both `which_fn=cos` and `which_fn=sin` at sim_tick ≥ 5.

---

## Iteration log + final outcome (2026-05-14)

| Attempt | Strategy | Common (which_fn, arg_masked) keys | Identical | Differing |
|---|---|---|---|---|
| #1 | `(float)cos((double)(angle & 0xFFF) * 2π/4096)` live (master) | 68 LUT entries | 40 | 28 |
| #2 | C `long double` LUT (`cosl(...)`) cached at startup | 108 | 63 | 45 |
| #3 | Inline asm `FILD/FMUL/FMUL/FCOS` LUT, PC=64 | 66 | 42 | 24 |
| #4 | **Embed dumped LUT bits from running TD5_d3d.exe** | 192 | **192** | **0** |

**Surprising finding:** even after matching the original instruction sequence byte-for-byte (`d9 ff` FCOS, same TWO_PI/INV_4096/SCALE_F constants, same PC=64 FPU control word), MinGW's runtime FCOS produced ±1 ULP drifts for ~24 of 66 common indices, including a 5-orders-of-magnitude miss at idx=3072 (cos(3π/2)). The instruction stream is byte-identical, the constants are byte-identical, the FPU CW is byte-identical, the input register state is byte-identical — yet FCOS results differ.

Hypothesis: there is residual x87 micro-state (FTOP, prior FPU register pollution from MinGW's startup-time math) that survives across the `fninit`-less call path and that subtly biases the FCOS internal calculation for near-singular inputs. We did NOT chase this further because:

1. The cost (deeper FPU state RE) is high.
2. The fix (embed the original LUT bytes) is **trivially correct**: it bypasses the whole question by using the very same float bits the original produces at runtime.

**Solution shipped:** dump `g_sinCosFloatTable[5120]` from a live TD5_d3d.exe via `tools/frida_pool3_trig_dump.js` and embed as `td5_trig_lut_data.c` (`const uint32_t td5_trig_lut_bits[5120]`). The port LUT-builder now memcpys these bits into `s_cosFloatTable` and derives the int LUT (`s_cosFixedTable`) via `FISTP` from the float values. Both LUTs are byte-faithful.

Final captures (sim_tick ≥ 5, 5000 rows on each side):
- 192 distinct `(which_fn, arg_masked)` keys exercised by both port and original
- **0 byte-divergences on `ret_float_bits`**
- 126 port-only keys + 600 orig-only keys (different call patterns, but each side's queries return correct bits)

Files of record:
- `tools/frida_pool3_trig.js` — runtime probe (one row per call, reads LUT at function entry)
- `tools/frida_pool3_trig_dump.js` — one-shot binary LUT dump
- `.claude/worktrees/precise-trig/td5mod/src/td5re/td5_pilot_trace_trig.{c,h}` — port-side emitter
- `.claude/worktrees/precise-trig/td5mod/src/td5re/td5_trig_lut_data.c` — embedded 5120-entry LUT (generated)
- `.claude/worktrees/precise-trig/td5mod/src/td5re/td5_render.c` — `CosFloat12bit` / `SinFloat12bit` / `CosFixed12bit` / `SinFixed12bit` now LUT-indexed
- `log/orig/pool3_trig_lut_dump.bin` — reference LUT dump (20 KB, byte-identical to embedded)
- `log/orig/pool3_trig.csv`, `log/port/pool3_trig.csv` — paired trace captures
