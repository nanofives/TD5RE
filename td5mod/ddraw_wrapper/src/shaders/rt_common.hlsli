/**
 * rt_common.hlsli -- shared HLSL for the DXR pipeline. Resources (global root
 * signature descriptor table 0), constant buffers, payloads, and reconstruction
 * helpers. All coordinates are game world space in FLOAT (24.8 / 256.0, +Y down);
 * the ShadowCB/LightCB layouts MIRROR the C structs in td5_wrapper_backend.h.
 */
#ifndef RT_COMMON_HLSLI
#define RT_COMMON_HLSLI

/* ---- Fixed descriptor table 0 (global root signature) --------------------- *
 * UAV range u0-u2 (heap slots 0..2) + SRV range t0-t2 (heap slots 3..5). */
RWTexture2D<float4>             g_output   : register(u0);  /* debug/smoke gradient   */
RWTexture2D<float>              g_sunvis   : register(u1);  /* P2b sun shade (1=lit)  */
RWTexture2D<float4>             g_lightcol : register(u2);  /* P2b additive light rgb */
RWTexture2D<float4>             g_reflcol  : register(u3);  /* P3 reflection rgb + weight.a */
RWTexture2D<float>              g_gi       : register(u4);  /* P4 sky-visibility (final multiplier) */
RaytracingAccelerationStructure g_tlas     : register(t0);
Texture2D<float>                g_depth    : register(t1);  /* scene depth (R32F)     */
Texture2D<float4>               g_gbuf     : register(t2);  /* normal(rgb*2-1)+matid/255.a */
ByteAddressBuffer               g_vb       : register(t3);  /* P3 vertex pool (BackendRTVertex 24B) */
ByteAddressBuffer               g_ib       : register(t4);  /* P3 index pool (u16)    */

/* P3 GeoRecord: one per (mesh,range) -- byte offsets into the pools + texture +
 * material. InstanceID() = the mesh's first GeoRecord index. */
struct GeoRecord { uint vb_byte_off; uint ib_byte_off; uint texture_index; uint matid; };
StructuredBuffer<GeoRecord>     g_geo      : register(t5);

/* P3 bindless per-page textures (classic unbounded range, NOT SM6.6 heap). One
 * SRV per texture page id at g_bindless[page_id]; index 0 = "no texture" (track
 * lane quads, which have no UV) -> chit keeps vertex colour. Unregistered ids
 * resolve to the 1x1 fallback (safe). Separate root table param, register space 1. */
Texture2D<float4>               g_bindless[] : register(t0, space1);
SamplerState                    g_samp     : register(s0);   /* static LINEAR wrap */

/* ---- b0: debug primary-ray view CB (Phase 1) ------------------------------ */
cbuffer RTViewCB : register(b0)
{
    float3 g_camPos;    float g_focal;
    float3 g_right;     float g_centerX;
    float3 g_up;        float g_centerY;
    float3 g_fwd;       float g_rayTMin;
    float3 g_sunDir;    float g_rayTMax;
    float2 g_paneOrigin;
    float2 g_paneSize;
};

/* ---- b1: ShadowCB (mirror of C ShadowCB, td5_wrapper_backend.h) ------------ */
cbuffer ShadowCB : register(b1)
{
    float4 sh_camPosFocal;    /* xyz cam, w focal            */
    float4 sh_rightCx;        /* xyz right, w centerX        */
    float4 sh_upCy;           /* xyz up, w centerY           */
    float4 sh_fwdDepthScale;  /* xyz fwd, w depthScale(195000) */
    float4 sh_misc;           /* x depthBias, y vpX, z vpY, w strength */
    float4 sh_sun;            /* xyz surface->light dir, w max dist  */
    float4 sh_params;         /* x steps, y thickness, z startOff, w paneW */
    float4 sh_params2;        /* x paneH, y biasScale, z RAYS, w coneScale */
};

/* ---- b2: LightCB (mirror of C LightCB) ------------------------------------ */
#define RT_LIGHT_MAX 32
cbuffer LightCB : register(b2)
{
    float4 li_camPosFocal;
    float4 li_rightCx;
    float4 li_upCy;
    float4 li_fwdDepthScale;
    float4 li_misc;           /* x depthBias, y count, z vpX, w vpY */
    float4 li_ext;            /* x occlSteps, y paneW, z paneH, w coneSoft [RT2 P7] */
    float4 li_lights[RT_LIGHT_MAX * 3];  /* k*3+0 pos+range, +1 rgb+intensity, +2 dir+coneCos */
    float4 li_ext2;           /* [RT2 P7] x = light shadow-ray samples K (soft penumbra) */
};

