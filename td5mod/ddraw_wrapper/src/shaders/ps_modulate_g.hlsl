/**
 * ps_modulate_g.hlsl - MODULATE + G-buffer write (lighting rework P0)
 *
 * Same color math as ps_modulate.hlsl, plus SV_Target1 = the raw COLOR1
 * (specular) dword: .rgb = world normal (biased 0..1), .a = material id/255.
 * Bound instead of ps_modulate for z-writing, non-blended draws while the
 * G-buffer is active, so the deferred light pass can do proper N.L.
 * Alpha-test discard drops BOTH targets — cutout holes never stamp normals.
 */

#include "ps_common.hlsli"

struct PS_OUTPUT
{
    float4 color : SV_Target0;
    float4 gbuf  : SV_Target1;
};

PS_OUTPUT main(PS_INPUT input)
{
    float4 tex = texMap.Sample(samplerState, input.uv);
    float4 color;
    color.rgb = tex.rgb * input.diffuse.rgb;
    color.a   = tex.a;

    PS_OUTPUT o;
    o.color = ApplyFogAndAlphaTest(color, input.depth);
    o.gbuf  = input.specular;
    return o;
}
