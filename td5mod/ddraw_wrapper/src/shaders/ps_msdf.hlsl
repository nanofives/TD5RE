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
 * Alpha  = screen-space coverage derived from the distance field. We IGNORE
 *          diffuse.a so callers that pack colours like 0xCCCCCC (alpha byte 0)
 *          still render -- matching the bitmap path (alpha came from the glyph
 *          texture, not the diffuse colour).
 *
 * AA WIDTH: derived from fwidth(uv) (smooth) + the field's pxRange, NOT from
 * fwidth(median). fwidth(median(rgb)) spikes along the loci where the median's
 * "middle" channel switches, which widens the smoothstep there and punches
 * diagonal dark cracks through glyph interiors. The fwidth(uv) form is the
 * canonical Chlumsky shader and is crack-free.
 *
 * PXRANGE = 2 * (build_msdf_font.py --range, default 6) = 12. It is independent
 * of the atlas/cell size (val = 0.5 + dist_atlas_texels / (2*range)), so it
 * stays correct for every MSDF atlas as long as --range is unchanged. The atlas
 * size is read at runtime via GetDimensions, so this shader needs no per-atlas
 * constant buffer. Requires a LINEAR sampler.
 */

#include "ps_common.hlsli"

static const float PXRANGE = 12.0;

float msdf_median(float a, float b, float c)
{
    return max(min(a, b), min(max(a, b), c));
}

float4 main(PS_INPUT input) : SV_TARGET
{
    float3 msd = texMap.Sample(samplerState, input.uv).rgb;
    float  sd  = msdf_median(msd.r, msd.g, msd.b);

    /* Screen-space coverage: how many screen pixels does the field's pxRange
     * cover here? Derived from the UV derivatives (smooth) so the median's
     * kinks never affect the AA width. */
    float2 atlasSize;
    texMap.GetDimensions(atlasSize.x, atlasSize.y);
    float2 unitRange     = PXRANGE / atlasSize;
    float2 screenTexSize = 1.0 / max(fwidth(input.uv), 1e-8);
    float  screenPxRange = max(0.5 * dot(unitRange, screenTexSize), 1.0);

    float screenPxDist = screenPxRange * (sd - 0.5);
    float alpha = saturate(screenPxDist + 0.5);

    if (alpha <= 0.0)
        discard;

    float4 color;
    color.rgb = input.diffuse.rgb;
    color.a   = alpha;
    return color;
}
