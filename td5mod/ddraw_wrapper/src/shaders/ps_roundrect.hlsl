/**
 * ps_roundrect.hlsl - Procedural rounded-rectangle (frontend buttons/frames).
 *
 * Analytic rounded-rect button replacing the bitmap ButtonBits 9-slice, crisp at
 * any resolution. VectorUI only; the bitmap path remains the fallback.
 *
 * Matches the original ButtonBits design:
 *  - Per-corner radii, diagonally symmetric: top-left & bottom-right SMOOTH
 *    (large radius), top-right & bottom-left ABRUPT (small radius).
 *  - Anisotropic border: the left/right sides are thicker than the top/bottom
 *    (rr_border.x vs rr_border.y). Modelled as the band between an outer and an
 *    inset inner rounded-rect.
 *  - Beveled rim: brighter in the MIDDLE of the border band, darker at its outer
 *    and inner edges (the metallic tube look), not a flat colour.
 *  - Interior alpha separate from the border: rr_fill.a = 0 => transparent
 *    interior (unselected/locked, border only); a = 1 => opaque fill (selected).
 *
 * b0 stays FogParams (ps_common); params come from the b1 constant buffer.
 */

#include "ps_common.hlsli"

cbuffer RoundRectParams : register(b1)
{
    float2 rr_sizePx;    /* button width,height in pixels */
    float2 rr_border;    /* border thickness px: x = left/right, y = top/bottom */
    float4 rr_radii;     /* outer corner radii px: (TL, TR, BL, BR) */
    float4 rr_mid;       /* border gradient: lightest, at the middle of the band */
    float4 rr_inner;     /* border gradient: inner-edge colour */
    float4 rr_outer;     /* border gradient: outer-edge colour (darkest, left) */
    float4 rr_fill;      /* interior rgb, a = interior alpha (0 = transparent) */
};

/* Signed distance to a rounded rectangle with per-corner radii. p centred;
 * in DX UV space p.y<0 is the top, p.y>=0 the bottom. */
float sd_round_rect4(float2 p, float2 b, float4 rad)
{
    float r;
    if (p.x >= 0.0) r = (p.y >= 0.0) ? rad.w : rad.y;   /* right: BR : TR */
    else            r = (p.y >= 0.0) ? rad.z : rad.x;   /* left : BL : TL */
    float2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

float4 main(PS_INPUT input) : SV_TARGET
{
    float2 p = (input.uv - 0.5) * rr_sizePx;
    float2 b = rr_sizePx * 0.5;

    float  d_out = sd_round_rect4(p, b, rr_radii);              /* outer shape */
    float2 b_in  = b - rr_border;                              /* inset inner edge */
    float4 rad_in = max(rr_radii - rr_border.y, 1.0);
    float  d_in  = sd_round_rect4(p, b_in, rad_in);            /* inner edge of border */

    float aa = max(fwidth(d_out), 1e-3);
    float coverage   = saturate(0.5 - d_out / aa);   /* crisp outer edge */
    float borderMask = saturate(0.5 + d_in / aa);    /* 1 in border band, 0 in interior */

    /* Position across the border band: 0 at outer edge, 1 at inner edge. */
    float t = saturate((-d_out) / max((-d_out) + d_in, 1e-3));
    /* 3-stop metallic bevel: outer edge -> bright middle -> inner edge. */
    float3 rimCol = (t < 0.5) ? lerp(rr_outer.rgb, rr_mid.rgb, saturate(t / 0.5))
                              : lerp(rr_mid.rgb, rr_inner.rgb, saturate((t - 0.5) / 0.5));
    /* 3D gloss: lift the dark (outer/inner) edges toward the RIGHT so the
     * outer-LEFT edge stays the darkest (the supplied #3C2F00 / #001675); the
     * bright middle of the band is left unchanged. */
    float edgeDark = abs(t - 0.5) * 2.0;                 /* 0 mid, 1 at band edges */
    float dir = smoothstep(0.0, 0.6, input.uv.x) * edgeDark;
    rimCol *= (1.0 + 0.18 * dir);

    float3 col = lerp(rr_fill.rgb, rimCol, borderMask);
    float  a   = coverage * lerp(rr_fill.a, 1.0, borderMask);

    if (a <= 0.0)
        discard;

    return float4(col, a);
}
