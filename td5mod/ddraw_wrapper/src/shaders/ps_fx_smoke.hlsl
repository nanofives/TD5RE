/**
 * ps_fx_smoke.hlsl - Procedural, texture-free smoke / dust / exhaust puff.
 *
 * Replaces the SMOKE atlas sprite. The quad is a unit square (uv 0..1). Rather
 * than a round disc that noise merely dims (which still reads as a circle), the
 * silhouette is built by DOMAIN-WARPING the sample position (so billows curl)
 * and then THRESHOLDING an fbm field — the boundary itself becomes ragged and
 * cloud-like, and dissolves as the puff ages. No texture page is sampled.
 *
 * Per-particle data rides in the vertex stream:
 *   diffuse  (COLOR0) = tint .rgb + master alpha .a
 *   specular (COLOR1) = age01 in .r  (0 = fresh, 1 = fully dissipated)
 *                       seed  in .g  (per-particle decorrelation)
 * Global animation time arrives in the b1 FxParams cbuffer.
 *
 * Blend: SRC_ALPHA / INV_SRC_ALPHA (TD5_PRESET_TRANSLUCENT_POINT_ZTEST), so the
 * returned colour is STRAIGHT (not premultiplied). Fog is applied so distant
 * smoke fades into the atmosphere like the world geometry behind it.
 */

#include "ps_common.hlsli"

cbuffer FxParams : register(b1)
{
    float g_time;   /* seconds, monotonic */
    float g_p0;     /* soft-particle flag: >0.5 = sample scene depth + fade */
    float g_p1;
    float g_p2;
};

/* Scene depth (R32_FLOAT view of the depth buffer), bound at t1 by
 * td5_plat_fx_soft_begin. Same LINEAR [0,1] space as this puff's input.depth
 * (the pretransformed VS stores input.pos.z straight through), so a plain
 * difference gives a uniform-width soft fade. */
Texture2D<float> sceneDepth : register(t1);

/* Soft-fade width in normalised linear depth. [0,1] maps to view-z ~[64,195064],
 * so ~0.0016 ~= 310 view-z units of fade as the puff approaches a surface. */
#define SOFT_FADE_RANGE  0.0016

/* Cheap hash -> value noise -> fbm. No texture, no gradient table. */
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

float fbm2(float2 p)
{
    float s = 0.0, amp = 0.5, tot = 0.0;
    [unroll] for (int i = 0; i < 2; ++i) { s += amp * vnoise(p); tot += amp; p = p * 2.03 + 19.0; amp *= 0.5; }
    return s / tot;
}

float fbm4(float2 p)
{
    float s = 0.0, amp = 0.5, tot = 0.0;
    [unroll] for (int i = 0; i < 4; ++i) { s += amp * vnoise(p); tot += amp; p = p * 2.02 + 11.0; amp *= 0.5; }
    return s / tot;
}

float4 main(PS_INPUT input) : SV_TARGET
{
    float2 c    = input.uv * 2.0 - 1.0;          /* centered [-1,1] */
    float  age  = saturate(input.specular.r);
    float  seed = input.specular.g * 255.0;

    /* Per-puff offset + upward scroll; the field also evolves over the puff's life. */
    float2 ofs = float2(seed * 1.7, seed * 0.9) + float2(0.0, -g_time * 0.22 - age * 1.1);

    /* Per-puff FIXED rotation of the noise field (from the seed) so neighbouring
     * puffs aren't identical stamped cards — breaks the "flat repeated sprite"
     * read. Fixed (no time term) so it does not add a visible spin. */
    float  ang = seed * 0.37;
    float  ca  = cos(ang), sa = sin(ang);
    float2 cr  = float2(c.x * ca - c.y * sa, c.x * sa + c.y * ca);

    /* --- Domain warp: displace the sample position by a low-freq fbm vector so
     * the billows curl and swirl instead of staying concentric. --- */
    float2 w = float2(fbm2(cr * 1.5 + ofs),
                      fbm2(cr * 1.5 + ofs + 7.3)) - 0.5;
    float2 p = cr + w * 1.25;

    /* --- Interior billow texture (higher-octave detail on the warped position). --- */
    float n = fbm4(p * 1.7 + ofs * 0.5);

    /* --- Ragged SILHOUETTE via a noise-perturbed radius. Pushing the boundary
     * in/out per-angle keeps the outline lumpy and cloud-like even when the puff
     * is fully DENSE — the old radial+threshold reverted to a clean circle once
     * the interior filled in (the "circle behind the car" at spawn). The radius
     * erodes/shrinks toward 0 with age so the cloud breaks up and fully clears
     * as it dissipates. --- */
    float r      = length(c);
    float edge   = fbm4(cr * 2.1 + ofs * 0.6 + 30.0);    /* boundary lumpiness */
    float radius = lerp(1.05, 0.28, age);
    float ragged = r - (edge - 0.5) * 0.85;              /* ±0.42 boundary push */
    float mask   = 1.0 - smoothstep(radius - 0.28, radius + 0.06, ragged);

    /* --- DENSE interior fill that thins to ZERO at age=1 so the puff fully
     * disappears (the old 0.40 floor kept aged smoke ~40% opaque, so it lingered
     * on the ground). --- */
    float fill    = lerp(0.95, 0.0, age) * (0.80 + 0.30 * n);
    float density = saturate(mask * fill);

    float a = density * input.diffuse.a;

    /* --- Soft particles: fade as the puff approaches scene geometry so it
     * dissolves into the ground/walls instead of slicing through them as a flat
     * card. sceneDepth and input.depth are both linear [0,1]; the difference is
     * positive when the puff is in front, shrinking to 0 at the intersection. --- */
    if (g_p0 > 0.5)
    {
        float sd   = sceneDepth.Load(int3((int)input.pos.x, (int)input.pos.y, 0));
        float pd   = saturate(input.depth);
        float fade = saturate((sd - pd) / SOFT_FADE_RANGE);
        a *= fade;
    }

    if (a < 0.03) discard;

    /* Internal shading: denser pockets read darker, wispy edges lighter. */
    float3 col = input.diffuse.rgb * (0.74 + 0.28 * n);

    return ApplyFogAndAlphaTest(float4(col, a), input.depth);
}
