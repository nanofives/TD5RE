# FPU Control Word audit — TD5_d3d.exe

**Date:** 2026-05-20
**Source:** Ghidra static analysis (TD5_pool0) + frida_fpu_state_probe (runtime confirmation pending)
**Pairs with:** [[reference_lateral_bias_steering_2x_root_cause]] (FPU residue), Agent D's "80→32 precision collapse" finding (todo_chassis_snap_fix_2026-05-16)

## TL;DR

The original game programs the x87 FPU control word **twice**:

1. At race init, calls `_controlfp(0x100, 0x300)` → MSVC `_PC_53` (53-bit double precision).
2. Builds two CW variants stored in globals, swapped per call-site:
   - `0x0048DB90` = current CW with **PC=00 (24-bit single precision)** — used inside render hot paths.
   - `0x0048DB94` = current CW with **PC=11 (64-bit extended)** — game default, loaded at init exit.

The port runs with MinGW's default CW (PC=11 = 64-bit extended throughout) and never narrows precision for render-side math. This is the structural cause of the "80→32 precision collapse" residue that has been flagged as irreducible across multiple convergence-sprint sessions.

## Evidence

### Initializer: `InitializeRaceRenderGlobals @ 0x0040AE10`

```asm
0040ae1c PUSH 0x300                  ; PC mask
0040ae21 PUSH 0x100                  ; PC value = _PC_53
0040ae26 CALL _controlfp             ; sets FPU precision to 53-bit double
0040ae2b FSTCW [0x0048DB90]          ; save current CW
0040ae32 MOV  EAX, [0x0048DB90]
0040ae37 AND  EAX, 0xFFFFFCFF        ; clear bits 8-9 (PC field) -> PC=00 (24-bit)
0040ae3c MOV  [0x0048DB90], EAX      ; store low-precision variant
0040ae41 OR   EAX, 0x300              ; set bits 8-9 (PC field) -> PC=11 (64-bit ext)
0040ae46 MOV  [0x0048DB94], EAX      ; store high-precision variant
0040ae4b FLDCW [0x0048DB94]          ; load high-precision as game default
```

### Per-call-site usage (13 FLDCW sites, 0 FNSTCW sites)

| Address | Function | Variant loaded | Direction |
|---|---|---|---|
| 0x00431817 | `ClipAndSubmitProjectedPolygon` | 0x0048DB90 (24-bit) | enter low-PC |
| 0x004323BA | `ClipAndSubmitProjectedPolygon` | 0x0048DB94 (64-bit) | exit, restore |
| 0x0043DCB8 | sub_0043DCA0 (render-family) | 0x0048DB90 (24-bit) | enter low-PC |
| 0x0043DD3D | sub_0043DCA0 | 0x0048DB94 (64-bit) | exit, restore |
| 0x0043DD72 | sub_0043DCA0 | 0x0048DB90 (24-bit) | enter low-PC |
| 0x0043DDDC | sub_0043DCA0 | 0x0048DB94 (64-bit) | exit, restore |
| 0x0043DDFA | sub_0043DCA0 | 0x0048DB90 (24-bit) | enter low-PC |
| 0x0043DEA7 | sub_0043DCA0 | 0x0048DB94 (64-bit) | exit, restore |

The pattern is unambiguous: low-PC at function entry → projection math → restore at exit. Four nested enter/exit pairs inside `sub_0043DCA0` suggest a per-vertex or per-poly inner loop.

Stack-local FLDCW sites (`__ftol @ 0x0044817C` and friends at 0x00448192/98, 0x0044830A, 0x0044E54F) are the MSVC `_ftol` / `_ftol2` float-to-int helpers — independent concern, already byte-faithful in the port via cast semantics.

## Hex CW values — EMPIRICALLY CAPTURED (2026-05-20)

`tools/frida_fpu_state_probe.js` ran against TD5_d3d.exe (Honolulu/Viper quickrace) and captured:

| Sample point | CW | PC | RC | Note |
|---|---|---|---|---|
| At Frida attach (process spawn) | `0x027F` | 53-bit (`10`) | RNE (`00`) | Windows process default |
| `UpdateRaceActors` per sub-tick | `0x077F` | **64-bit (`11`)** | **RDN (`01`)** | Steady-state physics |
| `Cascade_4340C0` (cascade math) | `0x077F` | 64-bit | RDN | Steady-state |
| `RoundedTransform @ 0x0042EB10` | `0x077F` | 64-bit | RDN | Steady-state render-math helper |
| `CosFloat12bit @ 0x0040A6A0` | `0x077F` | 64-bit | RDN | Steady-state trig |

**Two corrections to the static-only prediction:**

1. **PC = 64-bit extended, NOT 24-bit single.** The `0x0048DB90` (PC=00) variant **is** built by `InitializeRaceRenderGlobals` but only loaded inside `ClipAndSubmitProjectedPolygon` and the `sub_0043DCA0` family — none of which the probe hooks. Physics/AI/trig hot paths all run at the 64-bit default (`0x0048DB94`). The 24-bit clip-path is real but localized.

2. **RC = RDN (round toward -∞ / floor), NOT RNE (round-to-nearest-even).** This is the headline finding. Bit 10 of the CW is set across every steady-state sample. The orig runs all FPU rounding (including the `FISTP` inside `__ftol`, every `FIST`, every internal x87 truncation) in **round-down** mode for the entirety of race execution.

