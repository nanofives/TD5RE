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

/* [RT2-P2] Hit group "hg" any-hit: alpha-test cutout geometry (billboard trees/
 * signs, foliage). Only NON-opaque BLAS geometry (matid CUTOUT) invokes this;
 * opaque scenery keeps the fast early-accept path. Samples the hit page's alpha
 * at the barycentric UV and IgnoreHit()s transparent texels, so tree canopies
 * cast leaf-shaped shadows and reflect with holes instead of as solid quads.
 * Runs for BOTH shadow rays (they don't force-opaque) and reflection/primary
 * rays -> one test serves casting and hit shading. */
[shader("anyhit")]
void anyhit_cutout(inout RayPayload p, in BuiltInTriangleIntersectionAttributes attr)
{
    GeoRecord rec = g_geo[InstanceID()];
    if (rec.texture_index == 0u || rec.texture_index >= 1024u) return;   /* no page -> accept */
    uint3 idx = rt_load_tri_indices(rec.ib_byte_off, PrimitiveIndex());
    float2 uv0 = rt_vertex_uv(rec.vb_byte_off, idx.x);
    float2 uv1 = rt_vertex_uv(rec.vb_byte_off, idx.y);
    float2 uv2 = rt_vertex_uv(rec.vb_byte_off, idx.z);
    float3 bw = float3(1.0f - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.x, attr.barycentrics.y);
    float2 uv = uv0 * bw.x + uv1 * bw.y + uv2 * bw.z;
    float a = g_bindless[NonUniformResourceIndex(rec.texture_index)].SampleLevel(g_samp, uv, 0).a;
    if (a < 0.5f) IgnoreHit();
}
/* NOTE: shadow rays (rt_shadow_ray) use ShadowPayload and reflection/primary
 * rays use RayPayload, but a hit group has exactly ONE any-hit. anyhit_cutout
 * declares RayPayload yet NEVER reads or writes `p` (only IgnoreHit()/accept),
 * so the payload is inert bytes for both ray types and the mismatch is harmless
 * (both fit MaxPayloadSizeInBytes=32). Shadow rays keep RAY_FLAG_SKIP_CLOSEST_
 * HIT_SHADER (no CH) but do NOT force-opaque, so this any-hit still runs and
 * cutout geometry casts alpha-shaped shadows. */

/* Miss record 0: shadow rays (Phase 2b). Unblocked path -> visible. */
[shader("miss")]
void miss_shadow(inout ShadowPayload p)
{
    p.visible = 1;
}

/* Miss record 1: reflection / primary-ray SKY. A reflected ray that escapes to
 * the sky returns a bright sky gradient + a sun disc, so reflective car glass and
 * paint MIRROR the sun/sky — real "sunlight on the car", not a paint filter. The
 * ray dir is +Y-down world (up = -Y), same frame as the G-buffer normal, so the
 * sun is the UN-flipped SSR sun (sr_sun is Y-flipped for the TLAS shadow ray).
 * [CAR SUN 2026-08-04] */
