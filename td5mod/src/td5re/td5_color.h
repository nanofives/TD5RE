/*
 * td5_color.h -- ARGB8888 -> normalized-float colour helpers.
 *
 * Single source for the unpack idiom shared by the frontend vector UI,
 * the HUD and the platform layer (BGRA-ordered D3D11 constant buffers all
 * consume the same channel order these produce).
 */

#ifndef TD5_COLOR_H
#define TD5_COLOR_H

#include <stdint.h>

/** Unpack 0xAARRGGBB into dst[0..2] = R,G,B in [0,1]. Alpha untouched. */
static inline void td5_argb_to_rgb_f(float *dst, uint32_t argb)
{
    dst[0] = (float)((argb >> 16) & 0xFFu) / 255.0f;
    dst[1] = (float)((argb >>  8) & 0xFFu) / 255.0f;
    dst[2] = (float)( argb        & 0xFFu) / 255.0f;
}

/** Unpack 0xAARRGGBB into dst[0..3] = R,G,B,A in [0,1]. */
static inline void td5_argb_to_rgba_f(float *dst, uint32_t argb)
{
    td5_argb_to_rgb_f(dst, argb);
    dst[3] = (float)((argb >> 24) & 0xFFu) / 255.0f;
}

#endif /* TD5_COLOR_H */
