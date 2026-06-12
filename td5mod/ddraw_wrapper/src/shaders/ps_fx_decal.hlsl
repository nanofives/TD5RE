/**
 * ps_fx_decal.hlsl - Procedural, texture-free tire / skid mark road decal.
 *
 * Replaces the FADEWHT solid-white texel the tire-track strip used to sample.
 * The strip is a road-surface mesh built by td5_vfx_render_tire_tracks; uv.x
 * runs ACROSS the strip width (0..1), uv.y ALONG its length. Instead of a flat
 * black rectangle this shader feathers the strip edges and adds a faint
 * length-wise grain so the rubber reads as an "actual" scuffed mark.
 *
 *   diffuse (COLOR0) = mark colour .rgb (near-black) + intensity alpha .a
 *
 * Blend: SRC_ALPHA / INV_SRC_ALPHA (TD5_PRESET_SHADOW) — a ground decal.
 */

#include "ps_common.hlsli"

cbuffer FxParams : register(b1)
{
    float g_time;
    float g_p0;
    float g_p1;
    float g_p2;
};

float hash21(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float vnoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + float2(1.0, 0.0));
    float c = hash21(i + float2(0.0, 1.0));
    float d = hash21(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float4 main(PS_INPUT input) : SV_TARGET
{
    /* Soft feathered edges across the strip width (centre solid, edges fade). */
    float across  = 1.0 - abs(input.uv.x * 2.0 - 1.0);
    float feather = smoothstep(0.0, 0.40, across);

    /* Length-wise grain: two scales of noise so the rubber looks scuffed, not
     * a flat fill. Stays mostly opaque so the mark is still clearly visible. */
    float grain = 0.78
                + 0.14 * vnoise(float2(input.uv.y * 36.0, input.uv.x * 3.0))
                + 0.08 * vnoise(float2(input.uv.y * 140.0, 7.0));

    float a = feather * grain * input.diffuse.a;
    if (a < 0.02) discard;

    return float4(input.diffuse.rgb, a);
}