/* ---- b3: SSRCB (mirror of C SSRCB) ---------------------------------------- */
cbuffer SSRCB : register(b3)
{
    float4 sr_camPosFocal;
    float4 sr_rightCx;
    float4 sr_upCy;
    float4 sr_fwdDepthScale;
    float4 sr_misc;      /* x depthBias, y vpX, z vpY, w wet-road boost */
    float4 sr_params;    /* x steps, y maxDist, z thickness, w paneW    */
    float4 sr_params2;   /* x paneH, y intensity                        */
    float4 sr_reflA;     /* reflectivity matid 0-3                      */
    float4 sr_reflB;     /* reflectivity matid 4-7                      */
    float4 sr_sun;       /* xyz surface->light dir (unit); w shadow-ray enable */
};

/* Reflection / debug ray payload (<= 32 bytes). */
struct RayPayload { float3 color; float t; };
/* Shadow ray payload: miss_shadow sets visible=1; a hit leaves it 0. */
struct ShadowPayload { uint visible; };

/* [PERSPECTIVE DEPTH 2026-08-03] Inverse of the CPU td5_depth_persp(): recover
 * linear view-Z from the perspective-correct normalized depth. depthScale = RANGE
 * (far-near, 195000), depthBias = NEAR (64), A = far/range = (near+range)/range.
 * Exact everywhere now that the stored depth is screen-linear (the old
 * D*scale+bias assumed a linear-view-Z depth that was NOT screen-linear, so it
 * mis-reconstructed triangle interiors -> the per-span bouncy shadow edge). */
float depth_to_viewz(float d, float depthScale, float depthBias)
{
    float A = (depthBias + depthScale) / depthScale;
    return depthBias * A / (A - d);
}

/* World position from scene depth D at pane-local pixel `pp`, using the shared
 * camera reconstruction (matches ps_shadow.hlsl / ps_light.hlsl exactly). */
float3 rt_world_from_depth(float D, float2 pp,
                           float4 camPosFocal, float4 rightCx, float4 upCy,
                           float4 fwdDepthScale, float depthBias)
{
    float focal = camPosFocal.w;
    float vz = depth_to_viewz(D, fwdDepthScale.w, depthBias);
    float vx = -(pp.x - rightCx.w) * vz / focal;
    float vy = -(pp.y - upCy.w)    * vz / focal;
    return camPosFocal.xyz + vx * rightCx.xyz + vy * upCy.xyz + vz * fwdDepthScale.xyz;
}

/* Decode the G-buffer world normal (rgb biased 0..1 -> -1..1). a = matid/255. */
float3 rt_gbuf_normal(float4 g) { return normalize(g.rgb * 2.0f - 1.0f); }

/* Cheap per-pixel hash for shadow-ray cone jitter (no temporal accumulation). */
float rt_hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.x + p3.y) * p3.z);
}

float rt_hash13(float3 p)
{
    float3 p3 = frac(p * 0.1031f);
    p3 += dot(p3, p3.zyx + 31.32f);
    return frac((p3.x + p3.y) * p3.z);
}

/* [MOTION-STABLE JITTER] Hashing the SCREEN pixel (rt_hash12(fp)) locks the ray
 * jitter to the screen, so as the camera moves a surface point slides across the
 * fixed noise field and its shadow/GI value flickers frame-to-frame -> a crawling
 * pattern only visible IN MOTION (invisible in a still frame). Hashing a
 * grid-quantized WORLD position instead makes the jitter stick to the surface:
 * a given point keeps its value as the camera moves, so the temporal crawl goes
 * away. RT_NOISE_CELL trades stability (bigger = steadier, blockier) vs per-pixel
 * variety (smaller = finer, but distant depth precision can re-introduce flicker);
 * the à-trous denoiser then smooths whatever spatial blockiness remains. k
 * decorrelates multiple samples per pixel. */
#ifndef RT_NOISE_CELL
#define RT_NOISE_CELL 16.0f
#endif
/* [CAR PIXELATION FIX 2026-08-05] A car is a small, curved object; the 16-unit
 * world cell projects to coarse blocks across the bodywork, so the self-shadow
 * penumbra (heavy under the wide OVERCAST cone) reads as a blocky/pixelated
 * pattern on the rear panels. Use a much finer cell on CAR pixels so the jitter
 * becomes fine grain the à-trous denoiser can smooth into a clean penumbra,
 * instead of large stable blocks it can't. Road/world keep the coarse cell for
 * distant depth-precision stability. */
