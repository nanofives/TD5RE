/**
 * rt_common.hlsli -- shared HLSL declarations for the DXR pipeline. The per-
 * frame RT view constant buffer (b0) and ray payloads are FROZEN here and
 * mirrored byte-for-byte by RTViewCB in d3d12_dxr.c. All coordinates are game
 * world space in FLOAT (24.8 / 256.0, +Y down). See RT_LIGHTING_PLAN.md.
 */
#ifndef RT_COMMON_HLSLI
#define RT_COMMON_HLSLI

/* Per-dispatch view constants (root CBV b0). 16-byte-aligned rows; the C mirror
 * RTViewCB must match exactly (float4 per row). */
cbuffer RTViewCB : register(b0)
{
    float3 g_camPos;    float g_focal;      /* camera world pos; raster focal    */
    float3 g_right;     float g_centerX;    /* basis row0 (right); pane center x  */
    float3 g_up;        float g_centerY;    /* basis row1 (up);    pane center y  */
    float3 g_fwd;       float g_rayTMin;    /* basis row2 (fwd);   ray tmin       */
    float3 g_sunDir;    float g_rayTMax;    /* sun world dir;      ray tmax       */
    float2 g_paneOrigin;                    /* pane top-left in full-frame pixels */
    float2 g_paneSize;                      /* pane width/height (pixels)         */
};

/* Reflection / debug ray payload (<= 32 bytes). */
struct RayPayload {
    float3 color;   /* hit/miss shaded color   */
    float  t;       /* hit distance (-1 = miss)*/
};

/* Shadow ray payload (Phase 2b). Miss sets visible=1; a hit leaves it 0. */
struct ShadowPayload {
    uint visible;
};

/* Build the world-space primary ray direction for a pane-local pixel, matching
 * the raster projection in debug_line_project (view = basis*(world-cam);
 * screen = -v.xy*focal/v.z + center). Inverse: v = (-(sx-cx)/f, -(sy-cy)/f, 1),
 * world dir = v.x*right + v.y*up + v.z*fwd. */
float3 rt_primary_ray_dir(float sx, float sy)
{
    float inv_f = 1.0f / g_focal;
    float vx = -(sx - g_centerX) * inv_f;
    float vy = -(sy - g_centerY) * inv_f;
    float vz = 1.0f;
    return normalize(vx * g_right + vy * g_up + vz * g_fwd);
}

#endif /* RT_COMMON_HLSLI */
