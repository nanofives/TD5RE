/**
 * ps_modulate_alpha.hlsl - TEXTUREMAPBLEND = MODULATEALPHA (5)
 *
 * Color = texture * diffuse vertex color
 * Alpha = texture alpha * diffuse alpha
 *
 * Both color and alpha are modulated. Used when the game wants vertex
 * alpha to control overall opacity (e.g., fading effects).
 */

#include "ps_common.hlsli"

float4 main(PS_INPUT input) : SV_TARGET
{
    float4 tex = texMap.Sample(samplerState, input.uv);
    float4 color;
    color.rgb = tex.rgb * input.diffuse.rgb;
    color.a   = tex.a   * input.diffuse.a;
    return ApplyFogAndAlphaTest(color, input.depth);
}
