/**
 * ps_modulate_g.hlsl - MODULATE + G-buffer write (lighting rework P0)
 *
 * Same color math as ps_modulate.hlsl, plus SV_Target1 = the raw COLOR1
 * (specular) dword: .rgb = world normal (biased 0..1), .a = material id/255.
 * Bound instead of ps_modulate for z-writing, non-blended draws while the
 * G-buffer is active, so the deferred light pass can do proper N.L.
 * Alpha-test discard drops BOTH targets — cutout holes never stamp normals.
 */

#include "ps_common.hlsli"

struct PS_OUTPUT
{
    float4 color : SV_Target0;
    float4 gbuf  : SV_Target1;
};

PS_OUTPUT main(PS_INPUT input)
{
    float4 tex = texMap.Sample(samplerState, input.uv);
    /* [CAR REFL 2026-08-04] Per-texel car reflectivity from the OFFLINE material
     * mask (bake_car_material_mask.py), baked into the car skin's ALPHA at load:
     * tex.a holds a per-texel matid (1=body, 3=glass, 4=lights). On CARBODY draws
     * (whole car tagged matid 5 at the vertex), route that per-texel matid into
     * the G-buffer so the reflection pass mirrors glass strongly, body subtly,
     * lights not at all — precise, no runtime colour guessing. Skin alpha is
     * meaningless as opacity here, so force color.a = 1 to keep the body opaque
     * and never alpha-test-discard the car (matid values are tiny). */
    bool  isCar    = ((int)(input.specular.a * 255.0 + 0.5) == 5);
    float carMatid = tex.a;                      /* per-texel matid/255 */

    float4 color;
    color.rgb = tex.rgb * input.diffuse.rgb;
    color.a   = isCar ? 1.0 : tex.a;

    PS_OUTPUT o;
    o.color = ApplyFogAndAlphaTest(color, input.depth);
    o.gbuf  = input.specular;
    if (isCar) o.gbuf.a = carMatid;
    return o;
}
