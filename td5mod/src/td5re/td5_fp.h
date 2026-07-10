/*
 * td5_fp.h -- 24.8 fixed-point helper macros for RE-ported sim code.
 *
 * [2026-07-09, S1 refactor] REFACTOR_PLAN.md north star: raw `>>8`/`<<8`
 * shifts are no longer the blessed style for RE-ported fixed-point code;
 * migrate to these named macros instead (readability now outranks literal
 * Ghidra-shift fidelity). Each macro is a pure syntactic wrapper around the
 * exact same shift the ported code always did -- same operator, same
 * operand, same integer type/width/signedness -- so every call site is
 * bit-identical to its pre-migration form. Verified per-module via the
 * golden traces (trace_goldens.txt): any accidental semantic drift shows
 * up immediately as a hash mismatch.
 *
 * FP_TO_FLOAT(x)/FP_* for NEW port-only float math (see td5_vfx.c's
 * FP_TO_FLOAT local, td5_render.c) are a different, pre-existing thing --
 * these macros are specifically for the RE-ported integer truncate/scale
 * idiom.
 */

#ifndef TD5_FP_H
#define TD5_FP_H

#define FP_SHIFT        8            /* 24.8 fixed-point fractional bits */
#define FP_ANGLE_MASK   0xFFF        /* 12-bit angle, full circle = 0x1000 units */

/* Truncate a 24.8 fixed-point value to its integer part (was `(x) >> 8`). */
#define FP_TRUNC(x)     ((x) >> FP_SHIFT)

/* Scale an integer up into 24.8 fixed-point (was `(x) << 8`). */
#define FP_SCALE(x)     ((x) << FP_SHIFT)

/* Truncate then wrap into the 12-bit angle range (was `((x) >> 8) & 0xFFF`). */
#define FP_ANGLE(x)     (FP_TRUNC(x) & FP_ANGLE_MASK)

#endif /* TD5_FP_H */
