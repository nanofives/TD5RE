/**
 * ps_modulate_shadowed.hlsl - [RT2-P3] PS_MODULATE that RECEIVES RT sun shadows.
 *
 * Identical to ps_modulate (tex * diffuse, alpha = tex.a, fog + alpha test) then
 * multiplies the RGB by the full-frame sun-visibility mask (g_sunvis, t1) at the
 * pixel's screen position -- exactly the factor the opaque shadow composite
 * (ps_shadow_rt) applies to the opaque scene. So world alpha-blend translucents
 * (billboard trees/signs) darken where a building/bridge shadow falls on the
 * ground behind them. HIGH + SRCALPHA_INVSRC + depth-tested draws only (selected
 * in Backend_PlatDrawTris); additive light sources never use this variant.
 */
#include "ps_common.hlsli"

Texture2D<float> g_sunvis : register(t1);   /* rgen_shadow mask: 1=lit, 1-strength=shadow */

float4 main(PS_INPUT input) : SV_TARGET
{
    float4 tex = SampleTex(texMap, samplerState, input.uv);
    float4 color;
    color.rgb = tex.rgb * input.diffuse.rgb;
    color.a   = tex.a;
    color = ApplyFogAndAlphaTest(color, input.depth);
    float sv = saturate(g_sunvis.Load(int3(int2(input.pos.xy), 0)));
    color.rgb *= sv;                          /* receive sun shadow (alpha unchanged) */
    return color;
}
