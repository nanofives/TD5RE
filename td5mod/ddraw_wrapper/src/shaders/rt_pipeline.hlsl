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

#include "rt_common.hlsli"

/* Fixed descriptor table 0 (global root signature): u0 output, t0 TLAS. */
RWTexture2D<float4>        g_output : register(u0);
RaytracingAccelerationStructure g_tlas : register(t0);

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