[shader("miss")]
void miss_refl(inout RayPayload p)
{
    float3 dir = normalize(WorldRayDirection());
    float  up  = saturate(-dir.y);                          /* 1 at zenith, 0 at horizon */
    float3 horizon = float3(0.72f, 0.80f, 0.92f);           /* pale warm-white haze      */
    float3 zenith  = float3(0.18f, 0.38f, 0.78f);           /* saturated sky blue        */
    float3 sky = lerp(horizon, zenith, up * up);
    /* Sun disc + halo where the reflected ray points at the sun. */
    float3 sunDir = float3(sr_sun.x, -sr_sun.y, sr_sun.z);
    float  sd   = dot(dir, sunDir);
    float  disc = smoothstep(0.9993f, 0.99985f, sd);        /* crisp disc  */
    float  halo = pow(saturate(sd), 350.0f);                /* soft bloom  */
    sky += float3(1.0f, 0.96f, 0.86f) * (disc * 1.6f + halo * 0.5f);   /* toned down (was 8/2 = blowout) */
    p.color = sky;
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
    /* [shadow debug] sh_params.x < 0 (set by TD5RE_RT_SHADOW_DEBUG) tints the
     * NO-G-buffer ground 0.5 (grey) so the mult composite reveals coverage:
     * grey road = never shadowed because it wrote no G-buffer (root cause #1);
     * bright road = covered but the shadow ray missed the occluder (#2); dark
     * bands = shadows actually landing. Sky stays bright. */
    /* debug level: 1=raw vis, 2/3/4 = reconstructed world .x/.y/.z readout */
    int dbgLvl = sh_params.x < 0.0f ? (int)(-sh_params.x + 0.5f) : 0;
    bool sdbg = dbgLvl > 0;
    if (D >= 0.99999f)   { g_sunvis[fp] = 1.0f; return; }                /* sky = lit    */
    if (gb.a < 0.001f)   { g_sunvis[fp] = sdbg ? 0.5f : 1.0f; return; }  /* no gbuffer   */

    float3 world = rt_world_from_depth(D, float2(lpx), sh_camPosFocal, sh_rightCx, sh_upCy, sh_fwdDepthScale, sh_misc.x);
    /* [shadow debug] reconstructed world-pos readout: sample the pixel under the
     * car and decode value*scale, compare to the HUD car POS. Mismatch => the
     * road's depth reconstruction is in a different frame than the geometry. */
    if (dbgLvl == 2) { g_sunvis[fp] = saturate(abs(world.x) / 262144.0f); return; }
    if (dbgLvl == 3) { g_sunvis[fp] = saturate(abs(world.y) /  65536.0f); return; }
    if (dbgLvl == 4) { g_sunvis[fp] = saturate(abs(world.z) / 262144.0f); return; }
    /* [shadow debug] G-buffer normal check: 5 = validity (|raw normal|, 1=good
     * 0=zero/bad -> acne), 6/7/8 = encoded normal x/y/z (0.5 = zero component). */
    if (dbgLvl == 5) { g_sunvis[fp] = saturate(length(gb.rgb * 2.0f - 1.0f)); return; }
    if (dbgLvl == 6) { g_sunvis[fp] = gb.r; return; }
    if (dbgLvl == 7) { g_sunvis[fp] = gb.g; return; }
    if (dbgLvl == 8) { g_sunvis[fp] = gb.b; return; }
    float3 L = normalize(sh_sun.xyz);
    /* [ROAD-CAST FIX 2026-08-03] Offset the shadow-ray origin along the SUN
     * direction L, NOT the surface normal. The G-buffer normal's SIGN is
     * unreliable in the ray frame (the +Y lighting-vs-position convention split),
     * so a normal-offset bias pushed the road origin BELOW its own surface and
     * the up-toward-sun ray re-hit the road/track quads -> the whole road
     * self-shadowed. (Proven with TD5RE_RT_ONLYCARS=1: dropping track+scenery
     * from the TLAS turned the road fully lit, so the black was self-hit on world
     * geometry, not real occlusion; an 8x bias bump didn't help because it just
     * deepened the wrong-side offset.) L points toward the sun by construction,
     * so a step along it always lifts the origin off the receiver toward the lit
     * side; only genuine occluders between the surface and the sun then block it.
     * sh_params2.y = TD5RE_RT_BIAS scale. */
    float dist = length(world - sh_camPosFocal.xyz);
    float biasScale = sh_params2.y > 0.0001f ? sh_params2.y : 1.0f;
    /* [ROAD-CAST FIX 2026-08-03] Near-field constant raised 16 -> 64: at the
     * shortest camera distances the per-span road quads meet the sun ray at a
     * grazing angle, and a 16-unit lift was too small to clear the receiver's
     * own (and the adjacent span's) quad -> alternating black stripes per span
     * right in front of the camera. 64 clears it while staying far below car /
     * road-feature scale (no peter-panning; the car contact shadow stays tight).
     * The distance term (far-field) is unchanged. TD5RE_RT_BIAS scales both. */
    float3 origin = world + L * ((64.0f + dist * 0.004f) * biasScale);

    float3 up0 = abs(L.y) < 0.99f ? float3(0,1,0) : float3(1,0,0);
    float3 T = normalize(cross(up0, L));
    float3 Bv = cross(L, T);
    /* Multi-sample the cone (sh_params2.z = TD5RE_RT_RAYS, 0 -> 1): K stratified
     * rotations of the ~0.7deg cone, averaged. More samples -> smoother penumbra +
     * denoised sunvis (the single-ray cone jitter left mild grain). */
    int K = max(1, (int)(sh_params2.z + 0.5f));
    /* [RT2 P1] sh_params2.w = cone-spread scale (0 -> 1 = default ~0.7deg).
     * OVERCAST widens it (e.g. 5x) for a soft, directionless penumbra. */
    float coneScale = sh_params2.w > 0.0001f ? sh_params2.w : 1.0f;
    /* [CAR PIXELATION FIX 2026-08-05] Detect car pixels via the G-buffer alpha
     * isCar tag (bit 0x40, packed by ps_modulate_g.hlsl). Give the car body a
     * FINER jitter cell (so the coarse 16-unit world blocks that read as the
     * pixelated rear-panel pattern become fine grain the denoiser can smooth)
     * AND more cone samples (a smoother self-shadow penumbra straight out of the
     * trace, before denoise). Road/world keep the coarse, cheap path. */
    int  rawA  = (int)(gb.a * 255.0f + 0.5f);
    bool isCar = (rawA & 0x40) != 0;
    float jitterCell = isCar ? RT_CAR_NOISE_CELL : RT_NOISE_CELL;
    int  Kc = isCar ? max(K, 6) : K;
    float base = rt_hash_world_cell(world, 0.0f, jitterCell) * 6.2831853f;  /* [MOTION-STABLE] world-locked jitter */
    float visSum = 0.0f;
    for (int k = 0; k < Kc; k++) {
        float ang = base + (6.2831853f * (float)k) / (float)Kc;
        float3 dir = normalize(L + (cos(ang) * T + sin(ang) * Bv) * (0.012f * coneScale));
        visSum += rt_shadow_ray(origin, dir, 1.0f, sh_sun.w);
    }
    float vis = visSum / (float)Kc;
    /* [shadow debug] full-contrast raw visibility: covered+shadowed -> ~0 (black
     * band on the road where the ray hit an occluder), covered+lit -> 1 (white).
     * If the road stays uniformly white, the shadow rays are missing the fed
     * geometry (occluder mis-placed in the TLAS); black bands = shadows land. */
    if (dbgLvl == 1) { g_sunvis[fp] = vis; return; }
    float shade = 1.0f - sh_misc.w * (1.0f - vis);
    /* [CAR SUN] Overbright the sunlit car body (matid CARBODY=5). shade > 1 is a
     * BRIGHTEN under the unclamped MULT composite. vis keeps shadowed panels dark;
     * the (0.5 + 0.5*N·L) term lets sun-facing panels pop while the rest still
     * lifts a little. World/road (matid != 5) is left at shade <= 1. */
    /* Car sunlit BRIGHTENING is done additively in rgen_light (the mult composite
     * here clamps to [0,1] on the UNORM target, so it can only darken). */
    g_sunvis[fp] = shade;
}