The RC bit must be set somewhere *before* `InitializeRaceRenderGlobals` (whose decompile only touches PC). Static analysis didn't catch it because the writer is either a direct `MOV WORD PTR [0x48DBxx], imm16` masked under "data write" or set via `_controlfp(_RC_DOWN, _MCW_RC)` in process init. Doesn't matter for the port — the empirical value is what we match.

## Port-side fix (revised after empirical capture)

The single highest-leverage change: match the orig's **rounding mode** at process init. PC width matters less (64-bit ext matches the MinGW default already; the localized 24-bit clip path is a smaller secondary concern).

```c
#include <float.h>  /* _controlfp / _controlfp_s */

void td5_fpu_init(void) {
    /* Match the orig's CW = 0x077F:
     *   PC = 64-bit extended  (MSVC _PC_64,  MinGW _MCW_PC bits)
     *   RC = round toward -inf (MSVC _RC_DOWN, MinGW _MCW_RC bits)
     *   All 6 exception masks set (default).
     * The orig's CW also has bit 6 set, which is reserved/legacy on
     * modern CPUs and silently ignored by FLDCW. We don't bother with it.
     */
    unsigned int cur = 0;
    _controlfp_s(&cur, _PC_64 | _RC_DOWN, _MCW_PC | _MCW_RC);
}
```

Call once from `main()` before any FPU math (i.e., before any C runtime init that uses floats — practically, the first thing in `WinMain` after argv parsing).

### Verification

After landing the call, re-run `tools/run_fpu_state_probe.py` against **the port** (point HOOK_JS at a no-op script or remove the quickrace hook; only the FPU probe matters) and confirm port CW also reads `0x077F` at sampled call-sites.

### What this fixes (concretely)

- Every implicit `(int)floatval` cast in port code currently uses RNE (compiler builtin `cvttss2si` truncates toward zero, but x87 FISTP under default RC rounds-to-nearest). After this change, FISTP rounds toward -∞ — matching every `_ftol` call in the orig.
- All x87 intermediate rounding inside `sinf`/`cosf`/etc. matches orig.
- The "FPU residue irreducible" cluster (Agent D, Round 2) should compress significantly — re-run the labeled diff to measure.

### Caveat about `floorf` and `lrintf`

This finding **invalidates** the candidate fix in [[todo-chassis-snap-fix-2026-05-16]]: "try `floorf` → `lrintf` at td5_physics.c:6304". The orig runs RC=RDN, so:
- `floorf(x)` = round toward -∞ → **matches orig FISTP under RC=RDN**.
- `lrintf(x)` uses the *current* C rounding mode (typically RNE) → **diverges from orig**.

Keep `floorf`. If anything, check whether other port sites use `(int)x` truncation-toward-zero where the orig used FISTP — those need replacement with `floorf` or `(int)floorf(x)` for byte-faithfulness.

### Secondary: the 24-bit clip-path

Once the global RC=RDN fix lands, the localized PC=24-bit swap inside `ClipAndSubmitProjectedPolygon` is still real but lower priority. If render-side divergence persists after the global RC fix, wrap the port's clip-and-project equivalent with explicit `FLDCW`:

```c
static unsigned short s_cw_low_pc, s_cw_high_pc;
/* populate in td5_fpu_init: */
/*   __asm__ ("fnstcw %0" : "=m"(s_cw_high_pc)); */
/*   s_cw_low_pc = (s_cw_high_pc & 0xFCFF); */  /* clear PC bits -> 24-bit */
static inline void td5_fpu_enter_clip(void) { __asm__ ("fldcw %0" :: "m"(s_cw_low_pc)); }
static inline void td5_fpu_exit_clip(void)  { __asm__ ("fldcw %0" :: "m"(s_cw_high_pc)); }
```

Only wrap the port's `ClipAndSubmitProjectedPolygon` and `sub_0043DCA0` family equivalents — not all of render.

## Next steps

1. ~~Run `tools/run_fpu_state_probe.py`~~ DONE 2026-05-20. Output in `log/orig/fpu_state.csv`. Steady-state CW = `0x077F` (PC=64-bit, RC=RDN). Attach-time = `0x027F` (process default).
2. Land `td5_fpu_init(_PC_64 | _RC_DOWN, _MCW_PC | _MCW_RC)` as the first line of port `main()`. **This is the one-line fix.**
3. Add a port-side FPU probe pass: aim the runner at td5re.exe (replace HOOK_JS with a no-op) and confirm port CW reads `0x077F` after the init lands.
4. Re-run `run_whole_state_diff.py` against Honolulu_Human, Edinburgh, Moscow — measure the cluster reduction from the rounding-mode match. Hypothesis: closes the "FPU residue irreducible" bucket entirely, also potentially closes Edinburgh/Moscow chassis-launch sites that were previously flagged as fp8 rounding mismatches.
5. If residual remains: hook `ClipAndSubmitProjectedPolygon @ 0x004317F0` in the probe and confirm 24-bit transition is real; then wrap port equivalent.

## Related

- [[reference_lateral_bias_steering_2x_root_cause]] — convergence/lateral_bias residue, partially FPU-driven.
- [[todo_chassis_snap_fix_2026-05-16]] — Round 2 Agent D's "FPU residue irreducible" was about this exact 80→32 collapse; reframe as fixable once CW matches.
- [[reference_steering_cascade_root_cause_find_offset_peer]] — separate concern (uninitialized memory, not FPU).
