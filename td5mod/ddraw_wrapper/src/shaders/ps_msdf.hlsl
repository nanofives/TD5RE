/**
 * ps_msdf.hlsl - Multi-channel Signed Distance Field text/shape rendering.
 *
 * Used by the resolution-independent frontend (VectorUI). The bound texture is
 * an MSDF atlas (RGB = 3 signed-distance channels, 0.5 == glyph contour). The
 * runtime reconstructs a razor-sharp, anti-aliased edge at the BACKBUFFER
 * resolution regardless of the source cell size, so frontend text no longer
 * pixelates (point sampler) or blurs (linear sampler) when upscaled.
 *
 * Color  = per-vertex diffuse RGB (the requested text colour).
 * Alpha  = screen-space coverage derived from the distance field.
 *          We IGNORE diffuse.a so callers that pack colours like 0xCCCCCC
 *          (alpha byte 0) still render -- this matches the bitmap path, which
 *          took alpha from the glyph texture, not the diffuse colour.
 *
 * The fwidth() form needs no pxRange/atlas-size uniform: fwidth(sd) is the
 * per-screen-pixel rate of change of the sampled distance, so smoothstep over
 * +/- that width yields ~1px analytic AA at ANY magnification. Requires a
 * LINEAR sampler (point sampling would step the field).
 */

#include "ps_common.hlsli"

float msdf_median(float a, float b, float c)
{
    return max(min(a, b), min(max(a, b), c));
}

float4 main(PS_INPUT input) : SV_TARGET
{
    float3 msd = texMap.Sample(samplerState, input.uv).rgb;
    float  sd  = msdf_median(msd.r, msd.g, msd.b);

    /* Screen-space anti-aliasing width from the distance-field gradient. */
    float w = max(fwidth(sd), 1e-5);
    float alpha = smoothstep(0.5 - w, 0.5 + w, sd);

    if (alpha <= 0.0)
        discard;

    float4 color;
    color.rgb = input.diffuse.rgb;
    color.a   = alpha;
    return color;
}
