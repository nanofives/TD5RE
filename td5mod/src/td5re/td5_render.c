/**
 * td5_render.c -- Scene setup, mesh transform, frustum cull
 *
 * Software T&L pipeline: all vertex transformation, lighting, clipping, and
 * projection are CPU-side. GPU is used only for final rasterization via
 * DrawPrimitive (pre-transformed vertices, FVF 0x1C4).
 *
 * Wraps the D3D11 backend via td5_platform.h render calls.
 *
 * Original addresses (see td5_render.h for full list):
 *   0x40AE10  InitializeRaceRenderGlobals
 *   0x40AE80  InitializeRaceRenderState
 *   0x40AEC0  ReleaseRaceRenderResources
 *   0x40ADE0  BeginRaceScene
 *   0x40AE00  EndRaceScene
 *   0x43DA80  LoadRenderRotationMatrix
 *   0x43DC20  LoadRenderTranslation
 *   0x43DAF0  PushRenderTransform
 *   0x43DB70  PopRenderTransform
 *   0x43DC50  TransformVec3ByRenderMatrixFull
 *   0x43DD60  TransformMeshVerticesToView
 *   0x43DDF0  ComputeMeshVertexLighting
 *   0x42DCA0  IsBoundingSphereVisibleInCurrentFrustum
 *   0x42DE10  TestMeshAgainstViewFrustum
 *   0x43DCB0  TransformAndQueueTranslucentMesh
 *   0x4314B0  RenderPreparedMeshResource
 *   0x4317F0  ClipAndSubmitProjectedPolygon
 *   0x4312E0  InitializeTranslucentPrimitivePipeline
 *   0x431460  QueueTranslucentPrimitiveBatch
 *   0x431340  FlushQueuedTranslucentPrimitives
 *   0x4329E0  FlushImmediateDrawPrimitiveBatch
 *   0x43E3B0  InsertBillboardIntoDepthSortBuckets
 *   0x43E550  QueueProjectedPrimitiveBucketEntry
 *   0x43E2F0  FlushProjectedPrimitiveBuckets
 *   0x43E7E0  ConfigureProjectionForViewport
 */

#include "td5_render.h"
#include "td5_camera.h"
#include "td5_platform.h"
#include "td5_track.h"
#include "td5_game.h"
#include "td5_asset.h"
#include "td5_save.h"
#include "td5re.h"

#include "../../../re/include/td5_actor_struct.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <string.h>

extern uint32_t g_tick_counter;

/* ========================================================================
 * Constants
 * ======================================================================== */

#define LOG_TAG             "render"
#define RENDER_LOG_TAG      LOG_TAG

/** Transform stack depth (hierarchical models) */
#define TRANSFORM_STACK_MAX 8

/** Immediate draw buffer limits */
#define IMMEDIATE_MAX_VERTS   1024
#define IMMEDIATE_MAX_INDICES 4096

/* Forward declarations for trig and matrix builders (defined later in this file) */
float CosFloat12bit(unsigned int angle);
float SinFloat12bit(int angle);
void BuildRotationMatrixFromAngles(float *out, short *angles);

/** Near/far clip defaults.
 * Original depth normalization (0x00473bcc): depth = (vz - 64) / 65472, far = 65536.
 * Original frustum far-cull (0x0042D48E): round(3.0 * 65000) = 195000.
 * Source port uses far_cull as depth range so all geometry within frustum maps to [0,1]. */
#define DEFAULT_NEAR_CLIP   1.0f
#define DEFAULT_FAR_CLIP    195000.0f
#define DEFAULT_FAR_CULL    195000.0f

/** Billboard depth sort stride sizes (bytes) */
#define BILLBOARD_TRI_STRIDE  0x84
#define BILLBOARD_QUAD_STRIDE 0xB0

/** Backface cull threshold for vertex normals */
#define BACKFACE_THRESHOLD  0.03662f

/** Fog defaults (from M2DX analysis) */
#define FOG_START_DEFAULT   0.60f
#define FOG_END_DEFAULT     1.00f
#define FOG_DENSITY_DEFAULT 0.40f

/* ========================================================================
 * Render Transform (3x4 matrix, 8-byte aligned)
 *
 * Layout: m[0..8] = 3x3 rotation (row-major), m[9..11] = translation
 * Original: DAT_004bf6b8 (active), 0x4C36C8 (backup)
 * ======================================================================== */

static TD5_Mat3x4 s_render_transform;

/** Transform stack: 8-deep push/pop for hierarchical models */
static TD5_Mat3x4 s_transform_stack[TRANSFORM_STACK_MAX];
static int         s_transform_stack_depth;

/* ========================================================================
 * Projection Parameters
 *
 * Original globals:
 *   _DAT_004c3718 = focal_length (width * 0.5625)
 *   _DAT_004c371c = inv_focal (1.0 / focal)
 *   _DAT_004ab0b0 = frustum_h_cos
 *   _DAT_004ab0b8 = frustum_h_sin
 *   _DAT_004ab0a4 = frustum_v_cos
 *   _DAT_004ab0a8 = frustum_v_sin
 * ======================================================================== */

static float s_focal_length;
static float s_inv_focal;
static float s_frustum_h_cos, s_frustum_h_sin;
static float s_frustum_v_cos, s_frustum_v_sin;
static float s_near_clip;
static float s_far_clip;       /* depth normalization far distance (65536) */
static float s_far_cull;       /* frustum rejection far distance  (195000) */
static float s_center_x, s_center_y;
static int   s_viewport_width, s_viewport_height;

/* ========================================================================
 * Camera Basis (set externally by td5_camera module)
 *
 * Original: DAT_004aafa0..b4 = right/up/forward (3x3)
 *           DAT_004aafc4..cc = camera world position
 * ======================================================================== */

static float s_camera_basis[9];      /* 3x3 row-major: right, up, forward */
static float s_camera_pos[3];        /* world-space camera position */

/* ========================================================================
 * Lighting State
 *
 * 3 directional lights (9 floats total) + ambient intensity.
 * Original: DAT_004ab0d0..f0 = light directions, DAT_004bf6a8 = ambient
 * ======================================================================== */

static float s_light_dirs[9];        /* 3 light direction vectors (3 floats each) */
static float s_ambient_intensity;    /* base ambient [0..255] range */

/* ========================================================================
 * Fog State
 *
 * Original: d3d_exref+0x18 = color, d3d_exref+0xa34 = enable flag
 * ======================================================================== */

static uint32_t s_fog_color;
static int      s_fog_enabled;

/* ========================================================================
 * Texture Cache
 *
 * LRU texture page cache with per-page age tracking.
 * Original: DAT_0048dc40 = control block, 8 bytes per slot descriptor
 * ======================================================================== */

#define TEXTURE_CACHE_SLOTS TD5_MAX_TEXTURE_CACHE_SLOTS

typedef struct TextureCacheSlot {
    int      page_id;        /* texture page ID (-1 = empty) */
    uint8_t  status;         /* status byte */
    uint8_t  age;            /* frames since last use (saturates at 0xFF) */
    uint8_t  used_this_frame;/* set when bound during current frame */
    uint8_t  _pad;
} TextureCacheSlot;

static TextureCacheSlot s_texture_cache[TEXTURE_CACHE_SLOTS];
static int              s_texture_cache_active_count;
static int              s_current_texture_page;   /* DAT_0048da00 */
static int              s_previous_texture_page;  /* DAT_0048da04 */

/* ========================================================================
 * Translucent Primitive Pipeline
 *
 * Linked-list pool of 512 entries (510 usable + 2 sentinels).
 * Sorted by texture page (sort key at record+0x02).
 *
 * Original: HeapAllocTracked(0x1000) = 4096 bytes = 512 x 8 bytes
 *           DAT_004af270 = active batch count
 * ======================================================================== */

typedef struct TranslucentBatchEntry {
    uint16_t flags;
    uint16_t sort_key;         /* texture page ID for ordering */
    void    *record;           /* pointer to primitive command record */
    int      next;             /* index of next entry (-1 = end) */
} TranslucentBatchEntry;

#define TRANSLUCENT_POOL_SIZE 512
#define TRANSLUCENT_MAX_ACTIVE (TRANSLUCENT_POOL_SIZE - 2) /* 510 usable */

static TranslucentBatchEntry s_translucent_pool[TRANSLUCENT_POOL_SIZE];
static int                   s_translucent_head;       /* head of sorted list */
static int                   s_translucent_free;       /* head of free list */
static int                   s_translucent_count;      /* active count */

/* Luminance-to-ARGB color LUT (1024 entries).
 * Original: DAT_004aee68, init as i * 0x10101 - 0x1000000 */
static uint32_t s_color_lut[1024];

/* ========================================================================
 * Depth-Sorted Projected Primitive Pipeline (4096 Buckets)
 *
 * Back-to-front rendering via bucket sort on inverse Z.
 * Original: DAT_004bf6c8
 * ======================================================================== */

typedef struct DepthBucketEntry {
    int      next;             /* index to next entry in bucket (-1 = end) */
    void    *prim_data;        /* pointer to primitive vertex data */
    uint32_t flags;            /* 0x3=tri, 0x4=quad, bit31=projected variant */
    int      texture_page;     /* texture page ID */
} DepthBucketEntry;

#define DEPTH_BUCKET_COUNT TD5_DEPTH_SORT_BUCKETS  /* 4096 */
#define DEPTH_ENTRY_POOL   4096

static int              s_depth_buckets[DEPTH_BUCKET_COUNT]; /* head index per bucket */
static DepthBucketEntry s_depth_entries[DEPTH_ENTRY_POOL];
static int              s_depth_entry_count;                 /* next free entry */

/* ========================================================================
 * Immediate Draw Buffers
 *
 * Accumulated pre-transformed vertices/indices for batched DrawPrimitive.
 * Original: DAT_004afb14 (vertex buf), DAT_004af314 (index buf)
 * ======================================================================== */

static TD5_D3DVertex s_imm_verts[IMMEDIATE_MAX_VERTS];
static uint16_t      s_imm_indices[IMMEDIATE_MAX_INDICES];
static int           s_imm_vert_count;   /* DAT_004afb4c */
static int           s_imm_index_count;  /* DAT_004afb50 */

/* ========================================================================
 * Sky Rotation (12-bit fixed angle, +0x400 per tick)
 *
 * Original: DAT_004bf500
 * ======================================================================== */

static int32_t s_sky_rotation_angle;

/* ========================================================================
 * Billboard Animation State
 * ======================================================================== */

static int32_t s_billboard_anim_phase;

/* ========================================================================
 * Race render initialized flags
 * ======================================================================== */

static int s_globals_initialized;   /* DAT_0048dba8 */
static int s_state_active;          /* DAT_0048dba0 */
static int s_scene_has_renderer_geometry;
static int s_debug_fallback_log_count;
static int s_debug_clip_log_count;
static int s_debug_clip_near_rejects;
static int s_debug_clip_backface_rejects;
static int s_debug_clip_screen_rejects;
static int s_debug_clip_emitted_tris;
static int s_debug_prepared_mesh_calls;
static int s_debug_append_calls;
static int s_debug_flush_calls;
static int s_debug_flush_submitted_tris;
static int s_debug_texture_bind_calls;
static int s_debug_texture_cache_hits;
static int s_debug_texture_cache_misses;
static int s_debug_texture_cache_evictions;
static int s_debug_scene_draw_calls;
static int s_debug_span_meshes_submitted;
static TD5_MeshHeader *s_vehicle_meshes[TD5_ACTOR_MAX_TOTAL_SLOTS];

/* ========================================================================
 * Forward Declarations (dispatch handlers)
 * ======================================================================== */

static void dispatch_tristrip(TD5_PrimitiveCmd *cmd, TD5_MeshVertex *base_verts);
static void dispatch_projected_tri(TD5_PrimitiveCmd *cmd, TD5_MeshVertex *base_verts);
static void dispatch_projected_quad(TD5_PrimitiveCmd *cmd, TD5_MeshVertex *base_verts);
static void dispatch_billboard(TD5_PrimitiveCmd *cmd, TD5_MeshVertex *base_verts);
static void dispatch_tristrip_direct(TD5_PrimitiveCmd *cmd, TD5_MeshVertex *base_verts);
static void dispatch_quad_direct(TD5_PrimitiveCmd *cmd, TD5_MeshVertex *base_verts);

/* Vehicle shadow + wheel billboard rendering */
static void render_vehicle_shadow_quad(void);
static void render_vehicle_wheel_billboards(TD5_Actor *actor, int slot);

/** 7-entry dispatch table matching original at 0x473b9c */
typedef void (*PrimDispatchFn)(TD5_PrimitiveCmd *cmd, TD5_MeshVertex *base_verts);

static const PrimDispatchFn s_dispatch_table[7] = {
    dispatch_tristrip,          /* 0: EmitTranslucentTriangleStrip */
    dispatch_tristrip,          /* 1: EmitTranslucentTriangleStrip (duplicate) */
    dispatch_projected_tri,     /* 2: SubmitProjectedTrianglePrimitive */
    dispatch_projected_quad,    /* 3: SubmitProjectedQuadPrimitive */
    dispatch_billboard,         /* 4: InsertBillboardIntoDepthSortBuckets */
    dispatch_tristrip_direct,   /* 5: EmitTranslucentTriangleStripDirect */
    dispatch_quad_direct,       /* 6: EmitTranslucentQuadDirect */
};

/* ========================================================================
 * Internal Helpers
 * ======================================================================== */

/**
 * Apply 3x3 rotation + translation (3x4 matrix multiply) to a point.
 *
 *   out = M_rot * in + M_trans
 *
 * M layout: m[0..8] = 3x3 rotation (row-major), m[9..11] = translation
 */
static void mat3x4_transform_point(const float *m, const float *in, float *out)
{
    out[0] = in[0] * m[0] + in[1] * m[1] + in[2] * m[2] + m[9];
    out[1] = in[0] * m[3] + in[1] * m[4] + in[2] * m[5] + m[10];
    out[2] = in[0] * m[6] + in[1] * m[7] + in[2] * m[8] + m[11];
}

/**
 * Apply 3x3 rotation only (no translation) -- for direction vectors/normals.
 */
static void mat3x3_transform_dir(const float *m, const float *in, float *out)
{
    out[0] = in[0] * m[0] + in[1] * m[1] + in[2] * m[2];
    out[1] = in[0] * m[3] + in[1] * m[4] + in[2] * m[5];
    out[2] = in[0] * m[6] + in[1] * m[7] + in[2] * m[8];
}

static void mat3x3_mul(const float *a, const float *b, float *out)
{
    out[0] = a[0] * b[0] + a[1] * b[3] + a[2] * b[6];
    out[1] = a[0] * b[1] + a[1] * b[4] + a[2] * b[7];
    out[2] = a[0] * b[2] + a[1] * b[5] + a[2] * b[8];
    out[3] = a[3] * b[0] + a[4] * b[3] + a[5] * b[6];
    out[4] = a[3] * b[1] + a[4] * b[4] + a[5] * b[7];
    out[5] = a[3] * b[2] + a[4] * b[5] + a[5] * b[8];
    out[6] = a[6] * b[0] + a[7] * b[3] + a[8] * b[6];
    out[7] = a[6] * b[1] + a[7] * b[4] + a[8] * b[7];
    out[8] = a[6] * b[2] + a[7] * b[5] + a[8] * b[8];
}

/**
 * Perspective project a view-space vertex to screen coordinates.
 * Returns 0 if vertex is behind near clip plane (z <= near).
 */
