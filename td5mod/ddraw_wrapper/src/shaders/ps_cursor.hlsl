/**
 * ps_cursor.hlsl - Procedural mouse-cursor pointer from an SDF silhouette.
 *
 * The SnkMouse pointer is reconstructed from its SDF silhouette as three layers
 * (outer -> inner): a white edge, a slim black inner layer, then the #8700FF
 * purple fill -- crisp + AA at any resolution. A subtle directional shade gives
 * a 3D feel: a white light toward the top, a shadow toward the right corner.
 * VectorUI only; the bitmap cursor remains the fallback.
 */

#include "ps_common.hlsli"

static const float PXRANGE  = 12.0;  /* matches build_msdf_font.py --range 6 */
static const float W_WHITE  = 1.5;   /* white outer edge width, px            */
static const float W_BLACK  = 2.3;   /* white+black width, px (black = diff)  */

float cur_median(float a, float b, float c)
{
    return max(min(a, b), min(max(a, b), c));
}

float4 main(PS_INPUT input) : SV_TARGET
{
    float3 s  = texMap.Sample(samplerState, input.uv).rgb;
    float  sd = cur_median(s.r, s.g, s.b);

    float2 atlasSize;
    texMap.GetDimensions(atlasSize.x, atlasSize.y);
    float2 unitRange     = PXRANGE / atlasSize;
    float2 screenTexSize = 1.0 / max(fwidth(input.uv), 1e-8);
    float  spr = max(0.5 * dot(unitRange, screenTexSize), 1.0);

    float dpx = spr * (sd - 0.5);            /* signed screen-px distance from edge */
    float cov = saturate(dpx + 0.5);
    if (cov <= 0.0)
        discard;

    /* Purple fill #8700FF with a subtle 3D shade: lighter toward the top,
     * darker toward the right corner. */
    float3 purple = float3(0.529, 0.0, 1.0);
    float3 fill   = purple + (float3(1,1,1) - purple) * (saturate(0.55 - input.uv.y) * 0.18);
    fill *= (1.0 - saturate(input.uv.x - 0.5) * 0.22);

    /* Three layers: white edge -> slim black -> fill. */
    float toBlack = saturate(dpx - W_WHITE + 0.5);
    float toFill  = saturate(dpx - W_BLACK + 0.5);
    float3 col = lerp(float3(1,1,1), float3(0,0,0), toBlack);
    col = lerp(col, fill, toFill);

    return float4(col, cov);
}
