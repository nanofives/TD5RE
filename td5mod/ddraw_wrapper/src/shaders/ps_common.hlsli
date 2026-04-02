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
};

SamplerState samplerState : register(s0);
Texture2D    texMap       : register(t0);

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