#ifndef RT_CAR_NOISE_CELL
#define RT_CAR_NOISE_CELL 2.0f
#endif
/* [CAR OVER-LIGHT FIX 2026-08-11] The dynamic point-light pass composites
 * ADDITIVELY with no roll-off, so a close warm lamp (e.g. the Keswick tunnel
 * lamps) drives every channel of a car body to 1 -> the car reads pure WHITE
 * instead of warmly lit. Soft-cap the car's accumulated point-light magnitude
 * to this ceiling while preserving its COLOUR RATIO, so a bright lamp gives a
 * warm highlight (dark blue -> warmer blue) rather than blowing to white.
 * Non-car pixels (road/walls) are untouched. */
#ifndef RT_CAR_LIGHT_SOFT
#define RT_CAR_LIGHT_SOFT 0.70f
#endif
float rt_hash_world_cell(float3 world, float k, float cell)
{
    float3 c = floor(world / cell) + k * float3(1.7f, 2.3f, 3.1f);
    return rt_hash13(c);
}
float rt_hash_world(float3 world, float k)
{
    return rt_hash_world_cell(world, k, RT_NOISE_CELL);
}

/* Cast an occlusion (shadow) ray; returns 1 if UNBLOCKED (visible), 0 if hit.
 * ACCEPT_FIRST_HIT + SKIP_CLOSEST_HIT so no CH runs; miss index 0 (miss_shadow).
 * [ROAD-CAST FIX 2026-08-03] InstanceInclusionMask 0x01 = sun-shadow CASTERS only.
 * The flat synthetic road lane quads are fed with bit 0 cleared (0xFE) so they
 * never self-shadow (the per-span near-camera stripe acne); walls/buildings/props/
 * cars keep bit 0 set (0xFF) and cast normally. Reflection/primary rays still
 * trace 0xFF and see the road. */
float rt_shadow_ray(float3 origin, float3 dir, float tmin, float tmax)
{
    RayDesc ray; ray.Origin = origin; ray.Direction = dir; ray.TMin = tmin; ray.TMax = tmax;
    ShadowPayload p; p.visible = 0;
    /* [ROAD-CAST diag 2026-08-03] back-face cull (now that the shader actually
     * recompiles): if the per-span wall stripes are self-shadow acne (ray clips
     * the back of the wall's own triangle), this removes them. */
    TraceRay(g_tlas,
             RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
             | RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
             0x01, /*hitGroup*/0, /*mult*/0, /*miss*/0, ray, p);
    return (float)p.visible;
}

/* ---- P3 vertex fetch (BackendRTVertex: pos@0, uv@12, color@20; 24 bytes) --- */
uint3 rt_load_tri_indices(uint ib_byte_off, uint prim)
{
    uint b = ib_byte_off + prim * 6;            /* 3 u16 indices             */
    uint a = b & ~3u;                           /* 4-byte-aligned dword base */
    uint2 raw = uint2(g_ib.Load(a), g_ib.Load(a + 4));
    if ((b - a) == 0u) return uint3(raw.x & 0xffffu, raw.x >> 16, raw.y & 0xffffu);
    else               return uint3(raw.x >> 16,     raw.y & 0xffffu, raw.y >> 16);
}
float3 rt_vertex_pos(uint vb_byte_off, uint idx)   { return asfloat(g_vb.Load3(vb_byte_off + idx * 24u + 0u)); }
float2 rt_vertex_uv(uint vb_byte_off, uint idx)    { return asfloat(g_vb.Load2(vb_byte_off + idx * 24u + 12u)); }
float3 rt_vertex_color(uint vb_byte_off, uint idx)             /* packed BGRA -> rgb */
{
    uint c = g_vb.Load(vb_byte_off + idx * 24u + 20u);
    return float3((c >> 16) & 0xffu, (c >> 8) & 0xffu, c & 0xffu) / 255.0f;
}

/* Base reflectivity for a material id from the SSR LUT (matid 0-3 reflA, 4-7 reflB). */
float rt_reflectivity(int matid)
{
    if (matid < 4) return sr_reflA[matid];
    if (matid < 8) return sr_reflB[matid - 4];
    return 0.0f;
}

/* Build the world-space primary ray direction for a pane-local pixel (debug). */
float3 rt_primary_ray_dir(float sx, float sy)
{
    float inv_f = 1.0f / g_focal;
    float vx = -(sx - g_centerX) * inv_f;
    float vy = -(sy - g_centerY) * inv_f;
    return normalize(vx * g_right + vy * g_up + 1.0f * g_fwd);
}

#endif /* RT_COMMON_HLSLI */
