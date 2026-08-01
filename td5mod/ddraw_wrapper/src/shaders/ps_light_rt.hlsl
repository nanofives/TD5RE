/**
 * ps_light_rt.hlsl - RT dynamic-light composite (lighting rework, RT path).
 * ADDITIVE (ONE/ONE) fullscreen pass: adds the per-pixel accumulated light color
 * written by rgen_light (g_lightcol, full-frame) to the scene. Same additive
 * contract as ps_light.hlsl.
 */
Texture2D<float4> lightcol : register(t0);

struct PS_INPUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(PS_INPUT input) : SV_TARGET
{
    float3 c = lightcol.Load(int3((int)input.pos.x, (int)input.pos.y, 0)).rgb;
    return float4(c, 1.0f);
}
