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
RaytracingAccelerationStructure g_tlas     : register(t0);
Texture2D<float>                g_depth    : register(t1);  /* scene depth (R32F)     */
Texture2D<float4>               g_gbuf     : register(t2);  /* normal(rgb*2-1)+matid/255.a */
ByteAddressBuffer               g_vb       : register(t3);  /* P3 vertex pool (BackendRTVertex 24B) */
ByteAddressBuffer               g_ib       : register(t4);  /* P3 index pool (u16)    */

/* P3 GeoRecord: one per (mesh,range) -- byte offsets into the pools + texture +
 * material. InstanceID() = the mesh's first GeoRecord index. */
struct GeoRecord { uint vb_byte_off; uint ib_byte_off; uint texture_index; uint matid; };
StructuredBuffer<GeoRecord>     g_geo      : register(t5);

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
    float4 sh_params2;        /* x paneH                     */
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
    float4 li_ext;            /* x occlSteps, y paneW, z paneH      */
    float4 li_lights[RT_LIGHT_MAX * 3];  /* k*3+0 pos+range, +1 rgb+intensity, +2 dir+coneCos */
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
};

/* Reflection / debug ray payload (<= 32 bytes). */
struct RayPayload { float3 color; float t; };
/* Shadow ray payload: miss_shadow sets visible=1; a hit leaves it 0. */
struct ShadowPayload { uint visible; };

/* World position from scene depth D at pane-local pixel `pp`, using the shared
 * camera reconstruction (matches ps_shadow.hlsl / ps_light.hlsl exactly). */
float3 rt_world_from_depth(float D, float2 pp,
                           float4 camPosFocal, float4 rightCx, float4 upCy,
                           float4 fwdDepthScale, float depthBias)
{
    float focal = camPosFocal.w;
    float vz = D * fwdDepthScale.w + depthBias;
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

/* Cast an occlusion (shadow) ray; returns 1 if UNBLOCKED (visible), 0 if hit.
 * ACCEPT_FIRST_HIT + SKIP_CLOSEST_HIT so no CH runs; miss index 0 (miss_shadow). */
float rt_shadow_ray(float3 origin, float3 dir, float tmin, float tmax)
{
    RayDesc ray; ray.Origin = origin; ray.Direction = dir; ray.TMin = tmin; ray.TMax = tmax;
    ShadowPayload p; p.visible = 0;
    TraceRay(g_tlas,
             RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
             0xFF, /*hitGroup*/0, /*mult*/0, /*miss*/0, ray, p);
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
