/**
 * cs_color_atrous.hlsl - RGBA16F edge-aware à-trous denoise (compute).
 *
 * Sibling of cs_shadow_atrous.hlsl for the RGBA masks: RT dynamic-light (lightcol)
 * and RT reflection (reflcol). Same edge-stopping logic (depth + G-buffer normal),
 * but filters four channels so the jittered light/glossy-reflection grain smooths
 * while surface/geometry edges stay put. Driver ping-pongs src<->dst with a
 * doubling step over N iterations.
 *
 *   t0  src   (RGBA16F) input color mask (this iteration's source)
 *   t1  depth (R32F)    scene device depth
 *   t2  gbuf  (RGBA8)   .rgb = encoded normal, .a = coverage
 *   u0  dst   (RGBA16F) filtered output
 *   b0  DenoiseCB       pane rect + {step, radius, depthSigma, normalPow}
 */
Texture2D<float4>   src   : register(t0);
Texture2D<float>    gdepth: register(t1);
Texture2D<float4>   ggbuf : register(t2);
RWTexture2D<float4> dst   : register(u0);

cbuffer DenoiseCB : register(b0)
{
    float4 dn_rect;    /* x=paneX y=paneY z=paneW w=paneH (full-frame pixels) */
    float4 dn_params;  /* x=step y=radius z=depthSigma w=normalPow            */
    /* [round UA 2026-11-08] half-res RT: dn_rect is the pane in MASK space;
     * depth / G-buffer are read at the FULL-res pixel that was traced:
     * full = dn_full.xy + ((px - dn_rect.xy) << shift), clamped to dn_full.
     * shift 0 with dn_full == dn_rect reads exactly the pixels it always did. */
    float4 dn_map;     /* x=shift                                             */
    float4 dn_full;    /* x=paneX y=paneY z=paneW w=paneH (full-res)          */
};

int2 dn_fullpx(int2 q, int2 rmin, int sh, int2 fmin, int2 fmax)
{
    return min(fmin + ((q - rmin) << sh), fmax);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int minX = (int)dn_rect.x,  minY = (int)dn_rect.y;
    int W    = (int)dn_rect.z,  H    = (int)dn_rect.w;
    if ((int)tid.x >= W || (int)tid.y >= H) return;
    int2 px = int2(minX + (int)tid.x, minY + (int)tid.y);
    int  maxX = minX + W - 1,   maxY = minY + H - 1;

    float4 cc = src.Load(int3(px, 0));
    int    sh   = (int)(dn_map.x + 0.5f);
    int    st   = (int)(1u << (uint)sh);   /* full px per mask px */
    int2   rmin = int2(minX, minY);
    int2   fmin = int2((int)dn_full.x, (int)dn_full.y);
    int2   fmax = fmin + int2((int)dn_full.z, (int)dn_full.w) - 1;
    int2   pf   = dn_fullpx(px, rmin, sh, fmin, fmax);
    float  Dc = gdepth.Load(int3(pf, 0));
    float4 gc = ggbuf.Load(int3(pf, 0));

    /* Sky / no-G-buffer centre: pass through unfiltered. */
    if (Dc >= 0.99999f || gc.a < 0.001f) { dst[px] = cc; return; }
    float3 nc = gc.rgb * 2.0f - 1.0f;
    float  nlen = length(nc);
    if (nlen < 0.001f) { dst[px] = cc; return; }
    nc /= nlen;

    int   step = max(1, (int)(dn_params.x + 0.5f));
    int   R    = max(1, (int)(dn_params.y + 0.5f));
    float depthSigma = max(1e-5f, dn_params.z);
    float normalPow  = max(1.0f,  dn_params.w);
    float sigmaSp2   = max(1.0f, (float)(R * R));

    /* Planar depth prediction from the local gradient — see cs_shadow_atrous.hlsl
     * (avoids structured banding on walls seen at a grazing angle). */
    /* One MASK pixel = st full pixels, so the gradient is per mask px. */
    float Dgx = gdepth.Load(int3(int2(min(pf.x + st, fmax.x), pf.y), 0)) - Dc;
    float Dgy = gdepth.Load(int3(int2(pf.x, min(pf.y + st, fmax.y)), 0)) - Dc;

    float4 sum = float4(0,0,0,0); float wsum = 0.0f;
    [loop] for (int dy = -R; dy <= R; dy++)
    {
        [loop] for (int dx = -R; dx <= R; dx++)
        {
            int2 q = px + int2(dx * step, dy * step);
            q.x = clamp(q.x, minX, maxX);
            q.y = clamp(q.y, minY, maxY);

            int2   qf = dn_fullpx(q, rmin, sh, fmin, fmax);
            float  Dn = gdepth.Load(int3(qf, 0));
            float4 gn = ggbuf.Load(int3(qf, 0));
            if (Dn >= 0.99999f || gn.a < 0.001f) continue;

            float3 nn = gn.rgb * 2.0f - 1.0f;
            float  nnl = length(nn);
            if (nnl < 0.001f) continue;
            nn /= nnl;

            float ox = (float)(dx * step), oy = (float)(dy * step);
            float dPred = Dc + Dgx * ox + Dgy * oy;
            float phiZ  = depthSigma * (abs(Dgx * ox) + abs(Dgy * oy)) + 3e-4f;
            float wS = exp(-(float)(dx * dx + dy * dy) / (2.0f * sigmaSp2));
            float wZ = exp(-abs(Dn - dPred) / phiZ);
            float wN = pow(max(0.0f, dot(nn, nc)), normalPow);

            float w = wS * wZ * wN;
            sum  += src.Load(int3(q, 0)) * w;
            wsum += w;
        }
    }

    dst[px] = (wsum > 1e-6f) ? (sum / wsum) : cc;
}