static int project_vertex(float vx, float vy, float vz,
                          float *sx, float *sy, float *sz, float *rhw)
{
    if (vz <= s_near_clip) return 0;

    float inv_z = 1.0f / vz;
    *sx  = vx * s_focal_length * inv_z + s_center_x;
    *sy  = vy * s_focal_length * inv_z + s_center_y;
    *sz  = vz * (1.0f / s_far_clip);  /* normalized depth [0..1] (0x00473bcc) */
    *rhw = inv_z;
    return 1;
}

/**
 * Clamp an integer to [lo, hi].
 */
static int clampi(int x, int lo, int hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

/**
 * Clamp a float to [lo, hi].
 */
static float clampf(float x, float lo, float hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

/* Forward decl: defined alongside td5_render_bind_texture_page below.
 * Switches the current render preset based on the bound page's
 * transparency type byte (0/1/2/3 → opaque/alpha/alpha/additive). */
static void td5_render_apply_page_blend_preset(int page_id);

/**
 * Flush immediate vertex/index buffers to GPU via platform DrawPrimitive.
 * Corresponds to FlushImmediateDrawPrimitiveBatch (0x4329e0).
 */
static void flush_immediate_internal(void)
{
    if (s_imm_vert_count <= 0 || s_imm_index_count <= 0) return;
    s_debug_flush_calls++;
    s_debug_flush_submitted_tris += s_imm_index_count / 3;
    s_debug_scene_draw_calls++;

    /* Color fixup: remap luminance index in low byte through color LUT */
    for (int i = 0; i < s_imm_vert_count; i++) {
        if ((s_imm_verts[i].diffuse & 0xFF000000u) == 0) {
            uint32_t lum = s_imm_verts[i].diffuse & 0x3FF;
            s_imm_verts[i].diffuse = s_color_lut[lum];
        }
    }

    /* Bind current texture and apply per-page blend preset.
     * The actual render path bypasses td5_render_bind_texture_page (the
     * cmd handlers funnel through clip_and_submit_polygon → here), so the
     * preset hook lives at the flush site to mirror BindRaceTexturePage @
     * 0x40B660. Type 3 (additive) is the streetlight/glow path. */
    if (s_current_texture_page >= 0) {
        td5_render_apply_page_blend_preset(s_current_texture_page);
        td5_plat_render_bind_texture(s_current_texture_page);
    }

    /* Submit triangles */
    s_scene_has_renderer_geometry = 1;
    td5_plat_render_draw_tris(s_imm_verts, s_imm_vert_count,
                              s_imm_indices, s_imm_index_count);

    /* Reset batch */
    s_imm_vert_count  = 0;
    s_imm_index_count = 0;
    s_current_texture_page = s_previous_texture_page;
}

static void update_render_camera_from_game(void)
{
    td5_camera_get_basis(&s_camera_basis[0], &s_camera_basis[3], &s_camera_basis[6]);
    td5_camera_get_position(&s_camera_pos[0], &s_camera_pos[1], &s_camera_pos[2]);
}

/**
 * Append a projected triangle to the immediate draw buffer.
 * If the buffer is full, flush first.
 */
static void append_projected_triangle(const TD5_D3DVertex *v0,
                                      const TD5_D3DVertex *v1,
                                      const TD5_D3DVertex *v2)
{
    if (s_imm_vert_count + 3 > IMMEDIATE_MAX_VERTS ||
        s_imm_index_count + 3 > IMMEDIATE_MAX_INDICES) {
        flush_immediate_internal();
    }

    int base = s_imm_vert_count;
    s_imm_verts[base + 0] = *v0;
    s_imm_verts[base + 1] = *v1;
    s_imm_verts[base + 2] = *v2;

    s_imm_indices[s_imm_index_count + 0] = (uint16_t)(base + 0);
    s_imm_indices[s_imm_index_count + 1] = (uint16_t)(base + 1);
    s_imm_indices[s_imm_index_count + 2] = (uint16_t)(base + 2);

    s_imm_vert_count  += 3;
    s_imm_index_count += 3;
    s_debug_append_calls++;
}

/**
 * Clip and submit a projected polygon (triangle or quad) into the immediate
 * draw buffer. Performs near-plane clipping, perspective projection, screen-
 * space clipping, backface culling, and triangle fan decomposition.
 *
 * Corresponds to ClipAndSubmitProjectedPolygon (0x4317f0).
 *
 * vert_data: pointer to vertex array (MeshVertex, already view-transformed)
 * vert_count: 3 (triangle) or 4 (quad)
 * tex_page: texture page to bind
 */
static void clip_and_submit_polygon(TD5_MeshVertex *vert_data, int vert_count,
                                    int tex_page)
{
    /* Working buffer for clipped polygon (max 8 verts after clipping) */
    TD5_D3DVertex clipped[8];
    int clipped_count = 0;

    float near_z = s_near_clip;

    /* --- Near-plane clip (Sutherland-Hodgman) --- */
    /* Input polygon from view-space vertices */
    float in_vx[8], in_vy[8], in_vz[8], in_u[8], in_v[8];
    uint32_t in_color[8];
    int in_count = vert_count;

    for (int i = 0; i < vert_count; i++) {
        in_vx[i]    = vert_data[i].view_x;
        in_vy[i]    = vert_data[i].view_y;
        in_vz[i]    = vert_data[i].view_z;
        in_u[i]     = vert_data[i].tex_u;
        in_v[i]     = vert_data[i].tex_v;
        in_color[i] = vert_data[i].lighting;
    }

    /* Near-plane clip */
    float out_vx[8], out_vy[8], out_vz[8], out_u[8], out_v[8];
    uint32_t out_color[8];
    int out_count = 0;

    for (int i = 0; i < in_count; i++) {
        int j = (i + 1) % in_count;
        float zi = in_vz[i], zj = in_vz[j];
        int i_in = (zi > near_z), j_in = (zj > near_z);

        if (i_in) {
            out_vx[out_count]    = in_vx[i];
            out_vy[out_count]    = in_vy[i];
            out_vz[out_count]    = in_vz[i];
            out_u[out_count]     = in_u[i];
            out_v[out_count]     = in_v[i];
            out_color[out_count] = in_color[i];
            out_count++;
        }

        if (i_in != j_in) {
            float t = (near_z - zi) / (zj - zi);
            out_vx[out_count]    = in_vx[i] + t * (in_vx[j] - in_vx[i]);
            out_vy[out_count]    = in_vy[i] + t * (in_vy[j] - in_vy[i]);
            out_vz[out_count]    = near_z;
            out_u[out_count]     = in_u[i]  + t * (in_u[j]  - in_u[i]);
            out_v[out_count]     = in_v[i]  + t * (in_v[j]  - in_v[i]);
            out_color[out_count] = in_color[i]; /* snap to nearer vertex */
            out_count++;
        }

        if (out_count >= 8) break;
    }

    if (out_count < 3) {
        s_debug_clip_near_rejects++;
        return;
    }

    /* --- Perspective projection --- */
    for (int i = 0; i < out_count; i++) {
        float inv_z = 1.0f / out_vz[i];
        clipped[i].screen_x = -out_vx[i] * s_focal_length * inv_z + s_center_x;
        clipped[i].screen_y = -out_vy[i] * s_focal_length * inv_z + s_center_y;
        clipped[i].depth_z  = out_vz[i] * (1.0f / s_far_clip);
        clipped[i].rhw      = inv_z;
        clipped[i].diffuse  = out_color[i];
        clipped[i].specular = 0;
        clipped[i].tex_u    = out_u[i];
        clipped[i].tex_v    = out_v[i];
    }
    clipped_count = out_count;

    /* --- Backface cull (cross product winding test) --- */
    {
        float ax = clipped[1].screen_x - clipped[0].screen_x;
        float ay = clipped[1].screen_y - clipped[0].screen_y;
        float bx = clipped[2].screen_x - clipped[0].screen_x;
        float by = clipped[2].screen_y - clipped[0].screen_y;
        float cross = ax * by - ay * bx;
        /* Skip degenerate (zero-area) triangles but do NOT cull by winding.
         * The Y-negation in the projection reverses winding for half the
         * geometry (MODELS.DAT track meshes vs car meshes have opposite
         * winding conventions).  D3D11 rasterizer has CullMode=NONE. */
        if (cross == 0.0f) {
            s_debug_clip_backface_rejects++;
            return;
        }
    }

    /* --- Screen-space reject (all verts outside same edge) --- */
    {
        float x_min = 0.0f, x_max = (float)s_viewport_width;
        float y_min = 0.0f, y_max = (float)s_viewport_height;
        int all_left = 1, all_right = 1, all_top = 1, all_bottom = 1;
        for (int i = 0; i < clipped_count; i++) {
            if (clipped[i].screen_x >= x_min) all_left   = 0;
            if (clipped[i].screen_x <= x_max) all_right  = 0;
            if (clipped[i].screen_y >= y_min) all_top    = 0;
            if (clipped[i].screen_y <= y_max) all_bottom = 0;
        }
        if (all_left || all_right || all_top || all_bottom) {
            s_debug_clip_screen_rejects++;
            return;
        }
    }
    /* Do NOT clamp individual vertices to viewport bounds — that distorts
     * triangles at screen edges.  D3D11 handles viewport clipping internally. */

    /* --- Set texture page --- */
    if (tex_page >= 0 && tex_page != s_current_texture_page) {
        flush_immediate_internal();
        s_previous_texture_page = s_current_texture_page;
        s_current_texture_page  = tex_page;
    }

    /* --- Triangle fan decomposition --- */
    for (int i = 1; i < clipped_count - 1; i++) {
        append_projected_triangle(&clipped[0], &clipped[i], &clipped[i + 1]);
        s_debug_clip_emitted_tris++;
    }
}

/* ========================================================================
 * Dispatch Table Handlers
 *
 * These correspond to the 7-entry table at 0x473b9c.
 * Each processes a TD5_PrimitiveCmd and emits geometry.
 * ======================================================================== */

/**
 * Opcode 0/1: EmitTranslucentTriangleStrip (0x431750)
 *
 * Processes variable-count triangle strips. Sets vertex count = 3 per triangle,
 * iterates tri_count triangles, calling clip_and_submit_polygon for each.
 */
static void dispatch_tristrip(TD5_PrimitiveCmd *cmd, TD5_MeshVertex *base_verts)
{
    int tex_page = cmd->texture_page_id;
    int tri_count = cmd->triangle_count;
    int quad_count = cmd->quad_count;
    TD5_MeshVertex *verts = (TD5_MeshVertex *)(uintptr_t)cmd->vertex_data_ptr;
    if (!verts) verts = base_verts;

    /* Process triangles (3 verts each) */
    for (int i = 0; i < tri_count; i++) {
        clip_and_submit_polygon(&verts[i * 3], 3, tex_page);
    }

    /* Process quads (4 verts each, after triangles) */
    {
        TD5_MeshVertex *quad_start = verts + tri_count * 3;
        for (int i = 0; i < quad_count; i++) {
            clip_and_submit_polygon(&quad_start[i * 4], 4, tex_page);
        }
    }
}

/**
 * Opcode 2: SubmitProjectedTrianglePrimitive (0x4316F0)
 *
 * Single triangle, submitted through clip + project pipeline.
 */
static void dispatch_projected_tri(TD5_PrimitiveCmd *cmd, TD5_MeshVertex *base_verts)
{
    int tex_page = cmd->texture_page_id;
    TD5_MeshVertex *verts = (TD5_MeshVertex *)(uintptr_t)cmd->vertex_data_ptr;
    if (!verts) verts = base_verts;

    clip_and_submit_polygon(verts, 3, tex_page);
}

/**
 * Opcode 3: SubmitProjectedQuadPrimitive (0x431690)
 *
 * Single quad (4 vertices), submitted through clip + project pipeline.
 */
static void dispatch_projected_quad(TD5_PrimitiveCmd *cmd, TD5_MeshVertex *base_verts)
{
    int tex_page = cmd->texture_page_id;
    TD5_MeshVertex *verts = (TD5_MeshVertex *)(uintptr_t)cmd->vertex_data_ptr;
    if (!verts) verts = base_verts;

    clip_and_submit_polygon(verts, 4, tex_page);
}

/**
 * Opcode 4: InsertBillboardIntoDepthSortBuckets (0x43E3B0)
 *
 * Reads triangle/quad counts from command. Inserts each primitive into the
 * 4096-bucket depth sort array using inverse Z as key.
 * Triangles use stride 0x84, quads use stride 0xB0.
 */
static void dispatch_billboard(TD5_PrimitiveCmd *cmd, TD5_MeshVertex *base_verts)
{
    (void)base_verts;

    int tri_count  = cmd->triangle_count;
    int quad_count = cmd->quad_count;
    int tex_page   = cmd->texture_page_id;
    uint8_t *data  = (uint8_t *)(uintptr_t)cmd->vertex_data_ptr;
    if (!data) return;

    /* Insert quads (0x84 stride, type 3) into depth buckets */
    for (int i = 0; i < tri_count; i++) {
        TD5_MeshVertex *v = (TD5_MeshVertex *)data;
        float avg_z = (v[0].view_z + v[1].view_z + v[2].view_z) * (1.0f / 3.0f);
        if (avg_z > s_near_clip) {
            int bucket = (int)(avg_z * (float)(DEPTH_BUCKET_COUNT - 1) / s_far_clip);
            bucket = clampi(bucket, 0, DEPTH_BUCKET_COUNT - 1);
            bucket ^= (DEPTH_BUCKET_COUNT - 1);
            td5_render_queue_projected_entry(data, bucket, 0x3u, tex_page);
        }
        data += BILLBOARD_TRI_STRIDE;
    }

    /* Insert triangle fans (0xB0 stride, type 4) into depth buckets */
    for (int i = 0; i < quad_count; i++) {
        TD5_MeshVertex *v = (TD5_MeshVertex *)data;
        float avg_z = (v[0].view_z + v[1].view_z + v[2].view_z + v[3].view_z) * 0.25f;
        if (avg_z > s_near_clip) {
            int bucket = (int)(avg_z * (float)(DEPTH_BUCKET_COUNT - 1) / s_far_clip);
            bucket = clampi(bucket, 0, DEPTH_BUCKET_COUNT - 1);
            bucket ^= (DEPTH_BUCKET_COUNT - 1);
            td5_render_queue_projected_entry(data, bucket, 0x4u, tex_page);
        }
        data += BILLBOARD_QUAD_STRIDE;
    }
}

/**
 * Opcode 5: EmitTranslucentTriangleStripDirect (0x431730)
 *
 * Small wrapper: direct depth-sort insert for tri-strip data.
 */
static void dispatch_tristrip_direct(TD5_PrimitiveCmd *cmd, TD5_MeshVertex *base_verts)
{
    /* Direct submission -- same as tristrip but bypasses translucent queue */
    dispatch_tristrip(cmd, base_verts);
}

/**
 * Opcode 6: EmitTranslucentQuadDirect (0x4316D0)
 *
 * Direct depth-sort insert for quad data.
 */
static void dispatch_quad_direct(TD5_PrimitiveCmd *cmd, TD5_MeshVertex *base_verts)
{
    int tex_page = cmd->texture_page_id;
    TD5_MeshVertex *verts = (TD5_MeshVertex *)(uintptr_t)cmd->vertex_data_ptr;
    if (!verts) verts = base_verts;

    clip_and_submit_polygon(verts, 4, tex_page);
}

/* ========================================================================
 * Public API Implementation
 * ======================================================================== */

/* --- Module Lifecycle --- */

int td5_render_init(void)
{
    if (s_globals_initialized) return 0;

    TD5_LOG_I(RENDER_LOG_TAG, "Initializing render globals");

    /* Zero out the render transform */
    memset(&s_render_transform, 0, sizeof(s_render_transform));
    /* Identity rotation */
    s_render_transform.m[0] = 1.0f;
    s_render_transform.m[4] = 1.0f;
    s_render_transform.m[8] = 1.0f;

    /* Clear transform stack */
    s_transform_stack_depth = 0;

    /* Initialize color LUT: i * 0x10101 - 0x1000000
     * Maps luminance byte [0..255] to packed ARGB with alpha=0xFF */
    for (int i = 0; i < 1024; i++) {
        uint32_t val = (uint32_t)i;
        if (val > 255) val = 255;
        s_color_lut[i] = 0xFF000000u | (val << 16) | (val << 8) | val;
    }

    /* Initialize sky rotation */
    s_sky_rotation_angle = 0;

    /* Billboard animation phase */
    s_billboard_anim_phase = 0;

    /* Defaults */
    s_near_clip = DEFAULT_NEAR_CLIP;
    s_far_clip  = DEFAULT_FAR_CLIP;
    s_far_cull  = DEFAULT_FAR_CULL;
    s_ambient_intensity = (float)TD5_LIGHTING_MIN;
    memset(s_light_dirs, 0, sizeof(s_light_dirs));
    memset(s_camera_basis, 0, sizeof(s_camera_basis));
    s_camera_basis[0] = 1.0f; /* right.x */
    s_camera_basis[4] = 1.0f; /* up.y */
    s_camera_basis[8] = 1.0f; /* forward.z */
    memset(s_camera_pos, 0, sizeof(s_camera_pos));

    /* Fog defaults */
    s_fog_color   = 0x808080;
    s_fog_enabled = 0;

    /* Texture cache */
    for (int i = 0; i < TEXTURE_CACHE_SLOTS; i++) {
        s_texture_cache[i].page_id        = -1;
        s_texture_cache[i].status          = 0;
        s_texture_cache[i].age             = 0;
        s_texture_cache[i].used_this_frame = 0;
    }
    s_texture_cache_active_count = 0;
    s_current_texture_page       = -1;
    s_previous_texture_page      = -1;

    /* Initialize immediate draw buffers */
    s_imm_vert_count  = 0;
    s_imm_index_count = 0;

    s_globals_initialized = 1;

    /* Per-race init: translucent pipeline + projected primitives */
    td5_render_init_translucent_pipeline();

    /* Reset depth sort buckets */
    for (int i = 0; i < DEPTH_BUCKET_COUNT; i++) {
        s_depth_buckets[i] = -1;
    }
    s_depth_entry_count = 0;

    /* Clear screen on first frame */
    s_state_active = 1;

    TD5_LOG_I(LOG_TAG,
              "render init: near=%.2f far=%.2f far_cull=%.2f fog_enabled=%d fog_color=0x%06X texture_cache_slots=%d active_cache=%d",
              s_near_clip, s_far_clip, s_far_cull, s_fog_enabled,
              (unsigned int)(s_fog_color & 0x00FFFFFFu),
              TEXTURE_CACHE_SLOTS, s_texture_cache_active_count);

    return 1;
}

void td5_render_shutdown(void)
{
    TD5_LOG_I(RENDER_LOG_TAG, "Releasing render resources");

    /* Reset texture cache */
    td5_render_reset_texture_cache();

    /* Clear fog */
    s_fog_enabled = 0;

    s_state_active        = 0;
    s_globals_initialized = 0;
}

void td5_render_frame(void)
{
    /* Full per-frame render pass.
     * Orchestrated by td5_game.c; individual functions called in sequence:
     *   td5_render_begin_scene()
     *   [per-viewport rendering via external callers]
     *   td5_render_end_scene()
     *
     * This function serves as a convenience entry point for testing. */
}

/* --- Scene Brackets --- */

void td5_render_begin_scene(void)
{
    td5_plat_render_begin_scene();

    /* Force texture rebind on first draw call */
    s_previous_texture_page = -1;
    s_current_texture_page  = -1;
    s_scene_has_renderer_geometry = 0;

    /* Reset immediate draw buffer */
    s_imm_vert_count  = 0;
    s_imm_index_count = 0;
    s_debug_clip_near_rejects = 0;
    s_debug_clip_backface_rejects = 0;
    s_debug_clip_screen_rejects = 0;
    s_debug_clip_emitted_tris = 0;
    s_debug_prepared_mesh_calls = 0;
    s_debug_append_calls = 0;
    s_debug_flush_calls = 0;
    s_debug_flush_submitted_tris = 0;
    s_debug_texture_bind_calls = 0;
    s_debug_texture_cache_hits = 0;
    s_debug_texture_cache_misses = 0;
    s_debug_texture_cache_evictions = 0;
    s_debug_scene_draw_calls = 0;
    s_debug_span_meshes_submitted = 0;

    TD5_LOG_D(LOG_TAG,
              "begin scene: frame=%u reset tris=%d binds=%d draws=%d",
              (unsigned int)g_tick_counter,
              s_debug_flush_submitted_tris,
              s_debug_texture_bind_calls,
              s_debug_scene_draw_calls);
}

void td5_render_end_scene(void)
{
    if (g_td5.total_actor_count <= 0 && s_debug_clip_log_count < 5) {
        TD5_LOG_D(LOG_TAG,
                  "end scene: frame=%u tris=%d draws=%d span_meshes=%d",
                  (unsigned int)g_tick_counter,
                  s_debug_flush_submitted_tris,
                  s_debug_scene_draw_calls,
                  s_debug_span_meshes_submitted);
        s_debug_clip_log_count++;
    }

    td5_plat_render_end_scene();
    g_tick_counter++;

    /* Texture cache aging: advance ages, clear per-frame used flags */
    td5_render_advance_texture_ages();
}

/* --- Render State --- */

void td5_render_set_preset(TD5_RenderPreset preset)
{
    /* Flush any pending geometry before state change */
    flush_immediate_internal();

    td5_plat_render_set_preset(preset);
}

void td5_render_set_fog(int enable)
{
    if (enable && g_td5.ini.fog_enabled) {
        /* Apply full fog pipeline: linear table fog */
        td5_plat_render_set_fog(1, s_fog_color,
                                FOG_START_DEFAULT, FOG_END_DEFAULT,
                                FOG_DENSITY_DEFAULT);
    } else {
        /* Disable fog */
        td5_plat_render_set_fog(0, 0, 0.0f, 0.0f, 0.0f);
    }
}

void td5_render_configure_fog(uint32_t color, int enable)
{
    /* Strip alpha, store RGB only (original: color & 0xFFFFFF) */
    s_fog_color   = color & 0x00FFFFFFu;
    s_fog_enabled = enable;
}

/* --- Transform Stack (8-deep push/pop for hierarchical models) --- */

void td5_render_load_rotation(const TD5_Mat3x3 *rot)
{
    /* Copy 9 floats (3x3 rotation) into active transform, leave translation */
    for (int i = 0; i < 9; i++) {
        s_render_transform.m[i] = rot->m[i];
    }
}

void td5_render_load_translation(const TD5_Vec3f *pos)
{
    /*
     * LoadRenderTranslation (0x43DC20):
     *
     * Computes camera-relative view-space translation.
     *
     *   delta = pos - camera_pos
     *   m[9..11] = camera_basis * delta
     */
    float dx = pos->x - s_camera_pos[0];
    float dy = pos->y - s_camera_pos[1];
    float dz = pos->z - s_camera_pos[2];

    s_render_transform.m[9]  = dx * s_camera_basis[0] + dy * s_camera_basis[1] + dz * s_camera_basis[2];
    s_render_transform.m[10] = dx * s_camera_basis[3] + dy * s_camera_basis[4] + dz * s_camera_basis[5];
    s_render_transform.m[11] = dx * s_camera_basis[6] + dy * s_camera_basis[7] + dz * s_camera_basis[8];
}

void td5_render_push_transform(void)
{
    if (s_transform_stack_depth >= TRANSFORM_STACK_MAX) {
        TD5_LOG_W(RENDER_LOG_TAG, "Transform stack overflow (depth=%d)",
                  s_transform_stack_depth);
        return;
    }
    s_transform_stack[s_transform_stack_depth] = s_render_transform;
    s_transform_stack_depth++;
}

void td5_render_pop_transform(void)
{
    if (s_transform_stack_depth <= 0) {
        TD5_LOG_W(RENDER_LOG_TAG, "Transform stack underflow");
        return;
    }
    s_transform_stack_depth--;
    s_render_transform = s_transform_stack[s_transform_stack_depth];
}

void td5_render_transform_vec3(const float *in, float *out)
{
    /* 3x3 rotation only (no translation) -- for direction vectors */
    mat3x3_transform_dir(s_render_transform.m, in, out);
}

/* --- Vertex Transform & Lighting --- */

void td5_render_transform_mesh_vertices(TD5_MeshHeader *mesh)
{
    if (!mesh) return;

    const float *m = s_render_transform.m;
    int count = mesh->total_vertex_count;
    TD5_MeshVertex *verts = (TD5_MeshVertex *)(uintptr_t)mesh->vertices_offset;
    if (!verts || count <= 0) return;

    /*
     * Software world->view transform (0x43DD60):
     *   view_x = pos_x * M[0] + pos_y * M[1] + pos_z * M[2] + M[9]
     *   view_y = pos_x * M[3] + pos_y * M[4] + pos_z * M[5] + M[10]
     *   view_z = pos_x * M[6] + pos_y * M[7] + pos_z * M[8] + M[11]
     *
     * Vertex stride: 0x2C (44 bytes). Input XYZ at +0x00, output at +0x0C.
     */
    for (int i = 0; i < count; i++) {
        float px = verts[i].pos_x;
        float py = verts[i].pos_y;
        float pz = verts[i].pos_z;

        verts[i].view_x = px * m[0] + py * m[1] + pz * m[2] + m[9];
        verts[i].view_y = px * m[3] + py * m[4] + pz * m[5] + m[10];
        verts[i].view_z = px * m[6] + py * m[7] + pz * m[8] + m[11];
    }
}

void td5_render_compute_vertex_lighting(TD5_MeshHeader *mesh)
{
    if (!mesh) return;

    int count = mesh->total_vertex_count;
    TD5_MeshVertex *verts   = (TD5_MeshVertex *)(uintptr_t)mesh->vertices_offset;
    TD5_VertexNormal *norms = (TD5_VertexNormal *)(uintptr_t)mesh->normals_offset;
    if (!verts || !norms || count <= 0) return;

    /*
     * Per-vertex 3-directional diffuse lighting (0x43DDF0):
     *
     *   intensity = dot(normal, light0) + dot(normal, light1) + dot(normal, light2)
     *   intensity = clamp(intensity + ambient, 0x40, 0xFF)
     *   vertex.lighting = intensity  (stored at +0x18, low byte)
     *
     * Light directions: s_light_dirs[0..2] = light0, [3..5] = light1, [6..8] = light2
     * Ambient: s_ambient_intensity
     */
    const float *l0 = &s_light_dirs[0];
    const float *l1 = &s_light_dirs[3];
    const float *l2 = &s_light_dirs[6];

    for (int i = 0; i < count; i++) {
        float nx = norms[i].nx;
        float ny = norms[i].ny;
        float nz = norms[i].nz;

        /* 3-light diffuse accumulation */
        float intensity = 0.0f;
        intensity += nx * l0[0] + ny * l0[1] + nz * l0[2];
        intensity += nx * l1[0] + ny * l1[1] + nz * l1[2];
        intensity += nx * l2[0] + ny * l2[1] + nz * l2[2];

        /* Add ambient, clamp to [0x40, 0xFF] */
        intensity += s_ambient_intensity;
        int lum = (int)intensity;
        lum = clampi(lum, TD5_LIGHTING_MIN, TD5_LIGHTING_MAX);

        verts[i].lighting = (uint32_t)lum;
    }
}

/* --- Frustum Culling --- */

int td5_render_is_sphere_visible(float cx, float cy, float cz, float radius)
{
    /*
     * 5-plane bounding sphere test in camera space (0x42DCA0):
     *
     * 1. Transform world-space center to camera-relative coordinates
     * 2. Test against near plane (z + r > near)
     * 3. Test against far plane  (z - r < far)
     * 4. Test against left/right frustum planes (h_cos * |x| + h_sin * z < r)
     * 5. Test against top/bottom frustum planes (v_cos * |y| + v_sin * z < r)
     *
     * Returns 0 if culled (invisible), nonzero if visible.
     * Original returns 0x80000000 if culled.
     */

    /* Transform bounding sphere center to camera space */
    float dx = cx - s_camera_pos[0];
    float dy = cy - s_camera_pos[1];
    float dz = cz - s_camera_pos[2];

    /* Camera basis: s_camera_basis[0..2]=right, [3..5]=up, [6..8]=forward */
    float vx = dx * s_camera_basis[0] + dy * s_camera_basis[1] + dz * s_camera_basis[2];
    float vy = dx * s_camera_basis[3] + dy * s_camera_basis[4] + dz * s_camera_basis[5];
    float vz = dx * s_camera_basis[6] + dy * s_camera_basis[7] + dz * s_camera_basis[8];

    /* Near plane test: sphere must extend past near clip */
    if (vz + radius <= s_near_clip)
        return 0;

    /* Far plane test: sphere must not be entirely beyond far cull (195000, 0x0042D48E) */
    if (vz - radius >= s_far_cull)
        return 0;

    /* Left/right frustum planes (horizontal) */
    /* Plane normal = (h_cos, 0, h_sin), distance from origin = 0 */
    float h_dist = s_frustum_h_cos * vx + s_frustum_h_sin * vz;
    if (h_dist > radius)
        return 0;

    /* Check negative side (right plane is mirrored) */
    float h_dist_neg = -s_frustum_h_cos * vx + s_frustum_h_sin * vz;
    if (h_dist_neg > radius)
        return 0;

    /* Top/bottom frustum planes (vertical) */
    float v_dist = s_frustum_v_cos * vy + s_frustum_v_sin * vz;
    if (v_dist > radius)
        return 0;

    float v_dist_neg = -s_frustum_v_cos * vy + s_frustum_v_sin * vz;
    if (v_dist_neg > radius)
        return 0;

    return 1; /* visible */
}

int td5_render_test_mesh_frustum(TD5_MeshHeader *mesh, float *out_depth)
{
    const float *m;
    float cx, cy, cz, r;
    float vx, vy, vz;

    if (!mesh) return 0;

    /*
     * TestMeshAgainstViewFrustum (0x42DE10):
     * More detailed frustum test for vehicles. Also computes depth distance
     * for LOD/fade decisions.
     */
    cx = mesh->bounding_center_x + mesh->origin_x;
    cy = mesh->bounding_center_y + mesh->origin_y;
    cz = mesh->bounding_center_z + mesh->origin_z;
    r  = mesh->bounding_radius;

    m = s_render_transform.m;
    vx = cx * m[0] + cy * m[1] + cz * m[2] + m[9];
    vy = cx * m[3] + cy * m[4] + cz * m[5] + m[10];
    vz = cx * m[6] + cy * m[7] + cz * m[8] + m[11];

    if (vz + r <= s_near_clip)
        return 0;
    if (vz - r >= s_far_cull)
        return 0;
    if (s_frustum_h_cos * vx + s_frustum_h_sin * vz > r)
        return 0;
    if (-s_frustum_h_cos * vx + s_frustum_h_sin * vz > r)
        return 0;
    if (s_frustum_v_cos * vy + s_frustum_v_sin * vz > r)
        return 0;
    if (-s_frustum_v_cos * vy + s_frustum_v_sin * vz > r)
        return 0;

    if (out_depth)
        *out_depth = vz;

    return 1;
}

/* --- Mesh Rendering --- */

void td5_render_span_display_list(void *display_list_block)
{
    /*
     * RenderTrackSpanDisplayList (0x431270):
     * Core track world renderer. Iterates sub-meshes in a display list block.
     *
     * Block layout:
     *   [0] = sub_mesh_count
     *   [1..N] = pointers to MeshResourceHeader (relocated by ParseModelsDat)
     */
    static int s_debug_reject_ptr = 0;
    static int s_debug_reject_counts = 0;
    static int s_debug_reject_offsets = 0;
    static int s_debug_reject_blob = 0;
    static int s_debug_accept = 0;
    static int s_debug_dl_calls = 0;

    if (!display_list_block) return;

    uint32_t *block = (uint32_t *)display_list_block;
    int count = (int)block[0];
    s_debug_dl_calls++;
    if (count <= 0 || count > 256) return; /* sanity */

    TD5_LOG_D(LOG_TAG,
              "span display list: block=%p mesh_range=[0,%d)",
              display_list_block, count);

    for (int i = 0; i < count; i++) {
        TD5_MeshHeader *mesh = (TD5_MeshHeader *)(uintptr_t)block[i + 1];
        if (!mesh || (uintptr_t)mesh < 0x100000u || !td5_track_is_valid_mesh_ptr(mesh)) {
            s_debug_reject_ptr++; continue;
        }

        /* Validate mesh header fields — skip empty and out-of-range meshes */
        if (mesh->command_count <= 0 || mesh->command_count > 4096) {
            s_debug_reject_counts++; continue;
        }
        if (mesh->total_vertex_count <= 0 || mesh->total_vertex_count > 131072) {
            s_debug_reject_counts++; continue;
        }
        if (!mesh->commands_offset || !mesh->vertices_offset) {
            s_debug_reject_offsets++; continue;
        }
        if ((uintptr_t)mesh->commands_offset < 0x10000u) {
            s_debug_reject_offsets++; continue;
        }
        if ((uintptr_t)mesh->vertices_offset < 0x10000u) {
            s_debug_reject_offsets++; continue;
        }

        /* Validate commands and vertices pointers are within models blob
         * OR valid heap memory (strip-generated display lists use calloc). */
        if (!td5_track_is_ptr_in_blob((void *)(uintptr_t)mesh->commands_offset,
                (size_t)mesh->command_count * sizeof(TD5_PrimitiveCmd)) &&
            !td5_track_is_valid_mesh_ptr((void *)(uintptr_t)mesh->commands_offset)) {
            s_debug_reject_blob++; continue;
        }
        if (!td5_track_is_ptr_in_blob((void *)(uintptr_t)mesh->vertices_offset,
                (size_t)mesh->total_vertex_count * sizeof(TD5_MeshVertex)) &&
            !td5_track_is_valid_mesh_ptr((void *)(uintptr_t)mesh->vertices_offset)) {
            s_debug_reject_blob++; continue;
        }
        s_debug_accept++;

        /* Frustum cull via bounding sphere — bounding center is already in
           render-float world space (original reads +0x10/14/18 directly,
           no origin offset added) [CONFIRMED @ 0x42dcad] */
        float cx = mesh->bounding_center_x;
        float cy = mesh->bounding_center_y;
        float cz = mesh->bounding_center_z;
        float r  = mesh->bounding_radius;

        /* Validate bounding data isn't NaN/Inf */
        if (r != r || r < 0.0f) continue;
        if (cx != cx || cy != cy || cz != cz) continue;

        {
            static int s_span_diag = 0;
            if (s_span_diag < 5) {
                float ddx = cx - s_camera_pos[0];
                float ddy = cy - s_camera_pos[1];
                float ddz = cz - s_camera_pos[2];
                float dist = ddx*ddx + ddy*ddy + ddz*ddz;
                TD5_LOG_I(LOG_TAG,
                    "SPAN_MESH bc=(%.1f,%.1f,%.1f) origin_raw=(%.1f,%.1f,%.1f) r=%.1f cam_dist2=%.0f",
                    cx, cy, cz,
                    mesh->origin_x, mesh->origin_y, mesh->origin_z,
                    r, dist);
                s_span_diag++;
            }
        }

        if (!td5_render_is_sphere_visible(cx, cy, cz, r))
            continue;

        /* Build world-to-view basis from mesh origin — origin is in integer-
           coordinate space, must scale by 1/256 to match camera (render-float
           space) before subtraction [CONFIRMED @ 0x42d954-0x42d97a] */
        TD5_Vec3f origin;
        origin.x = mesh->origin_x * (1.0f / 256.0f);
        origin.y = mesh->origin_y * (1.0f / 256.0f);
        origin.z = mesh->origin_z * (1.0f / 256.0f);

        td5_render_load_translation(&origin);

        /* Billboard meshes (trees/signs/street-lights): replace rotation with
         * the yaw-stripped camera basis so the quad faces the camera
         * horizontally while still tilting with pitch/roll.
         * [CONFIRMED @ 0x00431296 raw 66 8b 46 02]: original reads
         * `MOV AX, [ESI+0x02]` and tests `==1 || ==2` to take the billboard
         * branch which calls LoadRenderRotationMatrix(&DAT_004ab070).
         * In TD5_MeshHeader the int16 at byte offset 2 is currently named
         * `texture_page_id`, but per-mesh texture binding is done from the
         * per-primitive cmd->texture_page_id, not this field — the mesh
         * header field is the billboard tag.
         * We load g_cameraSecondaryUnscaled (snapshot of g_cameraSecondary
         * BEFORE FinalizeCameraProjectionMatrices applies inv_proj*fov_factor)
         * because s_camera_basis above is also the unscaled g_cameraBasis.
         * Loading the SCALED g_cameraSecondary into the rotation slot of
         * s_render_transform mixes coordinate spaces and collapses every
         * billboard quad off-screen. */
        int billboard_tag = (int)mesh->texture_page_id;
        if (billboard_tag == 1 || billboard_tag == 2) {
            extern float g_cameraSecondaryUnscaled[9];
            static int s_bb_hit = 0, s_bb_log = 0;
            s_bb_hit++;
            td5_render_push_transform();
            td5_render_load_rotation((const TD5_Mat3x3 *)g_cameraSecondaryUnscaled);
            if (s_bb_log < 10) {
                TD5_LOG_I(LOG_TAG,
                    "billboard mesh: tag=%d origin=(%.1f,%.1f,%.1f) hits=%d",
                    billboard_tag, origin.x, origin.y, origin.z, s_bb_hit);
                s_bb_log++;
            }
            td5_render_transform_mesh_vertices(mesh);
            td5_render_compute_vertex_lighting(mesh);
            td5_render_prepared_mesh(mesh);
            s_debug_span_meshes_submitted++;
            td5_render_pop_transform();
        } else {
            td5_render_transform_mesh_vertices(mesh);
            td5_render_compute_vertex_lighting(mesh);
            td5_render_prepared_mesh(mesh);
            s_debug_span_meshes_submitted++;
        }
    }

    if ((s_debug_dl_calls % 500) == 1) {
        TD5_LOG_I(LOG_TAG,
            "mesh filter: calls=%d accept=%d rej_ptr=%d rej_cnt=%d "
            "rej_off=%d rej_blob=%d",
            s_debug_dl_calls, s_debug_accept, s_debug_reject_ptr,
            s_debug_reject_counts, s_debug_reject_offsets, s_debug_reject_blob);
    }
}

void td5_render_prepared_mesh(TD5_MeshHeader *mesh)
{
    /*
     * RenderPreparedMeshResource (0x4314B0):
     * Iterates the mesh command list, dispatching each through the 7-entry
     * translucent dispatch table.
     *
     * Commands consume vertices sequentially from the vertex buffer.
     * Each command's tri_count and quad_count determine how many verts
     * it consumes: tri_count*3 + quad_count*4.
     */
    if (!mesh) return;
    s_debug_prepared_mesh_calls++;

    int cmd_count = mesh->command_count;
    TD5_PrimitiveCmd *cmds = (TD5_PrimitiveCmd *)(uintptr_t)mesh->commands_offset;
    TD5_MeshVertex *base_verts = (TD5_MeshVertex *)(uintptr_t)mesh->vertices_offset;
    if (!cmds || !base_verts || cmd_count <= 0) return;
    if (cmd_count > 4096 || mesh->total_vertex_count > 65536) return;

    /* Validate pointers are accessible (within models blob or heap) */
    if ((uintptr_t)cmds < 0x10000u || (uintptr_t)base_verts < 0x10000u) return;

    /* Running vertex offset: commands consume vertices sequentially.
     * If vertex_data_ptr is 0, use the running offset from base_verts. */
    int vert_cursor = 0;

    for (int i = 0; i < cmd_count; i++) {
        TD5_PrimitiveCmd *cmd = &cmds[i];
        int opcode = cmd->dispatch_type;

        /* Bounds check dispatch opcode */
        if (opcode < 0 || opcode > 6) {
            static int s_bad_opcode_count = 0;
            if (s_bad_opcode_count < 20) {
                TD5_LOG_W(RENDER_LOG_TAG, "Invalid mesh dispatch opcode %d (occurrence %d)",
                          opcode, ++s_bad_opcode_count);
            }
            continue;
        }

        /* If vertex_data_ptr is 0, point to running cursor position.
         * For MODELS.DAT meshes, vertex_data_ptr is typically 0 and the
         * renderer uses a sequential cursor.  If non-zero, it may be
         * an unrelocated relative offset from the mesh header — relocate
         * it on the fly if it looks like a small offset rather than an
         * absolute pointer. */
        {
            TD5_MeshVertex *cmd_verts = base_verts;
            int verts_needed = cmd->triangle_count * 3 + cmd->quad_count * 4;

            if (cmd->vertex_data_ptr != 0) {
                uintptr_t vp = (uintptr_t)cmd->vertex_data_ptr;
                if (vp < 0x10000u) {
                    /* Looks like a relative offset — relocate from mesh base */
                    cmd_verts = (TD5_MeshVertex *)((uint8_t *)mesh + vp);
                    if (!td5_track_is_ptr_in_blob(cmd_verts, sizeof(TD5_MeshVertex)))
                        continue;
                } else if (td5_track_is_ptr_in_blob((void *)vp, sizeof(TD5_MeshVertex))) {
                    cmd_verts = (TD5_MeshVertex *)vp;
                } else {
                    continue; /* bad pointer, skip command */
                }
            } else if (vert_cursor > 0) {
                cmd_verts = base_verts + vert_cursor;
            }

            /* Bounds check: don't read past total vertex count */
            if (cmd->vertex_data_ptr == 0 &&
                vert_cursor + verts_needed > mesh->total_vertex_count) {
                break; /* out of vertices */
            }

            /* Dispatch through table with correct vertex pointer */
            TD5_PrimitiveCmd patched = *cmd;
            patched.vertex_data_ptr = (uint32_t)(uintptr_t)cmd_verts;
            s_dispatch_table[opcode](&patched, base_verts);

            /* Advance running cursor by vertices consumed */
            if (cmd->vertex_data_ptr == 0) {
                vert_cursor += verts_needed;
            }
        }

        /* After dispatch: if we have accumulated geometry, flush */
        if (s_imm_vert_count > 0 && s_imm_index_count > 0 &&
            s_imm_index_count >= IMMEDIATE_MAX_INDICES - 12) {
            flush_immediate_internal();
        }
    }

    /* Flush any remaining geometry from this mesh */
    flush_immediate_internal();
}

void td5_render_set_vehicle_mesh(int slot, TD5_MeshHeader *mesh)
{
    if (slot < 0 || slot >= TD5_ACTOR_MAX_TOTAL_SLOTS)
        return;

    s_vehicle_meshes[slot] = mesh;
}

TD5_MeshHeader *td5_render_get_vehicle_mesh(int slot)
{
    if (slot < 0 || slot >= TD5_ACTOR_MAX_TOTAL_SLOTS)
        return NULL;

    return s_vehicle_meshes[slot];
}

void td5_render_actors_for_view(int view_index)
{
    /*
     * RenderRaceActorsForView (0x40BD20):
     * Master per-view actor renderer. Iterates active actor slots and
     * renders vehicles with full transform/light/render pipeline.
     */
    int rendered_spans = 0;
    int span_count = td5_track_get_span_count();

    /* View distance span-window cull.
     * Original RunRaceFrame (0x42BB2E): effective_spans = (int)((v * 0.85 + 0.15) * max_spans)
     * where max_spans = 0x40 (64) for single-screen [CONFIRMED @ InitializeRaceViewportLayout 0x0042C2B0].
     * Source port uses max_spans=128 (200% of original) so slider at 1.0 shows 2× the original max.
     * Cull window = player_span ± half_window with ring-wrap for circuit tracks. */
#define VIEW_DIST_MAX_SPANS 128
    int player_span = 0;
    {
        TD5_Actor *player = td5_game_get_actor(0);
        if (player)
            player_span = (int)player->track_span_raw;
    }
    float view_dist_frac = td5_save_get_view_distance();
    int half_window = (int)((view_dist_frac * 0.85f + 0.15f) * (float)VIEW_DIST_MAX_SPANS);
    if (half_window < 1) half_window = 1;

    int actor_render_count = 0;
    int actor_meshes_submitted = 0;

    /* Refresh render-side camera snapshot from game-side globals every frame.
     * td5_render_configure_projection only runs when viewport dimensions change,
     * so without this the render camera stays frozen at its initial position. */
    update_render_camera_from_game();

    /* Always clear the backbuffer so previous frames don't bleed through */
    td5_plat_render_clear(0xFF4080C0u);

    /* Draw sky panorama behind all geometry */
    td5_render_draw_sky();

    /* Set render preset for track geometry (enables texture sampling) */
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);

    /* Load camera view basis into render transform rotation.
     * This is normally done by the actor rendering path (ApplyMeshRenderBasis),
     * but for track span geometry we need it set once before the span loop.
     * The camera basis is: right=[0..2], up=[3..5], forward=[6..8]. */
    for (int i = 0; i < 9; i++)
        s_render_transform.m[i] = s_camera_basis[i];

    for (int span_index = 0; span_index < span_count; span_index++) {
        void *display_list = td5_track_get_display_list(span_index);
        if (!display_list)
            continue;

        /* Span-window cull: skip spans outside ±half_window of the player's span.
         * Delta is wrapped into [-span_count/2, span_count/2] to handle circuit ring. */
        if (span_count > 0) {
            int delta = span_index - player_span;
            int half_count = span_count / 2;
            if (delta >  half_count) delta -= span_count;
            if (delta < -half_count) delta += span_count;
            if (delta > half_window || delta < -half_window)
                continue;
        }

        td5_render_span_display_list(display_list);
        rendered_spans++;
    }

    {
        int total_actors = td5_game_get_total_actor_count();

        for (int slot = 0; slot < total_actors; slot++) {
            TD5_Actor *actor = td5_game_get_actor(slot);
            TD5_MeshHeader *mesh = td5_render_get_vehicle_mesh(slot);
            TD5_Mat3x3 view_rot;
            TD5_Vec3f render_pos;
            float depth;

            if (!actor || !mesh)
                continue;

            td5_track_apply_segment_lighting(actor, view_index);

            /* Original (0x40C120): compute interpolated render position.
             * render_pos = (world_pos + linear_velocity * g_subTickFraction) / 256.
             * [CONFIRMED @ 0x40C164-0x40C1D4] */
            {
                extern float g_subTickFraction;
                float frac = g_subTickFraction;
                float interp_x = (float)actor->world_pos.x + (float)actor->linear_velocity_x * frac;
                float interp_y = (float)actor->world_pos.y + (float)actor->linear_velocity_y * frac;
                float interp_z = (float)actor->world_pos.z + (float)actor->linear_velocity_z * frac;
                render_pos.x = interp_x * (1.0f / 256.0f);
                render_pos.y = interp_y * (1.0f / 256.0f);
                render_pos.z = interp_z * (1.0f / 256.0f);
            }

            /* Original (0x40C1E2-0x40C25E) builds an interpolated rotation from
             * display_angles + angular_velocity * (1/256 * g_subTickFraction).
             * BuildRotationMatrixFromAngles has known axis-order issues for some
             * tracks, so fall back to the physics-step rotation matrix for now.
             * The position interpolation alone handles most of the visual lag. */
            mat3x3_mul(s_camera_basis, actor->rotation_matrix.m, view_rot.m);
            td5_render_load_rotation(&view_rot);
            td5_render_load_translation(&render_pos);

            if (!td5_render_test_mesh_frustum(mesh, &depth))
                continue;
            (void)depth;

            td5_render_transform_mesh_vertices(mesh);
            td5_render_compute_vertex_lighting(mesh);
            td5_render_prepared_mesh(mesh);

            /* Render car shadow (dark ground quad under vehicle) */
            render_vehicle_shadow_quad();

            /* Render wheel ring billboards (0x446F00) */
            render_vehicle_wheel_billboards(actor, slot);

            actor_render_count++;
            actor_meshes_submitted++;

            TD5_LOG_D(LOG_TAG,
                      "vehicle render: view=%d slot=%d pos=(%.2f, %.2f, %.2f) mesh=%p",
                      view_index, slot,
                      render_pos.x, render_pos.y, render_pos.z,
                      (void *)mesh);
        }
    }

    {
        static int s_diag_frame = 0;
        if (s_diag_frame < 60 || (s_diag_frame % 300) == 0) {
            TD5_Actor *a0 = td5_game_get_actor(0);
            TD5_MeshHeader *m0 = td5_render_get_vehicle_mesh(0);
            TD5_LOG_I(LOG_TAG,
                      "DIAG frame=%d cam=(%.1f,%.1f,%.1f) "
                      "actor0_rpos=(%.1f,%.1f,%.1f) "
                      "actors_rendered=%d span_meshes=%d/%d "
                      "mesh0=%p "
                      "clip_near=%d clip_back=%d clip_screen=%d tris_out=%d "
                      "mesh0_radius=%.1f mesh0_origin=(%.1f,%.1f,%.1f)",
                      s_diag_frame,
                      s_camera_pos[0], s_camera_pos[1], s_camera_pos[2],
                      a0 ? a0->render_pos.x : -1.0f,
                      a0 ? a0->render_pos.y : -1.0f,
                      a0 ? a0->render_pos.z : -1.0f,
                      actor_render_count,
                      s_debug_span_meshes_submitted, rendered_spans,
                      (void *)m0,
                      s_debug_clip_near_rejects, s_debug_clip_backface_rejects,
                      s_debug_clip_screen_rejects, s_debug_clip_emitted_tris,
                      m0 ? m0->bounding_radius : -1.0f,
                      m0 ? m0->origin_x : -1.0f,
                      m0 ? m0->origin_y : -1.0f,
                      m0 ? m0->origin_z : -1.0f);
        }
        s_diag_frame++;
    }

    if (rendered_spans > 0) {
        /* Keep the legacy renderer fallback disabled while debugging the
         * strip-backed scene, even if the generated spans clip away. */
        s_scene_has_renderer_geometry = 1;
    }

    if (rendered_spans > 0 && s_debug_fallback_log_count < 10) {
        TD5_LOG_I(RENDER_LOG_TAG,
                  "render view %d: submitted %d span display lists",
                  view_index, rendered_spans);
        s_debug_fallback_log_count++;
    }
}

/* --- Projection --- */

void td5_render_configure_projection(int width, int height)
{
    /*
     * ConfigureProjectionForViewport (0x43E7E0):
     *
     * focal = width * 0.5625   (4:3 assumption: 640*0.5625=360)
     * inv_focal = 1.0 / focal
     *
     * Horizontal frustum half-plane normal:
     *   h_len = sqrt(width*width*0.25 + focal*focal)
     *   h_cos = focal / h_len
     *   h_sin = -(width / (2*h_len))
     *
     * Vertical frustum half-plane normal:
     *   v_len = sqrt(height*height*0.25 + focal*focal)
     *   v_cos = focal / v_len
     *   v_sin = -(height / (2*v_len))
     */
    s_viewport_width  = width;
    s_viewport_height = height;
    s_center_x = (float)width  * 0.5f;
    s_center_y = (float)height * 0.5f;

    /* Focal length: FOV locked to 4:3 horizontal */
    s_focal_length = (float)width * 0.5625f;
    s_inv_focal    = 1.0f / s_focal_length;

    /* Near/far clip */
    s_near_clip = DEFAULT_NEAR_CLIP;
    s_far_clip  = DEFAULT_FAR_CLIP;
    s_far_cull  = DEFAULT_FAR_CULL;

    /* Horizontal frustum half-plane normals */
    float half_w = (float)width * 0.5f;
    float h_len = sqrtf(half_w * half_w + s_focal_length * s_focal_length);
    s_frustum_h_cos =  s_focal_length / h_len;
    s_frustum_h_sin = -half_w / h_len;

    /* Vertical frustum half-plane normals */
    float half_h = (float)height * 0.5f;
    float v_len = sqrtf(half_h * half_h + s_focal_length * s_focal_length);
    s_frustum_v_cos =  s_focal_length / v_len;
    s_frustum_v_sin = -half_h / v_len;

    /* Configure platform viewport */
    td5_plat_render_set_viewport(0, 0, width, height);
    update_render_camera_from_game();

    {
        float half_fov_rad = atanf(((float)width * 0.5f) / s_focal_length);
        float fov_deg = half_fov_rad * (360.0f / 3.14159265358979323846f);
        TD5_LOG_I(LOG_TAG,
                  "projection configured: %dx%d focal=%.1f near=%.1f far=%.1f far_cull=%.1f fov=%.2f",
                  width, height, s_focal_length, s_near_clip, s_far_clip, s_far_cull, fov_deg);
    }
}

/* --- Translucent Primitive Pipeline --- */

void td5_render_init_translucent_pipeline(void)
{
    /*
     * InitializeTranslucentPrimitivePipeline (0x4312E0):
     *
     * 1. Color LUT at 0x4AEE68: already initialized in td5_render_init()
     *
     * 2. Linked-list pool: 512 entries, each 8 bytes:
     *    - 2-byte flags (init to 0)
     *    - 2-byte sort key (init to 0xFFFF)
     *    - 4-byte next pointer (linked sequentially)
     *
     * 3. Active batch count = 0
     */
    s_translucent_count = 0;
    s_translucent_head  = -1; /* empty sorted list */

    /* Build free list: all entries chained sequentially */
    for (int i = 0; i < TRANSLUCENT_POOL_SIZE; i++) {
        s_translucent_pool[i].flags    = 0;
        s_translucent_pool[i].sort_key = 0xFFFF;
        s_translucent_pool[i].record   = NULL;
        s_translucent_pool[i].next     = (i + 1 < TRANSLUCENT_POOL_SIZE) ? (i + 1) : -1;
    }
    s_translucent_free = 0; /* head of free list */
}

void td5_render_queue_translucent_batch(void *record)
{
    /*
     * QueueTranslucentPrimitiveBatch (0x431460):
     * Inserts a primitive command record into the sorted linked-list.
     * Sort key = texture page ID (record+0x02).
     * Maximum 510 active batches.
     */
    if (!record) return;
    if (s_translucent_count >= TRANSLUCENT_MAX_ACTIVE) return;
    if (s_translucent_free < 0) return;

    /* Allocate from free list */
    int idx = s_translucent_free;
    s_translucent_free = s_translucent_pool[idx].next;

    TD5_PrimitiveCmd *cmd = (TD5_PrimitiveCmd *)record;
    uint16_t sort_key = (uint16_t)cmd->texture_page_id;

    s_translucent_pool[idx].flags    = 1;
    s_translucent_pool[idx].sort_key = sort_key;
    s_translucent_pool[idx].record   = record;

    /* Insert into sorted list (ascending by sort_key) */
    int prev = -1;
    int curr = s_translucent_head;

    while (curr >= 0 && s_translucent_pool[curr].sort_key <= sort_key) {
        prev = curr;
        curr = s_translucent_pool[curr].next;
    }

    s_translucent_pool[idx].next = curr;
    if (prev < 0) {
        s_translucent_head = idx;
    } else {
        s_translucent_pool[prev].next = idx;
    }

    s_translucent_count++;
}

void td5_render_flush_translucent(void)
{
    /*
     * FlushQueuedTranslucentPrimitives (0x431340):
     * Walk the sorted linked list, dispatching each record through the
     * 7-entry dispatch table. After all records processed, flush any
     * remaining accumulated vertices.
     */
    int curr = s_translucent_head;

    while (curr >= 0) {
        TranslucentBatchEntry *entry = &s_translucent_pool[curr];
        if (entry->record) {
            TD5_PrimitiveCmd *cmd = (TD5_PrimitiveCmd *)entry->record;
            int opcode = cmd->dispatch_type;
            if (opcode >= 0 && opcode <= 6) {
                s_dispatch_table[opcode](cmd, NULL);
            }
        }
        curr = entry->next;
    }

    /* Flush remaining geometry */
    flush_immediate_internal();

    /* Reset translucent pipeline for next frame */
    td5_render_init_translucent_pipeline();
}

void td5_render_flush_immediate_batch(void)
{
    flush_immediate_internal();
}

/* --- Depth-Sorted Pipeline (4096 Buckets) --- */

void td5_render_queue_projected_entry(void *entry, int bucket, uint32_t flags, int texture_page)
{
    /*
     * QueueProjectedPrimitiveBucketEntry (0x43E550):
     * Insert a primitive entry into a specific depth bucket.
     */
    if (!entry) return;
    if (bucket < 0 || bucket >= DEPTH_BUCKET_COUNT) return;
    if (s_depth_entry_count >= DEPTH_ENTRY_POOL) return;

    int idx = s_depth_entry_count++;
    s_depth_entries[idx].prim_data    = entry;
    s_depth_entries[idx].flags        = flags;
    s_depth_entries[idx].texture_page = texture_page;
    s_depth_entries[idx].next         = s_depth_buckets[bucket];
    s_depth_buckets[bucket] = idx;
}

void td5_render_flush_projected_buckets(void)
{
    /*
     * FlushProjectedPrimitiveBuckets (0x43E2F0):
     * Iterate all 4096 buckets in order (back-to-front due to XOR inversion
     * during insertion). Process each linked-list entry:
     *
     * - Flag bit 31 clear: raw polygon with explicit vertex count
     * - Flag == 0x80000003: 3-vertex projected triangle
     * - Flag == 0x80000004: 4-vertex projected quad
     *
     * All ultimately go through clip_and_submit_polygon.
     */
    int flushed_entries = 0;

    for (int b = 0; b < DEPTH_BUCKET_COUNT; b++) {
        int idx = s_depth_buckets[b];
        while (idx >= 0 && idx < DEPTH_ENTRY_POOL) {
            DepthBucketEntry *de = &s_depth_entries[idx];
            flushed_entries++;

            if (de->prim_data) {
                /* Set texture page for this primitive */
                if (de->texture_page >= 0 && de->texture_page != s_current_texture_page) {
                    flush_immediate_internal();
                    s_previous_texture_page = s_current_texture_page;
                    s_current_texture_page  = de->texture_page;
                }

                TD5_MeshVertex *verts = (TD5_MeshVertex *)de->prim_data;
                uint32_t flags = de->flags;

                if (flags & 0x80000000u) {
                    /* Immediate primitive (bit 31 set) */
                    if (flags == 0x80000003u) {
                        clip_and_submit_polygon(verts, 3, de->texture_page);
                    } else if (flags == 0x80000004u) {
                        clip_and_submit_polygon(verts, 4, de->texture_page);
                    }
                } else if (flags == 0x3u) {
                    /* Batched billboard quad (type 3, 0x84 stride) */
                    clip_and_submit_polygon(verts, 3, de->texture_page);
                } else if (flags == 0x4u) {
                    /* Batched billboard fan (type 4, 0xB0 stride) */
                    clip_and_submit_polygon(verts, 4, de->texture_page);
                } else {
                    /* Raw polygon: vertex count encoded in low bits */
                    int vert_count = (int)(flags & 0x7F);
                    if (vert_count >= 3 && vert_count <= 8) {
                        clip_and_submit_polygon(verts, vert_count, de->texture_page);
                    }
                }
            }

            idx = de->next;
        }
    }

    /* Flush any remaining vertices */
    flush_immediate_internal();

    TD5_LOG_D(LOG_TAG,
              "projected buckets flushed: entries=%d",
              flushed_entries);

    /* Reset depth buckets for next frame */
    for (int i = 0; i < DEPTH_BUCKET_COUNT; i++) {
        s_depth_buckets[i] = -1;
    }
    s_depth_entry_count = 0;
}

/* --- Texture Cache --- */

void td5_render_reset_texture_cache(void)
{
    /*
     * ResetTexturePageCacheState (0x40BA60):
     * Full cache reset before loading a new track's textures.
     * Clears active count, zeros all slot status/age bytes.
     */
    s_texture_cache_active_count = 0;
    for (int i = 0; i < TEXTURE_CACHE_SLOTS; i++) {
        s_texture_cache[i].page_id        = -1;
        s_texture_cache[i].status          = 0;
        s_texture_cache[i].age             = 0;
        s_texture_cache[i].used_this_frame = 0;
    }
    s_current_texture_page  = -1;
    s_previous_texture_page = -1;
}

void td5_render_advance_texture_ages(void)
{
    /*
     * AdvanceTexturePageUsageAges (0x40BA10):
     * Called from EndRaceScene. Per texture page slot:
     * - If used this frame: reset age to 0, clear used flag
     * - If not used: increment age (saturate at 0xFF)
     * Drives LRU eviction when cache is full.
     */
    for (int i = 0; i < TEXTURE_CACHE_SLOTS; i++) {
        if (s_texture_cache[i].page_id < 0) continue;

        if (s_texture_cache[i].used_this_frame) {
            s_texture_cache[i].age = 0;
            s_texture_cache[i].used_this_frame = 0;
        } else {
            if (s_texture_cache[i].age < 0xFF) {
                s_texture_cache[i].age++;
            }
        }
    }
}

/* Switch the current render preset to match a tpage's transparency type.
 * Mirrors the case dispatch in BindRaceTexturePage @ 0x0040B660 which
 * reads the type byte from per-page metadata and routes to one of:
 *   0 → opaque (no blend, alpha test on)
 *   1,2 → alpha-blend (SRCALPHA / INVSRCALPHA)
 *   3 → additive (ONE / ONE) — street lights, headlight glows
 * Pages with unknown type stay on the caller's preset (default opaque). */
static void td5_render_apply_page_blend_preset(int page_id)
{
    int t = td5_asset_get_page_transparency(page_id);
    if (t < 0) return;
    switch (t) {
    case 3:
        td5_plat_render_set_preset(TD5_PRESET_ADDITIVE);
        break;
    case 1:
    case 2:
        td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_ANISO);
        break;
    case 0:
    default:
        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        break;
    }
}

