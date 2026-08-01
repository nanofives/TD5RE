/**
 * ps_ssr_rt.hlsl - RT reflection composite (lighting rework, RT path).
 * Alpha-blends the reflection color + weight written by rgen_refl (g_reflcol,
 * full-frame) onto the scene. Same output contract as ps_ssr.hlsl:
 * float4(reflColor, weight) with BLEND_SRCALPHA_INVSRC.
 */
Texture2D<float4> reflcol : register(t0);

struct PS_INPUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(PS_INPUT input) : SV_TARGET
{
    float4 r = reflcol.Load(int3((int)input.pos.x, (int)input.pos.y, 0));
    return float4(r.rgb, r.a);   /* out = refl.rgb*a + dst*(1-a) */
}
