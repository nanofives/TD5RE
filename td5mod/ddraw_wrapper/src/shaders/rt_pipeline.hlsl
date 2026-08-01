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
    /* Phase 3: fetch the hit triangle from the VB/IB pools via the GeoRecord,
     * interpolate vertex color by barycentrics, shade with a two-sided up-facing
     * term (world normal = object normal * the rigid instance transform). Used by
     * both the reflection raygen and the debug view. (Textured shading + a sun
     * shadow ray are owed refinements.) */
    /* One range per mesh (Phase 1 feed) -> GeoRecord index = InstanceID().
     * (GeometryIndex() would need SM6.5; not available in lib_6_3.) */
    GeoRecord rec = g_geo[InstanceID()];
    uint3 idx = rt_load_tri_indices(rec.ib_byte_off, PrimitiveIndex());
    float3 p0 = rt_vertex_pos(rec.vb_byte_off, idx.x);
    float3 p1 = rt_vertex_pos(rec.vb_byte_off, idx.y);
    float3 p2 = rt_vertex_pos(rec.vb_byte_off, idx.z);
    float3 c0 = rt_vertex_color(rec.vb_byte_off, idx.x);
    float3 c1 = rt_vertex_color(rec.vb_byte_off, idx.y);
    float3 c2 = rt_vertex_color(rec.vb_byte_off, idx.z);
    float3 bw = float3(1.0f - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.x, attr.barycentrics.y);
    float3 col = c0 * bw.x + c1 * bw.y + c2 * bw.z;
    /* [P3] Textured hit shading: sample the hit page's texture (bindless) and
     * modulate the interpolated vertex colour. texture_index 0 = "no texture"
     * (track lane quads have no UV) -> keep the flat vertex colour. */
    if (rec.texture_index != 0u && rec.texture_index < 1024u) {   /* only filled slots */
        float2 uv0 = rt_vertex_uv(rec.vb_byte_off, idx.x);
        float2 uv1 = rt_vertex_uv(rec.vb_byte_off, idx.y);
        float2 uv2 = rt_vertex_uv(rec.vb_byte_off, idx.z);
        float2 uv  = uv0 * bw.x + uv1 * bw.y + uv2 * bw.z;
        float3 tex = g_bindless[NonUniformResourceIndex(rec.texture_index)].SampleLevel(g_samp, uv, 0).rgb;
        col *= tex;
    }
    float3 N = normalize(mul((float3x3)ObjectToWorld3x4(), cross(p1 - p0, p2 - p0)));
    float shade = 0.35f + 0.65f * saturate(abs(N.y));
    /* [P3] Sun shadow ray from the reflection hit point: reflected geometry (a car
     * reflected on another car's paint, the road under a car) now receives sun
     * shadows. Gated by sr_sun.w (SSRCB) so it costs nothing when the sun pass is
     * off / below the horizon. Depth 2 (rgen_refl -> chit_refl -> this ray). */
    if (sr_sun.w > 0.5f) {
        float3 wpos = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
        float3 so = wpos + N * (16.0f + RayTCurrent() * 0.004f);   /* normal-offset bias */
        float vis = rt_shadow_ray(so, normalize(sr_sun.xyz), 1.0f, sr_params.y);
        shade *= (vis > 0.5f) ? 1.0f : 0.45f;   /* in shadow -> darken, not black */
    }
    p.color = col * shade;
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

/* ----- Phase 3: RT reflections (b3 SSRCB) ---------------------------------- *
 * On reflective pixels (per-material reflectivity from the G-buffer matid +
 * wet-road boost), reflect the view dir and trace one ray through the hit group;
 * write the reflected color + a Fresnel*reflectivity weight to g_reflcol. The
 * composite (ps_ssr_rt) alpha-blends it onto the scene. Off-screen geometry is
 * visible in the reflection -- the RT win over the screen-space march. */
[shader("raygeneration")]
void rgen_refl()
{
    uint2 lpx = DispatchRaysIndex().xy;
    int2  fp  = int2((int)sr_misc.y, (int)sr_misc.z) + int2(lpx);
    g_reflcol[fp] = float4(0,0,0,0);
    float D = g_depth.Load(int3(fp, 0));
    float4 gb = g_gbuf.Load(int3(fp, 0));
    if (D >= 0.99999f || gb.a < 0.001f) return;

    int matid = (int)(gb.a * 255.0f + 0.5f);
    float base = rt_reflectivity(matid);
    float3 N = normalize(gb.rgb * 2.0f - 1.0f);
    if (matid == 1 && -N.y > 0.6f) base += sr_misc.w * saturate((-N.y - 0.6f) / 0.4f);  /* wet road */
    int dg = (int)(sr_params2.z + 0.5f);
    /* [P3 diag] dg1: R=base reflectivity, G=matid/8, B=up. */
    if (dg == 1) { g_reflcol[fp] = float4(base, (float)matid / 8.0f, saturate(-N.y), 1.0f); return; }
    if (base < 0.01f && dg == 0) return;   /* early-out: most pixels don't reflect (perf save) */

    float3 world = rt_world_from_depth(D, float2(lpx), sr_camPosFocal, sr_rightCx, sr_upCy, sr_fwdDepthScale, sr_misc.x);
    float3 V = normalize(world - sr_camPosFocal.xyz);
    float ndv = saturate(dot(-V, N));
    float w = base * (0.25f + 0.75f * pow(1.0f - ndv, 3.0f)) * sr_params2.y;   /* Fresnel * intensity */
    /* [P3 diag] dg2: weight w as grayscale (params2.y intensity check). */
    if (dg == 2) { g_reflcol[fp] = float4(w, w, w, 1.0f); return; }
    if (w < 0.02f && dg == 0) return;

    float3 R = reflect(V, N);
    float dist = length(world - sr_camPosFocal.xyz);
    RayDesc ray;
    ray.Origin = world + N * (16.0f + dist * 0.004f);
    ray.Direction = R; ray.TMin = 1.0f; ray.TMax = sr_params.y;   /* max reflect dist */
    RayPayload pl; pl.color = float3(0.02f, 0.02f, 0.12f); pl.t = -1.0f;
    TraceRay(g_tlas, RAY_FLAG_NONE, 0xFF, /*hitGroup*/0, /*mult*/0, /*miss*/1, ray, pl);
    /* [P3 diag] dg3: raw reflected color. */
    if (dg == 3) { g_reflcol[fp] = float4(pl.color, 1.0f); return; }
    g_reflcol[fp] = float4(pl.color, w);
}
