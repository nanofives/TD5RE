/**
 * ps_modulate_alpha_shadowed.hlsl - [RT2-P3] PS_MODULATE_ALPHA + RT sun shadows.
 * Like ps_modulate_alpha (alpha = tex.a * diffuse.a) then multiplies RGB by the
 * sun-visibility mask. See ps_modulate_shadowed.hlsl.
 */
#include "ps_common.hlsli"

Texture2D<float> g_sunvis : register(t1);

float4 main(PS_INPUT input) : SV_TARGET
{
    float4 tex = SampleTex(texMap, samplerState, input.uv);
    /* [CAR GHOST FADE FIX 2026-08-05] Car skin ALPHA holds a per-texel matid
     * (~0.004), not opacity — for a CAR body draw (vertex matid 5) take the
     * fade opacity from the vertex diffuse alpha alone, else the matid tex.a
     * collapses alpha to ~0 and the alpha test discards the whole car. Matches
     * ps_modulate_alpha / ps_modulate_g. Non-car draws unchanged. */
    bool  isCar = ((int)(input.specular.a * 255.0 + 0.5) == 5);
    float4 color;
    color.rgb = tex.rgb * input.diffuse.rgb;
    color.a   = isCar ? input.diffuse.a : (tex.a * input.diffuse.a);
    color = ApplyFogAndAlphaTest(color, input.depth);
    float sv = saturate(g_sunvis.Load(int3(int2(input.pos.xy), 0)));
    color.rgb *= sv;
    return color;
}
