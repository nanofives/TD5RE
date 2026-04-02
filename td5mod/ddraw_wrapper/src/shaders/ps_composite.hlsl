/**
 * ps_composite.hlsl - Fullscreen quad blit shader
 *
 * Used for two purposes:
 * 1. Present path: blit the render target to the swap chain back buffer
 * 2. BltFast compositing: overlay 2D UI content onto the 3D scene
 *
 * Simple texture passthrough with no fog or vertex color modulation.
 * The fullscreen quad vertices provide UV coordinates covering [0,1].
 */

Texture2D    texMap       : register(t0);
SamplerState samplerState : register(s0);

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PS_INPUT input) : SV_TARGET
{
    return texMap.Sample(samplerState, input.uv);
}
