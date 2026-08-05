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

    /* [CAR SUN 2026-08-04] Directional sunlit brighten — see ps_modulate_g.hlsl. */
    if (carSun.w > 0.0)
    {
        float3 nrm = normalize(input.specular.rgb * 2.0 - 1.0);
        float  nl  = saturate(dot(nrm, carSun.xyz));
        color.rgb *= (1.0 + carSun.w * nl);
    }

    PS_OUTPUT o;
    o.color = ApplyFogAndAlphaTest(color, input.depth);
    o.gbuf  = input.specular;
    /* [CAR SUN GLINT 2026-08-04] Per-texel matid + 0x40 car flag — see
     * ps_modulate_g.hlsl for the encoding rationale. */
    if (isCar) { int m = (int)(carMatid * 255.0 + 0.5); o.gbuf.a = (float)((m & 0x3F) | 0x40) / 255.0; }
    return o;
}
