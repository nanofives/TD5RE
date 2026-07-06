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
float4 SampleFoliageAA(Texture2D tex, float2 uv)
{
    uint texW, texH;
    tex.GetDimensions(texW, texH);
    int2 lo = int2(0, 0);
    int2 hi = int2(texW, texH) - int2(1, 1);

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

    float2 texel = uv * float2(texW, texH) - 0.5;
    float2 f     = frac(texel);
    int2   base  = int2(floor(texel));

    int2 i00 = clamp(base,                lo, hi);
    int2 i10 = clamp(base + int2(1, 0),   lo, hi);
    int2 i01 = clamp(base + int2(0, 1),   lo, hi);
    int2 i11 = clamp(base + int2(1, 1),   lo, hi);

    float4 c00 = tex.Load(int3(i00, 0));
    float4 c10 = tex.Load(int3(i10, 0));
    float4 c01 = tex.Load(int3(i01, 0));
    float4 c11 = tex.Load(int3(i11, 0));

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
float4 ApplyFogAndAlphaTest(float4 color, float depth)
{
    /* Alpha test: discard pixels below threshold (replaces D3D6 fixed-function alpha test).
     * This is critical for color-keyed textures (A1R5G5B5 with alpha=0 for transparent pixels).
     * Without this, transparent pixels render as opaque black, covering geometry behind them. */
    if (alphaTestEnabled && color.a < alphaRef)
        discard;

    if (fogEnabled)
    {
        /* Linear fog: factor = (end - z) / (end - start), clamped [0,1] */
        float fogFactor = saturate((fogEnd - depth) / (fogEnd - fogStart));
        color.rgb = lerp(fogColor.rgb, color.rgb, fogFactor);
    }
    return color;
}

#endif /* PS_COMMON_HLSLI */
