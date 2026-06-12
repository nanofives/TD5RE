/**
 * ps_fx_glow.hlsl - Procedural, texture-free radial glow.
 *
 * Replaces the BRAKED (taillight) and POLICELT_RED/BLUE (cop strobe) glow
 * sprites. The quad is a unit square (uv 0..1); the shader is a smooth radial
 * gaussian falloff — no texture sampled. Drives two blend regimes from one
 * shader via the b1 premultiply flag:
 *
 *   g_p0 == 0  -> SRC_ALPHA lamp  (return straight rgb, gaussian in alpha).
 *                 Used by vehicle brake lights: a solid-ish red lamp with a
 *                 soft round edge (TD5_PRESET_TRANSLUCENT_POINT_ZTEST).
 *   g_p0 != 0  -> ONE/ONE additive glow (rgb pre-scaled by the gaussian so the
 *                 additive contribution is the falloff, not a square). Used by
 *                 the cop-chase strobe markers (TD5_PRESET_ADDITIVE_GLOW).
 *
 *   diffuse (COLOR0) = glow tint .rgb + master intensity .a
 */

#include "ps_common.hlsli"

cbuffer FxParams : register(b1)
{
    float g_time;
    float g_p0;     /* 0 = SRC_ALPHA lamp, 1 = additive glow */
    float g_p1;     /* gaussian tightness (default 4.0 when 0) */
    float g_p2;
};

float4 main(PS_INPUT input) : SV_TARGET
{
    float2 uv = input.uv * 2.0 - 1.0;          /* centered [-1,1] */
    float  r2 = dot(uv, uv);

    float k = (g_p1 > 0.0) ? g_p1 : 4.0;
    float g = exp(-r2 * k);                     /* 1 centre -> 0 rim */

    float a = g * input.diffuse.a;
    if (a < 0.01) discard;

    if (g_p0 > 0.5)
        return float4(input.diffuse.rgb * a, a);   /* additive: intensity in rgb */

    return float4(input.diffuse.rgb, a);           /* src-alpha lamp */
}