int td5_render_bind_texture_page(int page_id)
{
    /*
     * BindRaceTexturePage (0x40B660):
     * Resolves page ID to cache slot, binds to GPU.
     * If page not resident, triggers texture streaming scheduler.
     * Returns 1 on success, 0 if page could not be bound.
     */
    if (page_id < 0) return 0;

    /* Check if already the current texture */
    if (page_id == s_current_texture_page) return 1;

    s_debug_texture_bind_calls++;

    /* Search cache for this page */
    int found_slot = -1;
    for (int i = 0; i < TEXTURE_CACHE_SLOTS; i++) {
        if (s_texture_cache[i].page_id == page_id) {
            found_slot = i;
            break;
        }
    }

    if (found_slot >= 0) {
        /* Page is resident: mark as used, bind */
        s_texture_cache[found_slot].used_this_frame = 1;
        s_previous_texture_page = s_current_texture_page;
        s_current_texture_page  = page_id;
        s_debug_texture_cache_hits++;

        td5_plat_render_bind_texture(found_slot);
        td5_render_apply_page_blend_preset(page_id);
        if ((g_tick_counter % 60u) == 0u) {
            TD5_LOG_D(LOG_TAG,
                      "texture bind: hit page=%d slot=%d active=%d",
                      page_id, found_slot, s_texture_cache_active_count);
        }
        return 1;
    }

    /* Page not resident: find a free slot or evict oldest (LRU) */
    int best_slot = -1;
    uint8_t oldest_age = 0;
    int evicted_page = -1;

    for (int i = 0; i < TEXTURE_CACHE_SLOTS; i++) {
        if (s_texture_cache[i].page_id < 0) {
            best_slot = i;
            break;
        }
        if (s_texture_cache[i].age > oldest_age) {
            oldest_age = s_texture_cache[i].age;
            best_slot  = i;
        }
    }

    if (best_slot < 0) return 0; /* cache completely full, no eviction possible */

    s_debug_texture_cache_misses++;
    if (s_texture_cache[best_slot].page_id >= 0) {
        evicted_page = s_texture_cache[best_slot].page_id;
        s_debug_texture_cache_evictions++;
    }

    /* Allocate/evict slot */
    s_texture_cache[best_slot].page_id        = page_id;
    s_texture_cache[best_slot].status          = 1;
    s_texture_cache[best_slot].age             = 0;
    s_texture_cache[best_slot].used_this_frame = 1;

    if (s_texture_cache_active_count < TEXTURE_CACHE_SLOTS)
        s_texture_cache_active_count++;

    s_previous_texture_page = s_current_texture_page;
    s_current_texture_page  = page_id;

    /* Bind (the actual texture upload happens in td5_asset streaming scheduler) */
    td5_plat_render_bind_texture(best_slot);
    td5_render_apply_page_blend_preset(page_id);
    if ((g_tick_counter % 60u) == 0u) {
        TD5_LOG_D(LOG_TAG,
                  "texture bind: miss page=%d slot=%d evicted=%d active=%d",
                  page_id, best_slot, evicted_page, s_texture_cache_active_count);
    }
    return 1;
}

