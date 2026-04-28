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
#include "td5_vfx.h"
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
 * Deferred Additive Pass
 *
 * The source port has no PrepareMeshResource opcode-rewrite pass, so
 * loaded-texture commands never reach the depth-sorted billboard bucket
 * path. Street-light meshes draw in mesh-list order through the immediate
 * path, which means either:
 *   - They draw first and later opaque geometry (trees) overwrites them, or
 *   - They draw last in any given span but still get occluded by trees from
 *     a later span mesh.
 *
 * To get correct additive compositing without implementing the full bucket
 * sort, copy every immediate batch whose current page is type-3 into a
 * side buffer during the world pass, then flush all of them AFTER the
 * opaque world has been laid down. That mirrors the "draw all transparent
 * effects last" pattern and handles the tree-vs-light ordering cleanly.
 * ======================================================================== */

#define DEFERRED_ADD_MAX_VERTS   8192
#define DEFERRED_ADD_MAX_INDICES 16384
#define DEFERRED_ADD_MAX_BATCHES 512

typedef struct DeferredAdditiveBatch {
    int page_id;
    int vert_start;
    int vert_count;
    int index_start;
    int index_count;
} DeferredAdditiveBatch;

static TD5_D3DVertex        s_deferred_add_verts[DEFERRED_ADD_MAX_VERTS];
static uint16_t             s_deferred_add_indices[DEFERRED_ADD_MAX_INDICES];
static int                  s_deferred_add_vert_count;
static int                  s_deferred_add_index_count;
static DeferredAdditiveBatch s_deferred_add_batches[DEFERRED_ADD_MAX_BATCHES];
static int                  s_deferred_add_batch_count;
/* Only defer while the world/opaque pass is active. HUD/FMV/frontend
 * draws don't need deferral. */
static int                  s_deferred_add_active;

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
 * Vehicle Projection Effect / Chrome Reflection (0x43DEC0 / 0x40CBD0)
 *
 * Original renders a second pass on the player car mesh with heading-
 * rotated UV coordinates sampling environment textures (environs.zip).
 * Mode 2 = chrome/specular reflection on car bodies.
 *
 * Effect state per-slot: heading cos/sin, sub-mode, texture page.
 * ======================================================================== */

#define ENVMAP_TEXTURE_PAGE_BASE 900  /* D3D page IDs for environs textures */
#define ENVMAP_MAX_PAGES         4

typedef struct {
    /* Mirrors the per-slot 0x20-byte struct at DAT_004bf520 written by
     * SetProjectionEffectState @ 0x0043E210. */
    float cos_heading;   /* mode 2: cos(-yaw); mode 1: 1.0 */
    float sin_heading;   /* mode 2: sin(-yaw); mode 1: 0.0 */
    float scroll_offset; /* mode 1: running accumulator += (sin(yaw)*vX + cos(yaw)*vZ) / 8192 */
    int   sub_mode;      /* 1=planar/velocity, 2=yaw-UV rotation, 3=world-anchor */
    int   texture_page;  /* environs page index for this slot */
    float anchor_x, anchor_y, anchor_z; /* mode 3: actor world position (FP float) */
} ProjectionEffectState;

static ProjectionEffectState s_proj_effect[TD5_ACTOR_MAX_TOTAL_SLOTS];
static int  s_proj_effect_mode;   /* 0=disabled, 2=enabled (g_vehicleProjectionEffectMode @ 0x4C3D44) */
static int  s_envmap_page_count;  /* number of uploaded environs textures */
static int  s_envmap_pages[ENVMAP_MAX_PAGES]; /* D3D page IDs, indexed 0..count-1 by entry */
static int  s_environs_level;     /* level_number used to key the per-track tables */

/* Per-actor light-zone index, mirroring actor->field_0x377 in the original.
 * ApplyTrackLightingForVehicleSegment @ 0x00430150 walks the per-track zone
 * array forward/backward each frame based on the actor's track_span_raw, so
 * we persist the last-known index per slot across frames. */
static uint8_t s_actor_light_zone[TD5_ACTOR_MAX_TOTAL_SLOTS];

/* Per-track environs names + flags (from exe VA 0x0046bb1c). */
#include "td5_environs_table.inc"
/* Per-track light-zone array (from exe VA 0x00469c78). */
#include "td5_light_zones_table.inc"

/* ========================================================================
 * Forward Declarations (dispatch handlers)
 * ======================================================================== */

static void dispatch_tristrip(TD5_PrimitiveCmd *cmd, TD5_MeshVertex *base_verts);
static void dispatch_projected_tri(TD5_PrimitiveCmd *cmd, TD5_MeshVertex *base_verts);
static void dispatch_projected_quad(TD5_PrimitiveCmd *cmd, TD5_MeshVertex *base_verts);
static void dispatch_billboard(TD5_PrimitiveCmd *cmd, TD5_MeshVertex *base_verts);
static void dispatch_tristrip_direct(TD5_PrimitiveCmd *cmd, TD5_MeshVertex *base_verts);
static void dispatch_quad_direct(TD5_PrimitiveCmd *cmd, TD5_MeshVertex *base_verts);

/* Vehicle shadow + wheel billboard + brake light + reflection rendering */
static void render_vehicle_shadow_quad(const TD5_Actor *actor);
static void render_vehicle_wheel_billboards(TD5_Actor *actor, int slot);
static void render_vehicle_brake_lights(const TD5_Actor *actor, int slot);
static void render_vehicle_reflection_overlay(TD5_MeshHeader *mesh, int slot);

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

