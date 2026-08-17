/**
 * ps_common.hlsli - Shared definitions for all pixel shaders
 *
 * Fog computation and constant buffer layout shared across all PS variants.
 */

#ifndef PS_COMMON_HLSLI
#define PS_COMMON_HLSLI

cbuffer FogParams : register(b0)
{
    float4 fogColor;    /* RGB fog color, A unused */
    float  fogStart;    /* fog start distance (Z value) */
    float  fogEnd;      /* fog end distance (Z value) */
    float  fogDensity;  /* fog density (for EXP modes, unused currently) */
    int    fogEnabled;  /* 0 = off, 1 = linear, 2 = exp, 3 = exp2 */
    int    alphaTestEnabled; /* 1 = discard pixels with alpha < alphaRef */
    float  alphaRef;    /* alpha test reference value (0..1) */
    float  _pad1;
    float  foliageAA;   /* 1.0 = use SampleFoliageAA() for texMap, else Sample() */
    float4 carSun;      /* [CAR SUN] w = sunlit-car brighten gain (per-draw, RT car bodies only) */
};

SamplerState samplerState : register(s0);
Texture2D    texMap       : register(t0);

/**
 * SampleFoliageAA - clamped, alpha-weighted 4-tap reconstruction for
 * color-keyed cutout textures (trees/fences/signs). Fetches texels directly
 * with Load() instead of going through the bound sampler:
 *   - Indices are clamped to the texture bounds, never wrapped, so the
 *     opposite edge of the bitmap can't bleed into the border of the
 *     billboard (the old WRAP+bilinear seam/"bars" bug).
 *   - Each tap's RGB is weighted by its OWN alpha before being averaged, so
 *     fully/partly-transparent texels — whose RGB is leftover source-art
 *     color (e.g. sky-blue painted behind the tree) never meant to be seen —
 *     can't bleed their color into the edge. Alpha itself is still a plain
 *     bilinear average, so the cutout still gets a smooth 0..1 edge ramp for
 *     the alpha test/blend to soften.
 */
/* One clamped, alpha-weighted 4-tap bilinear fetch at an EXPLICIT mip level.
 * `uv` must already be wrap-resolved by the caller. Level dimensions come from
 * shifting texW/texH, matching the chain d3d12_tex_upload_mipped builds (each
 * level = previous halved, min 1). Returns:
 *   .rgb = alpha-weighted average colour (transparent taps contribute none)
 *   .a   = plain bilinear alpha = fractional COVERAGE of the cutout at this level
 * The box-down that built the chain stores a plain alpha average per mip
 * (d3d12_mip_box_down: `o[3] = plain alpha avg`), so .a here is honest coverage. */
float4 FoliageTapLevel(Texture2D tex, float2 uv, int level, uint texW, uint texH)
{
    uint lw = max(texW >> (uint)level, 1u);
    uint lh = max(texH >> (uint)level, 1u);
    int2 lo = int2(0, 0);
    int2 hi = int2(lw, lh) - int2(1, 1);

    float2 texel = uv * float2(lw, lh) - 0.5;
    float2 f     = frac(texel);
    int2   base  = int2(floor(texel));

    int2 i00 = clamp(base,                lo, hi);
    int2 i10 = clamp(base + int2(1, 0),   lo, hi);
    int2 i01 = clamp(base + int2(0, 1),   lo, hi);
    int2 i11 = clamp(base + int2(1, 1),   lo, hi);

    float4 c00 = tex.Load(int3(i00, level));
    float4 c10 = tex.Load(int3(i10, level));
    float4 c01 = tex.Load(int3(i01, level));
    float4 c11 = tex.Load(int3(i11, level));

    float w00 = (1.0 - f.x) * (1.0 - f.y);
    float w10 = f.x         * (1.0 - f.y);
    float w01 = (1.0 - f.x) * f.y;
    float w11 = f.x         * f.y;

    float alpha = c00.a * w00 + c10.a * w10 + c01.a * w01 + c11.a * w11;

    float aw00 = w00 * c00.a, aw10 = w10 * c10.a, aw01 = w01 * c01.a, aw11 = w11 * c11.a;
    float aWeightSum = aw00 + aw10 + aw01 + aw11;
    float3 rgb = (aWeightSum > 1e-5)
        ? (c00.rgb * aw00 + c10.rgb * aw10 + c01.rgb * aw01 + c11.rgb * aw11) / aWeightSum
        : float3(0.0, 0.0, 0.0);

    return float4(rgb, alpha);
}