/* --- Environment Map UV Generation (0x43DEC0) --- */

void td5_render_apply_mesh_projection_effect(TD5_MeshVertex *verts, int count, int mode)
{
    int i;
    if (!verts || count <= 0) return;

    for (i = 0; i < count; i++) {
        if (mode == 1) {
            /* Water: scrolling UV from world XZ + time */
            float wx = verts[i].view_x * 0.001f;
            float wz = verts[i].view_z * 0.001f;
            float time_ofs = (float)g_tick_counter * 0.0002f;
            verts[i].proj_u = wx + time_ofs;
            verts[i].proj_v = wz + time_ofs * 0.7f;
        } else if (mode == 2) {
            /* Chrome/envmap fallback from view-space direction. */
            verts[i].proj_u = verts[i].view_x * 0.5f + 0.5f;
            verts[i].proj_v = verts[i].view_y * 0.5f + 0.5f;
        }
    }
}

/* --- Cross-Fade Blending --- */

void td5_render_crossfade_surfaces(uint32_t *dst, const uint32_t *src_a,
                                    const uint32_t *src_b,
                                    int pixel_count, int alpha)
{
    int i;
    int inv_alpha;
    if (!dst || !src_a || !src_b || pixel_count <= 0) return;

    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    inv_alpha = 255 - alpha;

    for (i = 0; i < pixel_count; i++) {
        uint32_t a = src_a[i];
        uint32_t b = src_b[i];
        uint32_t r = ((((a >> 16) & 0xFF) * inv_alpha + ((b >> 16) & 0xFF) * alpha) >> 8) & 0xFF;
        uint32_t g = ((((a >> 8) & 0xFF) * inv_alpha + ((b >> 8) & 0xFF) * alpha) >> 8) & 0xFF;
        uint32_t bl = (((a & 0xFF) * inv_alpha + (b & 0xFF) * alpha) >> 8) & 0xFF;
        dst[i] = 0xFF000000u | (r << 16) | (g << 8) | bl;
    }
}

