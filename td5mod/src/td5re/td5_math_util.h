/*
 * td5_math_util.h -- shared scalar clamp/abs/ARGB-pack helpers.
 *
 * [2026-07-09, A10 refactor] Consolidates a handful of small helpers that
 * had been independently reimplemented under different names in different
 * modules: clampi (td5_render_internal.h) vs clamp_i (td5_input.c), and
 * smart_iabs (td5_ai.c) vs the many inline `(x < 0) ? -x : x` sites. Also
 * introduces td5_argb8, replacing the many duplicate inline
 * `(a<<24)|(r<<16)|(g<<8)|b` color-pack expressions across the render/HUD/
 * frontend modules.
 */

#ifndef TD5_MATH_UTIL_H
#define TD5_MATH_UTIL_H

#include <stdint.h>

static inline int clampi(int x, int lo, int hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static inline int td5_iabs(int x)
{
    return x < 0 ? -x : x;
}

/* Pack 8-bit A/R/G/B components into a 0xAARRGGBB uint32 (TD5's in-memory
 * vertex-diffuse / fill-color format). */
static inline uint32_t td5_argb8(uint32_t a, uint32_t r, uint32_t g, uint32_t b)
{
    return (a << 24) | (r << 16) | (g << 8) | b;
}

#endif /* TD5_MATH_UTIL_H */