/* ----- [P4] Sky-visibility GI (b1 ShadowCB layout, camera reconstruct) ------ *
 * Per pixel: reconstruct world+normal from depth/G-buffer (same as rgen_shadow),
 * cast K cosine-weighted hemisphere rays around the surface normal. A ray that
 * REACHES the sky (misses within TMax) = open; one that hits geometry = covered.
 * Outputs the FINAL multiplier lerp(floor,1,skyvis) into g_gi (R32F) so the MULT
 * composite (ps_shadow_rt) just multiplies -- outdoor pixels stay bright, pixels
 * under a bridge/tunnel darken toward the floor, from real geometry. Medium TMax
 * (a bridge deck occludes; a far mountain shouldn't). AO CB packs: K = params2.z,
 * TMax = sun.w, floor = misc.w. */
[shader("raygeneration")]
void rgen_ao()
{
    uint2 lpx = DispatchRaysIndex().xy;
    int2  fp  = int2((int)sh_misc.y, (int)sh_misc.z) + int2(lpx);
    float D = g_depth.Load(int3(fp, 0));
    float4 gb = g_gbuf.Load(int3(fp, 0));
    if (D >= 0.99999f || gb.a < 0.001f) { g_gi[fp] = 1.0f; return; }   /* sky = fully open */

    float3 world = rt_world_from_depth(D, float2(lpx), sh_camPosFocal, sh_rightCx, sh_upCy, sh_fwdDepthScale, sh_misc.x);
    float3 N = rt_gbuf_normal(gb);
    float dist = length(world - sh_camPosFocal.xyz);
    float3 origin = world + N * (16.0f + dist * 0.004f);   /* normal-offset bias */

    int   K      = max(1, (int)(sh_params2.z + 0.5f));
    float tmax   = sh_sun.w   > 1.0f    ? sh_sun.w   : 6000.0f;
    float floorv = sh_misc.w  > 0.0001f ? sh_misc.w  : 0.45f;

    /* [CAR PIXELATION FIX 2026-08-05] Same treatment as rgen_shadow: cars get a
     * FINER azimuth-jitter cell + more hemisphere rays so the sky-vis AO on the
     * curved body is fine grain the denoiser smooths, not coarse 16-unit blocks
     * that read as the grey blotches on the rear panels. Road/world unchanged. */
    int  rawA  = (int)(gb.a * 255.0f + 0.5f);
    bool isCar = (rawA & 0x40) != 0;
    float jitterCell = isCar ? RT_CAR_NOISE_CELL : RT_NOISE_CELL;
    int  Kc = isCar ? max(K, 6) : K;
    /* cosine-weighted hemisphere around N; stratified in the polar coord. */
    float3 up0 = abs(N.y) < 0.99f ? float3(0,1,0) : float3(1,0,0);
    float3 T = normalize(cross(up0, N));
    float3 Bv = cross(N, T);
    float visSum = 0.0f;
    for (int k = 0; k < Kc; k++) {
        float u1 = ((float)k + 0.5f) / (float)Kc;                      /* stratified radius */
        float u2 = rt_hash_world_cell(world, (float)(k + 1), jitterCell); /* [MOTION-STABLE] world-locked azimuth */
        float r  = sqrt(u1);
        float ang = 6.2831853f * u2;
        float3 dir = normalize(T * (r * cos(ang)) + Bv * (r * sin(ang)) + N * sqrt(saturate(1.0f - u1)));
        visSum += rt_shadow_ray(origin, dir, 1.0f, tmax);             /* 1 = reached sky   */
    }
    float skyvis = visSum / (float)Kc;
    g_gi[fp] = lerp(floorv, 1.0f, skyvis);
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
        if (Dc.w > -0.5f) {
            float cd = dot(-Ld, Dc.xyz);
            float outer = Dc.w;
            /* [RT2 P7] li_ext.w = cone softness (0 -> legacy hard-edge falloff).
             * >0 -> smooth projector beam: full-bright core, feathered rim. */
            float soft = li_ext.w;
            if (soft > 1e-4f) {
                float inner = outer + (1.0f - outer) * soft;
                cone = smoothstep(outer, inner, cd);
            } else {
                cone = saturate((cd - outer) / (1.0f - outer)); cone *= cone;
            }
        }
        float ndotl = hasN ? saturate(dot(N, Ld)) * 0.85f + 0.15f : 1.0f;
        float w = C.w * atten * cone * ndotl;
        if (w < 0.003f) continue;
        float3 origin = world + (hasN ? N : Ld) * (8.0f + d * 0.004f);
        /* [RT2 P7] K stratified shadow rays jittered around the light dir for a
         * soft penumbra (K = li_ext2.x; 0/absent -> 1 = legacy single ray). K=1
         * reproduces the old binary 1.0/0.15 step exactly (single ray -> vsum is
         * 0 or 1 -> lerp gives 0.15 or 1.0); K>1 averages -> graduated edges. */
        int Klr = (int)(li_ext2.x + 0.5f); if (Klr < 1) Klr = 1;
        float vsum;
        if (Klr <= 1) {
            vsum = rt_shadow_ray(origin, Ld, 1.0f, d - 4.0f);
        } else {
            float3 up0 = abs(Ld.y) < 0.99f ? float3(0,1,0) : float3(1,0,0);
            float3 T  = normalize(cross(up0, Ld));
            float3 Bv = cross(Ld, T);
            float baseA = rt_hash_world(world, 0.0f) * 6.2831853f;   /* [MOTION-STABLE] world-locked jitter */
            const float PEN = 0.02f;   /* penumbra half-angle jitter (source-size proxy) */
            vsum = 0.0f;
            for (int s = 0; s < Klr; s++) {
                float a = baseA + (6.2831853f * (float)s) / (float)Klr;
                float3 jd = normalize(Ld + (cos(a) * T + sin(a) * Bv) * PEN);
                vsum += rt_shadow_ray(origin, jd, 1.0f, d - 4.0f);
            }
            vsum /= (float)Klr;
        }
        accum += C.rgb * (w * lerp(0.15f, 1.0f, vsum));
    }

    /* [CAR SUN 2026-08-03] ADDITIVE sunlit boost for car bodywork (matid CARBODY=5).
     * The car luminance is capped at its texture (0xFF modulate) and the MULT sun
     * composite can only darken (UNORM clamp), so the only way to make the car read
     * BRIGHTER in open sun is to ADD light here (ps_light_rt is ONE/ONE). li_ext2.yzw
     * packs the (Y-flipped, reconstruction-frame) sun dir SCALED by the gain, so
     * gain = length and the unit dir = sg/gain (0 length => feature off). A sun
     * shadow ray gates it (shadowed panels get nothing); N·L makes the sun-facing
     * side pop. Warm-white tint. Non-car pixels are untouched (accum stays as-is). */
    /* [CAR SUN 2026-08-03] Sunlit-car boost. Stored in g_lightcol.a as a scalar
     * MULTIPLY factor (NOT added to the light rgb): the ps_car_sun_rt composite
     * blends dst*(1+boost), a HUE-PRESERVING brighten of the car's own paint
     * (dark blue -> brighter blue), which the earlier flat-additive term could
     * not do (it veiled toward white — no albedo in the G-buffer to tint by).
     * li_ext2.yzw packs the POSITION-space (+Y down, same frame as the G-buffer
     * normal) sun dir * gain, so gain = length, unit dir = sg/gain. Gated by
     * sun-facing N·L (a broad diffuse lift) plus a specular glint. Car only
     * (matid CARBODY=5). */
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

    /* [CAR SUN GLINT 2026-08-04] The car's per-texel matid is packed with bit
     * 0x40 set (ps_modulate_g); strip it for the reflectivity LUT and keep the
     * flag so the sun-specular glint below fires on the CAR ONLY. */
    int raw = (int)(gb.a * 255.0f + 0.5f);
    bool isCar = (raw & 0x40) != 0;
    int matid = raw & 0x3F;
    float base = rt_reflectivity(matid);
    float3 N = normalize(gb.rgb * 2.0f - 1.0f);
    if (matid == 1 && !isCar && -N.y > 0.6f) base += sr_misc.w * saturate((-N.y - 0.6f) / 0.4f);  /* wet road */
    /* [CAR SUN 2026-08-04] Give car paint & glass real gloss so they MIRROR the
     * bright sky + sun disc (miss_refl) — "sunlight reflected through the chassis
     * and windows". Body = moderate clearcoat, glass = strong mirror, lights matte. */
    if (isCar) {
        if (matid == 3)      base = max(base, 0.40f);   /* windows  */
        else if (matid != 4) base = max(base, 0.15f);   /* bodywork */
    }
    int dg = (int)(sr_params2.z + 0.5f);
    /* [CAR SUN GLINT diag] dg5: green where the G-buffer has geometry, +blue where
     * the pixel is a detected car (isCar). Confirms the car reaches the RT pass. */
    if (dg == 5) { g_reflcol[fp] = float4(0.0f, (gb.a > 0.001f) ? 1.0f : 0.0f, isCar ? 1.0f : 0.0f, 1.0f); return; }
    /* [CAR SUN GLINT diag] dg6: N.L vs sr_sun over ALL geometry — green=sun-facing
     * (N.L>0, can glint), red=facing away. Validates the sun vector sign. */
    if (dg == 6) { float3 Ls = float3(sr_sun.x, -sr_sun.y, sr_sun.z); float nl = dot(N, Ls); g_reflcol[fp] = float4(saturate(-nl), saturate(nl), 0.0f, (gb.a > 0.001f) ? 1.0f : 0.0f); return; }
    /* [P3 diag] dg1: R=base reflectivity, G=matid/8, B=up. */
    if (dg == 1) { g_reflcol[fp] = float4(base, (float)matid / 8.0f, saturate(-N.y), 1.0f); return; }
    if (base < 0.01f && !isCar && dg == 0) return;   /* early-out: most pixels don't reflect (perf save; car keeps going for the glint) */

    float3 world = rt_world_from_depth(D, float2(lpx), sr_camPosFocal, sr_rightCx, sr_upCy, sr_fwdDepthScale, sr_misc.x);
    float3 V = normalize(world - sr_camPosFocal.xyz);
    float ndv = saturate(dot(-V, N));
    float w = base * (0.25f + 0.75f * pow(1.0f - ndv, 3.0f)) * sr_params2.y;   /* Fresnel * intensity */
    /* [P3 diag] dg2: weight w as grayscale (params2.y intensity check). */
    if (dg == 2) { g_reflcol[fp] = float4(w, w, w, 1.0f); return; }
    if (w < 0.02f && !isCar && dg == 0) return;

    float dist = length(world - sr_camPosFocal.xyz);

    /* Environment reflection (unchanged): trace only when it actually contributes. */
    RayPayload pl; pl.color = float3(0.02f, 0.02f, 0.12f); pl.t = -1.0f;
    if (w >= 0.02f) {
        float3 R = reflect(V, N);
        RayDesc ray;
        ray.Origin = world + N * (16.0f + dist * 0.004f);
        ray.Direction = R; ray.TMin = 1.0f; ray.TMax = sr_params.y;   /* max reflect dist */
        TraceRay(g_tlas, RAY_FLAG_NONE, 0xFF, /*hitGroup*/0, /*mult*/0, /*miss*/1, ray, pl);
    } else {
        w = 0.0f;   /* no meaningful env reflection on this pixel */
    }
    /* [CAR SUN 2026-08-04] Even sky sheen on the whole car: glass/roof that reflect
     * the dark ground behind the car still read as glossy-sunlit (not dead black),
     * so the effect isn't confined to the sun-facing fenders. Lift the reflected
     * colour to a dim sky floor and guarantee a little reflection weight. */
    if (isCar) {
        pl.color = max(pl.color, float3(0.20f, 0.32f, 0.52f));
        w = max(w, 0.14f);
    }
    /* [P3 diag] dg3: raw reflected color. */
    if (dg == 3) { g_reflcol[fp] = float4(pl.color, 1.0f); return; }

    /* [CAR SUN GLINT 2026-08-04] Blinn-Phong sun highlight on car bodywork &
     * glass so the sun visibly reflects off the paint and windows. sr_sun.xyz is
     * the unit surface->sun dir (position space, +Y down — same frame as N);
     * sr_sun.w>0 enables a sun shadow ray so glints don't appear on self-shadowed
     * panels. Glass = sharp/strong mirror glint, body = broad/medium, lights
     * (matid 4) = none. Warm-white. Non-car pixels never enter here. */
    float3 glint = float3(0,0,0); float glintA = 0.0f;
    if (isCar && matid != 4 && sr_sun.w > 0.5f) {
        /* [Y-FRAME 2026-08-04] sr_sun is Y-flipped for the TLAS shadow ray
         * (td5_render_mesh.c:531), but the G-buffer normal N (and V) are +Y-down
         * world. Un-flip Y for the N.L / half-vector dots; keep sr_sun for the ray. */
        float3 Lshade = float3(sr_sun.x, -sr_sun.y, sr_sun.z);
        float3 Lray   = sr_sun.xyz;
        float ndl = dot(N, Lshade);
        if (ndl > 0.0f) {
            float3 so = world + N * (16.0f + dist * 0.004f);
            float sv = rt_shadow_ray(so, Lray, 1.0f, 60000.0f);
            if (sv > 0.0f) {
                float3 H = normalize(Lshade - V);      /* -V = surface->camera */
                float ndh = saturate(dot(N, H));
                /* sr_params2.w = TD5RE_RT_CAR_GLINT live multiplier (0/absent -> 1). */
                float gg = sr_params2.w > 0.0001f ? sr_params2.w : 1.0f;
                float shin = (matid == 3) ? 60.0f : 14.0f;          /* glass sharp, body soft-broad */
                float gint = ((matid == 3) ? 2.2f : 0.9f) * gg;     /* softer body (was streaky) */
                float spec = pow(ndh, shin) * gint * ndl * sv;
                spec = spec / (1.0f + 0.7f * spec);            /* soft highlight rolloff -> glow, not a hard streak */
                glint  = float3(1.0f, 0.95f, 0.85f) * spec;    /* pre-multiplied (rgb*specA) */
                glintA = saturate(spec);
            }
        }
    }

    /* Compose glint OVER env-reflection as a single non-premultiplied "over"
     * (the composite does out = rgb*a + dst*(1-a)). Reduces to the plain
     * reflection when glintA==0, and to a pure white spot when w==0. */
    float aOut = glintA + w * (1.0f - glintA);
    float3 cPremul = glint + pl.color * w * (1.0f - glintA);
    float3 cOut = cPremul / max(aOut, 1e-4f);
    if (aOut < 0.001f && dg == 0) return;
    g_reflcol[fp] = float4(cOut, aOut);
}