/* --- Vehicle Shadow Projection (0x40C120 inline) --- */

/**
 * Render a dark translucent ground quad under a vehicle.
 * Uses the currently loaded s_render_transform (actor rotation + camera basis).
 * Shadow is a flat rectangle in model space at Y=0 (approximately ground level).
 *
 * Original builds shadow from 4 wheel contact positions (actor+0x198..+0x1C4)
 * and projects them. This simplified version uses a fixed-size rectangle.
 */
static void render_vehicle_shadow_quad(void)
{
    const float hw = 2.2f;   /* half-width (roughly car width / 2) */
    const float hl = 4.5f;   /* half-length (roughly car length / 2) */
    const float y_off = 0.3f; /* small Y offset above ground to prevent z-fighting */

    /* Shadow corners in model space (flat quad at car base level).
     * Winding: CW when viewed from above (+Y direction). */
    static const float corners[4][3] = {
        { -2.2f, 0.3f,  4.5f },  /* front-left */
        {  2.2f, 0.3f,  4.5f },  /* front-right */
        {  2.2f, 0.3f, -4.5f },  /* back-right */
        { -2.2f, 0.3f, -4.5f }   /* back-left */
    };
    (void)hw; (void)hl; (void)y_off;

    const float *m = s_render_transform.m;
    TD5_D3DVertex verts[4];

    for (int i = 0; i < 4; i++) {
        float px = corners[i][0], py = corners[i][1], pz = corners[i][2];
        float vx = px * m[0] + py * m[1] + pz * m[2] + m[9];
        float vy = px * m[3] + py * m[4] + pz * m[5] + m[10];
        float vz = px * m[6] + py * m[7] + pz * m[8] + m[11];

        if (vz <= s_near_clip) return; /* bail if any vertex behind camera */

        float inv_z = 1.0f / vz;
        verts[i].screen_x = -vx * s_focal_length * inv_z + s_center_x;
        verts[i].screen_y = -vy * s_focal_length * inv_z + s_center_y;
        verts[i].depth_z  = vz * (1.0f / s_far_clip);
        verts[i].rhw      = inv_z;
        verts[i].diffuse  = 0x60000000u; /* semi-transparent black (alpha=0x60) */
        verts[i].specular = 0;
        verts[i].tex_u    = 0.0f;
        verts[i].tex_v    = 0.0f;
    }

    uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    flush_immediate_internal();
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    td5_plat_render_bind_texture(-1);
    td5_plat_render_draw_tris(verts, 4, indices, 6);
}

