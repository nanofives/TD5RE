/**
 * rt_pipeline.hlsl -- unified DXR shader library for the ray-traced lighting
 * stack (LIGHTING QUALITY: HIGH). Compiled ONCE to a lib_6_3 DXIL blob
 * (rt_pipeline_bytes.h, g_rt_pipeline) by compile_shaders.bat via dxc; the
 * D3D12 backend selects entry points by name in a single DXIL_LIBRARY_DESC.
 *
 * This is DATA to the MinGW build (a BYTE array header) -- zero linking impact.
 *
 * Phase 0 scope: rgen_smoke only -- a raygen with NO TraceRay that writes a
 * UV gradient into the output UAV, proving the DispatchRays pipeline end to end
 * (state object + SBT + global root signature + shader-visible heap). Later
 * phases append rgen_debug / rgen_shadow / rgen_refl / miss_* / chit_* / anyhit
 * entry points to this same library (see RT_LIGHTING_PLAN.md sec.4).
 *
 * Global root signature (frozen, see plan sec.4):
 *   b0            : per-dispatch constants (root CBV)               [unused in P0]
 *   descriptor table 0 (fixed slots) : u0 output UAV, ...           [u0 used in P0]
 *   descriptor table 1 : unbounded SRV t0,space1 (bindless, Phase 3)
 *   static sampler s0  : LINEAR wrap
 */

#include "rt_common.hlsli"   /* resources, CBs, payloads, reconstruction helpers */

[shader("raygeneration")]
void rgen_smoke()
{
    uint2 px  = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;
    float2 uv = (float2(px) + 0.5f) / float2(dim);
    /* Gradient: R = x, G = y, B = 0. Proof the raygen ran over the full grid.
     * (Channel order is swapped when blitted to the BGRA backbuffer -- fine,
     * the smoke test only checks that a full-frame gradient appears.) */
    g_output[px] = float4(uv.x, uv.y, 0.0f, 1.0f);
}

/* ----- Phase 1: primary-ray debug view (TD5RE_RT_DEBUGVIEW=1) -------------- *
 * One camera ray per pane pixel against the TLAS. On hit: instance-hash false
 * color (silhouette) scaled by a soft depth cue. On miss: dark blue. The
 * framedump of this must align pixel-for-pixel with a raster framedump of the
 * same frame (the Phase 1 alignment gate). */
[shader("raygeneration")]
void rgen_debug()
{
    uint2 lpx = DispatchRaysIndex().xy;          /* pane-local pixel */
    float3 dir = rt_primary_ray_dir((float)lpx.x + 0.5f, (float)lpx.y + 0.5f);

    RayDesc ray;
    ray.Origin    = g_camPos;
    ray.Direction = dir;
    ray.TMin      = g_rayTMin;
    ray.TMax      = g_rayTMax;

    RayPayload p;
    p.color = float3(0.02f, 0.02f, 0.12f);       /* miss = dark blue */
    p.t     = -1.0f;
    /* RAY_FLAG_NONE = no face culling (winding settled empirically in the gate). */
    TraceRay(g_tlas, RAY_FLAG_NONE, 0xFF, /*hitGroup*/0, /*mult*/0, /*miss*/1, ray, p);

    /* Alpha = 0.65 on a hit, 0 on a miss: the blit ALPHA-BLENDS this over the
     * real raster scene, so one framedump overlays the RT geometry on the raster
     * world -- a direct alignment check (the Phase 1 gate). */
    uint2 opx = uint2(g_paneOrigin) + lpx;
    g_output[opx] = float4(p.color, p.t >= 0.0f ? 0.65f : 0.0f);
}

/* Hit group "hg" closest-hit. Phase 1: trivial -- instance-hash color + a mild
 * distance darkening so nearer geometry reads brighter. Phase 3 replaces the
 * body with textured hit shading (UV interpolation + bindless sample). */
[shader("closesthit")]
void chit_refl(inout RayPayload p, in BuiltInTriangleIntersectionAttributes attr)
{
    /* (index+1) so instance 0 (the track) is not black; floor the brightness so
     * every hit reads clearly against the dark-blue miss. A mild facing cue from
     * the barycentrics adds surface definition. */
    uint h = (InstanceIndex() + 1u) * 2654435761u;
    float3 c = float3(((h >> 16) & 255) / 255.0f,
                      ((h >> 8)  & 255) / 255.0f,
                      ( h        & 255) / 255.0f);
    c = 0.30f + 0.70f * c;
    float bary = 0.75f + 0.25f * (attr.barycentrics.x + attr.barycentrics.y);
    p.color = c * bary;
    p.t = RayTCurrent();
}

/* Miss record 0: shadow rays (Phase 2b). Unblocked path -> visible. */
[shader("miss")]
void miss_shadow(inout ShadowPayload p)
{
    p.visible = 1;
}

