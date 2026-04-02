/**
 * ps_luminance_alpha.hlsl - Luminance-to-alpha transparency shader
 *
 * Color = texture * diffuse vertex color
 * Alpha = luminance of texture (dark = transparent, bright = opaque)
 *
 * Used when ALPHABLENDENABLE=ON for textures WITHOUT an alpha channel
 * (R5G6B5 format). Trees, fences, street lights, and other foliage use
 * black backgrounds that should be transparent. The luminance-based alpha
 * gives smooth semi-transparency instead of harsh color-key cutouts.
 *
 * Luminance weights: ITU-R BT.601 (0.299R + 0.587G + 0.114B)
 */

#include "ps_common.hlsli"

float4 main(PS_INPUT input) : SV_TARGET
{
    float4 tex = texMap.Sample(samplerState, input.uv);
    float4 color;
    color.rgb = tex.rgb * input.diffuse.rgb;
    color.a   = dot(tex.rgb, float3(0.299, 0.587, 0.114));
    return ApplyFogAndAlphaTest(color, input.depth);
}
