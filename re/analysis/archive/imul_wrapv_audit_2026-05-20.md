# IMUL signed-overflow audit — port build flags

**Date:** 2026-05-20
**Result:** REAL BUG. Port is missing `-fwrapv`. Signed overflow is currently UB at `-O2`. One-line fix in two files.

## Finding

The port's build flags (`td5mod/Makefile:70`, `td5mod/src/td5re/build_standalone.bat:34`):

```
-c -O2 -Wall -Wextra -Wpedantic -DWIN32 -m32 ... -DTD5_INFLATE_USE_ZLIB
```

No `-fwrapv`. No `-fno-strict-overflow`. GCC at `-O2` treats signed integer overflow as **undefined behavior** and is permitted to:
- Eliminate "impossible" branches assuming overflow can't happen.
- Reorder arithmetic in ways that change the wraparound point.
- Apply loop-bound optimizations that depend on non-overflow.
- Replace integer arithmetic with wider intermediates where it "obviously" can't overflow.

## Why this matters for TD5RE

The orig binary's physics math is generated from MSVC VC6 source that **routinely relies on int32 wraparound**. Sample sites where the product **demonstrably overflows int32** in normal operation:

| File:Line | Expression | Upper-bound product | Overflows? |
|---|---|---|---|
| td5_physics.c:2763 | `cos_h * vx` | cos_h ≤ 4096, vx ≤ ~10^6 → ~4×10^9 | YES |
| td5_physics.c:2767 | `SAR12(cos_s * 0x271) * cos_s` | ~10^3 × 0x271 = ~6×10^5, × cos_s ≈ 2×10^9 | borderline |
| td5_physics.c:2776 | `SAR10(front_num) * 800` | front_num large; >> 10 then ×800 | possible |
| td5_physics.c:2778 | `num_a * 0x14C` | body-frame velocity × 332 | possible |
| td5_physics.c:8070 | `abs_shr8 * gear_ratio * 0x2d` | 10^5 × 10^4 × 45 ≈ 4×10^13 | **MASSIVELY** |
| td5_physics.c:8494 | `throttle * torque_mult * 0x1A` | similar pattern | possible |

The orig's IMUL **always** wraps in 32-bit (that's the x86 instruction semantics). The port's `int32_t × int32_t` under GCC `-O2` does not have that guarantee. The two diverge any time GCC decides to "optimize" using the assumption that overflow doesn't happen.

The port comments make the byte-faithful intent explicit, e.g. `td5_physics.c:8069`:
```
/* IMUL EAX,EDX; LEA*5; LEA*9  [@ 0x42EE2E-0x42EE34] — EAX = abs*ratio*45. */
int32_t prod = abs_shr8 * gear_ratio * 0x2d;
```

This is intentionally encoded as the orig's IMUL sequence. The byte-faithfulness assumes wraparound. Without `-fwrapv`, GCC may legally compile this differently from how MSVC compiled it.

## Recommended fix (one line, two files)

**`td5mod/Makefile:70`** — add `-fwrapv`:
```make
TD5RE_CFLAGS   := -c -O2 -fwrapv -Wall -Wextra -Wpedantic -DWIN32 -m32 \
                  -I$(TD5RE_SRC) -I$(WRAPPER_SRC) -I$(ZLIB_INC) \
                  -DTD5_INFLATE_USE_ZLIB
```

**`td5mod/src/td5re/build_standalone.bat:34`** — same `-fwrapv` insertion:
```bat
set CFLAGS=-c -O2 -fwrapv -Wall -Wextra -Wpedantic -DWIN32 -m32 ...
```

That's it. `-fwrapv` makes signed overflow well-defined as 2's complement wraparound — matching IMUL exactly.

### Cost

- Marginal codegen change: a handful of loops that GCC was previously vectorizing using non-overflow assumptions may slow down by ~1-2%. Negligible for a game running at 30 fps with millisecond-budget physics.
- Some `-Wall`/`-Wextra` warnings may shift, but no new compile errors.

### Risk

Convergence-diff numbers could move in either direction:
- If GCC was previously miscompiling wrap-dependent code, `-fwrapv` reduces divergence.
- If GCC happened to leave wrap semantics intact anyway, `-fwrapv` is a no-op.
- Net: no plausible scenario where this makes things worse.

## Verification path

1. Apply the one-line change to both files.
2. `cd td5mod && mingw32-make rebuild` (or use the .bat).
3. Run `run_whole_state_diff.py` against Honolulu_Human + Edinburgh + Moscow.
4. Measure cluster-level changes. If diff numbers drop significantly, this was contributing residue. If unchanged, the orig and GCC happened to agree on the codegen.

Combine with the FPU CW fix ([[reference_fpu_control_word_arch_divergence_2026-05-20]]) for the strongest measurement — they're independent fixes that compose.

## Status of instruction-semantics audit

| Axis | Status |
|---|---|
| FPU control word | Fix ready (`_controlfp_s(_PC_64 \| _RC_DOWN, ...)` in main.c). |
| SAR vs SHR | CLEAN (audit 2026-05-20). |
| **IMUL signed overflow** | **BUG — fix ready (`-fwrapv` in two build files)**. |
| FISTP / `__ftol` | Subsumed by FPU CW fix. |
| IDIV / signed division | Not audited; lower priority. |
| ROL / ROR | Not audited; low priority. |

Two of three high-leverage axes now have ready fixes. Both are one-liners; both can land independently; both should re-measure against the diff harness.

## Companion: deterministic uninitialized reads (PageHeap)

App Verifier (`C:\Windows\System32\appverif.exe`) is installed on this system. To run the uninit-read audit:

1. **As administrator**, run `appverif.exe`.
2. Add `TD5_d3d.exe` to the "Applications" list (File → Add Application, point to `original/TD5_d3d.exe`).
3. In the test list, enable: **Basics → Heaps → Full Page Heap**. Save.
4. Launch TD5_d3d.exe via the existing quickrace harness (`python tools/run_ai_probe.py` will work).
5. Observe: PageHeap fills every fresh heap alloc with `0xC0C0C0C0` and traps on access. Any crash or behavior change vs the unmodified run indicates an uninit-read site. The trap's instruction address points to the exact offending instruction.
6. After done: in appverif, **File → Reset Application Settings** to remove the registry hooks.

`gflags.exe` is the CLI equivalent but isn't installed (it ships with Windows SDK Debugging Tools). Use the GUI.

This step requires user interaction (admin elevation + GUI). Cannot be reliably automated from this shell.

## Next steps (in suggested order)

1. **Land the `-fwrapv` flag** — both files, single line each. Rebuild, run diff harness.
2. **Land the FPU CW init** — `_controlfp_s(_PC_64 | _RC_DOWN, _MCW_PC | _MCW_RC)` in main.c. Rebuild, run diff harness.
3. **Run PageHeap** when ready to chase residual uninit-read divergences.

Steps 1 and 2 are independent; either can land first. The IMUL fix is fractionally lower-risk (purely a compiler hint, no runtime CW change).
