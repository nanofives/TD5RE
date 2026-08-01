/**
 * ps_shadow_rt.hlsl - RT sun-shadow composite (lighting rework, RT path).
 * MULTIPLICATIVE fullscreen pass: reads the per-pixel shade written by rgen_shadow
 * (g_sunvis, full-frame) at this pixel and darkens the scene by it. Same output
 * contract as ps_shadow.hlsl (out = shade broadcast to rgb) so the composite
 * stays a MULT blend. Uses .Load(SV_Position) -- the mask is full-frame and the
 * pane viewport clips the draw.
 */
Texture2D<float> sunvis : register(t0);

struct PS_INPUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(PS_INPUT input) : SV_TARGET
{
    float s = sunvis.Load(int3((int)input.pos.x, (int)input.pos.y, 0));
    return float4(s, s, s, 1.0f);
}