/* [foliage soft coverage 2026-08-17] Returns alpha-weighted RGB and a SMOOTH
 * LOD-derived coverage in .a. The caller (foliage path) feeds that coverage to
 * the SRCALPHA/INVSRCALPHA blend and discards only fully-transparent texels
 * (ApplyFogAndAlphaTest lowers alphaRef when foliageAA!=0), so cutout edges are
 * anti-aliased instead of a hard 1-bit test.
 *
 * WHY LOD matters here (and why an earlier LOD-only attempt did nothing): the
 * mip chain already carries a coverage-preserving alpha average per level, but
 * the old hard `alpha < 0x80` discard RE-BINARIZED it, so distant canopies
 * still flickered texel-on/off (the diamond stipple). Sampling the chain by the
 * screen footprint AND letting the smooth coverage survive into the blend is
 * what removes the aliasing. Un-mipped pages report mipCount==1 -> lod clamps to
 * 0 and this degrades to a single level-0 bilinear (1-texel soft edge), so pages
 * without a chain are still safe. */
float4 SampleFoliageAA(Texture2D tex, float2 uv)
{
    uint texW, texH, mipCount;
    tex.GetDimensions(0, texW, texH, mipCount);

    /* Derivatives from the PRE-wrap uv: frac() introduces a seam discontinuity
     * and ddx/ddy across it would spike to ~texture-width and force a blurry
     * max LOD on the seam quad. The pre-wrap uv is continuous across the quad. */
    float2 duvdx = ddx(uv) * float2(texW, texH);
    float2 duvdy = ddy(uv) * float2(texW, texH);
    float  rho   = max(length(duvdx), length(duvdy));
    /* [foliage dither merge 2026-08-17] Some cutout pages bake a 2x2 ORDERED-
     * DITHER alpha (1-bit alpha faking partial coverage, a 1999 trick — e.g.
     * Blue Ridge page 317 is 15% perfect-checkerboard). At the native level that
     * dither reads as the visible diamond/plus stipple around canopies. A 2x2
     * dither is fully merged the moment a tap averages over >=2x2 texels, i.e.
     * mip level >= 1, so floor the LOD at 1.0 for foliage. This trades a hair of
     * near-tree sharpness (64->32 effective texels) for the dither vanishing at
     * ALL distances; foliage is soft-edged content so the sharpness loss is not
     * noticeable, whereas the stipple was. TD5RE_FOLIAGE_AA=0 bypasses this whole
     * path (raw hard cutout). Pages with a clean (non-dithered) silhouette are
     * unaffected visually — averaging a solid interior is a no-op. */
    float  lod   = clamp(log2(max(rho, 1e-6)), 1.0, float(mipCount - 1));
    if (mipCount <= 1) lod = 0.0;   /* un-mipped page: nothing to floor to */

    /* [2026-07-06] Tile OUT-OF-RANGE UVs the way hardware WRAP would: some
     * sprites (e.g. Moscow's streetlamp glow heads) sample uv well past 1.0
     * and relied on the sampler tiling them — clamping those smeared one
     * opaque corner texel across the whole quad (the "solid black ball"
     * regression). IN-RANGE UVs stay un-fracced so a 0..1 billboard's border
     * still can't wrap to the opposite edge (the seam bug this function
     * exists to fix; frac() at exactly 1.0 would wrap the border row to 0,
     * hence the conditional rather than an unconditional frac). */
    uv.x = (uv.x < 0.0 || uv.x > 1.0) ? frac(uv.x) : uv.x;
    uv.y = (uv.y < 0.0 || uv.y > 1.0) ? frac(uv.y) : uv.y;

    int   l0 = (int)floor(lod);
    int   l1 = min(l0 + 1, (int)mipCount - 1);
    float t  = lod - (float)l0;

    float4 s0 = FoliageTapLevel(tex, uv, l0, texW, texH);
    float4 s1 = FoliageTapLevel(tex, uv, l1, texW, texH);
    return lerp(s0, s1, t);   /* trilinear: smooth colour + smooth coverage */
}

