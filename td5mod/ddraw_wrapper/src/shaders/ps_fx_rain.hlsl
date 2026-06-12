/**
 * ps_fx_rain.hlsl - Procedural, texture-free rain streak.
 *
 * Replaces the RAINDROP atlas sprite (3x8 alpha streak). The quad is the thin
 * camera-relative streak built by td5_vfx_render_ambient_streaks; uv.x runs
 * across the 1px width, uv.y along the streak length. The shader shades a soft
 * vertical highlight that fades toward the trailing end — no texture sampled.
 *
 *   diffuse (COLOR0) = streak tint .rgb + master alpha .a
 *
 * Blend: SRC_ALPHA / INV_SRC_ALPHA, screen-space (no depth, no fog) — matches
 * the original HUD-overlay submit path for ambient streaks.
 */

#include "ps_common.hlsli"

float4 main(PS_INPUT input) : SV_TARGET
{
    /* Across-width falloff: bright core column, soft edges. */
    float cx     = abs(input.uv.x * 2.0 - 1.0);     /* 0 centre .. 1 edge */
    float across = 1.0 - smoothstep(0.35, 1.0, cx);

    /* Along-length: brighter at the leading (top) end, fading to the tail. */
    float along  = lerp(0.20, 1.0, saturate(input.uv.y));

    float a = across * along * input.diffuse.a * 0.6;   /* keep streaks subtle */
    if (a < 0.02) discard;

    /* Cool bluish-white rain regardless of the (white) vertex colour. */
    float3 col = input.diffuse.rgb * float3(0.80, 0.88, 1.0);
    return float4(col, a);
}
