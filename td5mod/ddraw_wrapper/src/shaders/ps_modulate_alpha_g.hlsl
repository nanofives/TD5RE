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
    /* [CAR REFL 2026-08-04] Per-texel car matid baked into the skin alpha — see
     * ps_modulate_g. Force color.a = 1 on car draws so the tiny matid value in
     * alpha never alpha-test-discards the (opaque) body. */
    bool  isCar    = ((int)(input.specular.a * 255.0 + 0.5) == 5);
    float carMatid = tex.a;

    float4 color;
    color.rgb = tex.rgb * input.diffuse.rgb;
    color.a   = isCar ? 1.0 : (tex.a * input.diffuse.a);

    PS_OUTPUT o;
    o.color = ApplyFogAndAlphaTest(color, input.depth);
    o.gbuf  = input.specular;
    if (isCar) o.gbuf.a = carMatid;
    return o;
}
