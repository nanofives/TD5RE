/**
 * cs_shadow_atrous.hlsl - RT sun-shadow DENOISE (edge-aware à-trous, compute).
 *
 * One iteration of an edge-aware bilateral (à-trous) filter over the sun-shadow
 * visibility mask. The driver runs it N times, ping-ponging src<->dst with a
 * doubling step (1,2,4,...), so a few 5-tap iterations reach a wide footprint and
 * kill the cone-jitter grain (heaviest on OVERCAST, whose wide cone leaves
 * blotches on cars / guardrails) — while depth + G-buffer normal edge-stopping
 * keeps shadow silhouettes crisp. Spatial only (no temporal / motion vectors).
 *
 *   t0  src   (R32F)   input sun visibility (this iteration's source)
 *   t1  depth (R32F)   scene device depth
 *   t2  gbuf  (RGBA8)  .rgb = encoded normal, .a = coverage
 *   u0  dst   (R32F)   filtered output
 *   b0  DenoiseCB      pane rect + {step, radius, depthSigma, normalPow}
 */
Texture2D<float>    src   : register(t0);
Texture2D<float>    gdepth: register(t1);
Texture2D<float4>   ggbuf : register(t2);
RWTexture2D<float>  dst   : register(u0);

cbuffer DenoiseCB : register(b0)
{
    float4 dn_rect;    /* x=paneX y=paneY z=paneW w=paneH (full-frame pixels) */
    float4 dn_params;  /* x=step y=radius z=depthSigma w=normalPow            */
};

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int minX = (int)dn_rect.x,              minY = (int)dn_rect.y;
    int W    = (int)dn_rect.z,              H    = (int)dn_rect.w;
    if ((int)tid.x >= W || (int)tid.y >= H) return;          /* outside the pane */
    int2 px = int2(minX + (int)tid.x, minY + (int)tid.y);
    int  maxX = minX + W - 1,               maxY = minY + H - 1;

    float  Dc = gdepth.Load(int3(px, 0));
    float4 gc = ggbuf.Load(int3(px, 0));
    float  sc = src.Load(int3(px, 0));

    /* Sky / no-G-buffer centre: pass through (keeps silhouettes hard). */
    if (Dc >= 0.99999f || gc.a < 0.001f) { dst[px] = sc; return; }
    float3 nc = gc.rgb * 2.0f - 1.0f;
    float  nlen = length(nc);
    if (nlen < 0.001f) { dst[px] = sc; return; }             /* bad normal -> no smear */
    nc /= nlen;

    int   step = max(1, (int)(dn_params.x + 0.5f));
    int   R    = max(1, (int)(dn_params.y + 0.5f));
    float depthSigma = max(1e-5f, dn_params.z);
    float normalPow  = max(1.0f,  dn_params.w);
    float sigmaSp2   = max(1.0f, (float)(R * R));

    /* Local depth gradient (1px) for a PLANAR prediction: a flat wall seen at a
     * grazing angle has a steep per-pixel depth slope, so a raw depth-difference
     * test would reject its own neighbours in a structured way -> a stationary
     * banding pattern on the wall. Predicting each tap's depth from the gradient
     * makes a planar surface pass fully; only off-plane discontinuities differ. */
    float Dgx = gdepth.Load(int3(int2(min(px.x + 1, maxX), px.y), 0)) - Dc;
    float Dgy = gdepth.Load(int3(int2(px.x, min(px.y + 1, maxY)), 0)) - Dc;

    float sum = 0.0f, wsum = 0.0f;
    [loop] for (int dy = -R; dy <= R; dy++)
    {
        [loop] for (int dx = -R; dx <= R; dx++)
        {
            int2 q = px + int2(dx * step, dy * step);
            q.x = clamp(q.x, minX, maxX);
            q.y = clamp(q.y, minY, maxY);

            float  Dn = gdepth.Load(int3(q, 0));
            float4 gn = ggbuf.Load(int3(q, 0));
            if (Dn >= 0.99999f || gn.a < 0.001f) continue;   /* sky / no-gbuf tap */

            float3 nn = gn.rgb * 2.0f - 1.0f;
            float  nnl = length(nn);
            if (nnl < 0.001f) continue;
            nn /= nnl;

            /* spatial gaussian * planar-depth similarity * (gentle) normal similarity.
             * A gentle normal power lets a gradually curving car body average
             * while a wall (near-perpendicular normal) still stops the blur. */
            float ox = (float)(dx * step), oy = (float)(dy * step);
            float dPred = Dc + Dgx * ox + Dgy * oy;          /* expected on-plane depth */
            float phiZ  = depthSigma * (abs(Dgx * ox) + abs(Dgy * oy)) + 3e-4f;
            float wS = exp(-(float)(dx * dx + dy * dy) / (2.0f * sigmaSp2));
            float wZ = exp(-abs(Dn - dPred) / phiZ);
            float wN = pow(max(0.0f, dot(nn, nc)), normalPow);

            float w = wS * wZ * wN;
            sum  += src.Load(int3(q, 0)) * w;
            wsum += w;
        }
    }

    dst[px] = (wsum > 1e-6f) ? (sum / wsum) : sc;
}
