/**
 * ps_modulate.hlsl - TEXTUREMAPBLEND = MODULATE (4)
 *
 * Color = texture * diffuse vertex color
 * Alpha = texture alpha (NOT diffuse alpha)
 *
 * This is the most common blend mode in TD5. Alpha comes from the texture
 * so that A1R5G5B5 color-key pixels (alpha=0) are discarded by alpha test,
 * while diffuse vertex color (always alpha=0xFF) doesn't override texture alpha.
 */

#include "ps_common.hlsli"

float4 main(PS_INPUT input) : SV_TARGET
{
    float4 tex = texMap.Sample(samplerState, input.uv);
    float4 color;
    color.rgb = tex.rgb * input.diffuse.rgb;
    color.a   = tex.a;

    return ApplyFogAndAlphaTest(color, input.depth);
}
