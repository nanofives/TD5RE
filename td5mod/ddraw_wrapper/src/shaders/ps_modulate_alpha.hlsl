/**
 * ps_modulate_alpha.hlsl - TEXTUREMAPBLEND = MODULATEALPHA (5)
 *
 * Color = texture * diffuse vertex color
 * Alpha = texture alpha * diffuse alpha
 *
 * Both color and alpha are modulated. Used when the game wants vertex
 * alpha to control overall opacity (e.g., fading effects).
 */

#include "ps_common.hlsli"

float4 main(PS_INPUT input) : SV_TARGET
{
    float4 tex = SampleTex(texMap, samplerState, input.uv);
    /* [CAR GHOST FADE FIX 2026-08-05] The car skin's ALPHA channel carries a
     * per-texel material id (matid/255, ~0.004) from the offline reflection-mask
     * bake, NOT opacity (see ps_modulate_g's isCar handling). For a CAR body draw
     * (vertex matid 5) the whole-car fade opacity must therefore come from the
     * VERTEX diffuse alpha ALONE; multiplying by the matid tex.a would drive
     * alpha ~0 and the alpha test would discard the entire car -> the ghost /
     * arcade-GHOST / battle-wreck / traffic-fade car went fully invisible under
     * RT reflections. Non-car draws keep normal MODULATEALPHA (tex.a*diffuse.a). */
    bool  isCar = ((int)(input.specular.a * 255.0 + 0.5) == 5);
    float4 color;
    color.rgb = tex.rgb * input.diffuse.rgb;
    color.a   = isCar ? input.diffuse.a : (tex.a * input.diffuse.a);
    return ApplyFogAndAlphaTest(color, input.depth);
}
