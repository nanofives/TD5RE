/**
 * ps_arrow.hlsl - Procedural selector arrow (frontend ◄ ► on selector rows).
 *
 * Analytic triangle SDF so the arrows stay crisp + anti-aliased at any
 * resolution, replacing the bitmap ArrowButtonz sprite. VectorUI only.
 *
 * Reuses the b1 RoundRectParams constant buffer (shared with ps_roundrect):
 *   rr_sizePx     - quad size in px (for fwidth scale only)
 *   rr_border.x   - direction: < 0.5 = left arrow, >= 0.5 = right arrow
 *   rr_mid.rgb    - arrow fill colour
 */

#include "ps_common.hlsli"

cbuffer RoundRectParams : register(b1)
{
    float2 rr_sizePx;
    float2 rr_border;
    float4 rr_radii;
    float4 rr_mid;
    float4 rr_inner;
    float4 rr_outer;
    float4 rr_fill;
};

/* Signed distance to a triangle (Inigo Quilez). Negative inside. */
float sd_triangle(float2 p, float2 p0, float2 p1, float2 p2)
{
    float2 e0 = p1 - p0, e1 = p2 - p1, e2 = p0 - p2;
    float2 v0 = p - p0,  v1 = p - p1,  v2 = p - p2;
    float2 pq0 = v0 - e0 * clamp(dot(v0, e0) / dot(e0, e0), 0.0, 1.0);
    float2 pq1 = v1 - e1 * clamp(dot(v1, e1) / dot(e1, e1), 0.0, 1.0);
    float2 pq2 = v2 - e2 * clamp(dot(v2, e2) / dot(e2, e2), 0.0, 1.0);
    float s = sign(e0.x * e2.y - e0.y * e2.x);
    float2 d = min(min(float2(dot(pq0, pq0), s * (v0.x * e0.y - v0.y * e0.x)),
                       float2(dot(pq1, pq1), s * (v1.x * e1.y - v1.y * e1.x))),
                       float2(dot(pq2, pq2), s * (v2.x * e2.y - v2.y * e2.x)));
    return -sqrt(d.x) * sign(d.y);
}

float4 main(PS_INPUT input) : SV_TARGET
{
    float2 uv = input.uv;
    if (rr_border.x < 0.5) uv.x = 1.0 - uv.x;   /* mirror for the left arrow */

    /* Right-pointing triangle in the unit quad. */
    float d = sd_triangle(uv, float2(0.82, 0.5),
                              float2(0.20, 0.16),
                              float2(0.20, 0.84));
    float aa = max(fwidth(d), 1e-4);
    float a = saturate(0.5 - d / aa);
    if (a <= 0.0)
        discard;
    return float4(rr_mid.rgb, a);
}