/* Miss record 1: reflection / primary-ray sky. Phase 3 feeds real sky/fog. */
[shader("miss")]
void miss_refl(inout RayPayload p)
{
    p.color = float3(0.02f, 0.02f, 0.12f);
    p.t = -1.0f;
}

/* ----- Phase 2b: RT sun shadows (b1 ShadowCB) ------------------------------ *
 * One sun shadow ray per pixel, cone-jittered. Writes shade (1=lit,
 * 1-strength=full shadow) to g_sunvis at the full-frame pixel; the MULT
 * composite (ps_shadow_rt) darkens the scene by it. */
[shader("raygeneration")]
void rgen_shadow()
{
    uint2 lpx = DispatchRaysIndex().xy;                       /* pane-local pixel   */
    int2  fp  = int2((int)sh_misc.y, (int)sh_misc.z) + int2(lpx); /* full-frame pixel */
    float D = g_depth.Load(int3(fp, 0));
    float4 gb = g_gbuf.Load(int3(fp, 0));
    if (D >= 0.99999f || gb.a < 0.001f) { g_sunvis[fp] = 1.0f; return; }  /* sky / no gbuffer = lit */

    float3 world = rt_world_from_depth(D, float2(lpx), sh_camPosFocal, sh_rightCx, sh_upCy, sh_fwdDepthScale, sh_misc.x);
    float3 N = rt_gbuf_normal(gb);
    /* normal-offset bias, distance-scaled (24.8-quantized geometry -> acne). */
    float dist = length(world - sh_camPosFocal.xyz);
    float3 origin = world + N * (16.0f + dist * 0.004f);

    float3 L = normalize(sh_sun.xyz);
    float3 up0 = abs(L.y) < 0.99f ? float3(0,1,0) : float3(1,0,0);
    float3 T = normalize(cross(up0, L));
    float3 Bv = cross(L, T);
    float ang = rt_hash12(float2(fp)) * 6.2831853f;
    float3 dir = normalize(L + (cos(ang) * T + sin(ang) * Bv) * 0.012f);  /* ~0.7deg cone */

    float vis = rt_shadow_ray(origin, dir, 1.0f, sh_sun.w);
    g_sunvis[fp] = 1.0f - sh_misc.w * (1.0f - vis);
}

/* ----- Phase 2b: RT dynamic-light occlusion (b2 LightCB) ------------------- *
 * Per pixel, accumulate each enabled light's contribution (attenuation + cone +
 * soft-wrap Lambert) gated by a shadow ray toward the light. Writes additive rgb
 * to g_lightcol; the additive composite (ps_light_rt) adds it to the scene. */
[shader("raygeneration")]
void rgen_light()
{
    uint2 lpx = DispatchRaysIndex().xy;
    int2  fp  = int2((int)li_misc.z, (int)li_misc.w) + int2(lpx);   /* vpX/vpY at misc.z/.w */
    float D = g_depth.Load(int3(fp, 0));
    if (D >= 0.99999f) { g_lightcol[fp] = float4(0,0,0,0); return; }

    float3 world = rt_world_from_depth(D, float2(lpx), li_camPosFocal, li_rightCx, li_upCy, li_fwdDepthScale, li_misc.x);
    float4 gb = g_gbuf.Load(int3(fp, 0));
    bool hasN = gb.a > 0.001f;
    float3 N = rt_gbuf_normal(gb);
    int count = (int)li_misc.y;

    float3 accum = float3(0,0,0);
    for (int k = 0; k < count && k < RT_LIGHT_MAX; k++)
    {
        float4 P  = li_lights[k*3+0];   /* pos.xyz + range   */
        float4 C  = li_lights[k*3+1];   /* rgb + intensity   */
        float4 Dc = li_lights[k*3+2];   /* dir.xyz + coneCos */
        float3 toL = P.xyz - world;
        float d = length(toL);
        if (d >= P.w || d < 1e-3f) continue;
        float3 Ld = toL / d;
        float atten = 1.0f - d / P.w; atten *= atten;
        float cone = 1.0f;
        if (Dc.w > -0.5f) { float cd = dot(-Ld, Dc.xyz); cone = saturate((cd - Dc.w) / (1.0f - Dc.w)); cone *= cone; }
        float ndotl = hasN ? saturate(dot(N, Ld)) * 0.85f + 0.15f : 1.0f;
        float w = C.w * atten * cone * ndotl;
        if (w < 0.003f) continue;
        float3 origin = world + (hasN ? N : Ld) * (8.0f + d * 0.004f);
        float vis = rt_shadow_ray(origin, Ld, 1.0f, d - 4.0f);
        accum += C.rgb * (w * (vis > 0.5f ? 1.0f : 0.15f));
    }
    g_lightcol[fp] = float4(accum, 1.0f);
}
