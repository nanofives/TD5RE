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
    float4 color;
    color.rgb = tex.rgb * input.diffuse.rgb;
    color.a   = tex.a * input.diffuse.a;
    color = ApplyFogAndAlphaTest(color, input.depth);
    float sv = saturate(g_sunvis.Load(int3(int2(input.pos.xy), 0)));
    color.rgb *= sv;
    return color;
}
