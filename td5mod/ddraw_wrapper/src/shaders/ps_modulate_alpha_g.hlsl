/**
 * ps_modulate_alpha_g.hlsl - MODULATEALPHA + G-buffer write (lighting rework P0)
 *
 * Same color math as ps_modulate_alpha.hlsl, plus SV_Target1 = the raw COLOR1
 * (specular) dword (world normal + material id). See ps_modulate_g.hlsl.
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
    color.a   = tex.a   * input.diffuse.a;

    PS_OUTPUT o;
    o.color = ApplyFogAndAlphaTest(color, input.depth);
    o.gbuf  = input.specular;
    return o;
}
