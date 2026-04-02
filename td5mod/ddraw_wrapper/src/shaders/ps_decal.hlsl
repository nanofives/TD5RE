/**
 * ps_decal.hlsl - TEXTUREMAPBLEND = DECAL (1)
 *
 * Color = texture (vertex color ignored)
 * Alpha = texture alpha
 *
 * Pure texture output. Used for UI elements, billboards, and other
 * geometry where vertex color should not tint the texture.
 */

#include "ps_common.hlsli"

float4 main(PS_INPUT input) : SV_TARGET
{
    float4 tex = texMap.Sample(samplerState, input.uv);
    return ApplyFogAndAlphaTest(tex, input.depth);
}