/* --- Vehicle Wheel Billboards (0x446F00) --- */

/**
 * Wheel rendering (0x446F00 + 0x446A70).
 *
 * Original renders 8 quad strips per wheel (tire sidewall) plus a flat
 * hub-cap quad, using textures from static.hed page 5:
 *   "WHEELS"  — tire sidewall (pos 0,0 size 256x128)
 *   "INWHEEL" — hub-cap disc  (pos 0,192 size 16x16)
 *
 * Dimensions from cardef (RE confirmed at 0x446E30-0x446E3C):
 *   rim_radius = cardef+0x82 * 0.76171875 (195/256, DAT_0045D7AC)
 *   axle_halfw = cardef+0x84 (raw int16, no scaling)
 *
 * Ring: 9 vertices at 45-degree steps (8 segments + closing vertex).
 * Hub-cap: flat quad perpendicular to axle, sized by rim_radius.
 */
#define WHEEL_SEGMENTS       8
#define WHEEL_RADIUS_SCALE   0.76171875f  /* 195/256, from DAT_0045D7AC */
#define WHEEL_RADIUS_DEFAULT 110.0f       /* fallback: 0x90 * 0.76171875 */
#define WHEEL_HALFW_DEFAULT  28.0f        /* fallback axle half-width */

/* Cached WHEELS/INWHEEL lookups from static.hed */
static int   s_wheel_tex_page = -1;
static float s_wheels_u0, s_wheels_v0, s_wheels_u1, s_wheels_v1;  /* tire sidewall UVs */
static float s_inwheel_u0, s_inwheel_v0, s_inwheel_u1, s_inwheel_v1;  /* hub-cap UVs */
static int   s_wheel_lookup_done = 0;

static void wheel_lookup_static_hed(void)
{
    s_wheel_lookup_done = 1;

    /* Use td5_asset_find_atlas_entry which searches the loaded s_atlas_table
     * (populated from static.hed at init). g_static_hed_entries is NULL —
     * the raw HED array is not stored separately. */
    TD5_AtlasEntry *wheels = td5_asset_find_atlas_entry(NULL, "WHEELS");
    if (wheels && wheels->texture_page > 0) {
        s_wheel_tex_page = wheels->texture_page;
        /* Normalize WHEELS UVs: pos=(0,0) size=(256,128) on 256x256 page */
        int tw = 256, th = 256;
        td5_plat_render_get_texture_dims(wheels->texture_page, &tw, &th);
        s_wheels_u0 = ((float)wheels->atlas_x + 0.5f) / (float)tw;
        s_wheels_v0 = ((float)wheels->atlas_y + 0.5f) / (float)th;
        s_wheels_u1 = ((float)(wheels->atlas_x + wheels->width) - 0.5f) / (float)tw;
        s_wheels_v1 = ((float)(wheels->atlas_y + wheels->height) - 0.5f) / (float)th;
        TD5_LOG_I(LOG_TAG, "wheel: WHEELS page=%d pos=(%d,%d) size=(%d,%d) UV=(%.4f,%.4f)-(%.4f,%.4f)",
                  s_wheel_tex_page, wheels->atlas_x, wheels->atlas_y,
                  wheels->width, wheels->height,
                  s_wheels_u0, s_wheels_v0, s_wheels_u1, s_wheels_v1);
    } else {
        /* Fallback: WHEELS at (0,0) 256x128 on 256x256 page */
        s_wheels_u0 = 0.5f / 256.0f;
        s_wheels_v0 = 0.5f / 256.0f;
        s_wheels_u1 = 255.5f / 256.0f;
        s_wheels_v1 = 127.5f / 256.0f;
        TD5_LOG_W(LOG_TAG, "wheel: WHEELS not found in static.hed atlas");
    }

    TD5_AtlasEntry *inwheel = td5_asset_find_atlas_entry(NULL, "INWHEEL");
    if (inwheel && inwheel->texture_page > 0) {
        /* Pixel UVs with half-pixel inset, normalized to [0,1] for D3D11 */
        int tw = 256, th = 256;
        if (s_wheel_tex_page >= 0)
            td5_plat_render_get_texture_dims(s_wheel_tex_page, &tw, &th);
        s_inwheel_u0 = ((float)inwheel->atlas_x + 0.5f) / (float)tw;
        s_inwheel_v0 = ((float)inwheel->atlas_y + 0.5f) / (float)th;
        s_inwheel_u1 = ((float)(inwheel->atlas_x + inwheel->width) - 0.5f) / (float)tw;
        s_inwheel_v1 = ((float)(inwheel->atlas_y + inwheel->height) - 0.5f) / (float)th;
        TD5_LOG_I(LOG_TAG, "wheel: INWHEEL UV=(%.4f,%.4f)-(%.4f,%.4f)",
                  s_inwheel_u0, s_inwheel_v0, s_inwheel_u1, s_inwheel_v1);
    } else {
        /* Fallback: INWHEEL at (0,192) 16x16 on a 256x256 page */
        s_inwheel_u0 = 0.5f / 256.0f;
        s_inwheel_v0 = 192.5f / 256.0f;
        s_inwheel_u1 = 15.5f / 256.0f;
        s_inwheel_v1 = 207.5f / 256.0f;
        TD5_LOG_W(LOG_TAG, "wheel: INWHEEL not found, using fallback UVs");
    }
}