int td5_render_transform_and_project(float mx, float my, float mz,
                                     float *sx, float *sy, float *sz, float *rhw)
{
    const float *m = s_render_transform.m;
    float vx = mx*m[0] + my*m[1] + mz*m[2] + m[9];
    float vy = mx*m[3] + my*m[4] + mz*m[5] + m[10];
    float vz = mx*m[6] + my*m[7] + mz*m[8] + m[11];
    return project_vertex(-vx, -vy, vz, sx, sy, sz, rhw);
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

void td5_render_begin_world_pass(void)
{
    s_deferred_add_active      = 1;
    s_deferred_add_batch_count = 0;
    s_deferred_add_vert_count  = 0;
    s_deferred_add_index_count = 0;
}

void td5_render_flush_deferred_additive(void)
{
    if (s_deferred_add_batch_count == 0) {
        s_deferred_add_active = 0;
        return;
    }

    /* Any pending non-additive batch must be drained first so the
     * deferred pass starts on a clean immediate buffer. */
    int prev_page = s_current_texture_page;

    for (int i = 0; i < s_deferred_add_batch_count; i++) {
        const DeferredAdditiveBatch *b = &s_deferred_add_batches[i];
        td5_plat_render_set_preset(TD5_PRESET_ADDITIVE);
        td5_plat_render_bind_texture(b->page_id);
        s_scene_has_renderer_geometry = 1;
        td5_plat_render_draw_tris(
            &s_deferred_add_verts[b->vert_start],
            b->vert_count,
            &s_deferred_add_indices[b->index_start],
            b->index_count);
    }

    TD5_LOG_D(LOG_TAG,
              "deferred additive flush: %d batches, %d verts, %d indices",
              s_deferred_add_batch_count,
              s_deferred_add_vert_count,
              s_deferred_add_index_count);

    /* Restore opaque preset for anything drawn after this point. */
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);

    s_deferred_add_batch_count = 0;
    s_deferred_add_vert_count  = 0;
    s_deferred_add_index_count = 0;
    s_deferred_add_active      = 0;
    s_current_texture_page     = prev_page;
}

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

    /* If this batch is a type-3 (additive) page AND the world pass is
     * active, defer the draw until after opaque geometry is laid down.
     * Mirrors the "draw all additive effects last" pattern and gets the
     * streetlight-vs-tree ordering right without implementing the full
     * BindRaceTexturePage depth-bucket sort. */
    if (s_deferred_add_active && s_current_texture_page >= 0 &&
        td5_asset_get_page_transparency(s_current_texture_page) == 3) {
        if (s_deferred_add_batch_count < DEFERRED_ADD_MAX_BATCHES &&
            s_deferred_add_vert_count + s_imm_vert_count <= DEFERRED_ADD_MAX_VERTS &&
            s_deferred_add_index_count + s_imm_index_count <= DEFERRED_ADD_MAX_INDICES) {
            DeferredAdditiveBatch *db =
                &s_deferred_add_batches[s_deferred_add_batch_count++];
            db->page_id     = s_current_texture_page;
            db->vert_start  = s_deferred_add_vert_count;
            db->vert_count  = s_imm_vert_count;
            db->index_start = s_deferred_add_index_count;
            db->index_count = s_imm_index_count;
            memcpy(&s_deferred_add_verts[s_deferred_add_vert_count],
                   s_imm_verts,
                   (size_t)s_imm_vert_count * sizeof(TD5_D3DVertex));
            memcpy(&s_deferred_add_indices[s_deferred_add_index_count],
                   s_imm_indices,
                   (size_t)s_imm_index_count * sizeof(uint16_t));
            s_deferred_add_vert_count  += s_imm_vert_count;
            s_deferred_add_index_count += s_imm_index_count;
        }
        /* Skip the direct draw — reset batch and fall through to tail. */
        s_imm_vert_count  = 0;
        s_imm_index_count = 0;
        s_current_texture_page = s_previous_texture_page;
        return;
    }

    /* Bind current texture and apply per-page blend preset.
     * The actual render path bypasses td5_render_bind_texture_page (the
     * cmd handlers funnel through clip_and_submit_polygon → here), so the
     * preset hook lives at the flush site to mirror BindRaceTexturePage @
     * 0x40B660. Type 3 (additive) is handled by the deferred path above;
     * the preset hook below is now a no-op for type-3 and only toggles
     * back to OPAQUE_LINEAR when needed for non-type-3 pages. */
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
            td5_render_push_transform();
            td5_render_load_rotation((const TD5_Mat3x3 *)g_cameraSecondaryUnscaled);
            td5_render_transform_mesh_vertices(mesh);
            /* Skip runtime vertex-lighting recompute for billboard meshes.
             * RenderTrackSpanDisplayList @ 0x00431270 only calls
             * TransformAndQueueTranslucentMesh which transforms XYZ only.
             * Per-vertex intensity comes from the asset's baked values
             * (pre-dimmed for type-3 additive billboards by
             * td5_track_dim_additive_billboard_meshes at track load). */
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
     * Cull window = player_span ± half_window with ring-wrap for circuit tracks.
     *
     * Original also pulls the cull-start back by −0x19 (≈100 spans) at 0x42BBDF when
     * g_selectedGameType != 0 (cup/wanted/drag). The port's doubled max_spans already
     * exceeds the original's (64 + 25 quarter-span = ~89 effective) trailing reach,
     * so no explicit offset is needed. [RE basis: research agent confirmed the
     * architectural coverage — task #9 resolved by doubled window.] */
#define VIEW_DIST_MAX_SPANS 128
    int player_span = 0;
    int player_branch_span = -1;
    {
        /* Use the actor assigned to this viewport for cull center.
         * In split-screen, viewport 1 follows P2 (slot 1), not P1 (slot 0).
         * [CONFIRMED @ RunRaceFrame 0x42BB2E: each view culls from its player's span] */
        TD5_Actor *player = td5_game_get_actor(td5_game_get_player_slot(view_index));
        if (player) {
            player_span = (int)player->track_span_raw;
            /* If on a branch road, use the junction's main-road span as
             * the culling center so nearby main road geometry stays visible.
             * Also track the branch span to render branch geometry. */
            int ring = td5_track_get_ring_length();
            if (player_span >= ring) {
                player_branch_span = player_span;
                int junction = td5_track_branch_to_junction(player_span);
                if (junction >= 0)
                    player_span = junction;
            }
        }
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
         * Delta is wrapped into [-span_count/2, span_count/2] to handle circuit ring.
         * When on a branch road, also accept spans near the branch span index. */
        if (span_count > 0) {
            int ring = td5_track_get_ring_length();
            int delta = span_index - player_span;
            /* Only wrap for main road spans (ring topology) */
            if (span_index < ring) {
                int half_ring = ring / 2;
                if (delta >  half_ring) delta -= ring;
                if (delta < -half_ring) delta += ring;
            }
            int visible = (delta <= half_window && delta >= -half_window);
            /* If on a branch, also render spans near the branch index */
            if (!visible && player_branch_span >= 0) {
                int bdelta = span_index - player_branch_span;
                if (bdelta < 0) bdelta = -bdelta;
                visible = (bdelta <= half_window);
            }
            if (!visible) continue;
        }

        td5_render_span_display_list(display_list);
        rendered_spans++;
    }

    {
        int total_actors = td5_game_get_total_actor_count();
        int drag_mode = g_td5.drag_race_enabled;

        for (int slot = 0; slot < total_actors; slot++) {
            TD5_Actor *actor = td5_game_get_actor(slot);
            TD5_MeshHeader *mesh = td5_render_get_vehicle_mesh(slot);
            TD5_Mat3x3 view_rot;
            TD5_Vec3f render_pos;
            float depth;

            if (!actor || !mesh)
                continue;

            /* Drag race: skip decoration slots (state==3). Originally this
             * gate skipped state==0 (faithful to RenderRaceActorsForView @
             * 0x40BD26 "absent AI" semantics where original drag had only
             * state==1 and state==3 slots). Port enhancement keeps slot 1
             * AI as state==0 so physics/AI tick it, so the gate is inverted
             * to skip state==3 instead. The mesh==NULL check above already
             * catches state==3 slots (their assets aren't loaded), making
             * this a defense-in-depth check that also prevents stale-mesh
             * rendering across race transitions. */
            if (drag_mode && slot < TD5_MAX_RACER_SLOTS) {
                int ss = td5_game_get_slot_state(slot);
                if (ss == 3)
                    continue;
            }

            /* (Lighting moved below to td5_render_apply_track_lighting,
             * which writes to the s_light_dirs[] basis ComputeMeshVertexLighting
             * actually consumes. The earlier td5_track_apply_segment_lighting
             * skeleton was a dead branch -- wrong struct offsets, never wired
             * to a populated table, and writing to a parallel set of globals
             * the renderer did not read.) */

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

            /* Original (0x40C1E2-0x40C25E): vehicle_mode==0 builds an
             * interpolated rotation; vehicle_mode!=0 (traffic/recovery) uses
             * the stored physics-step matrix directly.
             * Interpolation: angles[i] = display_angles[i] + ang_vel[i]*(1/256)*frac
             * Order: [roll+0x208, yaw+0x20A, pitch+0x20C] → BuildRotationMatrixFromAngles.
             * [CONFIRMED: scale=1/256 @ DAT_004749D0; field order @ 0x40C1E2] */
            if (actor->vehicle_mode != 0) {
                mat3x3_mul(s_camera_basis, actor->rotation_matrix.m, view_rot.m);
            } else {
                extern float g_subTickFraction;
                float interp_mat[9];
                short interp[3];
                float ifrac = g_subTickFraction * (1.0f / 256.0f);
                interp[0] = actor->display_angles.roll  + (short)(int)(actor->angular_velocity_roll  * ifrac + 0.5f);
                interp[1] = actor->display_angles.yaw   + (short)(int)(actor->angular_velocity_yaw   * ifrac + 0.5f);
                interp[2] = actor->display_angles.pitch + (short)(int)(actor->angular_velocity_pitch * ifrac + 0.5f);
                BuildRotationMatrixFromAngles(interp_mat, interp);
                mat3x3_mul(s_camera_basis, interp_mat, view_rot.m);
            }
            td5_render_load_rotation(&view_rot);
            td5_render_load_translation(&render_pos);

            if (!td5_render_test_mesh_frustum(mesh, &depth))
                continue;
            (void)depth;

            td5_render_transform_mesh_vertices(mesh);
            /* Per-actor track-zone driven 3-light + ambient basis (mirrors
             * ApplyTrackLightingForVehicleSegment @ 0x00430150). Must run
             * BEFORE compute_vertex_lighting since that's the consumer of
             * s_light_dirs[]/s_ambient_intensity. */
            td5_render_apply_track_lighting(slot, actor);
            td5_render_compute_vertex_lighting(mesh);

            /* Shadow before car mesh — car body + wheels paint over it via
             * z_write=1 (draw-order occlusion). z_test=0 on the shadow so
             * it doesn't z-fight with the track surface. */
            render_vehicle_shadow_quad(actor);

            td5_render_prepared_mesh(mesh);

            /* Chrome/envmap reflection overlay (0x40C120 second pass).
             * Original gates on `actor_00 == 0` — only the player car gets
             * the reflection pass. [CONFIRMED @ 0x40C120 second-pass branch] */
            if (s_proj_effect_mode == 2 && slot == 0) {
                td5_render_update_projection_effect(slot, actor);
                render_vehicle_reflection_overlay(mesh, slot);
            }

            /* Render wheel ring billboards (0x446F00) */
            render_vehicle_wheel_billboards(actor, slot);

            /* Render brake light billboards (0x4011C0) */
            render_vehicle_brake_lights(actor, slot);

            /* Smoke effects (0x40C120 tail): called per visible actor per frame.
             * SpawnRearWheelSmokeEffects (0x401330) — burnout hardpoint smoke
             * SpawnVehicleSmokeSprite (0x429CF0) — general exhaust smoke */
            td5_vfx_spawn_rear_wheel_smoke(actor, view_index);
            td5_vfx_spawn_smoke(actor);

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

    /* Platform viewport is already set by the caller with the correct x,y offset.
     * Do NOT call td5_plat_render_set_viewport here — it would reset x,y to (0,0)
     * and break split-screen where viewport 1 has a non-zero origin.
     * [RE basis: original SetProjectionCenterOffset only changes center, not clip rect] */
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

/* Set when the reflection overlay is mid-draw so type-2 pages (env-map) do
 * not switch the preset away from the manually-set ADDITIVE. Without this,
 * the per-bind preset switch overrides ADDITIVE with TRANSLUCENT_ANISO and
 * the reflection paints opaquely over the car body (bug: "cars render only
 * the reflection texture"). */
static int s_in_reflection_overlay = 0;

/* Dispatch render preset per tpage transparency type.
 * BindRaceTexturePage @ 0x0040B660 switch:
 *   type 0 → ALPHABLENDENABLE=0 (opaque)                  [CONFIRMED @ 0x0040B6B0]
 *   type 1 → ALPHABLENDENABLE=1, SRCALPHA/INVSRCALPHA     [CONFIRMED @ 0x0040B6CC]
 *   type 2 → same D3D state as type 1, no ZWRITE write    [CONFIRMED @ 0x0040B6CC, same case]
 *   type 3 → ALPHABLENDENABLE=1, ONE/ONE (additive)       [CONFIRMED @ 0x0040B6E8]
 *
 * Types 1 and 2 share the same D3D blend state but differ in pixel alpha:
 *   type 1 = binary 0/255 → OPAQUE_LINEAR (alpha_ref=1 discards 0-alpha pixels)
 *   type 2 = uniform 0x80 → TRANSLUCENT_ANISO (blend enabled for 50% opacity)
 *
 * Reflection overlay carve-out: when s_in_reflection_overlay is set, the
 * caller has explicitly chosen ADDITIVE for chrome highlights — keep it. */
static void td5_render_apply_page_blend_preset(int page_id)
{
    int t = td5_asset_get_page_transparency(page_id);
    TD5_RenderPreset p;
    if (s_in_reflection_overlay) return; /* preserve caller's ADDITIVE preset */
    if (t == 3)      p = TD5_PRESET_ADDITIVE;
    else if (t == 2) p = TD5_PRESET_TRANSLUCENT_ANISO;
    else             p = TD5_PRESET_OPAQUE_LINEAR;
    td5_plat_render_set_preset(p);
    TD5_LOG_D(LOG_TAG, "page_blend_preset: page=%d type=%d preset=%d", page_id, t, (int)p);
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

/* g_projectionDepthBias mirrors the original's integer at 0x00467368.
 * ConfigureProjectionForViewport @ 0x0043E7E0 writes 0x1000 (= 4096) at
 * race init; trackside camera presets 0 and 6 overwrite it with a clamped
 * per-frame depth value (clamp floor 0x1000). For chase/cockpit/default
 * cameras it persists at 0x1000 — which is what the reflection overlay
 * will see on Moscow. Mode-3 UV math divides 16.0 by this. */
int g_projectionDepthBias = 0x1000;

/**
 * ApplyMeshProjectionEffect (0x43DEC0):
 * Overwrites the mesh's per-vertex proj_u/proj_v based on the slot's
 * projection mode (set earlier by SetProjectionEffectState @ 0x0043E210).
 *
 *   Mode 1  (planar scroll; used for trees / tunnels / bridges):
 *     U = pos_x * (1/1500) + 0.5                                 [_DAT_0045d774, _DAT_0045d5d0]
 *     V = (scroll + pos_z) * (1/750)                             [_DAT_0045d770]
 *     Primitive-gated by normals[i].visible_flag (original reads iVar14+0xC).
 *
 *   Mode 2  (yaw-rotated chrome; dead code on real tracks, retained for completeness):
 *     U = (cos*pos_x - sin*pos_z + mesh.origin_x * 1/8192) * 1/2048  [_DAT_0045d778, _DAT_0045d77c]
 *     V = (cos*pos_z + sin*pos_x + mesh.origin_z * 1/8192) * 1/1024  [_DAT_0045d6a0]
 *
 *   Mode 3  (world-anchor sphere-map; used for SUN zone on Moscow et al.):
 *     anchor_view = basis · (slot.anchor_world - camera_world)
 *     depth       = 16.0 / g_projectionDepthBias   (= 16/4096 = 0.00390625)
 *     U = (n.x * m[0] + m[1]*n.y + m[2]*n.z) * 0.375  +  (vert.view_x - anchor_view.x) * depth  +  0.625
 *     V = (n.x * m[3] + m[4]*n.y + m[5]*n.z) * 0.375  +  (vert.view_y - anchor_view.y) * depth  +  0.75
 *     where n = per-vertex MODEL-SPACE NORMAL (from normals_offset, stride 16 bytes;
 *     the original's pfVar9 buffer in mode 3), m = g_currentRenderTransform rows 0/1
 *     (the model-to-view rotation = s_render_transform), basis = camera world-to-view
 *     rotation (= s_camera_basis = DAT_004aafe0 at runtime). [_DAT_0045d788 = 0.375,
 *     _DAT_0045d784 = 0.625, _DAT_0045d780 = 0.75, _DAT_0045d628 = 16.0]
 *
 *     Key: the rotated term uses the vertex NORMAL (unit vector), so it contributes
 *     at most ±0.375 to the UV — the "sphere map" dominant term. Previous port tried
 *     to use model-space POSITION here and produced the "tiny white dots" regression.
 */
void td5_render_apply_mesh_projection_effect(TD5_MeshHeader *mesh, int slot)
{
    const ProjectionEffectState *pe;
    TD5_MeshVertex  *verts;
    TD5_VertexNormal *normals;
    int vert_count, mode, i;

    if (!mesh) return;
    if (slot < 0 || slot >= TD5_ACTOR_MAX_TOTAL_SLOTS) return;
    pe = &s_proj_effect[slot];
    mode = pe->sub_mode;

    verts      = (TD5_MeshVertex  *)(uintptr_t)mesh->vertices_offset;
    normals    = (TD5_VertexNormal *)(uintptr_t)mesh->normals_offset;
    vert_count = mesh->total_vertex_count;
    if (!verts || vert_count <= 0) return;

    if (mode == 1) {
        /* _DAT_0045d774 = 1/1500 ≈ 0.00066666, _DAT_0045d5d0 = 0.5,
         * _DAT_0045d770 = 1/750 ≈ 0.00133333. scroll = slot+0x08 accumulator. */
        float scroll = pe->scroll_offset;
        for (i = 0; i < vert_count; i++) {
            /* Original gates the primitive by normals[i].visible_flag != 0.
             * With no primitive-level loop structure exposed here we gate per-vertex
             * for an equivalent visual — invisible vertices retain prior UVs. */
            if (normals && normals[i].visible_flag == 0) continue;
            verts[i].proj_u = verts[i].pos_x * (1.0f / 1500.0f) + 0.5f;
            verts[i].proj_v = (scroll + verts[i].pos_z) * (1.0f / 750.0f);
        }
    } else if (mode == 2) {
        /* _DAT_0045d778 = 1/2048, _DAT_0045d6a0 = 1/1024, _DAT_0045d77c = 1/8192.
         * The mesh-origin bias comes from mesh header +0x1C/+0x24 in the original;
         * our TD5_MeshHeader has those as origin_x/y/z at the same offsets. */
        float cos_h  = pe->cos_heading;
        float sin_h  = pe->sin_heading;
        float bias_u = mesh->origin_x * (1.0f / 8192.0f);
        float bias_v = mesh->origin_z * (1.0f / 8192.0f);
        for (i = 0; i < vert_count; i++) {
            if (normals && normals[i].visible_flag == 0) continue;
            float vx = verts[i].pos_x;
            float vz = verts[i].pos_z;
            verts[i].proj_u = (cos_h * vx - sin_h * vz + bias_u) * (1.0f / 2048.0f);
            verts[i].proj_v = (vz * cos_h + sin_h * vx + bias_v) * (1.0f / 1024.0f);
        }
    } else if (mode == 3) {
        extern float g_cameraPos[3];   /* td5_camera.c */
        const float *mv  = s_render_transform.m;
        const float *cam = s_camera_basis;
        /* Anchor in view space: TransformVector3ByBasis(DAT_004aafe0, slot.anchor - camera_world).
         * The port's s_camera_basis mirrors DAT_004aafe0 (camera world-to-view rotation). */
        float dx = pe->anchor_x - g_cameraPos[0];
        float dy = pe->anchor_y - g_cameraPos[1];
        float dz = pe->anchor_z - g_cameraPos[2];
        float anchor_vx = cam[0] * dx + cam[1] * dy + cam[2] * dz;
        float anchor_vy = cam[3] * dx + cam[4] * dy + cam[5] * dz;
        const float rot_scale   = 0.375f;                                 /* _DAT_0045d788 */
        const float depth_scale = 16.0f / (float)g_projectionDepthBias;   /* _DAT_0045d628 / g_projectionDepthBias */
        const float bias_u      = 0.625f;                                 /* _DAT_0045d784 */
        const float bias_v      = 0.75f;                                  /* _DAT_0045d780 */

        if (!normals) {
            /* Mesh has no per-vertex normals — skip mode-3 silently. */
            return;
        }
        for (i = 0; i < vert_count; i++) {
            float nx = normals[i].nx;
            float ny = normals[i].ny;
            float nz = normals[i].nz;
            float u_rot = (nx * mv[0] + mv[1] * ny + mv[2] * nz) * rot_scale;
            float v_rot = (nx * mv[3] + mv[4] * ny + mv[5] * nz) * rot_scale;
            verts[i].proj_u = u_rot + (verts[i].view_x - anchor_vx) * depth_scale + bias_u;
            verts[i].proj_v = v_rot + (verts[i].view_y - anchor_vy) * depth_scale + bias_v;
        }
    }
    /* mode 0 or unknown: leave proj_u/proj_v untouched. */
}

/* --- Environs Texture Loading --- */

int td5_render_load_environs_textures(int level_number)
{
    /*
     * LoadEnvironmentTexturePages (0x42F990):
     * Loads the per-track environs name list for this level (table at
     * exe VA 0x0046bb1c, mirrored in td5_environs_table.inc) and delegates
     * to td5_asset for PNG loading and GPU upload. Also caches the level
     * number for per-frame zone dispatch and resets each actor's light-zone
     * index so the next frame re-runs the zone walk from scratch.
     */
    s_environs_level = level_number;
    for (int i = 0; i < TD5_ACTOR_MAX_TOTAL_SLOTS; i++) s_actor_light_zone[i] = 0;

    s_envmap_page_count = td5_asset_load_environs_pages(
        level_number, ENVMAP_TEXTURE_PAGE_BASE, ENVMAP_MAX_PAGES, s_envmap_pages);

    /* Enable projection effect if we loaded at least one texture */
    s_proj_effect_mode = (s_envmap_page_count > 0) ? 2 : 0;

    TD5_LOG_I(LOG_TAG, "environs: loaded %d textures, effect_mode=%d",
              s_envmap_page_count, s_proj_effect_mode);

    return s_envmap_page_count;
}

/* --- Per-Frame Projection Effect Update --- */

/* Walk the per-track light-zone array forward/backward until the actor's
 * current track_span_raw falls inside the zone's [span_lo, span_hi]. Mirrors
 * the prologue of ApplyTrackLightingForVehicleSegment @ 0x00430150. Returns
 * the final zone index, or -1 if the track has no zones / level is invalid. */
static int update_actor_light_zone(int slot, int track_span)
{
    int track_slots;
    int first;
    int count;
    int idx;

    if (s_environs_level < 0 || s_environs_level >= TD5_LIGHT_ZONE_TRACK_COUNT)
        return -1;
    first = td5_light_zone_track[s_environs_level].first;
    count = td5_light_zone_track[s_environs_level].count;
    if (count <= 0) return -1;

    track_slots = TD5_ACTOR_MAX_TOTAL_SLOTS;
    if (slot < 0 || slot >= track_slots) return -1;

    idx = s_actor_light_zone[slot];
    if (idx >= count) idx = count - 1;
    if (idx < 0)      idx = 0;

    /* Walk backward if current span < zone.span_lo */
    while (idx > 0 && track_span < td5_light_zones[first + idx].span_lo)
        idx--;
    /* Walk forward while current span > zone.span_hi */
    while (idx < count - 1 && track_span > td5_light_zones[first + idx].span_hi)
        idx++;

    s_actor_light_zone[slot] = (uint8_t)idx;
    return idx;
}

/* ========================================================================
 * Track Lighting -- ApplyTrackLightingForVehicleSegment @ 0x00430150
 *
 * Per-vehicle zone-driven 3-light + ambient basis.
 * Each per-track zone (TD5_LightZone, td5_light_zones_table.inc) carries:
 *   dir[3]      static directional vector (world frame, scaled to ~4096)
 *   weight[3]   per-channel directional weight bytes
 *   amb[3]      per-channel ambient bytes
 *   pos_off[2]  XY world-space bias added to track-vertex sample positions
 *   blend_mode  0=static, 1=transition (blend at edges, sample mid),
 *               2=multi-sample mid-zone, 3=full-zone half/half blend
 *   spacing     stride for vertex sampling (cases 1/2)
 *   sub_mode    case-2 vertex pick: 0=left edge, 1=right edge, 2=midpoint
 *   multiplier  attenuation multiplier (cases 1/2)
 *
 * The original calls SetTrackLightDirectionContribution(slot,dir,R,G,B) up to
 * 3 times to populate world-frame contributions, ComputeAverageDepth(R,G,B)
 * to set the scalar ambient, and finally UpdateActiveTrackLightDirections to
 * transform contributions into body frame. ComputeMeshVertexLighting then
 * does per-vertex dot products against body-frame normals.
 * ======================================================================== */

/* Convert 24.8 fixed-point world coord to int16 with round-toward-zero for
 * negatives. Mirrors the (x + (x>>31 & 0xff)) >> 8 idiom in the original. */
static inline int16_t tl_fp_to_short(int32_t fp)
{
    return (int16_t)((fp + ((fp >> 31) & 0xFF)) >> 8);
}

/* Mirrors ConvertFloatVec4ToShortAngles @ 0x0042CDB0:
 *   - reads 3 input shorts (x,y,z)
 *   - normalizes to a 4096-scale unit vector and writes back to out[0..2]
 *   - returns the SQUARED magnitude (mag^2) as an int -- callers use it as a
 *     squared-distance metric for attenuation. */
static int tl_normalize_4096(const int16_t in[3], int16_t out[3])
{
    float fx = (float)in[0];
    float fy = (float)in[1];
    float fz = (float)in[2];
    float mag2 = fx*fx + fy*fy + fz*fz;
    if (mag2 <= 0.0f) {
        out[0] = 0; out[1] = 0; out[2] = 0;
        return 0;
    }
    float scale = 4096.0f / sqrtf(mag2);
    out[0] = (int16_t)(int32_t)(fx * scale);
    out[1] = (int16_t)(int32_t)(fy * scale);
    out[2] = (int16_t)(int32_t)(fz * scale);
    return (int)mag2;
}

/* Per-frame 3-slot contribution accumulator (one set per actor pass). */
typedef struct {
    int   enabled;       /* 0 = slot disabled, 1 = active */
    float vec_world[3];  /* world-frame contribution dir scaled by intensity */
} TL_Contribution;

static TL_Contribution s_tl_contrib[3];
static int             s_tl_ambient;  /* scalar ambient byte (post-ComputeAverageDepth) */

/* SetTrackLightDirectionContribution @ 0x0042E130:
 *   intensity = avg(R,G,B); contribution_world = dir * intensity * (1/1024).
 *   All-zero RGB disables the slot. */
static void tl_set_contrib(int slot, const int16_t dir[3], int r, int g, int b)
{
    if (slot < 0 || slot >= 3) return;
    if (r == 0 && g == 0 && b == 0) {
        s_tl_contrib[slot].enabled = 0;
        s_tl_contrib[slot].vec_world[0] = 0.0f;
        s_tl_contrib[slot].vec_world[1] = 0.0f;
        s_tl_contrib[slot].vec_world[2] = 0.0f;
        return;
    }
    s_tl_contrib[slot].enabled = 1;
    float intensity = (float)(r + g + b) / 3.0f;
    s_tl_contrib[slot].vec_world[0] = (float)dir[0] * intensity * (1.0f / 1024.0f);
    s_tl_contrib[slot].vec_world[1] = (float)dir[1] * intensity * (1.0f / 1024.0f);
    s_tl_contrib[slot].vec_world[2] = (float)dir[2] * intensity * (1.0f / 1024.0f);
}

/* ComputeAverageDepth @ 0x0043E7B0: scalar ambient = (R+G+B)/3 byte. */
static void tl_set_depth(int r, int g, int b)
{
    s_tl_ambient = (r + g + b) / 3;
}

/* UpdateActiveTrackLightDirections @ 0x0042CE90:
 *   For each slot, transform its world-frame contribution into body frame
 *   via M^T (port matrix layout: m[0..2]=row0, m[3..5]=row1, m[6..8]=row2,
 *   so column j = {m[j], m[j+3], m[j+6]}). Disabled slots fall back to the
 *   default zero vector (DAT_004ab0f8/0fc/100 verified zero in memory). */
static void tl_commit_to_render_globals(const TD5_Actor *actor)
{
    const float *m = actor->rotation_matrix.m;
    for (int s = 0; s < 3; s++) {
        if (s_tl_contrib[s].enabled) {
            float cx = s_tl_contrib[s].vec_world[0];
            float cy = s_tl_contrib[s].vec_world[1];
            float cz = s_tl_contrib[s].vec_world[2];
            s_light_dirs[s*3 + 0] = cx * m[0] + cy * m[3] + cz * m[6];
            s_light_dirs[s*3 + 1] = cx * m[1] + cy * m[4] + cz * m[7];
            s_light_dirs[s*3 + 2] = cx * m[2] + cy * m[5] + cz * m[8];
        } else {
            s_light_dirs[s*3 + 0] = 0.0f;
            s_light_dirs[s*3 + 1] = 0.0f;
            s_light_dirs[s*3 + 2] = 0.0f;
        }
    }
    s_ambient_intensity = (float)s_tl_ambient;
}

/* Per-actor lighting fallback used when no zone can be resolved. */
static void tl_apply_fallback(void)
{
    for (int s = 0; s < 3; s++) {
        s_tl_contrib[s].enabled = 0;
        s_tl_contrib[s].vec_world[0] = 0.0f;
        s_tl_contrib[s].vec_world[1] = 0.0f;
        s_tl_contrib[s].vec_world[2] = 0.0f;
    }
    s_tl_ambient = TD5_LIGHTING_MIN;
}

/* Bounds-checked accessor for the per-track-zone array. */
static const TD5_LightZone *tl_zone_at(int track_first, int track_count, int idx)
{
    if (idx < 0 || idx >= track_count) return NULL;
    return &td5_light_zones[track_first + idx];
}

/* BlendTrackLightEntryFromStart @ 0x0042FE20.
 *   Fades the previous zone's directional contribution out as the actor
 *   crosses from the previous zone into the current zone (via the strip-edge
 *   perpendicular at zone->span_lo). Ambient blends prev -> curr.
 *
 *   max_dist is the projected-distance cap (in strip-perpendicular units).
 *   At the boundary, prev dominates; deeper into curr, contribution decays. */
static void tl_blend_from_start(const TD5_Actor *actor,
                                const TD5_LightZone *prev,
                                const TD5_LightZone *curr,
                                int max_dist)
{
    if (!actor || !prev || !curr || max_dist <= 0) return;

    const TD5_StripSpan *sp = td5_track_get_span((int)curr->span_lo);
    if (!sp) return;
    /* "Right edge" of the strip-vertex span = vertex(right_vertex_index - 1).
     * The original walks right_vertex (== last_vertex in original code) and
     * then accesses (last_vertex - 1) for the cross-edge calculation. */
    int right_idx = (int)sp->right_vertex_index - 1;
    if (right_idx < 0) return;
    const TD5_StripVertex *vL = td5_track_get_vertex((int)sp->left_vertex_index);
    const TD5_StripVertex *vR = td5_track_get_vertex(right_idx);
    if (!vL || !vR) return;

    /* Build the strip's edge-perpendicular vector (XZ-plane 90deg rotation
     * of (vL - vR)) and normalize to 4096-scale. */
    int16_t edge_in[3] = {
        (int16_t)((int)vL->z - (int)vR->z),
        0,
        (int16_t)((int)vR->x - (int)vL->x),
    };
    int16_t edge[3];
    (void)tl_normalize_4096(edge_in, edge);

    /* Project actor's offset onto the perpendicular. The perpendicular is
     * oriented so that positive dot = behind the boundary (= prev zone side);
     * the original negates and clamps to [0, max_dist] for the curr-side
     * weight, leaving max_dist - clamped as the prev-side weight. */
    int16_t actor_x = tl_fp_to_short(actor->world_pos.x);
    int16_t actor_z = tl_fp_to_short(actor->world_pos.z);
    int dx = (int)((int16_t)(actor_x - (int16_t)sp->origin_x) - vR->x);
    int dz = (int)((int16_t)(actor_z - (int16_t)sp->origin_z) - vR->z);
    int dot = dz * (int)edge[2] + dx * (int)edge[0];
    int dist_curr = -((dot + ((dot >> 31) & 0xFFF)) >> 12);
    if (dist_curr < 0)            dist_curr = 0;
    if (dist_curr > max_dist)     dist_curr = max_dist;
    int dist_prev = max_dist - dist_curr;

    /* Ambient blends prev -> curr. */
    tl_set_depth(
        ((int)prev->amb_r * dist_prev + (int)curr->amb_r * dist_curr) / max_dist,
        ((int)prev->amb_g * dist_prev + (int)curr->amb_g * dist_curr) / max_dist,
        ((int)prev->amb_b * dist_prev + (int)curr->amb_b * dist_curr) / max_dist);

    /* Slot 0: prev's directional contribution scaled down as we move away. */
    {
        int16_t prev_dir[3] = { prev->dir_x, prev->dir_y, prev->dir_z };
        tl_set_contrib(0, prev_dir,
            (int)prev->weight_r * dist_prev / max_dist,
            (int)prev->weight_g * dist_prev / max_dist,
            (int)prev->weight_b * dist_prev / max_dist);
    }
    /* Slots 1/2: disabled (original passes curr->dir with all-zero weights). */
    {
        int16_t curr_dir[3] = { curr->dir_x, curr->dir_y, curr->dir_z };
        tl_set_contrib(1, curr_dir, 0, 0, 0);
        tl_set_contrib(2, curr_dir, 0, 0, 0);
    }
}

/* BlendTrackLightEntryFromEnd @ 0x0042FFC0.
 *   Symmetric to BlendStart but uses the strip RIGHT AFTER zone->span_hi
 *   (i.e. the next zone's first strip) and blends curr -> next as the actor
 *   approaches the zone end. */
static void tl_blend_from_end(const TD5_Actor *actor,
                              const TD5_LightZone *curr,
                              const TD5_LightZone *next,
                              int max_dist)
{
    if (!actor || !curr || !next || max_dist <= 0) return;

    /* Original reads strip[span_hi + 1] via byte +0x1c (= 0x18 + 0x4) of the
     * span_hi strip record. Use the next strip directly. */
    const TD5_StripSpan *sp = td5_track_get_span((int)curr->span_hi + 1);
    if (!sp) return;
    int right_idx = (int)sp->right_vertex_index - 1;
    if (right_idx < 0) return;
    const TD5_StripVertex *vL = td5_track_get_vertex((int)sp->left_vertex_index);
    const TD5_StripVertex *vR = td5_track_get_vertex(right_idx);
    if (!vL || !vR) return;

    int16_t edge_in[3] = {
        (int16_t)((int)vL->z - (int)vR->z),
        0,
        (int16_t)((int)vR->x - (int)vL->x),
    };
    int16_t edge[3];
    (void)tl_normalize_4096(edge_in, edge);

    int16_t actor_x = tl_fp_to_short(actor->world_pos.x);
    int16_t actor_z = tl_fp_to_short(actor->world_pos.z);
    int dx = (int)((int16_t)(actor_x - (int16_t)sp->origin_x) - vR->x);
    int dz = (int)((int16_t)(actor_z - (int16_t)sp->origin_z) - vR->z);
    int dot = dz * (int)edge[2] + dx * (int)edge[0];
    /* BlendEnd does NOT negate -- the perpendicular orientation here puts the
     * boundary at dot=0 from the inside, growing positive as the actor moves
     * back into the zone. */
    int dist_curr = (dot + ((dot >> 31) & 0xFFF)) >> 12;
    if (dist_curr < 0)            dist_curr = 0;
    if (dist_curr > max_dist)     dist_curr = max_dist;
    int dist_next = max_dist - dist_curr;

    tl_set_depth(
        ((int)curr->amb_r * dist_curr + (int)next->amb_r * dist_next) / max_dist,
        ((int)curr->amb_g * dist_curr + (int)next->amb_g * dist_next) / max_dist,
        ((int)curr->amb_b * dist_curr + (int)next->amb_b * dist_next) / max_dist);

    /* Slot 0: next zone's directional contribution scaled by closeness to
     * the boundary (dist_next grows as actor approaches span_hi). */
    {
        int16_t next_dir[3] = { next->dir_x, next->dir_y, next->dir_z };
        tl_set_contrib(0, next_dir,
            (int)next->weight_r * dist_next / max_dist,
            (int)next->weight_g * dist_next / max_dist,
            (int)next->weight_b * dist_next / max_dist);
    }
    {
        int16_t curr_dir[3] = { curr->dir_x, curr->dir_y, curr->dir_z };
        tl_set_contrib(1, curr_dir, 0, 0, 0);
        tl_set_contrib(2, curr_dir, 0, 0, 0);
    }
}

/* Compute one mid-zone vertex sample's contribution (case 1 / case 2 inner).
 *   sample_pos = strip_origin + chosen_vertex (+ pos_off_y on Y component)
 *   dir = (sample_pos - actor_pos) normalized to 4096
 *   atten = clamp(0x1000 - (mag^2 * multiplier) >> 14, 0, 0x1000)
 *   slot contribution = dir * atten * (weight/4096) per channel */
static void tl_sample_contrib(int slot,
                              const TD5_Actor *actor,
                              const int sample_pos[3],
                              int weight_r, int weight_g, int weight_b,
                              int multiplier)
{
    int16_t actor_x = tl_fp_to_short(actor->world_pos.x);
    int16_t actor_y = tl_fp_to_short(actor->world_pos.y);
    int16_t actor_z = tl_fp_to_short(actor->world_pos.z);

    int16_t dir_in[3] = {
        (int16_t)(sample_pos[0] - (int)actor_x),
        (int16_t)(sample_pos[1] - (int)actor_y),
        (int16_t)(sample_pos[2] - (int)actor_z),
    };
    int16_t dir[3];
    int mag2 = tl_normalize_4096(dir_in, dir);

    int atten = (mag2 * multiplier + (((mag2 * multiplier) >> 31) & 0x3FFF)) >> 14;
    if (atten < 0x1001) atten = 0x1000 - atten;
    else                atten = 0;

    /* (atten * weight) >> 12 with round-toward-zero for negatives.
     * atten is non-negative here so the sign-fixup is a no-op, but the
     * pattern is preserved for parity with the original. */
    int wr = (atten * weight_r + (((atten * weight_r) >> 31) & 0xFFF)) >> 12;
    int wg = (atten * weight_g + (((atten * weight_g) >> 31) & 0xFFF)) >> 12;
    int wb = (atten * weight_b + (((atten * weight_b) >> 31) & 0xFFF)) >> 12;
    tl_set_contrib(slot, dir, wr, wg, wb);
}

/* Resolve a strip's chosen vertex per case-2 sub_mode (0=left edge,
 * 1=right edge (right_vertex_index - 1), 2=midpoint). Returns 0 on failure. */
static int tl_pick_strip_vertex(const TD5_StripSpan *sp, int sub_mode,
                                int *out_vx, int *out_vy, int *out_vz)
{
    if (!sp) return 0;
    const TD5_StripVertex *vL = td5_track_get_vertex((int)sp->left_vertex_index);
    int right_idx = (int)sp->right_vertex_index - 1;
    const TD5_StripVertex *vR = (right_idx >= 0) ? td5_track_get_vertex(right_idx) : NULL;
    if (!vL || !vR) return 0;

    switch (sub_mode) {
    case 0:
        *out_vx = vL->x; *out_vy = vL->y; *out_vz = vL->z;
        return 1;
    case 1:
        *out_vx = vR->x; *out_vy = vR->y; *out_vz = vR->z;
        return 1;
    case 2:
        *out_vx = (vL->x + vR->x) / 2;
        *out_vy = (vL->y + vR->y) / 2;
        *out_vz = (vL->z + vR->z) / 2;
        return 1;
    default:
        return 0;
    }
}

/* Case 1 mid-span: 2 contribution samples taken from the LEFT edge and the
 * RIGHT edge of one spacing-aligned strip. */
static int tl_apply_case1_midspan(const TD5_Actor *actor, const TD5_LightZone *zone, int span)
{
    int spacing = (int)zone->spacing;
    if (spacing == 0) return 0;
    int pos_off_x = (int)zone->pos_off_x;
    int pos_off_y = (int)zone->pos_off_y;
    int multiplier = (int)zone->multiplier;
    int weight_r = (int)zone->weight_r;
    int weight_g = (int)zone->weight_g;
    int weight_b = (int)zone->weight_b;

    /* Snap span to the nearest spacing-grid sample relative to pos_off_x.
     * Mirrors ((spacing/2 - pos_off_x + span) / spacing) * spacing + pos_off_x. */
    int aligned = ((spacing / 2 - pos_off_x + span) / spacing) * spacing + pos_off_x;
    const TD5_StripSpan *sp = td5_track_get_span(aligned);
    if (!sp) return 0;
    /* Right edge index from packed lane-count nibble. */
    int right_lane = (((const uint8_t *)sp)[3]) & 0x0F;
    int right_idx = (int)sp->left_vertex_index + right_lane;
    const TD5_StripVertex *vL = td5_track_get_vertex((int)sp->left_vertex_index);
    const TD5_StripVertex *vR = td5_track_get_vertex(right_idx);
    if (!vL || !vR) return 0;

    int sample0[3] = {
        (int)vL->x + (int)(int16_t)sp->origin_x,
        (int)vL->y + (int)(int16_t)sp->origin_y + pos_off_y,
        (int)vL->z + (int)(int16_t)sp->origin_z,
    };
    int sample1[3] = {
        (int)vR->x + (int)(int16_t)sp->origin_x,
        (int)vR->y + (int)(int16_t)sp->origin_y + pos_off_y,
        (int)vR->z + (int)(int16_t)sp->origin_z,
    };

    tl_sample_contrib(0, actor, sample0, weight_r, weight_g, weight_b, multiplier);
    tl_sample_contrib(1, actor, sample1, weight_r, weight_g, weight_b, multiplier);
    /* Slot 2 disabled. */
    {
        int16_t dummy[3] = { zone->dir_x, zone->dir_y, zone->dir_z };
        tl_set_contrib(2, dummy, 0, 0, 0);
    }
    tl_set_depth((int)zone->amb_r, (int)zone->amb_g, (int)zone->amb_b);
    return 1;
}

/* Case 2 mid-zone: 3 contribution samples taken from (prev, curr, next) strips
 * at +/- spacing relative to the spacing-aligned center, each sample selecting
 * the chosen vertex (sub_mode: 0/1/2). */
static int tl_apply_case2(const TD5_Actor *actor, const TD5_LightZone *zone, int span)
{
    int spacing = (int)zone->spacing;
    if (spacing == 0) return 0;
    int pos_off_x = (int)zone->pos_off_x;
    int pos_off_y = (int)zone->pos_off_y;
    int sub_mode = (int)zone->sub_mode;
    int multiplier = (int)zone->multiplier;
    int weight_r = (int)zone->weight_r;
    int weight_g = (int)zone->weight_g;
    int weight_b = (int)zone->weight_b;

    int aligned = ((spacing / 2 - pos_off_x + span) / spacing) * spacing + pos_off_x;

    for (int s = 0; s < 3; s++) {
        int strip_idx = aligned + (s - 1) * spacing;
        const TD5_StripSpan *sp = td5_track_get_span(strip_idx);
        if (!sp) return 0;
        int vx, vy, vz;
        if (!tl_pick_strip_vertex(sp, sub_mode, &vx, &vy, &vz)) return 0;
        int sample_pos[3] = {
            vx + (int)(int16_t)sp->origin_x,
            vy + (int)(int16_t)sp->origin_y + pos_off_y,
            vz + (int)(int16_t)sp->origin_z,
        };
        tl_sample_contrib(s, actor, sample_pos, weight_r, weight_g, weight_b, multiplier);
    }
    tl_set_depth((int)zone->amb_r, (int)zone->amb_g, (int)zone->amb_b);
    return 1;
}

/* Static-zone case 0: single contribution slot with the zone's stored dir +
 * weights, scalar ambient = avg of amb bytes. */
static void tl_apply_case0(const TD5_LightZone *zone)
{
    int16_t dir[3] = { zone->dir_x, zone->dir_y, zone->dir_z };
    tl_set_contrib(0, dir, (int)zone->weight_r, (int)zone->weight_g, (int)zone->weight_b);
    tl_set_contrib(1, dir, 0, 0, 0);
    tl_set_contrib(2, dir, 0, 0, 0);
    tl_set_depth((int)zone->amb_r, (int)zone->amb_g, (int)zone->amb_b);
}

void td5_render_apply_track_lighting(int slot, TD5_Actor *actor)
{
    if (!actor || slot < 0 || slot >= TD5_ACTOR_MAX_TOTAL_SLOTS) {
        tl_apply_fallback();
        s_ambient_intensity = (float)s_tl_ambient;
        for (int i = 0; i < 9; i++) s_light_dirs[i] = 0.0f;
        return;
    }
    if (s_environs_level < 0 || s_environs_level >= TD5_LIGHT_ZONE_TRACK_COUNT) {
        tl_apply_fallback();
        s_ambient_intensity = (float)s_tl_ambient;
        for (int i = 0; i < 9; i++) s_light_dirs[i] = 0.0f;
        return;
    }

    int span = (int)(int16_t)actor->track_span_raw;
    if (span < 0) {
        tl_apply_fallback();
        s_ambient_intensity = (float)s_tl_ambient;
        for (int i = 0; i < 9; i++) s_light_dirs[i] = 0.0f;
        return;
    }

    int zone_idx = update_actor_light_zone(slot, span);
    if (zone_idx < 0) {
        tl_apply_fallback();
        s_ambient_intensity = (float)s_tl_ambient;
        for (int i = 0; i < 9; i++) s_light_dirs[i] = 0.0f;
        return;
    }

    int track_first = td5_light_zone_track[s_environs_level].first;
    int track_count = td5_light_zone_track[s_environs_level].count;
    const TD5_LightZone *zone = tl_zone_at(track_first, track_count, zone_idx);
    if (!zone) {
        tl_apply_fallback();
        tl_commit_to_render_globals(actor);
        return;
    }

    /* Dispatch on blend_mode (zone +0x18). */
    int handled = 0;
    switch (zone->blend_mode) {
    case 0:
        tl_apply_case0(zone);
        handled = 1;
        break;
    case 1: {
        /* Edges (within 3 spans of either end) blend with neighbour zones;
         * mid-span uses the 2-vertex sample loop. */
        if (span - (int)zone->span_lo < 3) {
            const TD5_LightZone *prev = tl_zone_at(track_first, track_count, zone_idx - 1);
            if (prev) {
                tl_blend_from_start(actor, prev, zone, 0x800);
                handled = 1;
            }
        } else if ((int)zone->span_hi - span < 3) {
            const TD5_LightZone *next = tl_zone_at(track_first, track_count, zone_idx + 1);
            if (next) {
                tl_blend_from_end(actor, zone, next, 0x800);
                handled = 1;
            }
        } else {
            handled = tl_apply_case1_midspan(actor, zone, span);
        }
        break;
    }
    case 2:
        /* Original case 2 falls through to the LAB_00430914 mid-zone loop
         * unconditionally with edge guards: <3 spans into start -> BlendStart,
         * <3 spans from end -> BlendEnd; otherwise multi-sample. */
        if (span - (int)zone->span_lo < 3) {
            const TD5_LightZone *prev = tl_zone_at(track_first, track_count, zone_idx - 1);
            if (prev) {
                tl_blend_from_start(actor, prev, zone, 0x800);
                handled = 1;
            }
        } else if ((int)zone->span_hi - span < 3) {
            const TD5_LightZone *next = tl_zone_at(track_first, track_count, zone_idx + 1);
            if (next) {
                tl_blend_from_end(actor, zone, next, 0x800);
                handled = 1;
            }
        } else {
            handled = tl_apply_case2(actor, zone, span);
        }
        break;
    case 3: {
        /* Half/half full-zone blend: first half uses BlendStart with width-
         * scaled max distance, second half uses BlendEnd. */
        int width = (int)zone->span_hi - (int)zone->span_lo + 1;
        if (width < 1) width = 1;
        int max_dist = width * 0x200;
        if (span - (int)zone->span_lo < width / 2) {
            const TD5_LightZone *prev = tl_zone_at(track_first, track_count, zone_idx - 1);
            if (prev) {
                tl_blend_from_start(actor, prev, zone, max_dist);
                handled = 1;
            }
        } else {
            const TD5_LightZone *next = tl_zone_at(track_first, track_count, zone_idx + 1);
            if (next) {
                tl_blend_from_end(actor, zone, next, max_dist);
                handled = 1;
            }
        }
        break;
    }
    default:
        break;
    }

    if (!handled) {
        /* Any path that couldn't resolve required strip/vertex data falls
         * back to the static case-0 fields so the chassis still gets sane
         * lighting -- matches the spirit of the original's logged "spacing
         * zero" early-out (which leaves the previous frame's basis intact). */
        tl_apply_case0(zone);
    }

    tl_commit_to_render_globals(actor);
}

void td5_render_update_projection_effect(int slot, TD5_Actor *actor)
{
    /*
     * UpdateActorTrackLightState (0x40CD10) -> ConfigureActorProjectionEffect (0x40CBD0)
     * -> SetProjectionEffectState (0x43E210).
     *
     * Walks per-track zones to resolve the actor's light_index, then reads
     * flag[light_index] + page_slot[light_index] from the environs table and
     * writes to the slot's 0x20-byte projection state:
     *   mode 1 (flag=1, planar): slot.+0x00=1.0, slot.+0x04=0.0, slot.+0x08 += (sin·a + cos·b)·1/8192
     *   mode 2 (flag=2, chrome): slot.+0x00=cos(yaw), slot.+0x04=sin(yaw) — DEAD on real tracks
     *     (InitializeTrackStripMetadata @ 0x42FAD0 never writes 2 into the flag table)
     *   mode 3 (flag=3, world-anchor): slot.+0x14/18/1c = anchor world xyz
     * Every mode also writes slot.+0x0C=mode and slot.+0x10=angle_tag.
     *
     * ApplyMeshProjectionEffect @ 0x43DEC0 later reads slot.+0x0C to dispatch,
     * and slot.+0x00/04/08/14/18/1c as mode-specific inputs.
     */
    ProjectionEffectState *pe;
    int zone_idx, light_index;
    int yaw_12bit;
    int flag;
    const TD5_EnvironsTrack *env;

    if (slot < 0 || slot >= TD5_ACTOR_MAX_TOTAL_SLOTS || !actor)
        return;

    pe = &s_proj_effect[slot];
    yaw_12bit = actor->display_angles.yaw & 0xFFF;

    zone_idx = update_actor_light_zone(slot, (int)(int16_t)actor->track_span_raw);
    if (zone_idx < 0 || s_environs_level < 0 ||
        s_environs_level >= TD5_ENVIRONS_TRACK_COUNT) {
        /* No zone: fall back to mode 2 on the first environs page. */
        pe->sub_mode     = 2;
        pe->texture_page = (s_envmap_page_count > 0) ? s_envmap_pages[0] : -1;
        pe->cos_heading  = CosFloat12bit((unsigned int)((-yaw_12bit) & 0xFFF));
        pe->sin_heading  = SinFloat12bit((unsigned int)((-yaw_12bit) & 0xFFF));
        return;
    }
    light_index = td5_light_zones[td5_light_zone_track[s_environs_level].first + zone_idx].light_index;
    if (light_index < 0 || light_index > 3) light_index = 0;
    env = &td5_environs_per_track[s_environs_level];
    flag = env->e[light_index].flag;

    /* Texture page via the per-entry page_slot[] aliasing table. */
    {
        int page_idx = env->page_slot[light_index];
        if (page_idx < 0)           page_idx = 0;
        if (page_idx >= env->count) page_idx = env->count - 1;
        pe->texture_page = (s_envmap_page_count > 0) ? s_envmap_pages[page_idx] : -1;
    }
    pe->sub_mode = flag;

    /* Cache the interpolated actor world position in world units.
     * Mode 3 uses this as the sphere-map anchor; modes 1/2 leave anchor unused. */
    {
        extern float g_subTickFraction;
        const float fp_scale = 1.0f / 256.0f;
        pe->anchor_x = ((float)actor->linear_velocity_x * g_subTickFraction +
                        (float)actor->world_pos.x) * fp_scale;
        pe->anchor_y = ((float)actor->linear_velocity_y * g_subTickFraction +
                        (float)actor->world_pos.y) * fp_scale;
        pe->anchor_z = ((float)actor->linear_velocity_z * g_subTickFraction +
                        (float)actor->world_pos.z) * fp_scale;
    }

    if (flag == 1) {
        /* Mode-1 accumulator: slot.+0x08 += (sin(yaw)·px + cos(yaw)·pz) · 1/8192.
         * The original's param_3 for this call is [UNCERTAIN] — whichever 3-float
         * vector ConfigureActorProjectionEffect passes. The port uses the actor's
         * linear velocity components, consistent with the mode's "forward scroll"
         * semantic: the scroll advances with forward motion so tree/tunnel
         * billboards drift past. Switching to world_pos here would cause the
         * accumulator to jump by world-pos-sized amounts per frame (unusable). */
        float cos_y = CosFloat12bit((unsigned int)(yaw_12bit & 0xFFF));
        float sin_y = SinFloat12bit((unsigned int)(yaw_12bit & 0xFFF));
        pe->cos_heading   = 1.0f;
        pe->sin_heading   = 0.0f;
        pe->scroll_offset += (sin_y * (float)actor->linear_velocity_x +
                              cos_y * (float)actor->linear_velocity_z) * (1.0f / 8192.0f);
    } else {
        /* Modes 2 and 3: store cos/sin(-yaw). Mode 2 reads them as UV rotators.
         * Mode 3 doesn't directly read them but the original always writes them
         * alongside the anchor/mode fields in SetProjectionEffectState. */
        pe->cos_heading = CosFloat12bit((unsigned int)((-yaw_12bit) & 0xFFF));
        pe->sin_heading = SinFloat12bit((unsigned int)((-yaw_12bit) & 0xFFF));
    }

    {
        static int s_proj_log_count = 0;
        if (slot == 0 && (s_proj_log_count < 8 || (s_proj_log_count % 300) == 0)) {
            TD5_LOG_I(LOG_TAG,
                      "proj_effect: slot=%d light_idx=%d flag=%d mode=%d page=%d "
                      "anchor=(%.1f,%.1f,%.1f) scroll=%.3f",
                      slot, light_index, flag, pe->sub_mode, pe->texture_page,
                      pe->anchor_x, pe->anchor_y, pe->anchor_z, pe->scroll_offset);
        }
        if (slot == 0) s_proj_log_count++;
    }
}

/**
 * Render the chrome/reflection overlay for a vehicle.
 * Called after the normal car mesh has been rendered.
 *
 * Original (RenderRaceActorForView @ 0x40C120): after normal mesh,
 * if mode==2 AND actor==slot 0: transform reflection mesh, apply
 * mode-2 UV rewrite, render with translucent blend.
 *
 * The original duplicates the himodel into a separate mesh resource
 * (g_playerReflectionMeshResource @ 0x4C3D40) with command_count=1.
 * For simplicity, we re-use the same mesh but override the texture
 * page per-command and render with translucent preset.
 */
static void render_vehicle_reflection_overlay(TD5_MeshHeader *mesh, int slot)
{
    ProjectionEffectState *pe;
    TD5_MeshVertex *verts;
    TD5_PrimitiveCmd *cmds;
    int cmd_count, vert_count;
    int i;

    if (s_proj_effect_mode != 2) return;
    if (!mesh) return;

    pe = &s_proj_effect[slot];
    if (pe->texture_page < 0) return;

    verts = (TD5_MeshVertex *)(uintptr_t)mesh->vertices_offset;
    vert_count = mesh->total_vertex_count;
    if (!verts || vert_count <= 0) return;

    /* Mode dispatch happens inside apply_mesh_projection_effect based on
     * this slot's pe->sub_mode (1=planar, 2=yaw-UV, 3=world-anchor). Mode 3
     * reads normals[] from mesh->normals_offset, so the helper now takes the
     * whole mesh header. */
    td5_render_apply_mesh_projection_effect(mesh, slot);

    /* Render the mesh with the environs texture and translucent blend.
     * Original uses a separate mesh with command_count=1, but we
     * iterate the full command list, overriding each command's texture
     * page to the environs page. */
    cmds = (TD5_PrimitiveCmd *)(uintptr_t)mesh->commands_offset;
    cmd_count = mesh->command_count;
    if (!cmds || cmd_count <= 0) return;

    /* Save original texture pages, override with environs */
    int saved_pages[256];
    if (cmd_count > 256) cmd_count = 256;

    for (i = 0; i < cmd_count; i++) {
        saved_pages[i] = cmds[i].texture_page_id;
        cmds[i].texture_page_id = (int16_t)pe->texture_page;
    }

    /* Cap vertex count to stack budget */
#define REFLECTION_MAX_VERTS 4096
    int save_count = (vert_count < REFLECTION_MAX_VERTS) ? vert_count : REFLECTION_MAX_VERTS;

    /* Additive blend for the reflection overlay.
     * Chrome reflections ADD light on top of the car body color, making
     * highlights brighter/whiter — matching the original's visual style.
     * ADDITIVE preset: src=ONE, dst=ONE, z_test=1, z_write=0.
     *
     * The s_in_reflection_overlay flag suppresses td5_render_apply_page_blend_preset
     * from overriding ADDITIVE during the reflection draw — env-map pages are
     * type 2 (TRANSLUCENT_ANISO normally), and without this flag the per-bind
     * switch would re-set the preset to TRANSLUCENT mid-draw, painting the
     * env-map opaquely over the body (bug: "cars only show reflection"). */
    s_in_reflection_overlay = 1;
    td5_plat_render_set_preset(TD5_PRESET_ADDITIVE);

    /* Save and override vertex lighting + UVs for the reflection pass.
     * Vertex color must be white with partial alpha so the environs
     * texture shows through as a chrome tint. The high byte being
     * non-zero (0x66) bypasses the color LUT in flush_immediate_internal.
     * ARGB format: A=0x66 (40%), R=G=B=0xFF (white). */
    uint32_t saved_lighting[REFLECTION_MAX_VERTS];
    float saved_uv[REFLECTION_MAX_VERTS][2];
    for (i = 0; i < save_count; i++) {
        saved_lighting[i] = verts[i].lighting;
        saved_uv[i][0] = verts[i].tex_u;
        saved_uv[i][1] = verts[i].tex_v;

        verts[i].lighting = 0xFFBBBBBBu; /* additive: ~73% white intensity */
        verts[i].tex_u = verts[i].proj_u;
        verts[i].tex_v = verts[i].proj_v;
    }

    /* Render the reflection mesh */
    td5_render_prepared_mesh(mesh);

    {
        static int s_refl_log_count = 0;
        if (s_refl_log_count < 10 || (s_refl_log_count % 300) == 0) {
            TD5_LOG_I(LOG_TAG,
                      "reflection overlay: slot=%d page=%d verts=%d cmds=%d "
                      "cos=%.3f sin=%.3f",
                      slot, pe->texture_page, save_count, cmd_count,
                      pe->cos_heading, pe->sin_heading);
        }
        s_refl_log_count++;
    }

    /* Restore original UVs, lighting, and texture pages */
    for (i = 0; i < save_count; i++) {
        verts[i].tex_u = saved_uv[i][0];
        verts[i].tex_v = saved_uv[i][1];
        verts[i].lighting = saved_lighting[i];
    }
    for (i = 0; i < cmd_count; i++) {
        cmds[i].texture_page_id = (int16_t)saved_pages[i];
    }

    /* Restore opaque preset for subsequent geometry */
    s_in_reflection_overlay = 0;
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
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

/* --- Vehicle Shadow Projection (0x40C120 / 0x40BB70) ---
 *
 * Original (InitializeVehicleShadowAndWheelSpriteTemplates @ 0x40bb70 +
 * RenderRaceActorForView @ 0x40c120) draws the vehicle shadow as textured
 * quads sampling the SHADOW atlas entry on tpage5 (128x64 at atlas
 * (128,64)), vertex color 0xFFFFFFFF, darkness from the texture alpha.
 * Corners derived from the 4 wheel probe positions at actor+0x90..+0xbc.
 *
 * The original draws shadows via a translucent sort list with z-test
 * disabled entirely — the -22.0f offset is a pure screen-space nudge, never
 * depth-tested against the car or the track.
 *
 * Source port approach:
 *   - TRANSLUCENT_POINT (z_test=0, z_write=0, alpha_ref=1, SRCALPHA/
 *     INVSRCALPHA). No depth test means no terrain-slope flicker and no
 *     alpha-cutoff cropping of the feathered shadow edges.
 *   - The call site draws the shadow BEFORE the car mesh (and the
 *     wheels). The opaque car body and wheels draw afterwards with
 *     z_write=1 and paint over the shadow where they exist in screen
 *     space, so the car correctly "occludes" the shadow without a
 *     depth test on the shadow itself.
 *   - Scale corners outward from the XZ centroid by 1.85 to approximate
 *     the unread _g_wheelSuspensionRenderScale and give the shadow a
 *     footprint larger than the raw wheel spread.
 *   - Corners stay at wheel-contact Y; no vertical lift is needed
 *     because the shadow is painted at ground level and then occluded
 *     by the car body via draw order, not depth test.
 *   - Subtick-interpolate corners with linear_velocity * g_subTickFraction
 *     so the shadow doesn't sawtooth-lag behind the car at speed (the
 *     car mesh is interpolated the same way at line ~1547).
 */
#define SHADOW_VERTICAL_OFFSET  (0.0f)
#define SHADOW_CORNER_SCALE     (1.85f)
#define SHADOW_VIEW_Y_OFFSET    (-22.0f)   /* [CONFIRMED @ 0x40C5CC] push shadow below car in view-space */

static int   s_shadow_lookup_done = 0;
static int   s_shadow_page        = -1;
static float s_shadow_u0, s_shadow_v0, s_shadow_u1, s_shadow_v1;

static void shadow_lookup_static_hed(void)
{
    s_shadow_lookup_done = 1;
    TD5_AtlasEntry *sh = td5_asset_find_atlas_entry(NULL, "SHADOW");
    if (!sh || sh->texture_page <= 0 || sh->width <= 0 || sh->height <= 0) {
        TD5_LOG_W(LOG_TAG, "shadow: SHADOW atlas entry not found");
        return;
    }
    int tw = 256, th = 256;
    td5_plat_render_get_texture_dims(sh->texture_page, &tw, &th);
    float inv_w = 1.0f / (float)tw;
    float inv_h = 1.0f / (float)th;
    /* Half-pixel inset to avoid neighbour bleed. */
    s_shadow_u0 = ((float)sh->atlas_x + 0.5f) * inv_w;
    s_shadow_v0 = ((float)sh->atlas_y + 0.5f) * inv_h;
    s_shadow_u1 = ((float)(sh->atlas_x + sh->width)  - 0.5f) * inv_w;
    s_shadow_v1 = ((float)(sh->atlas_y + sh->height) - 0.5f) * inv_h;
    s_shadow_page = sh->texture_page;
    TD5_LOG_I(LOG_TAG,
              "shadow: atlas uv=(%.3f,%.3f..%.3f,%.3f) page=%d",
              s_shadow_u0, s_shadow_v0, s_shadow_u1, s_shadow_v1, s_shadow_page);
}

static void render_vehicle_shadow_quad(const TD5_Actor *actor)
{
    if (!actor) return;
    if (!s_shadow_lookup_done) shadow_lookup_static_hed();
    if (s_shadow_page < 0) return;

    /* Wheel probe positions (world-space, 24.8 fixed) at actor +0x90..+0xbc.
     * [CONFIRMED @ 0x40c3d0-0x40c5a0, td5_actor_struct.h:232-235]
     * Order in original: FL, FR, RL, RR. For CW winding (viewed from +Y
     * down) we emit FL, FR, RR, RL. */
    const TD5_Vec3_Fixed *probes[4] = {
        &actor->probe_FL,
        &actor->probe_FR,
        &actor->probe_RR,
        &actor->probe_RL,
    };

    /* UV corners matching the FL/FR/RR/RL corner order. */
    const float uvs[4][2] = {
        { s_shadow_u0, s_shadow_v0 },
        { s_shadow_u1, s_shadow_v0 },
        { s_shadow_u1, s_shadow_v1 },
        { s_shadow_u0, s_shadow_v1 },
    };

    /* Convert the 4 probes to world-float, accumulating the XZ centroid so we
     * can scale corners outward from it (the original uses
     * _g_wheelSuspensionRenderScale for the same effect). Y is kept at each
     * wheel's contact height so the shadow follows uneven terrain.
     *
     * Subtick interpolation: probes are only refreshed once per sim tick, but
     * the car mesh is rendered with (world_pos + linear_velocity *
     * g_subTickFraction) / 256 per render frame (see line ~1547). Without
     * applying the same delta to the shadow corners, the shadow sawtooth-
     * lags behind the car at speed and produces edge flicker as probes jump
     * each sim tick. This mirrors the camera/overlay subtick invariant. */
    extern float g_subTickFraction;
    const float frac = g_subTickFraction;
    const float inv256 = 1.0f / 256.0f;
    const float interp_dx = (float)actor->linear_velocity_x * frac * inv256;
    const float interp_dy = (float)actor->linear_velocity_y * frac * inv256;
    const float interp_dz = (float)actor->linear_velocity_z * frac * inv256;

    float corners[4][3];
    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
    for (int i = 0; i < 4; i++) {
        corners[i][0] = (float)probes[i]->x * inv256 + interp_dx;
        corners[i][1] = (float)probes[i]->y * inv256 + interp_dy + SHADOW_VERTICAL_OFFSET;
        corners[i][2] = (float)probes[i]->z * inv256 + interp_dz;
        cx += corners[i][0];
        cy += corners[i][1];
        cz += corners[i][2];
    }
    cx *= 0.25f;
    cy *= 0.25f;
    cz *= 0.25f;
    /* Flatten all corners to the centroid Y so the shadow lies flat on the
     * ground regardless of slope. Without this, front/rear probe Y differences
     * (e.g. 24-88 world units on AI cars) create a tilted quad that renders
     * as tall diagonal streaks instead of a ground-hugging shadow. */
    for (int i = 0; i < 4; i++) {
        corners[i][0] = cx + (corners[i][0] - cx) * SHADOW_CORNER_SCALE;
        corners[i][1] = cy;
        corners[i][2] = cz + (corners[i][2] - cz) * SHADOW_CORNER_SCALE;
    }

    TD5_D3DVertex verts[4];
    for (int i = 0; i < 4; i++) {
        float dx = corners[i][0] - s_camera_pos[0];
        float dy = corners[i][1] - s_camera_pos[1];
        float dz = corners[i][2] - s_camera_pos[2];

        /* camera_basis is row-major { right, up, forward } */
        float vx = dx * s_camera_basis[0] + dy * s_camera_basis[1] + dz * s_camera_basis[2];
        float vy = dx * s_camera_basis[3] + dy * s_camera_basis[4] + dz * s_camera_basis[5];
        float vz = dx * s_camera_basis[6] + dy * s_camera_basis[7] + dz * s_camera_basis[8];

        /* Push shadow down in view-space so it renders on the ground below
         * the car, not at wheel-probe height. [CONFIRMED @ 0x40C5CC] */
        vy += SHADOW_VIEW_Y_OFFSET;

        if (vz <= s_near_clip) return;

        float inv_z = 1.0f / vz;
        verts[i].screen_x = -vx * s_focal_length * inv_z + s_center_x;
        verts[i].screen_y = -vy * s_focal_length * inv_z + s_center_y;
        verts[i].depth_z  = vz * (1.0f / s_far_clip);
        verts[i].rhw      = inv_z;
        verts[i].diffuse  = 0xFFFFFFFFu;   /* white — alpha comes from texture */
        verts[i].specular = 0;
        verts[i].tex_u    = uvs[i][0];
        verts[i].tex_v    = uvs[i][1];
    }

    static int s_shadow_draw_logged = 0;
    if (!s_shadow_draw_logged) {
        s_shadow_draw_logged = 1;
        TD5_LOG_I(LOG_TAG,
                  "shadow: first draw page=%d uv=(%.3f..%.3f,%.3f..%.3f) "
                  "FL=(%.1f,%.1f,%.1f) lift=%.1f scale=%.2f",
                  s_shadow_page, s_shadow_u0, s_shadow_u1, s_shadow_v0, s_shadow_v1,
                  (float)actor->probe_FL.x * (1.0f/256.0f),
                  (float)actor->probe_FL.y * (1.0f/256.0f),
                  (float)actor->probe_FL.z * (1.0f/256.0f),
                  SHADOW_VERTICAL_OFFSET, SHADOW_CORNER_SCALE);
    }

    uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    flush_immediate_internal();
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_POINT);
    td5_plat_render_bind_texture(s_shadow_page);
    td5_plat_render_draw_tris(verts, 4, indices, 6);

    /* Restore opaque preset so it doesn't leak into the car mesh draw. */
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

/* --- Vehicle Brake Lights (0x4011C0) --- */

/* BRAKED sprite atlas cache */
static int   s_braked_page = -1;
static float s_braked_u0, s_braked_v0, s_braked_u1, s_braked_v1;
static int   s_braked_lookup_done = 0;
static uint8_t s_brake_brightness[12]; /* per-slot brightness ramp */

static void brake_light_lookup_atlas(void)
{
    s_braked_lookup_done = 1;
    memset(s_brake_brightness, 0, sizeof(s_brake_brightness));

    TD5_AtlasEntry *e = td5_asset_find_atlas_entry(NULL, "BRAKED");
    if (!e || e->texture_page <= 0) {
        TD5_LOG_W(RENDER_LOG_TAG, "brake: BRAKED sprite not found");
        return;
    }
    int tw = 256, th = 256;
    td5_plat_render_get_texture_dims(e->texture_page, &tw, &th);
    s_braked_u0 = ((float)e->atlas_x + 0.5f) / (float)tw;
    s_braked_v0 = ((float)e->atlas_y + 0.5f) / (float)th;
    s_braked_u1 = ((float)(e->atlas_x + e->width) - 0.5f) / (float)tw;
    s_braked_v1 = ((float)(e->atlas_y + e->height) - 0.5f) / (float)th;
    s_braked_page = e->texture_page;
    TD5_LOG_I(RENDER_LOG_TAG,
              "brake: page=%d uv=(%.4f,%.4f)-(%.4f,%.4f) dim=%dx%d",
              s_braked_page, s_braked_u0, s_braked_v0,
              s_braked_u1, s_braked_v1, e->width, e->height);
}

/**
 * Draw brake light sprites at the two taillight hardpoints.
 * Called from the actor render loop where the render transform
 * (camera basis * actor rotation + translation) is already loaded.
 *
 * Hardpoints: car_config+0x60 (left), car_config+0x68 (right).
 * Each is int16[3] in model space.
 */
static void render_vehicle_brake_lights(const TD5_Actor *actor, int slot)
{
    if (!actor) return;
    if (!s_braked_lookup_done) brake_light_lookup_atlas();
    if (s_braked_page < 0) return;
    if (slot < 0 || slot >= 12) return;

    /* Read brake_flag at actor+0x36D */
    const uint8_t *ap = (const uint8_t *)actor;
    int braking = (*(ap + 0x36D) != 0);

    /* Brightness ramp / decay */
    uint8_t bright = s_brake_brightness[slot];
    if (braking) {
        if (bright < 0x80) bright += 8;
        if (bright > 0x80) bright = 0x80;
    } else {
        bright >>= 1;
    }
    s_brake_brightness[slot] = bright;
    if (bright == 0) return;

    /* Car definition pointer at actor+0x1B8 */
    void *car_def = NULL;
    memcpy(&car_def, ap + 0x1B8, sizeof(void *));
    if (!car_def) return;

    const float *m = s_render_transform.m;
    const float half_size = 40.0f; /* model-space half-extent (original ±80 / 2) */

    for (int light = 0; light < 2; light++) {
        /* Read hardpoint int16[3] at car_def+0x60 / +0x68 */
        int16_t hp[3];
        memcpy(hp, (uint8_t *)car_def + 0x60 + light * 8, 6);

        float px = (float)hp[0];
        float py = (float)hp[1];
        float pz = (float)hp[2];

        /* Transform hardpoint center through the render matrix to view space */
        float vx = px*m[0] + py*m[1] + pz*m[2] + m[9];
        float vy = px*m[3] + py*m[4] + pz*m[5] + m[10];
        float vz = px*m[6] + py*m[7] + pz*m[8] + m[11];

        if (vz <= s_near_clip) continue;

        float inv_z = 1.0f / vz;
        float cx = -vx * s_focal_length * inv_z + s_center_x;
        float cy = -vy * s_focal_length * inv_z + s_center_y;
        float depth = vz * (1.0f / s_far_clip);

        /* Screen-space half-size: perspective-scale the billboard */
        float h = half_size * s_focal_length * inv_z;

        TD5_D3DVertex v[4];
        /* TL */
        v[0].screen_x = cx - h;  v[0].screen_y = cy - h;
        v[0].depth_z  = depth;   v[0].rhw      = inv_z;
        v[0].diffuse  = 0xFFFFFFFF; v[0].specular = 0;
        v[0].tex_u    = s_braked_u0; v[0].tex_v = s_braked_v0;
        /* BL */
        v[1].screen_x = cx - h;  v[1].screen_y = cy + h;
        v[1].depth_z  = depth;   v[1].rhw      = inv_z;
        v[1].diffuse  = 0xFFFFFFFF; v[1].specular = 0;
        v[1].tex_u    = s_braked_u0; v[1].tex_v = s_braked_v1;
        /* TR */
        v[2].screen_x = cx + h;  v[2].screen_y = cy - h;
        v[2].depth_z  = depth;   v[2].rhw      = inv_z;
        v[2].diffuse  = 0xFFFFFFFF; v[2].specular = 0;
        v[2].tex_u    = s_braked_u1; v[2].tex_v = s_braked_v0;
        /* BR */
        v[3].screen_x = cx + h;  v[3].screen_y = cy + h;
        v[3].depth_z  = depth;   v[3].rhw      = inv_z;
        v[3].diffuse  = 0xFFFFFFFF; v[3].specular = 0;
        v[3].tex_u    = s_braked_u1; v[3].tex_v = s_braked_v1;

        uint16_t idx[6] = { 0, 1, 2, 1, 3, 2 };
        flush_immediate_internal();
        /* TRANSLUCENT_POINT disables z-test (like the shadow quad) so the
         * brake light billboard doesn't z-fight with the car body mesh. */
        td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_POINT);
        td5_plat_render_bind_texture(s_braked_page);
        td5_plat_render_draw_tris(v, 4, idx, 6);
    }

    /* Restore opaque so it doesn't leak into next mesh */
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

/* --- Vehicle Wheel Billboards (0x446F00) --- */

/**
 * Wheel rendering (0x446F00 + 0x446A70).
 *
 * The original renders each wheel as 8 tire-sidewall quads + 1 hub-cap quad,
 * all from static tpage5:
 *
 *   1. Tire sidewall (8 quads, rim ring): template binds COLOURS texture
 *      entry with all 4 corners at the single pixel
 *      (COLOURS.atlas_x+1, COLOURS.atlas_y+1). Flat-color sample — the
 *      pixel at (65,177) on tpage5 is (8,8,8) near-black → black tires.
 *      [CONFIRMED @ 0x446B44-0x446B6A + tpage5.png pixel read]
 *
 *   2. Hub-cap disc: INWHEEL atlas entry drawn as a flat quad perpendicular
 *      to the axle. [CONFIRMED @ 0x446A70 hub-cap template init]
 *
 * Wheel dimensions from cardef [CONFIRMED @ 0x446E30-0x446E3C]:
 *   rim_radius = cardef+0x82 * 0.76171875 (195/256, DAT_0045D7AC)
 *   axle_halfw = cardef+0x84 (raw int16, no scaling)
 *
 * NOTE: The original runtime-patches WHEELS tpage cells with per-car
 * CARHUB0-3.TGA blits (LoadRaceTexturePages @ 0x442770) and draws an
 * animated hub-cap quad using those cells.  The port composites all 4
 * frames into a 128×128 sprite sheet (2×2 of 64×64 tiles) per slot on
 * page 800+slot*2+1 (td5_asset.c, LoadRaceTexturePages analog).
 */
#define WHEEL_SEGMENTS       8
#define WHEEL_RADIUS_SCALE   0.76171875f  /* 195/256, from DAT_0045D7AC */
#define WHEEL_RADIUS_DEFAULT 110.0f       /* fallback: 0x90 * 0.76171875 */
#define WHEEL_HALFW_DEFAULT  28.0f        /* fallback axle half-width */

/* Cached lookups from static.hed (tpage5) */
static int   s_wheel_tex_page = -1;  /* static tpage5 — holds COLOURS + INWHEEL */
static float s_tire_u, s_tire_v;     /* COLOURS(+1,+1) flat-color sample */
static float s_inwheel_u0, s_inwheel_v0, s_inwheel_u1, s_inwheel_v1;
static int   s_wheel_lookup_done = 0;

static void wheel_lookup_static_hed(void)
{
    s_wheel_lookup_done = 1;

    int tw = 256, th = 256;
    int got_page = 0;

    /* COLOURS palette entry on tpage5 — tire color is the pixel at (+1,+1)
     * which is (8,8,8) near-black. [CONFIRMED @ 0x446B44-0x446B6A] */
    TD5_AtlasEntry *colours = td5_asset_find_atlas_entry(NULL, "COLOURS");
    if (colours && colours->texture_page > 0) {
        s_wheel_tex_page = colours->texture_page;
        td5_plat_render_get_texture_dims(colours->texture_page, &tw, &th);
        s_tire_u = ((float)colours->atlas_x + 1.5f) / (float)tw;
        s_tire_v = ((float)colours->atlas_y + 1.5f) / (float)th;
        got_page = 1;
    } else {
        /* Fallback: page 5 pixel (65,177) with half-pixel inset */
        s_tire_u = 65.5f / 256.0f;
        s_tire_v = 177.5f / 256.0f;
        TD5_LOG_W(LOG_TAG, "wheel: COLOURS not found, using fallback tire UV");
    }

    TD5_AtlasEntry *inwheel = td5_asset_find_atlas_entry(NULL, "INWHEEL");
    if (inwheel && inwheel->texture_page > 0) {
        if (!got_page) {
            s_wheel_tex_page = inwheel->texture_page;
            td5_plat_render_get_texture_dims(inwheel->texture_page, &tw, &th);
        }
        s_inwheel_u0 = ((float)inwheel->atlas_x + 0.5f) / (float)tw;
        s_inwheel_v0 = ((float)inwheel->atlas_y + 0.5f) / (float)th;
        s_inwheel_u1 = ((float)(inwheel->atlas_x + inwheel->width) - 0.5f) / (float)tw;
        s_inwheel_v1 = ((float)(inwheel->atlas_y + inwheel->height) - 0.5f) / (float)th;
    } else {
        s_inwheel_u0 = 0.5f / 256.0f;
        s_inwheel_v0 = 192.5f / 256.0f;
        s_inwheel_u1 = 15.5f / 256.0f;
        s_inwheel_v1 = 207.5f / 256.0f;
        TD5_LOG_W(LOG_TAG, "wheel: INWHEEL not found, using fallback UVs");
    }

    TD5_LOG_I(LOG_TAG,
              "wheel: page=%d tire_uv=(%.4f,%.4f) inwheel_uv=(%.4f,%.4f)-(%.4f,%.4f)",
              s_wheel_tex_page, s_tire_u, s_tire_v,
              s_inwheel_u0, s_inwheel_v0, s_inwheel_u1, s_inwheel_v1);
}

static void render_vehicle_wheel_billboards(TD5_Actor *actor, int slot)
{
    const float *m = s_render_transform.m;

    if (!s_wheel_lookup_done)
        wheel_lookup_static_hed();

    /* Read wheel dimensions from cardef (RE: 0x446E30-0x446E3C).
     *   cardef+0x82 (int16) -> raw rim value
     *     - Tire ring radius = raw * 0.76171875 (DAT_0045D7AC)
     *   cardef+0x84 (int16) -> axle_halfw (raw, no scaling) */
    float rim_radius = WHEEL_RADIUS_DEFAULT;
    float axle_halfw = WHEEL_HALFW_DEFAULT;
    if (actor->car_definition_ptr) {
        int16_t r = *(int16_t *)((uint8_t *)actor->car_definition_ptr + 0x82);
        if (r > 0) rim_radius = (float)r * WHEEL_RADIUS_SCALE;
        int16_t hw = *(int16_t *)((uint8_t *)actor->car_definition_ptr + 0x84);
        if (hw > 0) axle_halfw = (float)hw;
    }

    /* Hub-cap spin rotation around the wheel axle.
     * Original (0x446F00) pre-computes:
     *   front_angle_12b = accumulated_tire_slip_z * -4  [CONFIRMED @ 0x446F15]
     *   rear_angle_12b  = accumulated_tire_slip_x * -4  [CONFIRMED @ 0x446F23]
     * The slip fields accumulate forward speed every physics tick
     * (td5_physics.c:1223-1224), so they act as an odometer that drives
     * the continuous hub-cap rotation. Angle is in the game's 12-bit unit
     * (4096 = full turn) -> radians via (2*PI/4096) = PI/2048. */
    int32_t slip_front_12 = (int32_t)actor->accumulated_tire_slip_z * -4;
    int32_t slip_rear_12  = (int32_t)actor->accumulated_tire_slip_x * -4;
    float front_rad = (float)slip_front_12 * ((float)M_PI / 2048.0f);
    float rear_rad  = (float)slip_rear_12  * ((float)M_PI / 2048.0f);
    float front_cos = cosf(front_rad), front_sin = sinf(front_rad);
    float rear_cos  = cosf(rear_rad),  rear_sin  = sinf(rear_rad);

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
        TD5_LOG_I(LOG_TAG,
                  "wheel: slot=%d radius=%.1f halfw=%.1f steer_cmd=%d tex_page=%d",
                  slot, rim_radius, axle_halfw, actor->steering_command, tex_page);
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

        /* 18 vertices: inner ring (0..8) + outer ring (9..17).
         * Tire sidewall is textured with tpage5 sampled at a single COLOURS
         * pixel → all 4 quad corners pull the same near-black (8,8,8) texel,
         * which is how the original gets its flat black tire color. */
        TD5_D3DVertex verts[18];
        int all_visible = 1;

        for (int i = 0; i <= WHEEL_SEGMENTS; i++) {
            float cy = ring_y[i];
            float cz = ring_z[i];

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
                verts[i].tex_u = s_tire_u;
                verts[i].tex_v = s_tire_v;
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
                verts[9+i].tex_u = s_tire_u;
                verts[9+i].tex_v = s_tire_v;
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

        /* Hub-cap: per-slot 64x64 carhub texture (page 800+slot*2+1).
         * The carhub PNG stores 4 motion-blur frames in a 2x2 grid of 32x32
         * sub-tiles. Pixels outside the hubcap disc are alpha=0.
         * Use OPAQUE_LINEAR (alpha_ref=1, z_enable=1) so the transparent
         * carhub corners are discarded via alpha test while the depth test
         * still keeps the hub behind the car body. TRANSLUCENT_LINEAR is a
         * 2D overlay preset (z_enable=0) and would bleed through bodywork.
         *
         * Spin frame [CONFIRMED @ 0x446F00]:
         *   frame = min(abs(long_speed) >> 14, 3)
         *   col   = frame & 1,  row = frame >> 1 */
        int hub_page = 800 + slot * 2 + 1;
        int32_t spd_raw = actor->longitudinal_speed;
        uint32_t spd_abs = (uint32_t)(spd_raw < 0 ? -spd_raw : spd_raw);
        int spin_frame = (int)(spd_abs >> 14);
        if (spin_frame > 3) spin_frame = 3;
        int spin_col = spin_frame & 1;
        int spin_row = spin_frame >> 1;
        {
            /* Hub-cap disc: center vertex + 8 perimeter vertices (at the same
             * rim_radius as the tyre ring) drawn as a triangle fan. The
             * original port used a 4-vertex diamond inscribed in the rim
             * circle, which left four triangular gaps between the diamond
             * edges and the tyre ring through which the opposite side of the
             * car (and the ground) was visible. A 9-vertex disc fills the
             * whole wheel face.
             *
             * Spin rotation matches the original diamond convention: corner
             * at unrotated angle θ_i rotates to
             *   ( cos(θ_i)*C + sin(θ_i)*S, -cos(θ_i)*S + sin(θ_i)*C )
             * where C = rot_cos, S = rot_sin. Verified against the diamond
             * at θ=0 → ( C, -S), θ=π/2 → ( S,  C). */
            float ho = outer_off;
            float rot_cos = (w < 2) ? front_cos : rear_cos;
            float rot_sin = (w < 2) ? front_sin : rear_sin;
            float hub_r  = rim_radius;

            /* Hub texture: carhubN.png is a 64×64 sheet of four 32×32
             * sub-frames in a 2×2 layout. Tile col = frame&1, row = frame>>1
             * [CONFIRMED @ 0x004470C0]. UV samples a 30/64-half-width window
             * (1-texel margin) centered on the chosen sub-tile, so the
             * wrapper's LINEAR_WRAP sampler can't blend adjacent sub-frames
             * at the disc perimeter. */
            const float hub_cu = ((float)(spin_col * 32 + 16)) / 64.0f;
            const float hub_cv = ((float)(spin_row * 32 + 16)) / 64.0f;
            const float hub_ru = 15.0f / 64.0f;

            static const float k_hub_unit_y[8] = {
                1.0f,  0.70710678f,  0.0f, -0.70710678f,
               -1.0f, -0.70710678f,  0.0f,  0.70710678f,
            };
            static const float k_hub_unit_z[8] = {
                0.0f,  0.70710678f,  1.0f,  0.70710678f,
                0.0f, -0.70710678f, -1.0f, -0.70710678f,
            };

            TD5_D3DVertex hub[9];
            int hub_ok = 1;

            /* Vertex 0: disc center (axle centre at the outer face). */
            {
                float dx0 = ho;
                float px = wx + dx0 * cos_s;
                float py = wy;
                float pz = wz + dx0 * sin_s;
                float vx = px*m[0] + py*m[1] + pz*m[2] + m[9];
                float vy = px*m[3] + py*m[4] + pz*m[5] + m[10];
                float vz = px*m[6] + py*m[7] + pz*m[8] + m[11];
                if (vz <= s_near_clip) { hub_ok = 0; }
                else {
                    float inv_z = 1.0f / vz;
                    hub[0].screen_x = -vx * s_focal_length * inv_z + s_center_x;
                    hub[0].screen_y = -vy * s_focal_length * inv_z + s_center_y;
                    hub[0].depth_z  = vz * (1.0f / s_far_clip);
                    hub[0].rhw      = inv_z;
                    hub[0].diffuse  = 0xFFFFFFFFu;
                    hub[0].specular = 0;
                    hub[0].tex_u    = hub_cu;
                    hub[0].tex_v    = hub_cv;
                }
            }

            /* Perimeter vertices: unrotated unit directions in YZ rotated by
             * the spin angle and scaled to hub_r, then UV set from the
             * unrotated direction so the texture rotates with the disc. */
            for (int c = 0; c < 8 && hub_ok; c++) {
                float uy = k_hub_unit_y[c];
                float uz = k_hub_unit_z[c];
                float ry = uy * rot_cos + uz * rot_sin;
                float rz = -uy * rot_sin + uz * rot_cos;

                float dx0 = ho;
                float dz0 = rz * hub_r;
                float px = wx + dx0 * cos_s - dz0 * sin_s;
                float py = wy + ry * hub_r;
                float pz = wz + dx0 * sin_s + dz0 * cos_s;

                float vx = px*m[0] + py*m[1] + pz*m[2] + m[9];
                float vy = px*m[3] + py*m[4] + pz*m[5] + m[10];
                float vz = px*m[6] + py*m[7] + pz*m[8] + m[11];
                if (vz <= s_near_clip) { hub_ok = 0; break; }
                float inv_z = 1.0f / vz;
                hub[1+c].screen_x = -vx * s_focal_length * inv_z + s_center_x;
                hub[1+c].screen_y = -vy * s_focal_length * inv_z + s_center_y;
                hub[1+c].depth_z  = vz * (1.0f / s_far_clip);
                hub[1+c].rhw      = inv_z;
                hub[1+c].diffuse  = 0xFFFFFFFFu;
                hub[1+c].specular = 0;
                hub[1+c].tex_u    = hub_cu + uy * hub_ru;
                hub[1+c].tex_v    = hub_cv + uz * hub_ru;
            }

            if (hub_ok) {
                /* Triangle fan: 8 front tris + 8 back tris = 48 indices. */
                uint16_t hub_idx[48];
                int hi = 0;
                for (int c = 0; c < 8; c++) {
                    uint16_t a = (uint16_t)(1 + c);
                    uint16_t b = (uint16_t)(1 + ((c + 1) & 7));
                    /* Front face */
                    hub_idx[hi++] = 0; hub_idx[hi++] = a; hub_idx[hi++] = b;
                    /* Back face (reverse winding) */
                    hub_idx[hi++] = 0; hub_idx[hi++] = b; hub_idx[hi++] = a;
                }
                flush_immediate_internal();
                td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
                td5_plat_render_bind_texture(hub_page);
                td5_plat_render_draw_tris(hub, 9, hub_idx, 48);
            }
        }
    }
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
    /* Original AngleFromVector12 @ 0x0040A720 is a LUT-based arctan using the
     * 1024-entry int16 table at DAT_00463214, effectively returning the
     * round-to-nearest integer angle in 12-bit units (0..0xFFF = full circle).
     * The atan2-based port rounded toward zero via (int) cast, producing a
     * consistent ±1 off-by-one vs the LUT for most input vectors — which
     * propagated to disp_yaw at /diff-race sim_tick=1 post_ai (orig=3824 vs
     * port=3823 across every actor, every tick). lround matches the LUT's
     * round-half-away-from-zero behavior closely enough to close the gap
     * for the observed spawn-heading inputs. If residuals remain, port the
     * full DAT_00463214 LUT (0x400 int16 entries = 2048 bytes). */
    double rad = atan2((double)x, (double)z);
    long angle = lround(rad * (4096.0 / (2.0 * M_PI)));
    return (int)(angle & 0xFFF);
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

/* Submit a pre-built translucent quad using the POINT-filter preset, which
 * uses alpha_ref=1 instead of the LINEAR preset's 0x80. Needed for surfaces
 * that want fractional vertex-alpha transparency below 0x80 — primarily the
 * minimap background grid, whose tiles need to stay under ~50% opacity. */
void td5_render_submit_translucent_low_ref(uint16_t *quad_data) {
    float *fdata;
    TD5_D3DVertex verts[4];
    uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    int tex_page;
    int i;

    if (!quad_data) return;

    fdata = (float *)quad_data;
    for (i = 0; i < 4; i++) {
        int base = 2 + i * 7;
        verts[i].screen_x = fdata[base + 0];
        verts[i].screen_y = fdata[base + 1];
        verts[i].depth_z  = fdata[base + 2];
        verts[i].rhw      = fdata[base + 3];
        verts[i].diffuse  = *(uint32_t *)&fdata[base + 4];
        verts[i].specular = 0;
        verts[i].tex_u    = fdata[base + 5];
        verts[i].tex_v    = fdata[base + 6];
    }

    tex_page = (int)(*(float *)((uint8_t *)quad_data + 0x90));
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_POINT);
    td5_plat_render_bind_texture(tex_page);
    td5_plat_render_draw_tris(verts, 4, indices, 6);
}

void td5_render_set_clip_rect(float left, float right, float top, float bottom) {
    int ileft   = (int)left;
    int itop    = (int)top;
    int iright  = (int)(right + 0.5f);
    int ibottom = (int)(bottom + 0.5f);
    td5_plat_render_set_clip_rect(ileft, itop, iright, ibottom);
}

/* SetProjectionCenterOffset @ 0x0043E8E0 — writes cx/cy to the globals that
 * every projection formula reads (port: s_center_x / s_center_y).
 * Called per-frame by RunRaceFrame for each viewport, and by the minimap path
 * for the inset render. Restore to screen center after the minimap pass.
 * [CONFIRMED @ 0x0043E8E0] */
void td5_render_set_projection_center(float cx, float cy) {
    s_center_x = cx;
    s_center_y = cy;
}

/* RecomputeTracksideProjectionScale @ 0x0043E900 -- update frustum plane normals
 * when the trackside camera changes g_depthFovFactor (projection depth).
 * Formula mirrors original: h_len = sqrt(W^2*0.25 + depth^2), etc.
 * [CONFIRMED @ 0x0043E900] */
void td5_render_recompute_frustum_for_trackside(void) {
    extern int   g_depthFovFactor; /* camera.c: projection scale, 0x1000=identity */
    extern float g_projFovScale;   /* camera.c: 1/4096 */
    float depth = s_focal_length * (float)g_depthFovFactor * g_projFovScale;
    float w = (float)s_viewport_width;
    float h = (float)s_viewport_height;
    float h_len = sqrtf(w * w * 0.25f + depth * depth);
    float v_len = sqrtf(h * h * 0.25f + depth * depth);
    s_frustum_h_cos =  depth / h_len;
    s_frustum_h_sin = -(w / (h_len + h_len));
    s_frustum_v_cos =  depth / v_len;
    s_frustum_v_sin = -(h / (v_len + v_len));
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