/* Dispatch helper used by every PS variant that samples texMap: routes
 * foliage-AA draws through the manual reconstruction above, everything else
 * through the normal sampler path (unchanged behavior). */
float4 SampleTex(Texture2D tex, SamplerState samp, float2 uv)
{
    float4 result;
    if (foliageAA != 0.0)
        result = SampleFoliageAA(tex, uv);
    else
        result = tex.Sample(samp, uv);
    return result;
}

struct PS_INPUT
{
    float4 pos      : SV_POSITION;
    float4 diffuse  : COLOR0;
    float4 specular : COLOR1;
    float2 uv       : TEXCOORD0;
    float  depth    : TEXCOORD1;
};

/**
 * Apply fog to a computed color. Uses linear fog based on vertex Z depth.
 * D3D6 table fog uses the Z value from pre-transformed vertices as the
 * fog distance, with linear interpolation between fogStart and fogEnd.
 */
/* [PERSPECTIVE DEPTH 2026-08-03] The vertex `depth` (TEXCOORD1) is now the
 * perspective-correct normalized depth (td5_depth_persp), not the old linear
 * (vz-NEAR)/RANGE. Fog is authored in the OLD linear-normalized space (fogStart/
 * fogEnd are linear [0,1]), so convert back before the lerp to keep fog visually
 * identical. NEAR/RANGE mirror the C constants NEAR_DEPTH_OFFSET / (1/DEPTH_
 * NORMALIZE_INV). */
#define TD5_DEPTH_NEAR   64.0
#define TD5_DEPTH_RANGE  195000.0
float td5_persp_depth_to_linear(float d)
{
    float A  = (TD5_DEPTH_NEAR + TD5_DEPTH_RANGE) / TD5_DEPTH_RANGE;
    float vz = TD5_DEPTH_NEAR * A / (A - d);          /* inverse of td5_depth_persp */
    return (vz - TD5_DEPTH_NEAR) / TD5_DEPTH_RANGE;   /* old linear normalized depth */
}

float4 ApplyFogAndAlphaTest(float4 color, float depth)
{
    /* Alpha test: discard pixels below threshold (replaces D3D6 fixed-function alpha test).
     * This is critical for color-keyed textures (A1R5G5B5 with alpha=0 for transparent pixels).
     * Without this, transparent pixels render as opaque black, covering geometry behind them. */
    /* [foliage soft coverage 2026-08-17] Foliage draws (foliageAA!=0) carry a
     * SMOOTH LOD-derived coverage in color.a from SampleFoliageAA, meant to feed
     * the SRCALPHA/INVSRCALPHA blend for anti-aliased cutout edges. The normal
     * alphaRef (0x80 for TRANSLUCENT_ANISO) would re-binarize that coverage and
     * bring back the distant-canopy edge flicker (the diamond stipple), so for
     * foliage we discard only fully-transparent texels and let the coverage
     * drive the blend. Non-foliage draws are unchanged. TD5RE_FOLIAGE_AA=0 sets
     * foliageAA=0 -> falls back to the hard alphaRef cutout. */
    float aref = (foliageAA != 0.0) ? (1.0 / 255.0) : alphaRef;
    if (alphaTestEnabled && color.a < aref)
        discard;

    /* [CAR SUN 2026-08-04] The sunlit-car brighten is now DIRECTIONAL (N.L against
     * the sun) and applied in ps_modulate_g / ps_modulate_alpha_g, where the packed
     * world normal is available — so the car brightens only where the sun actually
     * hits it. (The old uniform (1+gain) multiply lived here but read as a flat lift
     * with no sun cue.) Non-car draws set carSun.w=0 and are byte-identical. */

    if (fogEnabled)
    {
        /* Linear fog in the authored linear-depth space (convert from the new
         * perspective depth first): factor = (end - z) / (end - start). */
        float linDepth  = td5_persp_depth_to_linear(depth);
        float fogFactor = saturate((fogEnd - linDepth) / (fogEnd - fogStart));
        color.rgb = lerp(fogColor.rgb, color.rgb, fogFactor);
    }
    return color;
}

#endif /* PS_COMMON_HLSLI */