static void render_vehicle_wheel_billboards(TD5_Actor *actor, int slot)
{
    const float *m = s_render_transform.m;

    if (!s_wheel_lookup_done)
        wheel_lookup_static_hed();

    /* Read wheel dimensions from cardef (RE: 0x446E30-0x446E3C).
     * cardef+0x82 -> rim_radius = value * 195/256
     * cardef+0x84 -> axle_halfw = raw value (no scaling) */
    float rim_radius = WHEEL_RADIUS_DEFAULT;
    float axle_halfw = WHEEL_HALFW_DEFAULT;
    if (actor->car_definition_ptr) {
        int16_t r = *(int16_t *)((uint8_t *)actor->car_definition_ptr + 0x82);
        if (r > 0) rim_radius = (float)r * WHEEL_RADIUS_SCALE;
        int16_t hw = *(int16_t *)((uint8_t *)actor->car_definition_ptr + 0x84);
        if (hw > 0) axle_halfw = (float)hw;
    }

    /* 9-vertex rim circle (8 segments + closing vertex, 45-degree steps) */
    float ring_y[9], ring_z[9];
    for (int i = 0; i <= WHEEL_SEGMENTS; i++) {
        float angle = (float)i * ((float)M_PI / 4.0f);
        ring_y[i] = cosf(angle) * rim_radius;
        ring_z[i] = sinf(angle) * rim_radius;
    }

    int tex_page = s_wheel_tex_page;

    static int s_wheel_log_counter = 0;
    if (slot == 0 && (s_wheel_log_counter++ % 60) == 0) {
        TD5_LOG_I(LOG_TAG, "wheel: slot=%d radius=%.1f halfw=%.1f steer_cmd=%d",
                  slot, rim_radius, axle_halfw, actor->steering_command);
    }

    for (int w = 0; w < 4; w++) {
        float wx = (float)actor->wheel_display_angles[w][0];
        float wy = (float)actor->wheel_display_angles[w][1];
        float wz = (float)actor->wheel_display_angles[w][2];

        if (wx == 0.0f && wy == 0.0f && wz == 0.0f)
            continue;

        /* Tire ring X offsets: inner = inboard (towards car center),
         * outer = outboard (away from car, where hub-cap is visible).
         * Left wheels (even, negative X): outboard = -halfw.
         * Right wheels (odd, positive X): outboard = +halfw. */
        float inner_off = (w & 1) ? -axle_halfw :  axle_halfw;
        float outer_off = (w & 1) ?  axle_halfw : -axle_halfw;

        /* Front wheel visual steering yaw (0x446F00 uses actor+0x340/0x342).
         * steering_command >> 8 gives 12-bit angle; convert to radians.
         * Only front wheels (w=0,1) get steering rotation. */
        float cos_s = 1.0f, sin_s = 0.0f;
        if (w < 2) {
            float steer_rad = (float)(actor->steering_command >> 8)
                            * ((float)M_PI / 2048.0f);
            cos_s = cosf(steer_rad);
            sin_s = sinf(steer_rad);
        }

        /* 18 vertices: inner ring (0..8) + outer ring (9..17) */
        TD5_D3DVertex verts[18];
        int all_visible = 1;

        for (int i = 0; i <= WHEEL_SEGMENTS; i++) {
            float cy = ring_y[i];
            float cz = ring_z[i];

            /* UV interpolation: map segment i across WHEELS atlas region */
            float u_t = (float)i / (float)WHEEL_SEGMENTS;
            float seg_u = s_wheels_u0 + u_t * (s_wheels_u1 - s_wheels_u0);

            /* Inner ring vertex (with front-wheel steering yaw) */
            {
                float dx = inner_off, dz = cz;
                float rx = dx * cos_s - dz * sin_s;
                float rz = dx * sin_s + dz * cos_s;
                float px = wx + rx, py = wy + cy, pz = wz + rz;
                float vx = px*m[0] + py*m[1] + pz*m[2] + m[9];
                float vy = px*m[3] + py*m[4] + pz*m[5] + m[10];
                float vz = px*m[6] + py*m[7] + pz*m[8] + m[11];
                if (vz <= s_near_clip) { all_visible = 0; break; }
                float inv_z = 1.0f / vz;
                verts[i].screen_x = -vx * s_focal_length * inv_z + s_center_x;
                verts[i].screen_y = -vy * s_focal_length * inv_z + s_center_y;
                verts[i].depth_z  = vz * (1.0f / s_far_clip);
                verts[i].rhw      = inv_z;
                verts[i].diffuse  = 0xFFFFFFFFu;
                verts[i].specular = 0;
                verts[i].tex_u = seg_u;
                verts[i].tex_v = s_wheels_v0;
            }

            /* Outer ring vertex (with front-wheel steering yaw) */
            {
                float dx = outer_off, dz = cz;
                float rx = dx * cos_s - dz * sin_s;
                float rz = dx * sin_s + dz * cos_s;
                float px = wx + rx, py = wy + cy, pz = wz + rz;
                float vx = px*m[0] + py*m[1] + pz*m[2] + m[9];
                float vy = px*m[3] + py*m[4] + pz*m[5] + m[10];
                float vz = px*m[6] + py*m[7] + pz*m[8] + m[11];
                if (vz <= s_near_clip) { all_visible = 0; break; }
                float inv_z = 1.0f / vz;
                verts[9+i].screen_x = -vx * s_focal_length * inv_z + s_center_x;
                verts[9+i].screen_y = -vy * s_focal_length * inv_z + s_center_y;
                verts[9+i].depth_z  = vz * (1.0f / s_far_clip);
                verts[9+i].rhw      = inv_z;
                verts[9+i].diffuse  = 0xFFFFFFFFu;
                verts[9+i].specular = 0;
                verts[9+i].tex_u = seg_u;
                verts[9+i].tex_v = s_wheels_v1;
            }
        }

        if (!all_visible) continue;

        /* Tire sidewall: 8 quads, double-sided = 96 indices */
        uint16_t indices[96];
        int idx = 0;
        for (int i = 0; i < WHEEL_SEGMENTS; i++) {
            int i0 = i, i1 = i + 1;
            int o0 = 9 + i, o1 = 9 + i + 1;
            indices[idx++] = (uint16_t)i0; indices[idx++] = (uint16_t)o0; indices[idx++] = (uint16_t)o1;
            indices[idx++] = (uint16_t)i0; indices[idx++] = (uint16_t)o1; indices[idx++] = (uint16_t)i1;
            indices[idx++] = (uint16_t)i0; indices[idx++] = (uint16_t)o1; indices[idx++] = (uint16_t)o0;
            indices[idx++] = (uint16_t)i0; indices[idx++] = (uint16_t)i1; indices[idx++] = (uint16_t)o1;
        }

        flush_immediate_internal();
        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        td5_plat_render_bind_texture(tex_page);
        td5_plat_render_draw_tris(verts, 18, indices, idx);

        /* Hub-cap: flat quad at the outer face, sized by rim_radius.
         * Original rotates corners by steering angles (actor+0x340/0x342).
         * Front wheels apply steering yaw via cos_s/sin_s computed above. */
        {
            float ho = outer_off;
            float corner_offsets[4][3] = {
                { ho, +rim_radius, -rim_radius },
                { ho, +rim_radius, +rim_radius },
                { ho, -rim_radius, +rim_radius },
                { ho, -rim_radius, -rim_radius },
            };
            float corners[4][3];
            for (int c = 0; c < 4; c++) {
                float dx = corner_offsets[c][0], dz = corner_offsets[c][2];
                corners[c][0] = wx + dx * cos_s - dz * sin_s;
                corners[c][1] = wy + corner_offsets[c][1];
                corners[c][2] = wz + dx * sin_s + dz * cos_s;
            }
            float hub_uv[4][2] = {
                { s_inwheel_u0, s_inwheel_v0 }, { s_inwheel_u1, s_inwheel_v0 },
                { s_inwheel_u1, s_inwheel_v1 }, { s_inwheel_u0, s_inwheel_v1 },
            };

            TD5_D3DVertex hub[4];
            int hub_ok = 1;
            for (int c = 0; c < 4; c++) {
                float px = corners[c][0], py = corners[c][1], pz = corners[c][2];
                float vx = px*m[0] + py*m[1] + pz*m[2] + m[9];
                float vy = px*m[3] + py*m[4] + pz*m[5] + m[10];
                float vz = px*m[6] + py*m[7] + pz*m[8] + m[11];
                if (vz <= s_near_clip) { hub_ok = 0; break; }
                float inv_z = 1.0f / vz;
                hub[c].screen_x = -vx * s_focal_length * inv_z + s_center_x;
                hub[c].screen_y = -vy * s_focal_length * inv_z + s_center_y;
                hub[c].depth_z  = vz * (1.0f / s_far_clip);
                hub[c].rhw      = inv_z;
                hub[c].diffuse  = 0xFFFFFFFFu;
                hub[c].specular = 0;
                hub[c].tex_u    = hub_uv[c][0];
                hub[c].tex_v    = hub_uv[c][1];
            }
            if (hub_ok) {
                uint16_t hub_idx[12] = { 0,1,2, 0,2,3, 0,2,1, 0,3,2 };
                flush_immediate_internal();
                td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
                td5_plat_render_bind_texture(tex_page);
                td5_plat_render_draw_tris(hub, 4, hub_idx, 12);
            }
        }
    }
}

void td5_render_project_vehicle_shadow(float pos_x, float pos_y, float pos_z,
                                        float half_w, float half_l, int tex_page)
{
    (void)pos_x; (void)pos_y; (void)pos_z;
    (void)half_w; (void)half_l; (void)tex_page;
    /* Legacy stub — shadow rendering now handled by render_vehicle_shadow_quad() */
}

/* --- Sky --- */

#define SKY_TEXTURE_PAGE 1020

static int             s_sky_loaded;
static int             s_sky_page;
static TD5_MeshHeader *s_sky_mesh = NULL;   /* sky.prr dome mesh */

void td5_render_load_sky(const char *path)
{
    void *pixels = NULL;
    int w = 0, h = 0;

    /* Reset so a new sky is loaded each race */
    s_sky_loaded = 0;

    /* td5_asset_decode_png_rgba32 handles R↔B swap to BGRA internally */
    if (td5_asset_decode_png_rgba32(path, &pixels, &w, &h)) {
        if (td5_plat_render_upload_texture(SKY_TEXTURE_PAGE, pixels, w, h, 2)) {
            s_sky_loaded = 1;
            s_sky_page = SKY_TEXTURE_PAGE;
            TD5_LOG_I(RENDER_LOG_TAG, "sky loaded: %s (%dx%d)", path, w, h);
        }
        free(pixels);
    } else {
        TD5_LOG_W(RENDER_LOG_TAG, "sky not found: %s", path);
    }

    /* --- Load sky.prr dome mesh [CONFIRMED @ 0x0042af5d-0x0042afd4] ---
     * Original loads from static.zip, processes with FUN_0040ac00 (mesh prepare),
     * and forces texture page to 0x0403. We load from extracted re/assets. */
    if (!s_sky_mesh) {
        const char *prr_path = "re/assets/static/sky.prr";
        FILE *f = fopen(prr_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz >= (long)sizeof(TD5_MeshHeader)) {
                void *buf = malloc((size_t)sz);
                if (buf && (long)fread(buf, 1, (size_t)sz, f) == sz) {
                    s_sky_mesh = (TD5_MeshHeader *)buf;
                    td5_track_prepare_mesh_resource(s_sky_mesh);
                    /* Patch command texture page to sky TGA page */
                    TD5_PrimitiveCmd *cmds = (TD5_PrimitiveCmd *)(uintptr_t)s_sky_mesh->commands_offset;
                    for (int c = 0; c < s_sky_mesh->command_count; c++)
                        cmds[c].texture_page_id = (int16_t)SKY_TEXTURE_PAGE;
                    TD5_LOG_I(RENDER_LOG_TAG,
                              "sky.prr loaded: %ld bytes, %d verts, %d cmds",
                              sz, s_sky_mesh->total_vertex_count, s_sky_mesh->command_count);
                } else {
                    free(buf);
                }
            }
            fclose(f);
        }
    }
}

void td5_render_draw_sky(void)
{
    if (!s_sky_loaded) return;

    /* --- 3D dome rendering (sky.prr) [CONFIRMED @ 0x0042bdf7-0x0042c044] ---
     * Original applies camera rotation only (no translation) to sky dome mesh,
     * then dispatches via the standard mesh pipeline. */
    if (s_sky_mesh) {
        TD5_Mat3x3 sky_rot;

        /* Camera basis IS the rotation — sky has identity model rotation */
        for (int i = 0; i < 9; i++)
            sky_rot.m[i] = s_camera_basis[i];

        td5_render_load_rotation(&sky_rot);
        /* Sky dome is camera-centered: translation must be zero in view space.
         * Do NOT use td5_render_load_translation() — it subtracts camera_pos,
         * which would offset the dome to world origin.
         * Original LoadRenderTranslation (0x43DC20) is a pure memcpy with no
         * camera subtraction; the subtraction lives in ApplyMeshResourceRenderTransform. */
        s_render_transform.m[9]  = 0.0f;
        s_render_transform.m[10] = 0.0f;
        s_render_transform.m[11] = 0.0f;
        td5_render_transform_mesh_vertices(s_sky_mesh);
        td5_render_prepared_mesh(s_sky_mesh);
        s_scene_has_renderer_geometry = 1;
        return;
    }

    /* --- Fallback: 2D panoramic quad (when sky.prr unavailable) --- */
    {
        float sw = (float)s_viewport_width;
        float sh = (float)s_viewport_height;

        /* Compute horizontal UV offset from camera yaw for panoramic scrolling */
        float yaw_frac = 0.0f;
        {
            float fx = s_camera_basis[6];
            float fz = s_camera_basis[8];
            float len = sqrtf(fx * fx + fz * fz);
            if (len > 0.001f) {
                float angle = atan2f(fx, fz);
                yaw_frac = angle / (2.0f * 3.14159265f) + 0.5f;
            }
        }

        float u0 = yaw_frac;
        float u1 = yaw_frac + 1.0f;

        TD5_D3DVertex verts[4];
        uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
        memset(verts, 0, sizeof(verts));

        verts[0].screen_x = 0;  verts[0].screen_y = 0;
        verts[0].depth_z = 0.999f; verts[0].rhw = 1.0f;
        verts[0].diffuse = 0xFFFFFFFF; verts[0].tex_u = u0; verts[0].tex_v = 0.0f;

        verts[1].screen_x = sw;  verts[1].screen_y = 0;
        verts[1].depth_z = 0.999f; verts[1].rhw = 1.0f;
        verts[1].diffuse = 0xFFFFFFFF; verts[1].tex_u = u1; verts[1].tex_v = 0.0f;

        verts[2].screen_x = sw;  verts[2].screen_y = sh;
        verts[2].depth_z = 0.999f; verts[2].rhw = 1.0f;
        verts[2].diffuse = 0xFFFFFFFF; verts[2].tex_u = u1; verts[2].tex_v = 1.0f;

        verts[3].screen_x = 0;   verts[3].screen_y = sh;
        verts[3].depth_z = 0.999f; verts[3].rhw = 1.0f;
        verts[3].diffuse = 0xFFFFFFFF; verts[3].tex_u = u0; verts[3].tex_v = 1.0f;

        flush_immediate_internal();
        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        td5_plat_render_bind_texture(s_sky_page);
        td5_plat_render_draw_tris(verts, 4, indices, 6);
        s_scene_has_renderer_geometry = 1;
    }
}

void td5_render_advance_sky_rotation(void)
{
    /*
     * AdvanceGlobalSkyRotation (0x43D7C0):
     * Increment by 0x400 per non-paused tick (12-bit fixed-point angle).
     * Full rotation = 0x1000 (4096).
     */
    s_sky_rotation_angle = (s_sky_rotation_angle + TD5_ANGLE_QUARTER) & 0xFFFFF;
}

/* --- Billboard Animation --- */

void td5_render_advance_billboard_anims(void)
{
    /*
     * AdvanceWorldBillboardAnimations (0x43CDC0):
     * Advance animation phase by +0x10 per tick for billboard animation pool.
     * Drives texture frame selection for animated signs/flags.
     */
    s_billboard_anim_phase += 0x20;
}

/* ========================================================================
 * 12-bit Trigonometry (migrated from td5re_stubs.c)
 *
 * Original game uses lookup-table trig with 12-bit angles (0-4095 = 360).
 * We provide real implementations using standard math.
 * ======================================================================== */

float CosFloat12bit(unsigned int angle) {
    return (float)cos((double)(angle & 0xFFF) * (2.0 * M_PI / 4096.0));
}

float SinFloat12bit(int angle) {
    return (float)sin((double)(angle & 0xFFF) * (2.0 * M_PI / 4096.0));
}

int CosFixed12bit(unsigned int angle) {
    return (int)(cos((double)(angle & 0xFFF) * (2.0 * M_PI / 4096.0)) * 4096.0);
}

int SinFixed12bit(int angle) {
    return (int)(sin((double)(angle & 0xFFF) * (2.0 * M_PI / 4096.0)) * 4096.0);
}

int AngleFromVector12(int x, int z) {
    double rad = atan2((double)x, (double)z);
    int angle = (int)(rad * (4096.0 / (2.0 * M_PI)));
    return angle & 0xFFF;
}

float td5_cos_12bit(uint32_t angle) {
    return CosFloat12bit(angle);
}

float td5_sin_12bit(uint32_t angle) {
    return SinFloat12bit((int)angle);
}

