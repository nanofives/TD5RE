/**
 * cs_rt_upsample.hlsl - [round UA 2026-11-08] HALF-res RT mask -> FULL-res.
 *
 * The half-res RT path (rs_shift = 1, see rt_common.hlsli RTScaleCB) traces one
 * pixel per 2x2 block -- the block's top-left -- and denoises that quarter-size
 * mask. This pass writes the full-res mask the composites (and the translucent
 * shadow-receive path) read, so nothing downstream knows the trace was half-res.
 *
 * Joint-bilateral 2x2 upsample: for full pixel p (pane-local pl) the candidate
 * half texels are j in {floor((pl-1)/2), +1} per axis; texel j was traced at
 * full pixel 2j. Weight = bilinear (1 - |pl - 2j| / 2 per axis) * depth
 * similarity (relative, so near and far surfaces behave alike) * normal
 * similarity, read from the FULL-res depth/G-buffer at p and at 2j. A centre
 * pixel that is sky (or has no G-buffer coverage when up_params.w = 1) takes the
 * pass's neutral value (what the raygen writes there itself); such taps are
 * skipped. With no usable tap the nearest-by-depth texel is used.
 *
 * Built twice: default = R32F mask (shadow, GI), -D RT_UP_COLOR=1 = RGBA16F
 * (dynamic light, reflection). Shares the denoise root signature:
 *   t0 src (half mask)  t1 depth  t2 gbuf  u0 dst (full mask)  b0 UpCB
 */
#ifdef RT_UP_COLOR
typedef float4 MaskT;
#else
typedef float  MaskT;
#endif

Texture2D<MaskT>    src    : register(t0);
Texture2D<float>    gdepth : register(t1);
Texture2D<float4>   ggbuf  : register(t2);
RWTexture2D<MaskT>  dst    : register(u0);

cbuffer UpCB : register(b0)
{
    float4 up_rect;     /* full-res pane x, y, w, h                          */
    float4 up_neutral;  /* value for sky / uncovered centre pixels            */
    float4 up_half;     /* half-res pane x, y, w, h (mask space)              */
    float4 up_params;   /* x = view-Z tolerance (relative), y = normal power,
                           w = 1: a no-G-buffer centre is neutral too         */
    float4 up_depth;    /* x = depthScale (RANGE), y = depthBias (NEAR)       */
};

/* Same inverse as rt_common.hlsli depth_to_viewz (perspective-normalized depth). */
float up_viewz(float d)
{
    float S = max(1.0f, up_depth.x), B = max(1e-3f, up_depth.y);
    float A = (B + S) / S;
    return B * A / max(1e-6f, A - d);
}

bool up_empty(float D, float4 g, bool nogbufNeutral)
{
    return D >= 0.99999f || (nogbufNeutral && g.a < 0.001f);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int W = (int)up_rect.z, H = (int)up_rect.w;
    if ((int)tid.x >= W || (int)tid.y >= H) return;
    int2 pmin = int2((int)up_rect.x, (int)up_rect.y);
    int2 pl   = int2(tid.xy);
    int2 p    = pmin + pl;
    int2 hmin = int2((int)up_half.x, (int)up_half.y);
    int2 hmax = hmin + int2((int)up_half.z, (int)up_half.w) - 1;
    int2 fmax = pmin + int2(W, H) - 1;
    bool ngn  = up_params.w > 0.5f;

    float  Dc = gdepth.Load(int3(p, 0));
    float4 gc = ggbuf.Load(int3(p, 0));
    if (up_empty(Dc, gc, ngn)) {
#ifdef RT_UP_COLOR
        dst[p] = up_neutral;
#else
        dst[p] = up_neutral.x;
#endif
        return;
    }
    float3 nc = gc.rgb * 2.0f - 1.0f;
    float  ncl = length(nc);
    bool   hasN = ncl > 0.001f;
    if (hasN) nc /= ncl;

    float Zc   = up_viewz(Dc);
    float tol  = max(1e-4f, up_params.x) * Zc + 1.0f;
    float npow = max(1.0f, up_params.y);

    int2 j0 = int2((pl.x - 1) >> 1, (pl.y - 1) >> 1);   /* arithmetic shift = floor */
    MaskT sum = (MaskT)0;
    float wsum = 0.0f;
    MaskT best = (MaskT)0;
    float bestd = 1e30f;
    bool  haveBest = false;
    [unroll] for (int dy = 0; dy < 2; dy++)
    [unroll] for (int dx = 0; dx < 2; dx++)
    {
        int2 j  = j0 + int2(dx, dy);
        int2 hq = clamp(hmin + j, hmin, hmax);
        int2 jj = hq - hmin;                              /* clamped texel     */
        int2 q  = min(pmin + jj * 2, fmax);               /* where it was traced */
        float  Dn = gdepth.Load(int3(q, 0));
        float4 gn = ggbuf.Load(int3(q, 0));
        if (up_empty(Dn, gn, ngn)) continue;
        MaskT v = src.Load(int3(hq, 0));
        float dd = abs(up_viewz(Dn) - Zc);
        if (dd < bestd) { bestd = dd; best = v; haveBest = true; }
        float2 o  = abs(float2(pl) - float2(jj * 2));
        float  wb = max(0.0f, 1.0f - o.x * 0.5f) * max(0.0f, 1.0f - o.y * 0.5f);
        float  wz = exp(-dd / tol);
        float  wn = 1.0f;
        if (hasN) {
            float3 nn = gn.rgb * 2.0f - 1.0f;
            float  nl = length(nn);
            wn = (nl > 0.001f) ? pow(saturate(dot(nn / nl, nc)), npow) : 0.25f;
        }
        float w = wb * wz * wn;
        sum  += v * w;
        wsum += w;
    }
    MaskT outv;
    if (wsum > 1e-4f)   outv = sum / wsum;
    else if (haveBest)  outv = best;
    else {
#ifdef RT_UP_COLOR
        outv = up_neutral;
#else
        outv = up_neutral.x;
#endif
    }
    dst[p] = outv;
}