/* ========================================================================
 * Matrix / Vector Operations (migrated from td5re_stubs.c)
 * ======================================================================== */

void MultiplyRotationMatrices3x3(float *A, float *B, float *out) {
    int i, j, k;
    float tmp[9];
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) {
            tmp[i*3+j] = 0.0f;
            for (k = 0; k < 3; k++)
                tmp[i*3+j] += A[i*3+k] * B[k*3+j];
        }
    memcpy(out, tmp, 9 * sizeof(float));
}

void TransformVector3ByBasis(float *matrix, void *vec, int *out) {
    /*
     * 0x42dbd0 -- Transform a short[3] vector by a 3x3 rotation matrix,
     * producing an int[3] result.  The matrix is row-major float[9+].
     */
    short *v = (short *)vec;
    if (!out) return;
    if (!matrix || !v) { out[0] = 0; out[1] = 0; out[2] = 0; return; }

    float fx = (float)v[0];
    float fy = (float)v[1];
    float fz = (float)v[2];

    out[0] = (int)(matrix[0] * fx + matrix[1] * fy + matrix[2] * fz);
    out[1] = (int)(matrix[3] * fx + matrix[4] * fy + matrix[5] * fz);
    out[2] = (int)(matrix[6] * fx + matrix[7] * fy + matrix[8] * fz);
}

void BuildRotationMatrixFromAngles(float *out, short *angles) {
    /*
     * 0x42e1e0 -- Build a 3x3 rotation matrix from 12-bit Euler angles
     * (pitch, yaw, roll).  Uses the same axis convention as
     * BuildCameraBasisFromAngles: yaw -> pitch -> roll.
     *
     * angles[0] = pitch, angles[1] = yaw, angles[2] = roll
     * All in 12-bit fixed-point (0-4095 = 0-360 degrees).
     *
     * NOTE: The original binary's trig lookup at 0x40a6a0 is a cosine
     * table (table[0]=1), and 0x40a6c0 offsets by -1024 giving sine.
     * Our stubs label them backwards (SinFloat12bit=sin, CosFloat12bit=cos).
     * The matrix slot pattern was decompiled from the original where the
     * "first" trig call (func_A) returns cos.  We swap s/c here to match:
     *   s = CosFloat12bit (= original func_B = sin)
     *   c = SinFloat12bit (= original func_A = cos)
     * so the rest of the matrix construction stays correct.
     */
    float rot[9];
    float s, c;

    if (!out || !angles) return;

    /* Start with identity */
    out[0] = 1.0f; out[1] = 0.0f; out[2] = 0.0f;
    out[3] = 0.0f; out[4] = 1.0f; out[5] = 0.0f;
    out[6] = 0.0f; out[7] = 0.0f; out[8] = 1.0f;

    /* Yaw (angles[1]): rotate around Y axis */
    s = CosFloat12bit((unsigned int)(unsigned short)angles[1]);
    c = SinFloat12bit(angles[1]);
    rot[4] = 1.0f;
    rot[3] = 0.0f; rot[5] = 0.0f;
    rot[1] = 0.0f; rot[7] = 0.0f;
    rot[0] = s;  rot[8] = s;
    rot[2] = c;  rot[6] = -c;
    MultiplyRotationMatrices3x3(rot, out, out);

    /* Pitch (angles[0]): rotate around X axis */
    s = CosFloat12bit((unsigned int)(unsigned short)angles[0]);
    c = SinFloat12bit(angles[0]);
    rot[0] = 1.0f;
    rot[1] = 0.0f; rot[2] = 0.0f;
    rot[3] = 0.0f; rot[6] = 0.0f;
    rot[4] = s;  rot[8] = s;
    rot[7] = c;  rot[5] = -c;
    MultiplyRotationMatrices3x3(rot, out, out);

    /* Roll (angles[2]): rotate around Z axis */
    s = CosFloat12bit((unsigned int)(unsigned short)angles[2]);
    c = SinFloat12bit(angles[2]);
    rot[8] = 1.0f;
    rot[2] = 0.0f; rot[5] = 0.0f;
    rot[6] = 0.0f; rot[7] = 0.0f;
    rot[0] = s;  rot[4] = s;
    rot[3] = c;  rot[1] = -c;
    MultiplyRotationMatrices3x3(rot, out, out);
}

/*
 * Static matrix loaded by LoadRenderRotationMatrix for use by
 * ConvertFloatVec3ToShortAngles.  This mirrors the original engine's
 * global at ~0x43DA80 target.
 */
static float s_loaded_render_matrix[12] = {
    1,0,0, 0,1,0, 0,0,1, 0,0,0
};

void ConvertFloatVec3ToShortAngles(short *in, short *out) {
    /*
     * 0x42e2e0 -- Transform a short[3] direction vector through the
     * currently loaded render rotation matrix and store the result as
     * short[3].  Despite the misleading name, this is a matrix*vector
     * transform, not a unit conversion.
     */
    if (!out) return;
    if (!in) { out[0] = 0; out[1] = 0; out[2] = 0; return; }

    float fx = (float)in[0];
    float fy = (float)in[1];
    float fz = (float)in[2];

    out[0] = (short)(int)(s_loaded_render_matrix[0] * fx +
                          s_loaded_render_matrix[1] * fy +
                          s_loaded_render_matrix[2] * fz);
    out[1] = (short)(int)(s_loaded_render_matrix[3] * fx +
                          s_loaded_render_matrix[4] * fy +
                          s_loaded_render_matrix[5] * fz);
    out[2] = (short)(int)(s_loaded_render_matrix[6] * fx +
                          s_loaded_render_matrix[7] * fy +
                          s_loaded_render_matrix[8] * fz);
}

void LoadRenderRotationMatrix(float *matrix) {
    /*
     * 0x43da80 -- Load a rotation matrix (float[12] = 3x3 + translation)
     * into the static render matrix used by ConvertFloatVec3ToShortAngles.
     * Only the 3x3 rotation part (first 9 floats) is needed for the
     * direction transform; we copy all 12 for completeness.
     */
    if (!matrix) return;
    memcpy(s_loaded_render_matrix, matrix, 12 * sizeof(float));
}

/* ========================================================================
 * Render Pipeline Helpers (migrated from td5re_stubs.c)
 * ======================================================================== */

typedef struct TD5_RenderSpriteQuadParams {
    void     *dest;
    int       mode_flags;
    float     scr_x[4];
    float     scr_y[4];
    float     depth_z[4];
    float     tex_u[4];
    float     tex_v[4];
    uint32_t  diffuse[4];
    int       texture_page;
    int       reserved;
} TD5_RenderSpriteQuadParams;

typedef struct TD5_RenderSpriteQuad {
    int      geometry_ptr;
    int      vertex_count;
    float    v0_x, v0_y, v0_z, v0_rhw;
    uint32_t v0_color;
    float    v0_u, v0_v;
    float    v1_x, v1_y, v1_z, v1_rhw;
    uint32_t v1_color;
    float    v1_u, v1_v;
    float    v2_x, v2_y, v2_z, v2_rhw;
    uint32_t v2_color;
    float    v2_u, v2_v;
    float    v3_x, v3_y, v3_z, v3_rhw;
    uint32_t v3_color;
    float    v3_u, v3_v;
    float    tex_u0, tex_v0;
    float    tex_u1, tex_v1;
    float    quad_width;
    float    quad_height;
    float    texture_page;
    uint8_t  padding[0xB8 - 0x94];
} TD5_RenderSpriteQuad;

void td5_render_build_sprite_quad(int *params) {
    const TD5_RenderSpriteQuadParams *src = (const TD5_RenderSpriteQuadParams *)params;
    TD5_RenderSpriteQuad *dst;
    float z;
    float rhw;

    if (!src || !src->dest) {
        return;
    }

    dst = (TD5_RenderSpriteQuad *)src->dest;

    if (src->mode_flags != 2) {
        z = src->depth_z[0];
        rhw = (z > 0.0f) ? (1.0f / z) : 1.0f;

        dst->geometry_ptr = 0;
        dst->vertex_count = 4;

        dst->v0_x = src->scr_x[0]; dst->v0_y = src->scr_y[0];
        dst->v1_x = src->scr_x[3]; dst->v1_y = src->scr_y[3];
        dst->v2_x = src->scr_x[2]; dst->v2_y = src->scr_y[2];
        dst->v3_x = src->scr_x[1]; dst->v3_y = src->scr_y[1];

        dst->v0_z = dst->v1_z = dst->v2_z = dst->v3_z = z;
        dst->v0_rhw = dst->v1_rhw = dst->v2_rhw = dst->v3_rhw = rhw;
    }

    dst->v0_color = src->diffuse[0];
    dst->v1_color = src->diffuse[3];
    dst->v2_color = src->diffuse[2];
    dst->v3_color = src->diffuse[1];

    dst->v0_u = src->tex_u[0]; dst->v0_v = src->tex_v[0];
    dst->v1_u = src->tex_u[3]; dst->v1_v = src->tex_v[3];
    dst->v2_u = src->tex_u[2]; dst->v2_v = src->tex_v[2];
    dst->v3_u = src->tex_u[1]; dst->v3_v = src->tex_v[1];

    dst->tex_u0 = src->tex_u[0];
    dst->tex_v0 = src->tex_v[0];
    dst->tex_u1 = src->tex_u[2];
    dst->tex_v1 = src->tex_v[2];
    dst->quad_width = src->scr_x[2] - src->scr_x[0];
    dst->quad_height = src->scr_y[1] - src->scr_y[0];
    dst->texture_page = (float)src->texture_page;
}

void td5_render_submit_translucent(uint16_t *quad_data) {
    float *fdata;
    TD5_D3DVertex verts[4];
    uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    int tex_page;
    int i;

    if (!quad_data) {
        return;
    }

    /*
     * HUD translucent quads are already emitted as pre-transformed 0xB8 sprite
     * records. They are not TD5_PrimitiveCmd batches, so forwarding them into
     * td5_render_queue_translucent_batch() makes the batch parser read garbage
     * dispatch_type state and crash after the first frame.
     */
    fdata = (float *)quad_data;
    for (i = 0; i < 4; i++) {
        int base = 2 + i * 7;
        verts[i].screen_x = fdata[base + 0];
        verts[i].screen_y = fdata[base + 1];
        verts[i].depth_z = fdata[base + 2];
        verts[i].rhw = fdata[base + 3];
        verts[i].diffuse = *(uint32_t *)&fdata[base + 4];
        verts[i].specular = 0;
        verts[i].tex_u = fdata[base + 5];
        verts[i].tex_v = fdata[base + 6];
    }

    tex_page = (int)(*(float *)((uint8_t *)quad_data + 0x90));
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    td5_plat_render_bind_texture(tex_page);
    td5_plat_render_draw_tris(verts, 4, indices, 6);
}

void td5_render_set_clip_rect(float left, float right, float top, float bottom) {
    (void)left; (void)right; (void)top; (void)bottom;
}

void td5_render_set_projection_center(float cx, float cy) {
    (void)cx; (void)cy;
}

void td5_render_radial_pulse(float dt) { (void)dt; }

/* ========================================================================
 * 4-Pass Race Rendering (0x40B070 -- SetRaceRenderStatePreset)
 *
 * The original called a render state function 4 times per frame with pass IDs:
 *   Pass 0 (SKY):     texture blend = MODULATEALPHA, alpha blend = OFF
 *   Pass 1 (OPAQUE):  texture blend = COPY, alpha blend = ON
 *   Pass 3 (ALPHA):   texture blend = COPY, alpha blend = OFF
 *
 * In the D3D11 wrapper this maps to different blend/preset combinations.
 * ======================================================================== */

void td5_render_set_race_pass(TD5_RaceRenderPass pass)
{
    /* Flush any pending geometry before state change */
    flush_immediate_internal();

    switch (pass) {
    case TD5_RACE_PASS_SKY:
        /* Pass 0: sky dome -- MODULATEALPHA blend mode, no alpha blending */
        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_ANISO);
        break;

    case TD5_RACE_PASS_OPAQUE:
        /* Pass 1: opaque geometry + overlays -- alpha blend ON */
        td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
        break;

    case TD5_RACE_PASS_ALPHA:
        /* Pass 3: alpha effects -- alpha blend OFF, copy mode */
        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        break;

    default:
        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        break;
    }
}

/* ========================================================================
 * Per-Tick Fog Fade (0x40A490)
 *
 * Manages fog transition during scene changes. The transition_counter
 * counts down from a starting value, dividing by 10240 to get the
 * fog level (0 = clear, higher = denser).
 * ======================================================================== */

static int s_fog_transition_counter;
static int s_fog_headlight_state;
static int s_fog_ambient_state;

void td5_render_set_fog_transition(int counter)
{
    s_fog_transition_counter = counter;
}

int td5_render_get_fog_transition(void)
{
    return s_fog_transition_counter;
}

void td5_render_set_fog_level(int viewport, int level)
{
    (void)viewport;

    if (level <= 0 || !g_td5.ini.fog_enabled) {
        /* Fog disabled (level==0 or user preference off) */
        td5_plat_render_set_fog(0, 0, 0.0f, 0.0f, 0.0f);
    } else {
        /* Scale fog start/end based on level.
         * Higher level = closer fog (denser).
         * Level 1 = normal visibility (fog_start=0.6, fog_end=1.0)
         * Level 2 = reduced visibility
         * Level 3+ = heavy fog */
        float scale = 1.0f / (float)level;
        float fog_start = FOG_START_DEFAULT * scale;
        float fog_end   = FOG_END_DEFAULT * scale;
        td5_plat_render_set_fog(1, s_fog_color, fog_start, fog_end,
                                FOG_DENSITY_DEFAULT);
    }
}

void td5_render_per_tick_fog_fade(void)
{
    int fog_level;

    if (s_fog_transition_counter == 0) {
        /* No transition active: clear fog on both viewports */
        td5_render_set_fog_level(0, 0);
        td5_render_set_fog_level(1, 0);
        return;
    }

    if (s_fog_transition_counter < 257) {
        /* Transition nearly complete: finalize */
        s_fog_transition_counter = 0;

        /* Determine night/weather mode from global state */
        int night_mode = (g_td5.weather != TD5_WEATHER_CLEAR) ? 1 : 0;

        /* Setup final visibility based on night_mode.
         * When night/weather: enable fog with standard parameters.
         * When clear: disable fog. */
        if (night_mode) {
            td5_render_configure_fog(s_fog_color, 1);
            td5_render_set_fog(1);
        } else {
            td5_render_configure_fog(0, 0);
            td5_render_set_fog(0);
        }
        return;
    }

    /* Decrement counter by 256 per tick */
    s_fog_transition_counter -= 256;

    /* Compute fog level: counter / 10240 + 1 */
    fog_level = s_fog_transition_counter / 10240 + 1;
    td5_render_set_fog_level(0, fog_level);
    td5_render_set_fog_level(1, fog_level);

    /* When fog is minimal, clear headlight/ambient overrides */
    if (fog_level <= 1) {
        s_fog_headlight_state = 0;
        s_fog_ambient_state = 0;
    }
}

/* ========================================================================
 * Display Globals (migrated from td5re_stubs.c)
 * ======================================================================== */

float   g_render_width_f        = 640.0f;
float   g_render_height_f       = 480.0f;
int     g_render_width          = 640;
int     g_render_height         = 480;

float   g_renderBasisMatrix[12] = { 1,0,0, 0,1,0, 0,0,1, 0,0,0 };
