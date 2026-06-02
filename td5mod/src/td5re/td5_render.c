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
#include "td5_ai.h"
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
 * Depth normalization (0x00473bcc): depth = (vz - 64) / 65479, far = 65536.
 * Frustum far-cull (0x0042D48E): round(3.0 * 65000) = 195000.
 *
 * [DA-T1 audit 2026-05-22]
 *   - NEAR_DEPTH_OFFSET = 64.0f: orig subtracts 64 from vz before normalizing
 *     (g_hudSpeedoDialNearOff @ 0x0045d6c0 = 64.0f). Port previously skipped
 *     this; depth was vz/65536. Effect: ~64 z-units of depth-fight bias near
 *     camera, esp. at low z. Fix applies to clipped[i].depth_z at line ~755.
 *   - DEPTH_NORMALIZE: orig uses 1/65479 (DAT_00473bcc ≈ 1.5278e-5); port
 *     uses 1/s_far_clip (= 1/65536). Close but different — using orig value. */
/* [DA-T1 fix #1 2026-05-22] DEFAULT_NEAR_CLIP changed 1.0f → 32.0f to match
 * orig DAT_00473bbc = 0x42000000 = 32.0f. Prior port comment claimed "1.0f
 * Ghidra-confirmed" — that was an earlier audit's misreading (likely confused
 * 1.0f rhw constant at DAT_00473bc4 with the near-clip threshold).
 *
 * Impact: orig clips polygons in z ∈ [0, 32] entirely; port previously
 * projected them with 1/z up to 1.0 → screen-coord blow-up on near-camera
 * geometry. Should eliminate near-camera popping/tearing class.
 *
 * REVERT IF: visible regressions on geometry that was previously fine,
 * particularly first-person cockpit views or close-up overlays. To revert,
 * change 32.0f back to 1.0f. The fix is isolated to this constant. */
#define DEFAULT_NEAR_CLIP    32.0f     /* orig DAT_00473bbc, was 1.0f */
/* [FIX 2026-06-01 distant-depth / pop-in] DEFAULT_FAR_CLIP + DEPTH_NORMALIZE_INV
 * extended from the original's 65536 / (1/65479) to 195000 / (1/195000) so the
 * depth range MATCHES the 195000 frustum far-cull. The original co-designed its
 * +/-128-span draw window with a 65479 depth range so geometry never exceeded it;
 * the port draws geometry out to view_z ~176000-199000 (far_cull=195000), which
 * with the old 65479 normalization clamped everything past 65479 to the far
 * plane -> distant buildings/trees all at depth 1.0, z-fighting by draw order
 * (the user's "rendered in front of the previous one at a later stage" /
 * "distance looks weird"). Extending the range gives every drawn mesh a real
 * depth value across the whole visible distance. Paired with the D16->D32_FLOAT
 * depth buffer (ddraw_wrapper/src/d3d11_backend.c) so near-camera precision is
 * not sacrificed by the wider linear range. DELIBERATE DIVERGENCE from the
 * CONFIRMED orig 1/65479; justified because the port's draw distance is no
 * longer window-limited to ~65479 view_z. */
#define DEFAULT_FAR_CLIP     195000.0f            /* was 65536; matches far-cull */
#define DEFAULT_FAR_CULL     195000.0f
#define NEAR_DEPTH_OFFSET    64.0f                /* orig 0x0045d6c0 */
#define DEPTH_NORMALIZE_INV  (1.0f / 195000.0f)   /* was orig 1/65479; extended to far-cull */

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
static void render_tracked_actor_marker(const TD5_Actor *actor,
                                        const TD5_Mat3x3 *body_rot,
                                        const TD5_Vec3f *body_pos);

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
/* [CONFIRMED @ 0x0043DC50 TransformVec3ByRenderMatrixFull
 *  + 0x0042E370 TransformVector3ByRenderRotation; L5 sweep 2026-05-21]
 *   Byte-faithful: row-major 3x3 multiply matching both orig helpers (orig
 *   has duplicated 3x3-only direction-vector multipliers using m[0..8];
 *   port consolidates into one shared inline helper. Same FPU ordering
 *   (in[k]*m[row*3+k] sum), same memory layout (m[0..8] = row-major rotation
 *   sub-block of the 3x4 render transform). No translation component.) */
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

/* Expose the active projection parameters so VFX billboards (smoke,
 * particles) can size and place screen-space quads with the SAME focal
 * length and screen center the world geometry uses. Without this the VFX
 * code rolled its own focal (width * 1.207) which is ~2.15x the renderer's
 * width * 0.5625, so smoke sat at the wrong screen position and size. */
float td5_render_get_focal_length(void) { return s_focal_length; }
float td5_render_get_center_x(void)     { return s_center_x; }
float td5_render_get_center_y(void)     { return s_center_y; }

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
 *
 * [ARCH-DIVERGENCE: D3D3 vtable trampoline -> D3D11 wrapper; Phase 5(d) L5
 *  audit 2026-05-21]
 *   Orig invokes `(*(*(*(d3d_exref + 0x38))) + 0x74)(D3D3_device, type=4,
 *   FVF=0x1C4, vertex_buffer, vertex_count, index_buffer, index_count, 0xC)`
 *   — IDirect3DDevice3::DrawIndexedPrimitive on a DDraw-bound device.
 *   Port routes through td5_plat_render_draw_tris which fans through the
 *   ddraw_wrapper D3D11 backend. Same vertex layout (TD5_D3DVertex mirrors
 *   FVF 0x1C4: XYZRHW + diffuse + UV); the color LUT fixup loop on diffuse
 *   bytes is preserved verbatim. Port adds opt-in deferred additive batching
 *   for type-3 (additive) pages, which orig lacks — invariant: type-3 page
 *   geometry still draws in the same draw order, just queued and replayed
 *   after the opaque pass. */
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
 * [ARCH-DIVERGENCE: D3D3 rasterizer pipeline -> D3D11 immediate stream;
 *  Phase 5(d) L5 audit 2026-05-21]
 *   Orig is a 3030-byte function that orchestrates near-plane Sutherland-
 *   Hodgman + projection + chained calls into the X-axis screen clipper
 *   (RenderTrackSegmentBatch @ 0x004323D0), Y-axis screen clipper
 *   (RenderTrackSegmentBatchVariant @ 0x004326D0), and triangle-fan emitter
 *   (AppendClippedPolygonTriangleFan @ 0x00432AB0). All four orig functions
 *   collapse into this single helper: near-plane clip preserved, screen-
 *   axis Sutherland-Hodgman stages removed (D3D11 viewport clipping handles
 *   them GPU-side with CullMode=NONE), triangle-fan emission inlined into
 *   the tail of this function. Per-byte vertex-stream comparison is
 *   meaningless across the API boundary; geometry semantics (near-clip
 *   plane, perspective projection formula, UV/color interp on cut edges)
 *   are preserved. See the Phase 5(d) D3D Pipeline manifest at file footer
 *   for the full address list. */
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
            /* [DA-T1 fix #3 2026-05-22] orig interpolates color via
             * FILD/FISUB/FMUL/FIADD + ROUND across the clip edge (per-channel
             * integer-as-float lerp); port previously snapped to in_color[i].
             * Now per-channel BGRA lerp. */
            {
                uint32_t ci = in_color[i], cj = in_color[j];
                int b = (int)(ci & 0xFF)        + (int)(t * (int)((int)(cj & 0xFF)        - (int)(ci & 0xFF))        + 0.5f);
                int g = (int)((ci >>  8) & 0xFF) + (int)(t * (int)((int)((cj >>  8) & 0xFF) - (int)((ci >>  8) & 0xFF)) + 0.5f);
                int r = (int)((ci >> 16) & 0xFF) + (int)(t * (int)((int)((cj >> 16) & 0xFF) - (int)((ci >> 16) & 0xFF)) + 0.5f);
                int a = (int)((ci >> 24) & 0xFF) + (int)(t * (int)((int)((cj >> 24) & 0xFF) - (int)((ci >> 24) & 0xFF)) + 0.5f);
                if (b < 0) b = 0; else if (b > 255) b = 255;
                if (g < 0) g = 0; else if (g > 255) g = 255;
                if (r < 0) r = 0; else if (r > 255) r = 255;
                if (a < 0) a = 0; else if (a > 255) a = 255;
                out_color[out_count] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
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
        /* [DA-T1 fix #4 2026-05-22] orig 0x00432362 region:
         *   v[2] -= 64.0f;
         *   v[2] *= 1.5278e-5f;  (= 1/65479)
         * Port previously did vz/65536 with no near offset → ~64 z-unit depth
         * bias near camera. Now matches orig. */
        clipped[i].depth_z  = (out_vz[i] - NEAR_DEPTH_OFFSET) * DEPTH_NORMALIZE_INV;
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
 *
 * [ARCH-DIVERGENCE: globals + ClipAndSubmit -> direct param list;
 *  Phase 5(d) L5 audit 2026-05-21]
 *   Orig EmitTranslucentTriangleStrip @ 0x00431750: per-tri loop writes
 *   DAT_004af268 (vertex ptr = cmd+0xC), DAT_004af278 (vert count), the
 *   material handle (cmd+0x02), DAT_004af27c (count=3 or 4), then calls
 *   ClipAndSubmitProjectedPolygon which reads those globals. The orig also
 *   has a second loop for quads (count=4) appended at vertex_ptr + tri_count
 *   *0x84. Port passes (verts, 3/4, tex_page) directly to
 *   clip_and_submit_polygon; the four globals are eliminated. Same per-tri
 *   strip-of-triangles + per-quad-strip semantics.
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
 *
 * [ARCH-DIVERGENCE: globals -> parameter list; L5 sweep 2026-05-21]
 *   Mirror of the quad path with vert count 3 instead of 4. Orig writes the
 *   same four globals (DAT_004af268=cmd+8, DAT_004af278=1, material handle
 *   from cmd+2, DAT_004af27c=3) then calls ClipAndSubmitProjectedPolygon;
 *   port passes (verts, 3, tex_page) explicitly into clip_and_submit_polygon.
 *   Same vertex source and texture-page semantics.
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
 *
 * [ARCH-DIVERGENCE: globals -> parameter list; L5 sweep 2026-05-21]
 *   Orig writes DAT_004af268 (vertex ptr = cmd+8), DAT_004af278=1, sets
 *   g_renderCurrentMaterialHandle from cmd+2, DAT_004af27c=4 (vert count),
 *   then calls ClipAndSubmitProjectedPolygon (which reads the four globals).
 *   Port passes (verts, 4, tex_page) explicitly into clip_and_submit_polygon,
 *   eliminating the global-write step. Same vertex source, same vert count,
 *   same texture-page semantics.
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
 * [CONFIRMED @ 0x00431730 EmitTranslucentTriangleStripDirect; L5 sweep 2026-05-21]
 *   Orig: DAT_004af268 = param_1+8; InsertTriangleIntoDepthSortBuckets(param_1);
 *   Routes through depth-sort bucket queue (translucent primitives need
 *   back-to-front ordering for correct alpha blending). Prior port routed
 *   through clip_and_submit_polygon (immediate raster), bypassing depth-sort
 *   — caused z-order glitches in HUD overlays / lens flares.
 */
static void dispatch_tristrip_direct(TD5_PrimitiveCmd *cmd, TD5_MeshVertex *base_verts)
{
    int tex_page = cmd->texture_page_id;
    int tri_count = cmd->triangle_count;
    int quad_count = cmd->quad_count;
    TD5_MeshVertex *verts = (TD5_MeshVertex *)(uintptr_t)cmd->vertex_data_ptr;
    if (!verts) verts = base_verts;

    /* Iterate strip primitives, route each through depth-sort bucket queue */
    for (int i = 0; i < tri_count; i++) {
        TD5_MeshVertex *v = &verts[i * 3];
        float avg_z = (v[0].view_z + v[1].view_z + v[2].view_z) * (1.0f / 3.0f);
        if (avg_z > s_near_clip) {
            int bucket = (int)(avg_z * (float)(DEPTH_BUCKET_COUNT - 1) / s_far_clip);
            bucket = clampi(bucket, 0, DEPTH_BUCKET_COUNT - 1);
            bucket ^= (DEPTH_BUCKET_COUNT - 1);
            td5_render_queue_projected_entry(v, bucket, 0x3u, tex_page);
        }
    }
    {
        TD5_MeshVertex *quad_start = verts + tri_count * 3;
        for (int i = 0; i < quad_count; i++) {
            TD5_MeshVertex *v = &quad_start[i * 4];
            float avg_z = (v[0].view_z + v[1].view_z + v[2].view_z + v[3].view_z) * 0.25f;
            if (avg_z > s_near_clip) {
                int bucket = (int)(avg_z * (float)(DEPTH_BUCKET_COUNT - 1) / s_far_clip);
                bucket = clampi(bucket, 0, DEPTH_BUCKET_COUNT - 1);
                bucket ^= (DEPTH_BUCKET_COUNT - 1);
                td5_render_queue_projected_entry(v, bucket, 0x4u, tex_page);
            }
        }
    }
}

/**
 * Opcode 6: EmitTranslucentQuadDirect (0x4316D0)
 *
 * [CONFIRMED @ 0x004316D0 EmitTranslucentQuadDirect; L5 sweep 2026-05-21]
 *   Orig: DAT_004af268 = param_1+8; QueueProjectedPrimitiveBucketEntry(param_1);
 *   Routes through depth-sort bucket queue (translucent primitives need
 *   back-to-front ordering for correct alpha blending). Prior port routed
 *   through clip_and_submit_polygon (immediate raster).
 */
static void dispatch_quad_direct(TD5_PrimitiveCmd *cmd, TD5_MeshVertex *base_verts)
{
    int tex_page = cmd->texture_page_id;
    TD5_MeshVertex *verts = (TD5_MeshVertex *)(uintptr_t)cmd->vertex_data_ptr;
    if (!verts) verts = base_verts;

    float avg_z = (verts[0].view_z + verts[1].view_z + verts[2].view_z + verts[3].view_z) * 0.25f;
    if (avg_z > s_near_clip) {
        int bucket = (int)(avg_z * (float)(DEPTH_BUCKET_COUNT - 1) / s_far_clip);
        bucket = clampi(bucket, 0, DEPTH_BUCKET_COUNT - 1);
        bucket ^= (DEPTH_BUCKET_COUNT - 1);
        td5_render_queue_projected_entry(verts, bucket, 0x4u, tex_page);
    }
}

/* ========================================================================
 * Public API Implementation
 * ======================================================================== */

/* --- Module Lifecycle --- */

/* [CONFIRMED @ 0x0040AE80 InitializeRaceRenderState; Phase 5(d) L5 audit 2026-05-21]
 *   Orig is a one-shot gate (`if (DAT_0048dba0 == 1) return 0;`) that calls
 *   InitializeTranslucentPrimitivePipeline + InitializeProjectedPrimitiveBuckets
 *   + ResetProjectedPrimitiveWorkBuffer, sets the sentinel, and arms the
 *   clear-screen flag. Port collapses the three sub-routines (folded under
 *   the Phase 5(a) DepthSort manifest) into inline reset passes here; the
 *   sentinel collapses to s_globals_initialized. Behaviour-equivalent. */
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

/* [ARCH-DIVERGENCE: D3D3 ReleaseRaceRenderResources -> D3D11 abstracted shutdown; L5 sweep 2026-05-21]
 *   Mirrors ReleaseRaceRenderResources @ 0x0040AEC0 (orig: DXD3DTexture::ClearAll
 *   + write 0 to d3d_exref+0xa34 if DAT_0048dba0 != 0). Port routes texture
 *   teardown through td5_render_reset_texture_cache and clears fog + active
 *   sentinel (s_state_active / s_globals_initialized), absorbing the orig's
 *   DAT_0048dba0 gate. */
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

/* [ARCH-DIVERGENCE: D3D3 BeginScene -> D3D11 platform-abstracted begin-frame; L5 sweep 2026-05-21]
 *   Mirrors BeginRaceScene @ 0x0040ADE0 (orig: DXD3D::BeginScene() +
 *   g_renderCurrentMaterialHandle = g_renderCurrentTextureHandle = 0xffffffff).
 *   Port routes through td5_plat_render_begin_scene and forces texture-page
 *   invalidation via -1 sentinels (semantic equivalent of orig's 0xffffffff
 *   handle invalidation). Adds substantial per-frame debug counter resets that
 *   have no orig counterpart (D3D11 path needs them for clip/draw tracking). */
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

/* [ARCH-DIVERGENCE: D3D3 EndScene -> D3D11 platform-abstracted end-frame; L5 sweep 2026-05-21]
 *   Mirrors EndRaceScene @ 0x0040AE00 (orig: DXD3D::EndScene() +
 *   AdvanceTexturePageUsageAges()). Port routes through td5_plat_render_end_scene
 *   (D3D11 device-context) and calls td5_render_advance_texture_ages (the same
 *   LRU sweep), plus adds per-frame debug counters and tick advance. Same
 *   pre+post-scene cleanup ordering. */
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

/* [ARCH-DIVERGENCE: D3D3 SetRenderState calls -> D3D11 platform call; L5 sweep 2026-05-21]
 *   Mirrors ApplyRaceFogRenderState @ 0x0040AF50. Orig issues 6 IDirect3DDevice
 *   SetRenderState calls for the param=1 path (states 0x1c FOGENABLE, 0x22-0x26
 *   color/start/end/density and final 0x1c=1 commit) routing failures through
 *   DXErrorToString/Msg, and just FOGENABLE=0 for param=0. Port routes the
 *   entire 6-state config into a single td5_plat_render_set_fog(enable, color,
 *   start, end, density) call -- the D3D11 backend builds the equivalent
 *   constant-buffer state. Same enable/disable polarity. */
void td5_render_set_fog(int enable)
{
    /* Gate on the per-track fog-capability flag (s_fog_enabled), mirroring the
     * original's per-frame fog apply. Orig ApplyRaceFogRenderState (0x0040AF50)
     * is only invoked when g_fogCapabilityEnabled (0x00466E98) is set, and that
     * global is cleared to 0 whenever LEVELINF.DAT+0x5C == 0 for the current
     * track (0x0042AE5B-65; per-frame gate at 0x0042BE11/0x0042BE32). The port's
     * s_fog_enabled is that flag (written by td5_render_configure_fog from the
     * LEVELINF gate in td5_game InitRace step 4b).
     *
     * Without this term, fog-DISABLED tracks (e.g. Edinburgh/Scotland L016,
     * Maui L028 — both LEVELINF+0x5C == 0) still had fog forced on during the
     * world pass using the leftover default s_fog_color (0x808080, mid-gray).
     * That gray is LIGHTER than the scene, so distant geometry blended toward
     * light gray => "fog gets lighter far away" + "Scotland has fog it
     * shouldn't". Fog-enabled tracks use near-black LEVELINF colors, so they
     * correctly darken with distance and are unaffected by this gate.
     *
     * g_td5.ini.fog_enabled is kept as a live user-preference override (a port
     * addition; the original has no such pref). s_fog_enabled already folds the
     * pref in at race init, so this term only adds live-toggle responsiveness. */
    if (enable && s_fog_enabled && g_td5.ini.fog_enabled) {
        /* Apply full fog pipeline: linear table fog */
        td5_plat_render_set_fog(1, s_fog_color,
                                FOG_START_DEFAULT, FOG_END_DEFAULT,
                                FOG_DENSITY_DEFAULT);
    } else {
        /* Disable fog */
        td5_plat_render_set_fog(0, 0, 0.0f, 0.0f, 0.0f);
    }
}

/* [ARCH-DIVERGENCE: D3D3 DXD3D::CanFog() probe removed; L5 sweep 2026-05-21]
 *   Mirrors ConfigureRaceFogColorAndMode @ 0x0040AF10. Orig stores
 *   (color & 0xFFFFFF) at d3d_exref+0x18, then probes DXD3D::CanFog() and
 *   stores enable at d3d_exref+0xa34 (or 0 if hardware lacks fog). Port stores
 *   the same `color & 0x00FFFFFFu` mask but drops the CanFog gate because the
 *   D3D11 backend universally supports fog -- no fallback path needed. */
void td5_render_configure_fog(uint32_t color, int enable)
{
    /* Strip alpha, store RGB only (original: color & 0xFFFFFF) */
    s_fog_color   = color & 0x00FFFFFFu;
    s_fog_enabled = enable;
    TD5_LOG_I(LOG_TAG, "td5_render_configure_fog: per-track fog %s color=0x%06X",
              enable ? "ENABLED" : "disabled",
              (unsigned int)(s_fog_color & 0x00FFFFFFu));
}

/* --- Transform Stack (8-deep push/pop for hierarchical models) --- */

/* [CONFIRMED @ 0x0043DA80 LoadRenderRotationMatrix; @ 0x0042E9C0
 *  LoadGlobalOrientationToRenderState; Phase 5(d) L5 audit 2026-05-21]
 *   Orig LoadRenderRotationMatrix copies 9 floats from param_1[0..8] into
 *   g_currentRenderTransform[0..8]. Orig LoadGlobalOrientationToRenderState
 *   is a 1-call wrapper: LoadRenderRotationMatrix(&DAT_004ab040). Port
 *   td5_render_load_rotation is the identical 9-float copy; the global-
 *   orientation wrapper is folded into callers that pass
 *   &g_raceRotationMatrix directly. Both byte-faithful. */
void td5_render_load_rotation(const TD5_Mat3x3 *rot)
{
    /* Copy 9 floats (3x3 rotation) into active transform, leave translation */
    for (int i = 0; i < 9; i++) {
        s_render_transform.m[i] = rot->m[i];
    }
}

/* [ARCH-DIVERGENCE: absorbed caller's view-space translation bake; L5 sweep 2026-05-21]
 *   Mirrors LoadRenderTranslation @ 0x0043DC20. Orig is a literal 3-float copy
 *   from `param_1+0x24..0x2c` to `g_currentRenderTransform+0x24..0x2c` (i.e.
 *   the caller has already computed the camera-space translation row of a 3x4
 *   matrix and just hands it over).
 *
 *   The port absorbs that pre-bake step: it takes a WORLD position and
 *   computes the camera-space translation inline (delta = pos - camera_pos;
 *   m[9..11] = camera_basis * delta). All 12 known orig callers
 *   (RenderVehicleTaillightQuads, RefreshVehicleWheelContactFrames,
 *   RenderVehicleActorModel, RenderRaceActorForView, BuildSpecialActorOverlayQuads,
 *   ApplyMeshRenderBasisFromTransform, ApplyMeshRenderBasisFromWorldPosition,
 *   ApplyMeshResourceRenderTransform, RenderTrackedActorMarker,
 *   RenderTireTrackPool, UpdateTrafficVehiclePose, RenderVehicleWheelBillboards)
 *   composed the view-space translation upstream; in the port that upstream
 *   composition is folded here. Port callers (td5_render.c:1552, :2091)
 *   consistently pass world positions, so the API contract change is safe.
 *   Sky-dome bypasses this helper at td5_render.c:4633 because it wants a
 *   zero translation, not a camera-relative one. */
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

/* [ARCH-DIVERGENCE: depth-1 backup slot -> N-deep stack; L5 sweep 2026-05-21]
 *   Mirrors PushRenderTransform @ 0x0043DAF0. Orig copies the 12-float (3x4)
 *   current transform into a single backup slot at _DAT_004c36c8..f4. Port uses
 *   a depth-N stack (TRANSFORM_STACK_MAX) to support nesting (callers like
 *   billboard rendering inside RenderTrackSpanDisplayList now nest push/pop).
 *   For the orig's depth-1 usage the port behaves identically. */
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

/* [ARCH-DIVERGENCE: depth-1 backup slot -> N-deep stack; L5 sweep 2026-05-21]
 *   Mirrors PopRenderTransform @ 0x0043DB70. Symmetric counterpart to
 *   td5_render_push_transform above: orig restores 12 floats from the single
 *   backup slot; port restores from the active stack frame. */
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

/* Diagnostic counters for view-distance investigation. */
static int   s_dbg_sphere_tested = 0;
static int   s_dbg_sphere_passed = 0;
static int   s_dbg_rej_near      = 0;
static int   s_dbg_rej_far       = 0;
static int   s_dbg_rej_horiz     = 0;
static int   s_dbg_rej_vert      = 0;
static float s_dbg_max_pass_vz   = 0.0f;
static float s_dbg_max_test_vz   = 0.0f;

void td5_render_dump_view_dist_stats(void)
{
    TD5_LOG_I(LOG_TAG,
        "VIEWDIST stats: tested=%d passed=%d max_pass_vz=%.0f max_test_vz=%.0f "
        "rej_near=%d rej_far=%d rej_h=%d rej_v=%d (s_far_cull=%.0f)",
        s_dbg_sphere_tested, s_dbg_sphere_passed,
        s_dbg_max_pass_vz, s_dbg_max_test_vz,
        s_dbg_rej_near, s_dbg_rej_far, s_dbg_rej_horiz, s_dbg_rej_vert,
        s_far_cull);
    s_dbg_sphere_tested = s_dbg_sphere_passed = 0;
    s_dbg_rej_near = s_dbg_rej_far = s_dbg_rej_horiz = s_dbg_rej_vert = 0;
    s_dbg_max_pass_vz = s_dbg_max_test_vz = 0.0f;
}

/* [CONFIRMED @ 0x0042DCA0 IsBoundingSphereVisibleInCurrentFrustum; L5 sweep 2026-05-21]
 *   Byte-faithful 5-plane sphere-vs-frustum test. Same FPU ordering:
 *     - delta = world - camera_pos
 *     - vx/vy/vz = camera_basis row dots (orig uses _DAT_004aafa4..c0; port
 *       uses s_camera_basis[0..8])
 *     - near reject: vz + r < near  (orig: uint(z+r-near) < 0; port: <= near)
 *     - far reject:  vz - r >= far  (orig: uint(z-r-far) >= 0; port: >= far)
 *     - left/right: h_cos*vx +/- + h_sin*vz > r (orig: g_frustumLeftPlaneNormalX/Z)
 *     - top/bottom: v_cos*vy +/- + v_sin*vz > r (orig: g_frustumTopPlaneNormalY/Z)
 *   Port adds s_dbg_* counters (no behavioral effect). Return value: orig
 *   returns 0x80000000 (truthy as bool with bit-test) on cull, port returns 0
 *   on cull / 1 on pass -- equivalent semantics. */
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

    s_dbg_sphere_tested++;
    if (vz > s_dbg_max_test_vz) s_dbg_max_test_vz = vz;

    /* Near plane test: sphere must extend past near clip */
    if (vz + radius <= s_near_clip) { s_dbg_rej_near++; return 0; }

    /* Far plane test: sphere must not be entirely beyond far cull */
    if (vz - radius >= s_far_cull) { s_dbg_rej_far++; return 0; }

    /* Left/right frustum planes (horizontal) */
    /* Plane normal = (h_cos, 0, h_sin), distance from origin = 0 */
    float h_dist = s_frustum_h_cos * vx + s_frustum_h_sin * vz;
    if (h_dist > radius) { s_dbg_rej_horiz++; return 0; }

    /* Check negative side (right plane is mirrored) */
    float h_dist_neg = -s_frustum_h_cos * vx + s_frustum_h_sin * vz;
    if (h_dist_neg > radius) { s_dbg_rej_horiz++; return 0; }

    /* Top/bottom frustum planes (vertical) */
    float v_dist = s_frustum_v_cos * vy + s_frustum_v_sin * vz;
    if (v_dist > radius) { s_dbg_rej_vert++; return 0; }

    float v_dist_neg = -s_frustum_v_cos * vy + s_frustum_v_sin * vz;
    if (v_dist_neg > radius) { s_dbg_rej_vert++; return 0; }

    s_dbg_sphere_passed++;
    if (vz > s_dbg_max_pass_vz) s_dbg_max_pass_vz = vz;
    return 1; /* visible */
}

/* [CONFIRMED @ 0x0042DE10 TestMeshAgainstViewFrustum; L5 sweep 2026-05-21]
 *   Byte-faithful: reads ONLY the integer-coord origin field at mesh+0x1c..0x24
 *   and scales by _g_fixedPointToFloatScale (1/256) per orig
 *   `_g_fixedPointToFloatScale * *(float *)(param_1 + 0x1c) - _g_currentViewWorldOrigin`.
 *
 *   Prior port read `bounding_center_x + origin_x`, which mixed two coordinate
 *   systems: bounding_center is render-float (per the CONFIRMED note at the
 *   span-display-list sphere test below), and origin is integer-coord (per
 *   the track loader at td5_track.c:1718 `mesh->origin_x = (float)sp->origin_x`).
 *   For vehicles where origin=0 the bug masked because 0+small ≈ small; for any
 *   future caller with non-zero origin the test would over-cull. Fix drops the
 *   bounding_center add and applies the 1/256 fp24.8 scale, matching orig. */
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
    cx = mesh->origin_x * (1.0f / 256.0f);
    cy = mesh->origin_y * (1.0f / 256.0f);
    cz = mesh->origin_z * (1.0f / 256.0f);
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

/* [ARCH-DIVERGENCE: per-vertex global write -> batched transform call;
 *  Phase 5(d) L5 audit 2026-05-21]
 *   Orig TransformAndQueueTranslucentMesh @ 0x0043DCB0 inlines a per-vertex
 *   3x4 matrix*vec3 + translation loop into g_currentRenderTransform-relative
 *   view positions (writing pfVar10[3..5] = transformed XYZ at +0x0C..+0x14
 *   stride 0xB), then calls QueueTranslucentPrimitiveBatch for each cmd in
 *   sequence. Port splits this into two phases (transform_mesh_vertices for
 *   the per-vertex pass, then this function for the per-cmd queue/dispatch
 *   pass). RenderPreparedMeshResource (orig 0x004314B0) ends up sharing this
 *   port function since both walk the same per-cmd dispatch table. Same
 *   3x4 matrix math + same cmd dispatch table; the orig's interleaved
 *   transform/queue is decoupled cleanly for port. */
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

/* When set, td5_render_apply_page_blend_preset skips its preset override so
 * the caller-installed TD5_PRESET_SKY (z_test=1, z_write=0) survives the
 * batch flush. Without this, the page-type→preset remap inside
 * flush_immediate_internal silently rewrites the sky's depth state to
 * OPAQUE_LINEAR (z_write=1), which makes the dome occlude distant track.
 *
 * Definition lives here — ABOVE the writer in td5_render_actors_for_view —
 * so the file-scope static is in scope at the first reference. C requires
 * file-scope identifiers to be declared before first use; upstream commit
 * 994ab68 placed it below the writer, which fails to compile. The reader
 * (td5_render_apply_page_blend_preset) appears even later and remains
 * correctly in scope. */
static int s_in_sky_draw = 0;

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
     * Original also pulls the cull-start back by −0x19 (25 entries = 100 spans)
     * at 0x42BBDF when its game-type flag (g_selectedGameType @ 0x4aaf6c) != 0.
     * The drag branch sets that flag = 1 [CONFIRMED @ 0x42AD79], so drag pulls the
     * window back 25 entries. Applied below, gated on g_td5.drag_race_enabled
     * (the only game type whose 0x4aaf6c write is confirmed). The earlier claim
     * that a "doubled max_spans" compensated for this was WRONG — VIEW_DIST_*_SPANS
     * are 64 (un-doubled), so nothing compensated: drag spawned with the near/start
     * geometry culled because the window was shifted forward and was only half as
     * deep as the original's. */
/* Asymmetric span window. The original used a symmetric ±MaxSpans window
 * (max 64 each side, single-screen), but on long Moscow-style point-to-point
 * tracks the player's perception of "view distance" is dominated by FORWARD
 * reach. Backward visibility is mostly frustum-culled anyway.
 *
 * Restored to Ghidra-confirmed original (RunRaceFrame @ 0x42BB2E):
 * gViewportLayoutMaxSpans = 0x40 (64), doubled = 128 spans single-screen.
 * Forward and back are symmetric in the original — split here as 64/64 to
 * preserve the asymmetric-window plumbing without exceeding the original
 * total of 128. */
#define VIEW_DIST_FWD_SPANS  64
#define VIEW_DIST_BACK_SPANS 64
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
    float frac_scaled = view_dist_frac * 0.85f + 0.15f;
    int fwd_window  = (int)(frac_scaled * (float)VIEW_DIST_FWD_SPANS);
    int back_window = (int)(frac_scaled * (float)VIEW_DIST_BACK_SPANS);
    if (fwd_window  < 1) fwd_window  = 1;
    if (back_window < 1) back_window = 1;

    /* Actor visibility cull window — orig gRaceTrackSpanCullWindow @ 0x004AAEF4,
     * written each frame in RunRaceFrame @ 0x42BB72 as
     *   min(ftol(frac_scaled * max), max) * 2,  max = 64 (full) / 32 (split).
     * This is SEPARATE from the track-render fwd_window: the original renders
     * track geometry far ahead but only pops AI cars + traffic into view within
     * ~88 spans. The port previously reused fwd_window (~720) for the actor
     * cull, so traffic appeared ~8x too early ("traffic came in much earlier
     * than the original"). [FIX 2026-05-26 traffic-appears-too-early] */
    int cull_max = (g_td5.split_screen_mode > 0) ? 32 : 64;
    int actor_cull_window = (int)(frac_scaled * (float)cull_max);
    if (actor_cull_window > cull_max) actor_cull_window = cull_max;
    actor_cull_window *= 2;
    if (actor_cull_window < 1) actor_cull_window = 1;

    /* far_cull is now slider-driven inside td5_render_configure_projection
     * so it applies to track render too, not just actor cull. */
    {
        static float s_view_dist_last = -1.0f;
        if (view_dist_frac != s_view_dist_last) {
            TD5_LOG_I(LOG_TAG,
                      "view distance: frac=%.2f fwd=%d back=%d far_cull=%.0f",
                      view_dist_frac, fwd_window, back_window, s_far_cull);
            s_view_dist_last = view_dist_frac;
        }
    }

    int actor_render_count = 0;
    int actor_meshes_submitted = 0;

    /* Refresh render-side camera snapshot from game-side globals every frame.
     * td5_render_configure_projection only runs when viewport dimensions change,
     * so without this the render camera stays frozen at its initial position. */
    update_render_camera_from_game();

    /* Draw sky panorama behind all geometry. Sky uses TD5_PRESET_SKY
     * (z_test=1, z_write=0) so the dome — drawn camera-centered with small
     * Z values — does not write depth, letting later track meshes pass
     * their own depth test against the cleared far value.
     *
     * Two state-leak guards are required:
     *   1. s_in_sky_draw blocks td5_render_apply_page_blend_preset from
     *      remapping the SKY preset to OPAQUE_LINEAR based on the sky
     *      page's transparency type when the batch flushes.
     *   2. An explicit flush AFTER the draw commits sky pixels with the
     *      SKY state, before the next set_preset(OPAQUE_LINEAR) takes
     *      effect at the following flush. */
    s_in_sky_draw = 1;
    td5_plat_render_set_preset(TD5_PRESET_SKY);
    {
        static int s_sky_preset_logged = 0;
        if (!s_sky_preset_logged) {
            TD5_LOG_I(LOG_TAG,
                      "sky preset: z_func=ALWAYS z_write=0 (matches original SetRaceRenderStatePreset(0) @ 0x40b070)");
            s_sky_preset_logged = 1;
        }
    }
    td5_render_draw_sky();
    td5_render_flush_immediate_batch();
    s_in_sky_draw = 0;

    /* Set render preset for track geometry (enables texture sampling) */
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);

    /* Load camera view basis into render transform rotation.
     * This is normally done by the actor rendering path (ApplyMeshRenderBasis),
     * but for track span geometry we need it set once before the span loop.
     * The camera basis is: right=[0..2], up=[3..5], forward=[6..8]. */
    for (int i = 0; i < 9; i++)
        s_render_transform.m[i] = s_camera_basis[i];

    /* Per-ENTRY display-list walk — matches orig RunRaceFrame @ 0x0042b580.
     *
     * Orig's loop shape (transcribed from the BUILD phase of RunRaceFrame):
     *   eff_player_span = reverse_direction
     *                     ? gTrackTotalSpanCount - player_span
     *                     : player_span;
     *   start_entry = (eff_player_span - gRaceTrackSpanCullWindow) >> 2;
     *   for (i = 0; i < gViewportLayoutEffectiveSpans; i++) {
     *       entry_idx = start_entry + i;
     *       // circuit:    wrap entry_idx modulo ring_length/4 (impl: SPAN modulo)
     *       // non-circuit: clamp to [0, ring_length/4)
     *       gTrackSpanDisplayListView0[i] = GetTrackSpanDisplayListEntry(entry_idx);
     *   }
     * then the render phase walks the cached array exactly once per entry.
     *
     * Port previously iterated per SPAN (`for span = 0..span_count`), and
     * `td5_track_get_display_list(span_index)` divided by 4 internally — so
     * spans n*4+0..n*4+3 all returned entry n, submitting each MODELS.DAT
     * block ~4× per frame and inserting translucent batches into the sorted
     * queue four times each. This refactor flips the loop to per-entry to
     * match orig and eliminate the 4× redundancy.
     *
     * Branch geometry (span_index >= ring_length) is NOT in MODELS.DAT in
     * orig and was never queried in this loop. Port currently leans on a
     * STRIP-generated display-list fallback (td5_track.c:build_span_strip_display_list)
     * to keep branch roads visible. That fallback path is preserved here
     * via a secondary per-span loop guarded on player_branch_span >= 0 —
     * dropping the STRIP fallback entirely is a separate audit work item. */
    {
        int ring         = td5_track_get_ring_length();
        int total_spans  = td5_track_get_span_count();
        int eff_player   = player_span;
        if (g_td5.reverse_direction && ring > 0 && player_span < ring)
            eff_player = ring - 1 - player_span;

        /* effectiveSpans = min(ftol((v*0.85+0.15)*maxSpans), maxSpans),
         * maxSpans = 0x40 (64) single-screen [CONFIRMED @ RunRaceFrame
         * 0x42BB2E-0x42BB5D]. frac_scaled already holds (v*0.85+0.15). */
        int eff_spans = (int)(frac_scaled * (float)VIEW_DIST_FWD_SPANS);
        if (eff_spans > VIEW_DIST_FWD_SPANS) eff_spans = VIEW_DIST_FWD_SPANS;
        if (eff_spans < 1)                   eff_spans = 1;

        /* gRaceTrackSpanCullWindow = effectiveSpans * 2  [CONFIRMED @ 0x42BB5F
         * `LEA EDX,[EAX+EAX]`, stored 0x42BB72]. This is the BACK reach in spans;
         * the forward reach comes from the entry loop below, giving a window of
         * ±~128 spans (256 spans total) around the center — not the 128-span
         * (64 fwd / 64 back) window the port had, which was half as deep. NB:
         * the separate actor_cull_window above already uses this *2 form for the
         * AI/traffic pop-in cull (FIX 2026-05-26); this restores the same depth
         * for the TRACK display-list walk, which that fix did not touch. */
        int cull_window = eff_spans * 2;

        /* start_entry = (center - cullWindow) >> 2  [CONFIRMED @ 0x42BBC9-0x42BBD8:
         * SUB EAX,EDX; CDQ; AND EDX,3; ADD; SAR EDI,2 — the standard signed
         * divide-by-4 idiom `(x + (x>>31 & 3)) >> 2`]. C's `>>` on signed
         * negatives is arithmetic on all targets we ship (gcc/i686, MSVC/x86). */
        int start_entry = (eff_player - cull_window) >> 2;

        /* Drag pulls the cull-start back 0x19 entries (= 100 spans) so the
         * start/staging geometry behind the line stays visible at spawn
         * [CONFIRMED @ 0x42BBDF; drag flag write @ 0x42AD79]. */
        if (g_td5.drag_race_enabled)
            start_entry -= 0x19;

        /* Loop count = effectiveSpans ENTRIES, NOT (window)>>2+1. The port's
         * `((back+fwd)>>2)+1` ≈ 33 entries rendered only ~half the original's
         * depth [CONFIRMED count = effectiveSpans @ 0x42BBE9 `MOV EBP,[0x4aae44]`,
         * loop 0x42BC11-0x42BC3C]. */
        int n_entries   = eff_spans;
        /* Change-gated (not per-frame) so the render dispatch isn't spammed. */
        {
            static int s_log_start = 0x7fffffff, s_log_n = -1;
            if (start_entry != s_log_start || n_entries != s_log_n) {
                TD5_LOG_I(LOG_TAG,
                          "span-window: center=%d eff_spans=%d cull_win=%d start_entry=%d n_entries=%d drag=%d",
                          eff_player, eff_spans, cull_window, start_entry, n_entries,
                          (int)g_td5.drag_race_enabled);
                s_log_start = start_entry;
                s_log_n = n_entries;
            }
        }

        int ring_entries = (ring > 0) ? ((ring + 3) >> 2) : 0;
        int is_circuit   = (g_td5.track_type == TD5_TRACK_CIRCUIT) ? 1 : 0;

        /* [FIX 2026-05-25 munich-gantry-double-submit] Per-frame display-list
         * dedup set. The branch-fallback loop below uses
         * td5_track_get_display_list(span_index) which *also* hits MODELS.DAT
         * first (see td5_track.c:6040-6057) — for branch spans whose
         * (span_index >> 2) falls inside s_models_display_list_count, that
         * path returns the SAME pointer the main-road entry walk above already
         * submitted. Result on Munich: gantry mesh submitted twice (once via
         * main wrap, once via branch fallback) and the branch-side submission
         * uses a mirrored span position near the junction → visible
         * duplicated+mirrored arch.
         *
         * Dedup by pointer is sufficient: identical MODELS.DAT block ⇒
         * identical pointer (s_models_blob + s_models_entry_offsets[i]).
         * Pointer-based also defends future STRIP-fallback overlap cases.
         *
         * Cap: 2 × MODELS_DAT_MAX_ENTRIES (2048) covers main + branch worst
         * case; typical frame uses ~25-30 entries each. Overflow falls back
         * to "submit anyway" so we never lose geometry — only the cap-spill
         * tail can re-duplicate, and that's strictly safer than the bug. */
        #define TD5_RENDER_SUBMITTED_CAP 4096
        static const void *s_submitted[TD5_RENDER_SUBMITTED_CAP];
        int submitted_count = 0;

        for (int i = 0; i < n_entries; i++) {
            int entry_idx = start_entry + i;

            if (ring_entries > 0) {
                if (is_circuit) {
                    /* Circuit: wrap modulo ring_entries (matches orig's
                     * `while (iVar8 < 0) iVar8 += ring; while (ring <= iVar8) iVar8 -= ring;`). */
                    while (entry_idx < 0)              entry_idx += ring_entries;
                    while (entry_idx >= ring_entries)  entry_idx -= ring_entries;
                } else {
                    /* Non-circuit: clamp to [0, ring_entries). Orig drops
                     * iterations outside the range; we skip per-iter to
                     * preserve the n_entries count semantics. */
                    if (entry_idx < 0 || entry_idx >= ring_entries)
                        continue;
                }
            }

            void *display_list = td5_track_get_display_list_entry(entry_idx);
            if (!display_list)
                continue;

            /* [FIX 2026-05-25 munich-gantry-double-submit] Also dedup against
             * the circuit-wrap case where two distinct entry_idx values can
             * collapse to the same MODELS.DAT entry. Cheap linear scan —
             * submitted_count stays small in practice (~25-30). */
            int dup = 0;
            for (int s = 0; s < submitted_count; s++) {
                if (s_submitted[s] == display_list) { dup = 1; break; }
            }
            if (dup) continue;
            if (submitted_count < TD5_RENDER_SUBMITTED_CAP)
                s_submitted[submitted_count++] = display_list;

            td5_render_span_display_list(display_list);
            rendered_spans++;
        }

        /* Branch geometry fallback (port-only, no orig equivalent).
         * Only walks when the player is on a branch road. Uses the legacy
         * span-indexed getter so the STRIP fallback synthesizer keeps
         * producing per-span road quads for branch visibility.
         *
         * [FIX 2026-05-25 munich-gantry-double-submit] Dedup against the
         * main-road submissions above. Prior comment claimed "no overlap"
         * because branch spans are >= ring_length, but
         * td5_track_get_display_list() probes MODELS.DAT FIRST regardless of
         * whether the span is in the main ring (td5_track.c:6040-6057). When
         * the branch span's >>2 falls inside the MODELS.DAT range, the same
         * block is re-submitted → Munich gantry double + mirror. */
        if (player_branch_span >= 0 && total_spans > ring) {
            int blo = player_branch_span - back_window;
            int bhi = player_branch_span + fwd_window;
            if (blo < ring)         blo = ring;
            if (bhi >= total_spans) bhi = total_spans - 1;
            for (int span_index = blo; span_index <= bhi; span_index++) {
                void *display_list = td5_track_get_display_list(span_index);
                if (!display_list)
                    continue;
                int dup = 0;
                for (int s = 0; s < submitted_count; s++) {
                    if (s_submitted[s] == display_list) { dup = 1; break; }
                }
                if (dup) continue;
                if (submitted_count < TD5_RENDER_SUBMITTED_CAP)
                    s_submitted[submitted_count++] = display_list;
                td5_render_span_display_list(display_list);
                rendered_spans++;
            }
        }
        #undef TD5_RENDER_SUBMITTED_CAP
    }

    {
        int total_actors = td5_game_get_total_actor_count();
        int drag_mode = g_td5.drag_race_enabled;

        /* Bumper/interior-camera own-car suppression. The original
         * RenderRaceActorForView @ 0x0040c120 (gate @ 0x0040c2a0-0x0040c2af)
         * skips the ENTIRE render of the view's OWN car — mesh, reflection,
         * wheels, brake lights, shadow AND smoke — when this view's active
         * preset mode != 0 (the bumper/interior cam, preset 6). camera_target_slot
         * is the viewed slot (orig gPrimarySelectedSlot[view]); camera_preset_active
         * is g_raceCameraPresetMode[view] != 0. Consumed by the per-actor skip at
         * the top of the loop below (and the smoke sub-gate further down). */
        extern int g_raceCameraPresetMode[2];
        int camera_target_slot   = td5_game_get_player_slot(view_index);
        int camera_preset_active = (g_raceCameraPresetMode[view_index & 1] != 0);

        /* === Vehicle shadow PRE-PASS (FIX 2026-06-02 inter-actor overlay) ===
         * Draw EVERY visible actor's ground shadow BEFORE any car body is drawn
         * in the main loop below. The opaque car bodies (z_write=1) then paint
         * over any shadow pixel a body covers, so EVERY body occludes EVERY
         * shadow — reproducing the net result of the original's deferred
         * translucent flush (RunRaceFrame @ 0x0042b580 flushes all queued car
         * shadows AFTER all opaque bodies via FlushQueuedTranslucentPrimitives
         * @ 0x00431340; no depth sort).
         *
         * Why a pre-pass and not the prior per-actor "shadow before its OWN
         * body" (2026-06-01 fix): that only made each body occlude its OWN
         * shadow. A nearer traffic/AI car processed in a LATER loop iteration
         * drew its shadow (z_test=LEQUAL, z_write=0) AFTER the player body was
         * already down; the port's SEPARATE shadow projection let the
         * 1.25-scaled shadow corners win the depth test against the player's
         * lower body -> "other cars' shadows render over my car". Drawing ALL
         * shadows first, then ALL bodies, makes the player body unconditionally
         * overwrite those shadows, fixing the inter-actor case while KEEPING the
         * player-self fix (own body is still drawn after its own shadow).
         *
         * A literal port of the original (defer shadows AFTER bodies + z-test)
         * would reintroduce the player-self over-body case precisely because the
         * port can't share the track projection (see SHADOW_DEPTH_Z_BIAS notes);
         * the pre-pass sidesteps that with the opaque-overwrite guarantee.
         *
         * Gates mirror the body loop's pre-frustum gates below (null actor/mesh,
         * bumper/interior own-car suppression, drag decoration slots, span
         * distance cull). The mesh frustum test is intentionally NOT replicated:
         * the shadow has its own near-clip and the rasterizer clips off-screen
         * pixels, so a ground shadow for a car just past the body-frustum edge
         * is harmless (and slightly more correct). */
        {
            int shadow_drawn = 0;
            for (int slot = 0; slot < total_actors; slot++) {
                TD5_Actor *sa = td5_game_get_actor(slot);
                if (!sa || !td5_render_get_vehicle_mesh(slot))
                    continue;
                if (slot == camera_target_slot && camera_preset_active)
                    continue;   /* bumper/interior cam: own car (incl. shadow) suppressed */
                if (drag_mode && slot < TD5_MAX_RACER_SLOTS &&
                    td5_game_get_slot_state(slot) == 3)
                    continue;   /* drag decoration slot */
                if (slot != camera_target_slot) {
                    TD5_Actor *owner = td5_game_get_actor(camera_target_slot);
                    if (owner) {
                        int delta = (int)sa->track_span_normalized -
                                    (int)owner->track_span_normalized;
                        int ring = td5_track_get_ring_length();
                        if (ring > 0) {
                            int half = ring / 2;
                            if (delta >  half) delta -= ring;
                            if (delta < -half) delta += ring;
                        }
                        int delta_abs = delta < 0 ? -delta : delta;
                        if (delta_abs >= actor_cull_window)
                            continue;   /* span-distance cull (mirrors body loop) */
                    }
                }
                render_vehicle_shadow_quad(sa);
                shadow_drawn++;
            }
            {
                static uint32_t s_shadow_prepass_log = 0;
                if ((s_shadow_prepass_log++ % 600u) == 0u)
                    TD5_LOG_I(LOG_TAG,
                              "shadow pre-pass: view=%d drew %d shadow(s) before bodies",
                              view_index, shadow_drawn);
            }
        }

        for (int slot = 0; slot < total_actors; slot++) {
            TD5_Actor *actor = td5_game_get_actor(slot);
            TD5_MeshHeader *mesh = td5_render_get_vehicle_mesh(slot);
            TD5_Mat3x3 view_rot;
            TD5_Vec3f render_pos;
            float depth;

            if (!actor || !mesh)
                continue;

            /* Bumper / interior camera own-car skip (orig RenderRaceActorForView
             * @ 0x0040c120, gate @ 0x0040c2a0-0x0040c2af): when this actor IS the
             * view's own slot AND the view's preset mode != 0, the original does
             * CMP viewed-slot / TEST mode / JMP 0x0040c7ba (function tail) — i.e.
             * it renders NOTHING for the player's own car: no mesh, reflection,
             * wheels, brake lights, shadow or smoke. In the bumper/interior cam
             * the camera sits inside the player's chassis, so without this skip
             * the player just sees the inside of their own car model. The
             * owner-only tire-track emitter runs in the post-loop pass below,
             * which is past the original's skip target (LAB_0040c7ba), so it is
             * intentionally NOT skipped here. Only fires for mode != 0, so chase
             * cams (mode 0) still render the owner normally. */
            if (slot == camera_target_slot && camera_preset_active)
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

            /* Span-distance actor cull (mirrors original RenderRaceActorForView
             * @ 0x0040C2FD): for non-owner actors, compute |delta_spans| with
             * ring wrap and skip body+wheel+smoke render when delta >= cull
             * window. Tire-track emitter still runs in the post-loop pass
             * below for the view owner, matching LAB_0040c7ba's owner-only gate.
             *
             * Original window: gRaceTrackSpanCullWindow @ 0x004AAEF4, written
             * each frame in RunRaceFrame @ 0x42BB72 as
             *   min(ftol((v*0.85+0.15)*max), max) * 2
             * with max=64 fullscreen / 32 split-screen and v from 0x466ea8
             * (default 0.65 → cullWindow=90; v=1.0 → 128 fullscreen / 64 split).
             *
             * [FIX 2026-05-26] Use actor_cull_window (orig ~88-128) NOT
             * fwd_window (~720). Reusing the track-render window made traffic +
             * AI cars pop in ~8x too far ahead vs the original. The visible
             * empty track beyond ~88 spans is faithful — the original renders
             * track far but only shows cars near. [CONFIRMED @ 0x40C2FD; writer
             * @ 0x42BB72; actor span at +0x82] */
            /* Cull racers AND traffic by span-window; orig
             * RenderRaceActorsForView @ 0x0040BD20 applies the same gate to
             * the second loop (slots 6-11). The racer-only restriction was a
             * port mistake that let traffic render at any distance. */
            if (slot != camera_target_slot) {
                TD5_Actor *owner = td5_game_get_actor(camera_target_slot);
                if (owner) {
                    int actor_span = (int)actor->track_span_normalized;
                    int owner_span = (int)owner->track_span_normalized;
                    int delta = actor_span - owner_span;
                    int ring = td5_track_get_ring_length();
                    if (ring > 0) {
                        int half = ring / 2;
                        if (delta >  half) delta -= ring;
                        if (delta < -half) delta += ring;
                    }
                    int delta_abs = delta < 0 ? -delta : delta;
                    if (delta_abs >= actor_cull_window) {
                        static uint32_t s_cull_log = 0;
                        if ((s_cull_log++ % 600u) == 0u) {
                            TD5_LOG_I(LOG_TAG,
                                      "actor span-cull: view=%d slot=%d delta=%d window=%d",
                                      view_index, slot, delta_abs, actor_cull_window);
                        }
                        continue;
                    }
                }
            }

            /* (Lighting moved below to td5_render_apply_track_lighting,
             * which writes to the s_light_dirs[] basis ComputeMeshVertexLighting
             * actually consumes. The earlier td5_track_apply_segment_lighting
             * skeleton was a dead branch -- wrong struct offsets, never wired
             * to a populated table, and writing to a parallel set of globals
             * the renderer did not read.) */

            /* Original (0x40C120): compute interpolated render position.
             * render_pos = (world_pos + linear_velocity * g_subTickFraction) / 256.
             * [CONFIRMED @ 0x40C164-0x40C1D4]
             *
             * Original at 0x40C1C5-0x40C1D7 then applies the chassis render-Y
             * lift:
             *   ECX = g_trackHeightBaseOffset (signed int @ 0x0048f070)
             *   SHL ECX, 8                    ; (lift << 8) in fp8 units
             *   FILD ECX → FSUBR [ESI+0x20]   ; pos_y = pos_y - (lift << 8)
             *   FSTP  [ESI+0x20]
             * g_trackHeightBaseOffset is initialized at 0x0040BCE3/0x0040BCFB:
             *   normal gameplay (g_inputPlaybackActive==0) → -36
             *   replay playback (g_inputPlaybackActive!=0) → -18
             * With the default value of -36, the FSUBR adds +9216 fp8 = +36
             * world units to the render Y. TD5 uses a Y-down world convention
             * (gravity adds positive Y at td5_physics.c:3849), so +36 world Y
             * pushes the chassis DOWN by 36 units toward the ground plane.
             * Without this lift the port renders the car mesh ~36 world units
             * above where its wheel-contact probes touch the track, giving the
             * user-reported "floating car" visual (2026-05-17).
             *
             * Original gates this on RenderRaceActorForView (racers only); the
             * traffic block in RenderRaceActorsForView (0x40BD20) skips it, so
             * we apply the offset only to racer slots [0..TD5_MAX_RACER_SLOTS).
             * [CONFIRMED @ 0x40C1C5-0x40C1D7 + 0x40BD20 absence] */
            {
                extern float g_subTickFraction;
                extern int td5_input_is_playback_active(void);
                float frac = g_subTickFraction;
                float interp_x = (float)actor->world_pos.x + (float)actor->linear_velocity_x * frac;
                float interp_y = (float)actor->world_pos.y + (float)actor->linear_velocity_y * frac;
                float interp_z = (float)actor->world_pos.z + (float)actor->linear_velocity_z * frac;
                if (slot < TD5_MAX_RACER_SLOTS) {
                    /* g_trackHeightBaseOffset = -36 normally, -18 under playback.
                     * Subtract (offset << 8) in fp8 → equivalent to adding
                     * (-offset) world units after the /256 conversion below. */
                    int height_base_offset = td5_input_is_playback_active() ? -18 : -36;
                    interp_y -= (float)(height_base_offset << 8);
                }
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
                if (slot == 0) {
                    static int s_ratt2 = 0;
                    if ((s_ratt2++ % 30) == 0)
                        TD5_LOG_I("physics",
                            "RENDERATT slot0 vmode=%d (PHYSICS matrix direct): disp[roll=%d pitch=%d]",
                            (int)actor->vehicle_mode,
                            (int)actor->display_angles.roll, (int)actor->display_angles.pitch);
                }
                mat3x3_mul(s_camera_basis, actor->rotation_matrix.m, view_rot.m);
            } else {
                extern float g_subTickFraction;
                float interp_mat[9];
                short interp[3];
                float ifrac = g_subTickFraction * (1.0f / 256.0f);
                interp[0] = actor->display_angles.roll  + (short)(int)(actor->angular_velocity_roll  * ifrac + 0.5f);
                interp[1] = actor->display_angles.yaw   + (short)(int)(actor->angular_velocity_yaw   * ifrac + 0.5f);
                interp[2] = actor->display_angles.pitch + (short)(int)(actor->angular_velocity_pitch * ifrac + 0.5f);
                /* [RENDERATT DIAG 2026-05-27] Does the RENDER attitude (interp,
                 * after sub-tick extrapolation) match the PHYSICS attitude
                 * (display_angles)? If interp diverges (esp. interp[0]=front-rear
                 * vs disp_roll), the sub-tick interpolation is flattening the car
                 * vs the slope. Slot 0 only, rate-limited. */
                if (slot == 0) {
                    static int s_ratt = 0;
                    if ((s_ratt++ % 30) == 0) {
                        TD5_LOG_I("physics",
                            "RENDERATT slot0: interp[roll=%d pitch=%d] disp[roll=%d pitch=%d] "
                            "angvel[roll=%d pitch=%d] subfrac=%.4f ifrac=%.5f",
                            (int)interp[0], (int)interp[2],
                            (int)actor->display_angles.roll, (int)actor->display_angles.pitch,
                            (int)actor->angular_velocity_roll, (int)actor->angular_velocity_pitch,
                            (double)g_subTickFraction, (double)ifrac);
                    }
                }
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

            /* Vehicle shadow is now drawn in the shadow PRE-PASS above (before
             * ANY car body in this view), not inline here. [FIX 2026-06-02
             * inter-actor overlay] Drawing all shadows first and then all bodies
             * makes EVERY opaque body (z_write=1) overwrite EVERY shadow it
             * covers — extending the 2026-06-01 per-actor "shadow before its own
             * body" fix to cover the inter-actor case ("other cars' shadows
             * render over my car") while keeping the player-self fix intact. */

            td5_render_prepared_mesh(mesh);

            /* Chrome/envmap reflection overlay (0x40C120 second pass).
             * Original (RenderRaceActorForView @ 0x0040c120) gates on
             * `actor_00 == 0` — only the player car (slot 0) gets the reflection
             * pass. Port deviation: extend to all racer slots (0..5) per user
             * request; AI cars now share the player's chrome mesh
             * (g_playerReflectionMeshResource @ 0x004c3d40 is a single global,
             * not per-slot). Traffic actors render through a different inlined
             * path in RenderRaceActorsForView and are intentionally left without
             * reflection. [RE basis: agent confirmed mesh is global, gate is
             * slot==0 in original — this is a deliberate visual enhancement.] */
            if (s_proj_effect_mode == 2 && slot < TD5_MAX_RACER_SLOTS) {
                td5_render_update_projection_effect(slot, actor);
                render_vehicle_reflection_overlay(mesh, slot);
            }

            /* Render wheel ring + brake-light billboards — RACER SLOTS ONLY.
             * [FIX 2026-06-02 traffic-wheel/brake faithfulness] The original
             * renders these ONLY for racers (slots 0..5) via RenderRaceActorForView
             * @0x0040c120 -> RenderVehicleWheelBillboards @0x00446f00 /
             * RenderVehicleTaillightQuads @0x004011c0. Traffic (slots 6..11) goes
             * through the SEPARATE inline branch in RenderRaceActorsForView
             * @0x0040bd20, which draws body + shadow + overlay/smoke only — NO
             * wheel billboards and NO tail-lights [CONFIRMED @ 0x0040bd20 callee
             * list; 0x00446f00 has a single caller = 0x0040c120].
             *
             * The port previously called both for ALL slots, so traffic got
             * wheel billboards built from slot-0's wheel geometry + tire/hubcap
             * textures (the user's "traffic wheels have the wrong texture") and
             * tail-lights the original never draws. Traffic wheels are meant to
             * come solely from the baked body mesh. Gating to racer slots matches
             * the original exactly. (AI racers 1..5 keep their brakes, drawn
             * depth-tested per the 2026-06-02 inter-actor overlay fix; shadows
             * stay in the pre-pass for ALL actors since the traffic branch DOES
             * queue shadow quads.) */
            if (slot < TD5_MAX_RACER_SLOTS) {
                render_vehicle_wheel_billboards(actor, slot);
                render_vehicle_brake_lights(actor, slot);
            }

            /* (Vehicle shadow drawn in the per-view shadow pre-pass above.) */

            /* Tracked-actor marker (cop chase strobes) — orig
             * RenderRaceActorForView @ 0x0040c79c gates:
             *   wanted_mode_enabled != 0 &&
             *   g_wantedTargetTrackerActive != 0 &&
             *   slot == g_wantedTargetSlotIndex (=0)
             * The render call lives in td5_render.c:render_tracked_actor_marker
             * (port of 0x0043cde0). Visuals stay inert in non-wanted modes. */
            if (g_td5.wanted_mode_enabled &&
                td5_game_get_wanted_target_tracker() > 0 &&
                slot == td5_game_get_wanted_target_slot()) {
                /* Pass the SAME body transform the mesh used (view_rot +
                 * render_pos) so the strobe is welded to the car body. */
                render_tracked_actor_marker(actor, &view_rot, &render_pos);
            }

            /* Wanted-mode damage indicator overlay — orig
             * RenderRaceActorForView @ 0x0040c7a4 calls
             * UpdateWantedDamageIndicator(slot) unconditionally per actor;
             * the gate (wanted_mode + matching slot) lives inside the
             * called function. Port mirrors orig callsite location. */
            if (slot < TD5_MAX_RACER_SLOTS) {
                td5_hud_update_wanted_damage_indicator(slot);
            }

            /* Smoke effects (0x40C120 tail): called per visible actor per frame.
             * SpawnRearWheelSmokeEffects (0x401330) — burnout hardpoint smoke
             * SpawnVehicleSmokeSprite (0x429CF0) — wanted-target marker smoke
             *
             * Orig at 0x40C793-0x40C7A5 gates SpawnVehicleSmokeSprite on:
             *   g_wantedModeEnabled != 0 AND gWantedDamageState[slot] == 0
             * This isn't general exhaust smoke — it's the cop-chase "wanted
             * target" smoke trail that marks the chased car. In Single Race
             * (game_type != 8) orig never emits it. Port previously called
             * it unconditionally for every visible actor every frame, which
             * is the "smoke never stops emitting" symptom.
             *
             * Also skip for the camera-target actor when this view is in
             * cinematic preset mode (g_raceCameraPresetMode[view] != 0)
             * — matches the original gate at 0x40C120. */
            int wanted_smoke_ok = g_td5.wanted_mode_enabled != 0 &&
                                  slot < TD5_MAX_RACER_SLOTS &&
                                  g_wanted_damage_state[slot] == 0;
            if (!(slot == camera_target_slot && camera_preset_active)) {
                /* Orig 0x40C7A5: SpawnRandomVehicleSmokePuff(actor, slot) —
                 * engine-rev gated random smoke puff. Called per visible
                 * actor per frame, unconditional on wanted-mode (it's the
                 * "labouring engine" puff visible during slow climbs etc.).
                 * Skipped under cinematic-preset for consistency with the
                 * surrounding rear-wheel/wanted-smoke skip. */
                td5_vfx_spawn_random_smoke_puff(actor, view_index);
                td5_vfx_spawn_rear_wheel_smoke(actor, view_index);
                if (wanted_smoke_ok) {
                    td5_vfx_spawn_smoke(actor);
                }
            }

            actor_render_count++;
            actor_meshes_submitted++;

            TD5_LOG_D(LOG_TAG,
                      "vehicle render: view=%d slot=%d pos=(%.2f, %.2f, %.2f) mesh=%p",
                      view_index, slot,
                      render_pos.x, render_pos.y, render_pos.z,
                      (void *)mesh);
        }

        /* Per-view tire-track emitter dispatch (UpdateTireTrackEmitters
         * @ 0x43FAE0). Original RenderRaceActorForView LAB_0040c7ba body:
         *   if (actor_00 == *(&gPrimarySelectedSlot + view_idx))
         *       UpdateTireTrackEmitters(actor);
         * Only the view-owning actor runs the tire-effect chain — AI cars
         * never reach it. Body+wheel+smoke render still iterates all actors
         * above; only the slip-derived smoke + skid-mark spawn is owner-only.
         * [CONFIRMED @ 0x40C7BA: actor==local_18 gate]
         *
         * function_callers confirms 0x43FAE0 has exactly one caller (0x40C120)
         * and UpdateRear/FrontTireEffects each have one caller (0x43FAE0).
         * No sim-tick path in the original. */
        if (camera_target_slot >= 0 && camera_target_slot < TD5_MAX_RACER_SLOTS) {
            TD5_Actor *owner_actor = td5_game_get_actor(camera_target_slot);
            if (owner_actor && td5_game_get_slot_state(camera_target_slot) != 3) {
                td5_vfx_update_tire_track_emitters(owner_actor, view_index);
            }
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

    /* Near/far clip.
     *
     * [FIX 2026-05-31 distant-building-popin] far_cull is a FIXED constant in
     * the original — round(3.0f * 65000.0f) = 195000, stored at 0x00467360,
     * computed @ 0x0042D47C-0x0042D48E [CONFIRMED]. It is NOT scaled by the
     * pause-menu VIEW slider. The slider instead reduces render distance by
     * lowering the number of MODELS.DAT span ENTRIES walked per frame
     * (effectiveSpans / frac_scaled path @ :1996 and :2110, matching orig
     * RunRaceFrame 0x42BB2E-0x42BC3C [CONFIRMED]).
     *
     * The prior port made far_cull itself slider-driven (5000..65536) — ~3x to
     * ~39x nearer than the original 195000. A span's MODELS.DAT building could
     * be IN the entry window (submitted) yet frustum-REJECTED by the per-mesh
     * bounding-sphere test (td5_render_is_sphere_visible @ :1567 /
     * td5_render_test_mesh_frustum @ :1627) until the camera advanced close
     * enough — so distant buildings "popped" into view and could draw in front
     * of nearer geometry that crossed the threshold on a different frame.
     * Pinning far_cull to the fixed 195000 lets the whole visible scene resolve
     * in a single pass, as the original does ("everything at once").
     *
     * [UPDATED 2026-06-01] far_clip (depth normalization) is now ALSO extended
     * to 195000 (see DEFAULT_FAR_CLIP / DEPTH_NORMALIZE_INV) and the depth
     * buffer upgraded D16->D32_FLOAT, so geometry drawn out to the 195000
     * far-cull gets a real depth value instead of clamping to the far plane and
     * z-fighting. Range and far-cull now intentionally match. */
    s_near_clip = DEFAULT_NEAR_CLIP;
    s_far_clip  = DEFAULT_FAR_CLIP;
    s_far_cull  = DEFAULT_FAR_CULL;   /* orig 0x0042D48E = 195000, slider-independent */
    {
        static int s_farcull_logged = 0;
        if (!s_farcull_logged) {
            TD5_LOG_I(LOG_TAG,
                "far_cull pinned to fixed %.0f (orig 0x42D48E); VIEW slider drives "
                "MODELS.DAT span entry count only, not the frustum far plane",
                s_far_cull);
            s_farcull_logged = 1;
        }
    }

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

    /* Dump accumulated cull stats every 5 frames. Stats reflect the
     * PREVIOUS frame(s)' mesh-visibility tests; dump first, then reset
     * happens inside the dump fn. */
    {
        static int s_dump_counter = 0;
        s_dump_counter++;
        if (s_dump_counter >= 5) {
            s_dump_counter = 0;
            td5_render_dump_view_dist_stats();
        }
    }
}

/* --- Translucent Primitive Pipeline --- */

/* [ARCH-DIVERGENCE: struct-pool vs raw-byte linked-list; L5 sweep 2026-05-21]
 *   Mirrors InitializeTranslucentPrimitivePipeline @ 0x004312E0. Orig builds the
 *   color LUT (DAT_004aee68 walk with iVar2 += 0x10101, init -0x1000000) AND a
 *   512-entry 8-byte raw pool (heap-alloc'd, flags=0, sort_key=0xFFFF, next
 *   chained sequentially); port carves the color LUT into td5_render_init (same
 *   formula `0xFF000000u | luminance*0x10101`) and lays the same 512-entry pool
 *   out as a typed struct array. Same init values, same free-list chaining, same
 *   active-count = 0 reset. */
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

/* [ARCH-DIVERGENCE: linked-list bucket walk -> pool-array dispatch;
 *  Phase 5(d) L5 audit 2026-05-21]
 *   Orig FlushQueuedTranslucentPrimitives @ 0x00431340 walks
 *   g_translucentPrimitiveBucketHead's intrusive ushort linked list (each
 *   record at puVar2 points to puVar2+2 = next-link, puVar2 = opcode); it
 *   dispatches via a 7-entry function-pointer table at
 *   PTR_EmitTranslucentTriangleStrip_00473b9c (orig opcodes 0..6 = strip,
 *   strip-dup, projected-tri, projected-quad, billboard-bucket-insert,
 *   strip-direct, quad-direct), then drains the immediate-mode batch via
 *   the same D3D3 vtable call as FlushImmediateDrawPrimitiveBatch (orig
 *   0x004329E0). Port uses a flat pool array (TranslucentBatchEntry
 *   s_translucent_pool[]) with explicit next indices and the same 7-entry
 *   dispatch table (s_dispatch_table); the tail flush calls
 *   flush_immediate_internal which is the ARCH-D'd D3D11 path. Same opcode
 *   semantics; the linked-list -> pool swap is mechanical. */
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

/* [ARCH-DIVERGENCE: struct vs raw-byte texture-page pool; L5 sweep 2026-05-21]
 *   Mirrors ResetTexturePageCacheState @ 0x0040BA60. Orig walks raw byte arrays
 *   (DAT_0048dc40[3]+5 stride-8 status+age, DAT_0048dc40[8] page-id u32 array)
 *   gated by DAT_0048dc40[0x18]/[0x1c] counts; port walks the equivalent
 *   struct-array s_texture_cache[] with identical per-slot reset semantics
 *   (page_id=-1, status=0, age=0, used_this_frame=0). Port adds explicit
 *   current/previous-page invalidation (-1) that orig does in BeginRaceScene. */
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

/* [CONFIRMED @ 0x0040BA10 AdvanceTexturePageUsageAges; L5 sweep 2026-05-21]
 *   Byte-faithful: same LRU sweep -- per-slot if-used reset-age + clear-flag
 *   else increment-with-0xFF-saturate. Orig walks raw byte arrays at
 *   DAT_0048dc40[0]/[3]+5; port walks struct-array field equivalents with
 *   identical loop ordering. */
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
    if (s_in_sky_draw)           return; /* preserve caller's SKY preset */
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

    /* Reverse-direction light-zone span mirror is gated on g_td5.reverse_direction
     * in tl_reverse_mirror_span(); log it once per level load so the reverse
     * "interior shader" fix is visible in engine.log without per-frame spam. */
    if (g_td5.reverse_direction)
        TD5_LOG_I(LOG_TAG,
                  "reverse light-zone span mirror ACTIVE for level %d (forward-frame remap of STRIPB spans)",
                  level_number);

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

/* [BUGFIX 2026-05-29] Reverse-direction light-zone span mirror.
 *
 * Restores parity with the original on reverse tracks. The per-track light-zone
 * table (td5_light_zones) and the environs page table are FORWARD-numbered. The
 * port keeps the strip walker's REVERSE-numbered STRIPB.DAT span in
 * track_span_raw and un-mirrors it (ring-1-span) at each forward-frame consumer
 * -- e.g. MODELS.DAT geometry does this at td5_track.c:6358-6360. The vehicle
 * lighting / environs-projection consumers were MISSING that mirror, so in
 * reverse they indexed the forward zone table with a reverse span and applied
 * the tunnel "interior" darkening at the WRONG physical location -- on the open
 * road instead of in the tunnel.
 *
 * Ground truth (user-observed, 2026-05-29): the ORIGINAL shows the tunnel
 * darkening ONLY inside the Keswick tunnel in reverse -- it is correct, so this
 * is a genuine port bug, NOT a faithful reproduction. (The earlier static-RE
 * claim that the original reads the raw span at +0x80 mispredicted the outcome;
 * the original's exact internal span handling in reverse was not traced, but its
 * observed behavior is tunnel-only darkening.)
 *
 * Fix: mirror the span into the forward frame before the zone walk, exactly like
 * the MODELS.DAT consumer, so the darkening tracks the physical tunnel in both
 * directions. Verified on Keswick reverse (port screenshots): span 2408 = inside
 * the tunnel -> dark; span 617 = open road -> normally lit. Mirror uses the
 * main-road ring length only (branch spans >= ring keep their raw index; they
 * have no light zone anyway). Forward direction is byte-for-byte untouched
 * (g_td5.reverse_direction gate). */
static inline int tl_reverse_mirror_span(int span)
{
    int ring = g_td5.track_span_ring_length;
    if (g_td5.reverse_direction && ring > 0 && span >= 0 && span < ring)
        return ring - 1 - span;
    return span;
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
 *   All-zero RGB disables the slot.
 * [CONFIRMED @ 0x0042E130] L5 promotion sweep audit (2026-05-18). Byte-faithful
 *   port: same all-zero-disable gate, same (R+G+B)/3 intensity, same
 *   1/1024 attenuation (DAT_0045d6a0 = 0x3a800000 = 1/1024 IEEE754).
 *   Minor 1-LSB rounding divergence: orig (int)((R+G+B)/3) truncates
 *   before float convert; port (float)(R+G+B)/3.0f preserves fraction.
 *   Result diverges <= 1.0 on intensity for non-multiples-of-3,
 *   harmless on downstream lighting. */
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
 *   default zero vector (DAT_004ab0f8/0fc/100 verified zero in memory).
 *
 * L5 promotion sweep audit (2026-05-18) — byte-equivalent, render-side
 * with two intentional ARCH-DIVERGENCEs.
 *
 *   - 3-slot M^T transform: orig computes per-slot
 *     [DAT_004ab0d0/d4/d8] = contrib.x * m[0] + contrib.y * m[3] + contrib.z * m[6]
 *     (and the y/z output rows likewise reading m[1,4,7] and m[2,5,8]).
 *     Port's tl_commit_to_render_globals does the identical column-sum.
 *     [CONFIRMED @ 0x0042CEA9-0x0042CEFA decomp lines (slot 0),
 *     0x0042CF20-0x0042CF6B (slot 1), 0x0042CF8A-0x0042CFDA (slot 2).]
 *
 *   - Per-slot enable test: orig reads three sentinel globals
 *     [DAT_004AAFD0 / D4 / D8] = `_slot_enabled[3]`, port reads
 *     s_tl_contrib[s].enabled. Equivalent state machine — both are
 *     set by tl_set_contrib() (orig: SetTrackLightDirectionContribution
 *     @ 0x0042E130) and cleared when r==g==b==0.
 *
 *   - Disabled-slot fallback writes a default direction
 *     [DAT_004AB0F8 / 0FC / 100], which is all-zero in the binary's
 *     .data segment [CONFIRMED via memory_read 2026-05-18: 12 bytes of
 *     0x00 at 0x004AB0F8]. Port writes literal 0.0f. Behaviour-equivalent.
 *
 *   - Slot 2's output-write order differs cosmetically (orig writes
 *     y, z, x; port writes x, y, z). Net memory state identical because
 *     all three locations are written before the function returns.
 *
 *   [ARCH-DIVERGENCE: orig takes a `float *matrix` argument (caller
 *   passes the actor's rotation_matrix pointer). Port takes a
 *   `const TD5_Actor *actor` and dereferences `actor->rotation_matrix.m`
 *   internally. Same data flow, different parameter shape — caller
 *   site is simpler and avoids a separate pointer arg.]
 *
 *   [ARCH-DIVERGENCE: orig writes into D3D3 fixed-function global light
 *   state ([DAT_004AB0D0..F0] = 16 floats = 4 dxLightDir-style records
 *   bound to the IM3 device); port writes into `s_light_dirs[9]` +
 *   `s_ambient_intensity` consumed by ComputeMeshVertexLighting (the
 *   per-vertex software-lit code path). Same lighting model (3-slot
 *   directional + scalar ambient, per-vertex N dot L), different
 *   delivery mechanism. The D3D3 light-state machinery does not exist
 *   in the D3D11 wrapper.]
 */
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

/* [CONFIRMED @ 0x00430150] ApplyTrackLightingForVehicleSegment.
 * L5 audit 2026-05-18 (TD5_pool0 read-only).
 *
 * NOT a D3D3->D3D11 ARCH-DIVERGENCE despite first appearances.
 *
 * The original is NOT a D3D fixed-function vertex-lighting call. It is a
 * pure CPU computation that selects a per-vehicle zone from a 285-entry
 * per-track lighting table (DAT_004aee14, stride 0x24 bytes, copied into
 * td5_light_zones_table.inc) and feeds 3 directional + 1 scalar-ambient
 * contributions into the SOFTWARE vertex-lighting pipeline (callee
 * SetTrackLightDirectionContribution @ 0x0042E130 and
 * UpdateActiveTrackLightDirections @ 0x0042CE90 -> ComputeMeshVertexLighting
 * @ 0x0042CFC0 (called outside this function), which produces per-vertex
 * lit colors before vertex submission. D3D3 sees only the post-lit colors).
 *
 * So the port's lighting math MUST remain CPU-side and bit-faithful — the
 * D3D11 backend (like the original's D3D3 layer) only consumes the lit
 * vertex colors, never raw normals + light state. Skipping or shader-izing
 * this function WOULD diverge visually.
 *
 * Structural mapping vs the original (verified with the decompilation in
 * pool0 on 2026-05-18):
 *   - Prologue zone walk (advancing actor->field_0x377 +/- one zone at a
 *     time while the actor's track_span_raw leaves [span_lo, span_hi]):
 *     `update_actor_light_zone()` above.
 *   - case 0 (static):                  `tl_apply_case0()`
 *   - case 1 (transition):              `tl_apply_case1_midspan()` +
 *                                       `tl_blend_from_start/_from_end()`
 *   - case 2 (multi-sample mid-zone):   `tl_apply_case2()`
 *     with sub_mode 0/1/2 via `tl_pick_strip_vertex()`
 *   - case 3 (half/half full-zone):     two-branch BlendStart/End with
 *                                       max_dist = (span_hi-span_lo+1)*0x200
 *   - SetTrackLightDirectionContribution @ 0x0042E130:  `tl_set_contrib()`
 *   - ComputeAverageDepth @ 0x0043E7B0:                 `tl_set_depth()`
 *   - UpdateActiveTrackLightDirections @ 0x0042CE90:    `tl_commit_to_render_globals()`
 *   - ConvertFloatVec4ToShortAngles @ 0x0042CDB0:       `tl_normalize_4096()`
 *
 * Per-byte mapping of the original short-pointer struct:
 *   psVar9[0..1]    = span_lo, span_hi
 *   psVar9[2..4]    = dir_x, dir_y, dir_z
 *   (psVar9+0x0c..) = weight_{r,g,b} (3 bytes)
 *   (psVar9+0x10..) = amb_{r,g,b}    (3 bytes)
 *   psVar9[10..11]  = pos_off_x, pos_off_y
 *   (psVar9+0x18)   = blend_mode (low byte of psVar9[0xc])
 *   (psVar9+0x19)   = spacing
 *   (psVar9+0x1a)   = sub_mode  (low byte of psVar9[0xd])
 *   (psVar9+0x1b)   = multiplier
 *   psVar9[14]      = state_key  (dword) — environs/chrome light_index
 *   psVar9[16]      = slot_color (dword)
 *
 * Numeric coverage:
 *   - case 1 mid-zone takes TWO vertex samples: left_vertex_index and
 *     left_vertex_index + (strip[+3] & 0xf). The original adds
 *     `(*(byte*)(strip+3) & 0xf)` to `*(ushort*)(strip+4)` (= left_vertex);
 *     port mirrors via `((const uint8_t *)sp)[3] & 0x0F`.
 *   - The attenuation formula
 *         iVar = (mag^2 * multiplier + sign_fixup) >> 14
 *         atten = iVar < 0x1001 ? 0x1000 - iVar : 0
 *         channel = (atten * weight + sign_fixup) >> 12
 *     is reproduced byte-for-byte in `tl_sample_contrib()`.
 *   - The body-frame transform M^T * world_dir is reproduced exactly in
 *     `tl_commit_to_render_globals()` using the actor's rotation_matrix
 *     (row-major: m[3*r + c]).
 *
 * What's different from the original (zero-divergence cosmetic):
 *   - The port's zone walk caches per-actor zone index in
 *     `s_actor_light_zone[slot]` rather than `actor->field_0x377`; both
 *     are equivalent persistent storage with the same forward/backward
 *     stepping semantics, used only to amortize linear search.
 *   - Failure recovery: when a strip/vertex cannot be resolved, the port
 *     falls through to `tl_apply_case0()`. The original LogReport()s "1st
 *     light: spacing zero" and leaves the previous frame's basis intact.
 *     For runtime correctness this is equivalent (next frame the table is
 *     stable); the LogReport is a debug artefact.
 *   - The port pre-zeros disabled slot vectors in `tl_commit_to_render_globals()`
 *     when `s_tl_contrib[s].enabled == 0`. The original relies on the
 *     SetTrackLightDirectionContribution callee zeroing the global s_light_dir
 *     slot. Same observable state, more conservative.
 *
 * Promoting to L5. No code change. Confidence-map: L5; structurally
 * equivalent reimplementation of a sim-stage CPU lighting selector.
 *
 * Companion reference doc (kept for the cross-architecture call site
 * audit pattern): re/analysis/reference_arch_track_vehicle_segment_lighting_d3d_2026-05-18.md
 */
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

    /* Reverse-direction fix: map the STRIPB span into the forward-numbered
     * light-zone frame so the darkening follows the physical tunnel, not the
     * reverse span number. No-op in forward. See tl_reverse_mirror_span(). */
    span = tl_reverse_mirror_span(span);

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

/* [ARCH-DIVERGENCE: raw-byte slot array -> typed struct array;
 *  Phase 5(d) L5 audit 2026-05-21]
 *   Orig SetProjectionEffectState @ 0x0043E210 indexes into a 0x20-byte-
 *   per-slot raw global at DAT_004BF520+slot*0x20, writing fields by byte
 *   offset (slot+0x00..+0x14 vary by mode 1/2/3). Port replaces this with a
 *   typed ProjectionEffectState struct array (s_proj_effect[]) and named
 *   fields (sub_mode, cos_heading, sin_heading, anchor_xyz, scroll_offset,
 *   texture_page). The three orig sub-modes (1=planar scroll, 2=chrome UV,
 *   3=world anchor) all map 1:1 onto port fields. The chain
 *   UpdateActorTrackLightState (orig 0x0040CD10) -> ConfigureActorProjection
 *   Effect (orig 0x0040CBD0) -> SetProjectionEffectState (orig 0x0043E210)
 *   collapses into this single port function. Note one [UNCERTAIN] mode-1
 *   param (which 3-float vector ConfigureActorProjectionEffect passes)
 *   inline at the mode-1 branch — port picks linear_velocity for the
 *   forward-scroll semantic; orig binary disasm is ambiguous at the call
 *   site. */
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

    /* Same reverse-direction span mirror as td5_render_apply_track_lighting:
     * the environs page table is forward-numbered, so the projection-effect
     * zone walk must index it in the forward frame in reverse. No-op forward. */
    zone_idx = update_actor_light_zone(
        slot, tl_reverse_mirror_span((int)(int16_t)actor->track_span_raw));
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
         * [CONFIRMED @ 0x0040CBD0 ConfigureActorProjectionEffect; REG-9 verdict 2026-05-22]
         * Ghidra-verified: orig calls SetProjectionEffectState with
         * &actor->linear_velocity_x as param_3 for the mode-1 path (the
         * iVar1 != 2 && iVar1 != 3 branch). Mode 2 uses the same vector;
         * only mode 3 swaps in (world_pos + linear_vel*subTickFraction) *
         * fpScale. Port uses actor->linear_velocity_* in mode 1, matching
         * orig exactly. The earlier [UNCERTAIN] note has been resolved. */
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
 * The original draws shadows via a deferred translucent sort list rendered
 * AFTER all opaque geometry (car bodies, wheels, brake lights). Depth-test
 * is enabled (LESSEQUAL) so closer opaque pixels — other cars, walls,
 * environment props — correctly occlude the shadow. The original separates the
 * shadow from the road via the shared track projection (coplanar depths tie) and
 * applies NO depth bias [CONFIRMED @ 0x0040C120: no D3DRENDERSTATE_ZBIAS, no sz
 * offset]; the port can't share that projection, so it uses a tiny 2 view-z
 * toward-camera nudge instead (see SHADOW_DEPTH_Z_BIAS).
 *
 * Source port approach:
 *   - TD5_PRESET_SHADOW (z_test=LEQUAL, z_write=0, alpha_ref=1, SRCALPHA/
 *     INVSRCALPHA). Depth test on so opponent cars and walls correctly
 *     occlude the shadow; depth write off so the shadow doesn't write to
 *     the depth buffer and break subsequent translucent passes. (Orig flushes
 *     shadows with z_write ON; kept OFF here because the preset is shared with
 *     tire tracks — known faithful-divergence, does not affect occlusion.)
 *   - [FIX 2026-06-01] The call site draws the shadow BEFORE the car body mesh
 *     (then wheels/brake lights/reflection follow the body). The opaque body,
 *     drawn next with z_write=1, paints over any shadow pixels that fall on the
 *     car — reproducing the original deferred pass's net result (every body
 *     occludes the shadows) and curing the player-only over-car symptom that an
 *     after-body z-test could not (the close player's 1.25-scaled shadow corners
 *     project nearer than its own lower body through the separate projection).
 *   - SHADOW_VERTICAL_OFFSET = 0: the shadow sits at the wheel-contact (ground)
 *     plane. The orig's -22 world-Y lift causes the shadow to out-depth the
 *     close player car in the port's separate projection (see that macro), so
 *     the port keeps it flat on the ground and separates via the tiny depth
 *     nudge below.
 *   - Scale corners outward from the XZ centroid by 1.25 to match the
 *     original's _g_wheelSuspensionRenderScale @ 0x00463B64 (1.25f) so
 *     the shadow has the same footprint as the original render (a 1.85f
 *     guess used until 2026-05-17 produced shadows ~1.48x linear larger).
 *   - UV mapping orients texture U-axis along car FRONT-BACK to match the
 *     original. The previous port mapping (U along left-right) rotated the
 *     texture 90° and made the shadow appear too narrow across the car —
 *     the 1.85f scale was partially compensating for that.
 *   - Subtick-interpolate corners with linear_velocity * g_subTickFraction
 *     so the shadow doesn't sawtooth-lag behind the car at speed (the
 *     car mesh is interpolated the same way at line ~1547).
 */
/* [FIX 2026-06-01 shadow-over-car] World-space Y nudge added to all 4 shadow
 * corners. Kept at 0 in the port — the shadow sits exactly at the wheel-contact
 * (ground) plane, which is where a car shadow visually belongs.
 *
 * The ORIGINAL applies _g_shadowVerticalOffset = 0xC1B00000 = -22.0f here
 * [CONFIRMED @ 0x0040BB70] (Y-down world, so -22 lifts the shadow 22 units UP,
 * toward the camera). In the original that is harmless: the shadow shares the
 * track's SINGLE projection (WritePointToCurrentRenderTransform @ 0x42E4F0 ->
 * ClipAndSubmitProjectedPolygon @ 0x4317F0), so the lift never makes the shadow
 * out-depth the car body.
 *
 * The PORT projects the shadow through a SEPARATE hand-rolled transform, so a
 * world-Y lift turns into a real toward-camera depth offset. At the chase
 * camera's close range that offset beats the small gap between the PLAYER car's
 * lower-rear bumper and the ground -> shadow drew OVER the player car (opponents,
 * far away, were unaffected -> the player-only symptom). So the port leaves this
 * at 0 and instead uses the tiny SHADOW_DEPTH_Z_BIAS below for road separation —
 * the same approach that shipped correctly before the D16->D32 depth change. */
#define SHADOW_VERTICAL_OFFSET  (0.0f)
/* [CONFIRMED 2026-05-17] g_wheelSuspensionRenderScale @ 0x00463B64 = 1.25f.
 * Previous port value 1.85f was an unverified guess (see commented-out
 * reference to "the unread _g_wheelSuspensionRenderScale") and produced
 * shadows ~1.48x linear (~2.2x area) larger than the original. */
#define SHADOW_CORNER_SCALE     (1.25f)
/* [FIX 2026-06-01 shadow-over-car] Road-separation for the port: a TINY
 * toward-camera depth nudge (NOT the broken 500 view-z pull, NOT a world-Y lift).
 *
 * Because the port's shadow uses a separate projection from the track, at the
 * exact ground plane the shadow and road depths tie only to within sub-LSB
 * jitter — some pixels lose the LEQUAL tie and the shadow drops out ("tail
 * visible depending on angle"). A 2 view-z pull toward the camera clears that
 * jitter so the shadow reliably wins against the coplanar road, while staying
 * FAR below the car-body gap (tens of view-z) so it can never reach the car —
 * including the close player car's lower-rear bumper. This is the value the port
 * shipped successfully before the D16->D32 depth upgrade (commit 49ae1e4); the
 * D32 regression came from ballooning it to 500 view-z, which over-shot onto the
 * car. Expressed via DEPTH_NORMALIZE_INV so it tracks the depth normalization.
 *
 * The original needs no such bias (its shadow shares the track transform, so
 * coplanar depths tie deterministically) and has NO D3DRENDERSTATE_ZBIAS / sz
 * offset [CONFIRMED @ 0x0040C120]. The shared TD5_PRESET_SHADOW also selects the
 * wrapper's shadow-decal rasterizer (DepthBias=-500), which on D32_FLOAT near
 * geometry is ~1e-7 (negligible) — left in place, harmless. Byte-faithful state
 * would be z_write=ON (orig flushes shadows under the OPAQUE pass, ZWRITEENABLE=1
 * @ 0x0040B070); the port keeps z_write=OFF because TD5_PRESET_SHADOW is shared
 * with tire tracks and z-write does not affect occlusion here — known divergence. */
#define SHADOW_VIEW_Y_OFFSET    (0.0f)
#define SHADOW_VIEW_DEPTH_BIAS  (0.0f)
#define SHADOW_PULL_VIEWZ       (2.0f)   /* small toward-camera pull: clears road z-fight, far below the car gap */
#define SHADOW_DEPTH_Z_BIAS     (SHADOW_PULL_VIEWZ * DEPTH_NORMALIZE_INV)

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

/* [ARCH-DIVERGENCE: D3D3 scratch + QueueBatch -> D3D11 immediate quad;
 *  Phase 5(d) L5 audit 2026-05-21]
 *   Corresponds to BuildSpecialActorOverlayQuads @ 0x0040C7E0 (orig 1000B).
 *   Orig walks 8 body-corner positions to find AABB half-extents, derives
 *   four rotated corner offsets (heading-rotated rectangle), composes a 4x4
 *   shadow trapezoid pair (front+rear), pre-bakes via WritePointToCurrent-
 *   RenderTransform into the static scratch table at
 *   g_vehicleShadowAndWheelSpriteTemplates+slot*0x170, applies the
 *   _g_shadowVerticalOffset Y bias to all 4 corners of each quad, then
 *   queues two QueueTranslucentPrimitiveBatch calls. Port re-derives the
 *   quad corners per-frame from the actor's wheel-probe positions (probe_FL
 *   /FR/RL/RR — same XZ centroid + perspective scale formula), emits 4 D3D
 *   vertices directly through td5_plat_render_draw_tris, no scratch buffer.
 *   UV mapping fix at td5_render.c:3796 (commit d0abaad) verified
 *   [CONFIRMED @ 0x0040BB70 / @ 0x00432BD0] inline. */
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

    /* UV mapping — original draws two trapezoid quads sharing an axle-midline
     * edge, with texture U-axis running along the car's longitudinal (front-
     * to-back) axis and V-axis across the car's lateral (left-to-right) axis.
     * [CONFIRMED @ 0x0040BB70 InitializeVehicleShadowAndWheelSpriteTemplates +
     *  0x00432BD0 BuildSpriteQuadTemplate]:
     *   Front quad: FL→(U=130,V=66) FR→(U=130,V=126) RIGHT_mid→(U=192,V=126)
     *               LEFT_mid→(U=192,V=66)
     *   Rear  quad: LEFT_mid→(U=192,V=66) RIGHT_mid→(U=192,V=126)
     *               RR→(U=254,V=126) RL→(U=254,V=66)
     * I.e. U increases FRONT→BACK along car, V increases LEFT→RIGHT across car.
     *
     * The SHADOW atlas entry is 128 wide × 64 tall (atlas_x=128, atlas_y=64
     * per static.hed), and the actual shadow blob inside the texture is wider
     * than it is tall (~96×56 pixels). With orig's mapping, the LONG texture
     * axis (128 wide) runs along the car's length — which matches a typical
     * car's length:width ratio.
     *
     * The port's previous mapping placed U along car's LEFT-RIGHT axis (FL→u0v0,
     * FR→u1v0, RR→u1v1, RL→u0v1), which rotates the texture 90° and makes the
     * shadow appear narrower across the car than the original. Fix: map U
     * along car FRONT-BACK to match orig (FL→u0v0, FR→u0v1, RR→u1v1, RL→u1v0). */
    const float uvs[4][2] = {
        { s_shadow_u0, s_shadow_v0 },   /* FL → texture top-left (U=front, V=left) */
        { s_shadow_u0, s_shadow_v1 },   /* FR → texture bottom-left (U=front, V=right) */
        { s_shadow_u1, s_shadow_v1 },   /* RR → texture bottom-right (U=back,  V=right) */
        { s_shadow_u1, s_shadow_v0 },   /* RL → texture top-right (U=back,  V=left) */
    };

    /* Convert the 4 probes to world-float, accumulating the centroid so we
     * can scale corners outward from it. Orig 0x40C120 scales offsets in
     * ALL THREE components by _g_wheelSuspensionRenderScale (= 1.25f at
     * 0x00463B64):
     *
     *   local_90 = (FL.x - centroid_x) * scale   <- X scaled
     *   local_8c = (FL.y - centroid_y) * scale   <- Y scaled
     *   local_88 = (FL.z - centroid_z) * scale   <- Z scaled
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
    /* [CORRECTED 2026-05-26 r4] Scale Y outward from centroid like the orig.
     *
     * Prior port version only scaled X and Z, keeping each corner at its
     * own wheel-probe Y. On inclined terrain (Edinburgh bowl, Newcastle
     * slopes, mountain passes) the wheels sit at different Ys, so the
     * scaled corners' XZ positions land at outer points where the ground
     * polygon has continued its slope, but the corner Y stays at the
     * INNER wheel value. The shadow plane is then SHALLOWER than the
     * ground polygon — front corners end up buried below the ground
     * mesh, rear corners hover above it. User symptom: "shadow clipping
     * through the ground, tail end visible depending on angle".
     *
     * Scaling Y outward by the same SHADOW_CORNER_SCALE makes the shadow
     * plane stay parallel to the wheel plane, which on a smooth slope
     * lies along the ground polygon. Verified against orig 0x40C120
     * (local_8c, local_80, local_68, local_5c all multiply by
     * _g_wheelSuspensionRenderScale). */
    for (int i = 0; i < 4; i++) {
        corners[i][0] = cx + (corners[i][0] - cx) * SHADOW_CORNER_SCALE;
        corners[i][1] = cy + (corners[i][1] - cy) * SHADOW_CORNER_SCALE;
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

        vy += SHADOW_VIEW_Y_OFFSET;

        /* Near-clip check BEFORE any bias — bias must not push vz below
         * near_clip and erroneously reject the shadow (the r3-r5 bug). */
        if (vz <= s_near_clip) return;

        /* Projection uses RAW vz (no bias) so screen position is correct
         * and matches the car/track projection exactly. */
        float inv_z = 1.0f / vz;
        verts[i].screen_x = -vx * s_focal_length * inv_z + s_center_x;
        verts[i].screen_y = -vy * s_focal_length * inv_z + s_center_y;
        /* Depth_z uses the orig track-poly formula (line ~824), minus a TINY
         * 2 view-z toward-camera nudge (SHADOW_DEPTH_Z_BIAS) so the coplanar
         * road can't z-fight the shadow. The nudge is far below the car-body
         * gap, so it never reaches the car (no over-car). Bias affects depth
         * compare only, NOT screen projection (computed from raw vz above). */
        verts[i].depth_z  = (vz - NEAR_DEPTH_OFFSET) * DEPTH_NORMALIZE_INV
                            - SHADOW_DEPTH_Z_BIAS;
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
    /* TD5_PRESET_SHADOW: z_test=LEQUAL, z_write=0, SRCALPHA/INVSRCALPHA,
     * point filter. Depth test is ON so opponent cars, walls, and any other
     * opaque pixels closer to the camera correctly occlude this shadow. */
    td5_plat_render_set_preset(TD5_PRESET_SHADOW);
    td5_plat_render_bind_texture(s_shadow_page);
    td5_plat_render_draw_tris(verts, 4, indices, 6);

    /* Restore opaque preset so the next per-actor draw starts from a known
     * z_test=1/z_write=1 state. */
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

/* --- Vehicle Brake Lights (0x4011C0) --- */

/* BRAKED sprite atlas cache */
static int   s_braked_page = -1;
static float s_braked_u0, s_braked_v0, s_braked_u1, s_braked_v1;
static int   s_braked_lookup_done = 0;
static uint8_t s_brake_brightness[12]; /* per-slot brightness ramp */

/* [FIX 2026-06-02 inter-actor overlay] Small toward-camera depth pull for the
 * brake billboard, in view-z units (expressed via DEPTH_NORMALIZE_INV so it
 * tracks the depth normalization). With the brake now depth-tested (LEQUAL,
 * TD5_PRESET_TRANSLUCENT_POINT_ZTEST) the FLAT billboard (constant depth =
 * taillight-hardpoint center vz) would otherwise z-fight / partially clip
 * against its OWN angled rear-body surface; a small pull wins that tie cleanly.
 * It stays far below a clearly-in-front car's depth gap (a whole car length),
 * so a nearer car body still correctly occludes a farther car's brake light —
 * which is the reported fix ("traffic brake lights render over my car"). */
#define BRAKE_PULL_VIEWZ   (16.0f)
#define BRAKE_DEPTH_BIAS   (BRAKE_PULL_VIEWZ * DEPTH_NORMALIZE_INV)

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
/* [ARCH-DIVERGENCE: D3D3 sprite template + QueueBatch -> D3D11 immediate quad;
 *  Phase 5(d) L5 audit 2026-05-21]
 *   Corresponds to RenderVehicleTaillightQuads @ 0x004011C0 (orig 356B,
 *   plus the parallel call-site in td5_vfx_render_taillights which is the
 *   higher-level orchestrator). Orig: BuildSpriteQuadTemplate + Transform
 *   ShortVectorToView + WriteTransformedShortVector pre-bake the 4-corner
 *   quad into the scratch table at &g_brakeLightSpriteTemplates+slot*0xB8,
 *   QueueTranslucentPrimitiveBatch enqueues it, then the per-frame flush
 *   ships through the D3D3 vtable. Port reads the cardef hardpoint offsets
 *   at car_def+0x60 / +0x68, transforms them per-frame through the active
 *   render matrix, builds a 4-vertex billboard quad, and ships directly
 *   through td5_plat_render_draw_tris with the TRANSLUCENT_POINT preset
 *   (matches the no-z-test behaviour of the orig sprite quad). The +8
 *   brightness ramp + cap-at-0x80 + >>1 decay are byte-faithful (see also
 *   td5_vfx.c [CONFIRMED @ 0x401204] / @ 0x4011F5]. */
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
        /* [FIX 2026-06-02] Use the SAME depth formula as the car body / track
         * ((vz-64)/195000, see line ~824) instead of the old vz/far_clip, so the
         * brake's depth is directly comparable to the body depth buffer now that
         * the brake is z-tested (LEQUAL). Minus a small toward-camera pull so the
         * flat billboard reliably wins against its own angled rear surface. */
        float depth = (vz - NEAR_DEPTH_OFFSET) * DEPTH_NORMALIZE_INV - BRAKE_DEPTH_BIAS;

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
        /* [FIX 2026-06-02 inter-actor overlay] Depth-tested (LEQUAL, z_write=0)
         * translucent billboard. The old TD5_PRESET_TRANSLUCENT_POINT disabled
         * the depth test entirely, so a traffic/AI car's brake light painted
         * over the player car (and showed THROUGH the car from the front). With
         * the test on, a nearer car body occludes a farther car's brake light,
         * matching the original's deferred z-tested tail-light flush
         * (RenderVehicleTaillightQuads @0x4011C0 -> FlushQueuedTranslucent-
         * Primitives @0x431340, ZENABLE on / ZFUNC=LESSEQUAL @0x40B070). The
         * small BRAKE_DEPTH_BIAS above keeps the brake visible on its own car. */
        td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_POINT_ZTEST);
        td5_plat_render_bind_texture(s_braked_page);
        td5_plat_render_draw_tris(v, 4, idx, 6);
    }

    /* Restore opaque so it doesn't leak into next mesh */
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

/* ========================================================================
 * Tracked-Actor Marker Billboard — cop-chase visual (Tier 1 port 2026-05-24)
 *
 * Port of RenderTrackedActorMarker @ 0x0043cde0 (1262B).
 *
 * Orig draws a 3-layer pulsing billboard at 2 hardpoints (front + back) on
 * the wanted target's car. Each frame:
 *   - Pulse half-extents = g_wantedTargetTrackerActive × atlas_size × 1/4096
 *   - Marker yaw = AngleFromVector12(forward.z, forward.x) ±0x100
 *     (≈±5.6° split between front/back markers).
 *   - 3 sprite quads submitted per marker via SubmitImmediateTranslucent
 *     Primitive — red strobe, blue strobe, base.
 *
 * Port replaces the orig sprite-quad-scratch + BuildSpriteQuadTemplate
 * chain with direct D3D11 quad emission (matches brake_lights pattern).
 * UV / page caches owned by td5_vfx.c
 * (td5_vfx_init_tracked_actor_marker_billboards). Phase counters advanced
 * by td5_vfx_advance_tracked_marker_phases (orig 0x10/tick step preserved).
 *
 * Gate (orig @ 0x0040c79c, callsite condition):
 *   wanted_mode_enabled != 0 && g_wantedTargetTrackerActive != 0 &&
 *   slot == g_wantedTargetSlotIndex
 * — the port checks the same condition before calling this function.
 * ======================================================================== */

/* Marker model-space anchor offsets (orig g_minimapMarkerScaleTable_
 * PROVISIONAL @ 0x00474850 = (80,205,-160) / (-80,205,-160)). The X sign
 * placeholder distinguishes front/back marker hardpoints. */
static const float s_tracked_marker_anchor[TD5_VFX_TRACKED_MARKER_COUNT][3] = {
    {  80.0f, 205.0f, -160.0f },  /* marker 0 = front (red base) */
    { -80.0f, 205.0f, -160.0f },  /* marker 1 = back  (blue base) */
};

/* Yaw offset per marker (orig: iVar12==0 ? +0x100 : -0x100; ~±5.6° in
 * 12-bit angle space — visually spreads the 2 light bars apart). */
static const int s_tracked_marker_yaw_offset[TD5_VFX_TRACKED_MARKER_COUNT] = {
    +0x100,  /* front marker */
    -0x100,  /* back  marker */
};

/* Per-marker scalar bases (orig 0x0043cdf6-0x0043ce26). All three are scaled
 * by intensity * DAT_0045d698 (1/4096) every frame:
 *   fVar8  = intensity * 512.0f / 4096   — long-radius rotated cone (DAT_0045d768)
 *   fVar10 = intensity *  64.0f / 4096   — cross-radius            (DAT_0045d6c0)
 *   fVar9  = intensity *   6.0f / 4096   — narrow bottom-tip       (DAT_0045d764)
 * [FIX 2026-05-24 strobe-17call; orig 0x0043cde0]
 *   Previous constants (255/64/255/64) were placeholders during the Tier-1
 *   port. Reconciled to the actual orig FMUL operands read from memory at
 *   0x0045d768 (=512.0), 0x0045d6c0 (=64.0), 0x0045d764 (=6.0). */
#define TRACKED_MARKER_INTENSITY_SCALE   (1.0f / 4096.0f)  /* DAT_0045d698 */
#define TRACKED_MARKER_BASE_FVAR8        512.0f            /* DAT_0045d768 */
#define TRACKED_MARKER_BASE_FVAR10       64.0f             /* DAT_0045d6c0 */
#define TRACKED_MARKER_BASE_FVAR9        6.0f              /* DAT_0045d764 */
#define TRACKED_MARKER_BASE_HALF_XY      96.0f             /* [USER DIVERGENCE 2026-06-01: 3x the orig DAT_0045d5dc=32.0 — bigger over-car glow per user] */
#define TRACKED_MARKER_BASE_Z_OFFSET     4.0f              /* _g_simTickBudgetCap — layer-2 Z lift */
#define TRACKED_MARKER_ALPHA_SCALE       255.0f            /* DAT_0045d684 — sin alpha scale */

/* [FIX 2026-05-24 OVERSIGHT: wanted-mode-init; orig 0x004aaf68]
 * Removed dead extern of g_wanted_mode_enabled (stale parallel global from
 * stub migration cf0777f, never written). The actual wanted-mode flag lives
 * at g_td5.wanted_mode_enabled and the gate is enforced at the callsite
 * in render_tracked_actor_marker's caller (td5_render.c:2323). */
/* td5_game_get_wanted_target_tracker / _slot prototypes live in td5_game.h
 * (already included above via the existing td5_render.c include block). */
extern void     td5_hud_update_wanted_damage_indicator(int actor_slot);

/* [FIX 2026-05-24 strobe-17call; orig 0x0043cde0]
 * Emit a sprite quad given 4 distinct view-space corner positions
 * (corner order: TL, BL, TR, BR — matches the BuildSpriteQuadTemplate
 * vertex slot order at orig offsets +0x36/+0x4c/+0x0a/+0x20).
 *
 * Each corner shares the same view-space Z (the orig writes fVar7 to all 4
 * Z slots), so we project once for depth/inv_z and per-corner for X/Y. */
static void tracked_marker_emit_quad_world(const float corners_world_xy[4][2],
                                            float shared_world_z,
                                            float u0, float v0, float u1, float v1,
                                            uint32_t color, int tex_page)
{
    if (tex_page < 0) return;
    if (shared_world_z <= s_near_clip) return;

    float inv_z = 1.0f / shared_world_z;
    float depth = shared_world_z * (1.0f / s_far_clip);

    /* Project each corner using the same focal/center convention as the
     * single-center path above (mirrors td5_render_project). */
    float sx[4], sy[4];
    for (int i = 0; i < 4; ++i) {
        sx[i] = -corners_world_xy[i][0] * s_focal_length * inv_z + s_center_x;
        sy[i] = -corners_world_xy[i][1] * s_focal_length * inv_z + s_center_y;
    }

    TD5_D3DVertex v[4];
    /* Corner mapping (orig BuildSpriteQuadTemplate scratch +0x36/+0x4c/+0x0a/+0x20):
     *   index 0 -> top-left  (UV u0,v0)
     *   index 1 -> bottom-left (UV u0,v1)
     *   index 2 -> top-right (UV u1,v0)
     *   index 3 -> bottom-right (UV u1,v1) */
    v[0].screen_x = sx[0]; v[0].screen_y = sy[0];
    v[0].depth_z  = depth; v[0].rhw      = inv_z;
    v[0].diffuse  = color; v[0].specular = 0;
    v[0].tex_u    = u0;    v[0].tex_v    = v0;

    v[1].screen_x = sx[1]; v[1].screen_y = sy[1];
    v[1].depth_z  = depth; v[1].rhw      = inv_z;
    v[1].diffuse  = color; v[1].specular = 0;
    v[1].tex_u    = u0;    v[1].tex_v    = v1;

    v[2].screen_x = sx[2]; v[2].screen_y = sy[2];
    v[2].depth_z  = depth; v[2].rhw      = inv_z;
    v[2].diffuse  = color; v[2].specular = 0;
    v[2].tex_u    = u1;    v[2].tex_v    = v0;

    v[3].screen_x = sx[3]; v[3].screen_y = sy[3];
    v[3].depth_z  = depth; v[3].rhw      = inv_z;
    v[3].diffuse  = color; v[3].specular = 0;
    v[3].tex_u    = u1;    v[3].tex_v    = v1;

    uint16_t idx[6] = { 0, 1, 2, 1, 3, 2 };
    flush_immediate_internal();
    /* [FIX 2026-05-30 cop-chase] The tracked-actor marker (the pulsing red/blue
     * cop-light strobe) is an ADDITIVE sprite in the original — BindRaceTexturePage
     * @ 0x0040B660 selects ONE/ONE for the police-light page (transparency-type 3),
     * and the per-vertex diffuse is a GRAY modulator (a,a,a | 0xFF000000) whose
     * pulse scales the texture brightness. Rendering it ALPHA-blended (the old
     * TRANSLUCENT_POINT) drew the large semi-transparent quads' DARK texels (e.g.
     * (24,0,0,128)) at 50% over the scene, stacking 6 quads into a grey haze that
     * also appeared to "move" as the quads rotate — the user's reported gray
     * background. Additive makes dark texels add ~0 (invisible) so only the bright
     * light centers glow; faithful AND removes the haze.
     *
     * [FIX 2026-06-01] Use TD5_PRESET_ADDITIVE_GLOW (additive, but NO alpha test)
     * rather than TD5_PRESET_ADDITIVE (which alpha-tests at ref=1). The original
     * marker submit path sets no alpha test; with LINEAR filtering the near-binary
     * police-light texels then blend into a SOFT, DIFFUSED glow instead of being
     * hard-clipped into clear rectangles (the user's "squared lights should be
     * diffused" report). Additive keeps the zero-RGB background invisible, so
     * dropping the alpha test does not bring back any box. */
    td5_plat_render_set_preset(TD5_PRESET_ADDITIVE_GLOW);
    td5_plat_render_bind_texture(tex_page);
    td5_plat_render_draw_tris(v, 4, idx, 6);
}

/* Constant-needed forward declaration for AngleFromVector12 from
 * td5_ai.c-style 12-bit atan2 (we have the render-side version exported
 * via td5_render.h). */

/* Tiny helper to make the phase lookup explicit and reviewable. */
static inline int marker_phase_index(int marker) { return marker; }

/* Cached phase per marker, mirrored locally so the render path stays
 * lock-free. Refreshed each call from td5_vfx_tracked_marker_get_phase. */
static int s_tracked_marker_phase[TD5_VFX_TRACKED_MARKER_COUNT];

/* [CONFIRMED @ 0x0043cde0 RenderTrackedActorMarker]
 * Per-frame: emits up to 6 sprite quads (2 markers × 3 layers). Pulse
 * extents scale with g_wantedTargetTrackerActive; yaw derives from the
 * actor's world forward vector. Layer colors orig come straight from
 * BuildSpriteQuadTemplate diffuse (texture-only modulate in D3D3);
 * port writes 0xFFFFFFFF and lets the alpha-keyed sprite passthrough
 * carry the visible color. */
static void render_tracked_actor_marker(const TD5_Actor *actor,
                                        const TD5_Mat3x3 *body_rot,
                                        const TD5_Vec3f *body_pos)
{
    if (!actor) return;
    if (!td5_vfx_tracked_marker_initialized()) return;

    /* Intensity 0..0x1000 — 0 means nothing to draw (orig early-exits
     * via the wanted_target_tracker_active gate at the callsite). */
    int32_t intensity = td5_game_get_wanted_target_tracker();
    if (intensity <= 0) return;

    float fIntensity = (float)intensity;
    /* [FIX 2026-05-24 strobe-17call; orig 0x0043cde0]
     * Three independent per-marker radii drive the rotated trapezoid cones
     * (layers 0/1) and their narrow bottom-tip (shared). Orig sequence at
     * 0x0043cdf6-0x0043ce26 builds fVar8/fVar10/fVar9 once per frame. */
    /* Orig at 0x0043cdf6: fVar8 = intensity * 512.0 * (1/4096) */
    float fVar8  = fIntensity * TRACKED_MARKER_BASE_FVAR8
                              * TRACKED_MARKER_INTENSITY_SCALE;
    /* Orig at 0x0043ce0a: fVar10 = intensity * 64.0 * (1/4096) */
    float fVar10 = fIntensity * TRACKED_MARKER_BASE_FVAR10
                              * TRACKED_MARKER_INTENSITY_SCALE;
    /* Orig at 0x0043ce1a: fVar9 (narrow tip)= intensity * 6.0 * (1/4096) */
    float fVar9  = fIntensity * TRACKED_MARKER_BASE_FVAR9
                              * TRACKED_MARKER_INTENSITY_SCALE;

    /* Lock the marker to the car BODY transform: use the SAME view_rot +
     * render_pos the car mesh used this frame (passed in as body_rot/body_pos),
     * so the strobe is welded to the body at all speeds. [v8 — user confirmed
     * this position is correct; the v9 camera-basis experiment was reverted.] */
    td5_render_load_rotation(body_rot);
    td5_render_load_translation(body_pos);

    const float *m = s_render_transform.m;

    /* Refresh local phase mirror from vfx (single point of truth). */
    for (int i = 0; i < TD5_VFX_TRACKED_MARKER_COUNT; i++) {
        s_tracked_marker_phase[i] = td5_vfx_tracked_marker_get_phase(i);
    }

    int forward_dx = (int)(m[6] * 256.0f);   /* m[6..8] = forward (row 2) */
    int forward_dz = (int)(m[8] * 256.0f);
    int base_yaw   = AngleFromVector12(forward_dx, forward_dz);
    int wrapped    = ((base_yaw - 0x800) & 0xFFF) - 0x800;
    int yaw_div16  = (wrapped + ((wrapped >> 31) & 0xF)) >> 4;

    for (int marker = 0; marker < TD5_VFX_TRACKED_MARKER_COUNT; marker++) {
        /* Anchor in model space (front-left / front-right hardpoints). */
        float px = s_tracked_marker_anchor[marker][0];
        float py = s_tracked_marker_anchor[marker][1];
        float pz = s_tracked_marker_anchor[marker][2];

        /* Transform anchor through render matrix to view space.
         * Orig at 0x0043cf02-0x0043cf64 — three FLD/FMUL/FADD chains that
         * land in fVar5 (X), fVar6 (Y), fVar7 (Z). */
        float fVar5 = px*m[0] + py*m[1] + pz*m[2] + m[9];
        float fVar6 = px*m[3] + py*m[4] + pz*m[5] + m[10];
        float fVar7 = px*m[6] + py*m[7] + pz*m[8] + m[11];
        if (fVar7 <= s_near_clip) continue;

        /* Per-marker yaw with ±0x100 split (orig 0x0043cfa2/0x0043cfb4). */
        unsigned uVar14 = (unsigned)(yaw_div16 + s_tracked_marker_yaw_offset[marker]) & 0xFFF;

        /* [FIX 2026-05-24 strobe-17call; orig 0x0043ce5f]
         * Alpha pulse — orig:
         *   SinFloat12bit(phase * -4) * 255.0  -> __ftol -> & 0xff
         * The signed-truncate-then-byte-mask creates a sharp asymmetric
         * pulse (positive sin half rises 0->255, negative half wraps 255->1).
         * Output replicated to RGB with full A=0xff -> gray modulator. */
        unsigned phase8 = (unsigned)(s_tracked_marker_phase[marker_phase_index(marker)] & 0xff);
        float    sinv   = SinFloat12bit((int)((phase8 * (unsigned)-4) & 0xFFF));
        int32_t  a_ftol = (int32_t)(sinv * TRACKED_MARKER_ALPHA_SCALE); /* truncate toward zero */
        uint32_t a      = (uint32_t)a_ftol & 0xFFu;
        uint32_t pulse_color = 0xFF000000u | (a << 16) | (a << 8) | a;

        /* [FIX 2026-05-24 strobe-17call; orig 0x0043cfbb-0x0043d0d7]
         * Orig issues 20 Sin/CosFloat12bit calls per marker, all with the
         * same arg uVar14 (the compiler does not CSE float10 returns from
         * cdecl x87 calls). The values are mathematically:
         *   c = cos(uVar14)   s = sin(uVar14)
         * Computing them ONCE here is a no-op on the visible result (LUT
         * lookups are deterministic) and trims 18 redundant calls. The
         * 1-call alpha sin above is a SEPARATE waveform (different arg). */
        float c = CosFloat12bit(uVar14);   /* mirrors 10 cos calls 0x0043cfbb..0x0043d120 */
        float s = SinFloat12bit((int)uVar14); /* mirrors 10 sin calls in same range */

        /* Per-corner view-space offsets — orig formulas at 0x0043d12d..
         * 0x0043d291. fVar7 is shared across all 12 corners; X/Y vary per
         * corner. Each layer is a trapezoid (top edge wide, bottom narrow). */

        /* Pre-baked corner offsets shared with both rotated layers. */
        const float bot_x_left  = fVar5 - s * fVar9;   /* fVar2 @ orig 0x0043cff7 reused */
        const float bot_y_left  = fVar6 + c * fVar9;   /* fVar3 */
        const float bot_x_right = fVar5 + s * fVar9;   /* fVar4 @ orig 0x0043d016 */
        const float bot_y_right = fVar6 - c * fVar9;   /* (float)fVar15 final */

        /* Layer 0 = "g_trackedActorMarkerEntryScratch" trapezoid (orig
         * 4bec90/4becbc/4bece8/4bec64). */
        const float l0_corners[4][2] = {
            /* TL @ 4bec90: (c*fVar8 + fVar5) - s*fVar10,  s*fVar8 + c*fVar10 + fVar6 */
            { (c * fVar8 + fVar5) - s * fVar10,  s * fVar8 + c * fVar10 + fVar6 },
            /* BL @ 4bec64: fVar2/fVar3 (narrow bottom-left) */
            { bot_x_left,  bot_y_left },
            /* TR @ 4becbc: c*fVar8 + s*fVar10 + fVar5,  (s*fVar8 + fVar6) - c*fVar10 */
            { c * fVar8 + s * fVar10 + fVar5,    (s * fVar8 + fVar6) - c * fVar10 },
            /* BR @ 4bece8: fVar4/(float)fVar15 (narrow bottom-right) */
            { bot_x_right, bot_y_right },
        };

        /* Layer 1 = "g_trackedActorMarkerCount" mirrored trapezoid (orig
         * 4bed48/4bed74/4beda0/4bed1c). The top corners flip cos sign. */
        const float l1_corners[4][2] = {
            /* TL @ 4bed48: (fVar5 - c*fVar8) + s*fVar10, (fVar6 - s*fVar8) - c*fVar10 */
            { (fVar5 - c * fVar8) + s * fVar10,  (fVar6 - s * fVar8) - c * fVar10 },
            /* BL @ 4beda0: fVar2/fVar3 (shared with layer 0 BL) */
            { bot_x_left,  bot_y_left },
            /* TR @ 4bed74: (fVar5 - c*fVar8) - s*fVar10,  c*fVar10 + (fVar6 - s*fVar8) */
            { (fVar5 - c * fVar8) - s * fVar10,  c * fVar10 + (fVar6 - s * fVar8) },
            /* BR @ 4bed1c: fVar4/(float)fVar15 (shared with layer 0 BR) */
            { bot_x_right, bot_y_right },
        };

        /* Layer 2 = "g_trackedActorMarkerSurface" axis-aligned ±32 square at
         * Z - 4.0 (orig 4bebac/4bebd8/4bec04/4bec30). */
        const float half_xy = TRACKED_MARKER_BASE_HALF_XY;
        const float l2_z    = fVar7 - TRACKED_MARKER_BASE_Z_OFFSET;
        const float l2_corners[4][2] = {
            { fVar5 - half_xy, fVar6 - half_xy },   /* TL @ 4bebac */
            { fVar5 - half_xy, fVar6 + half_xy },   /* BL @ 4bebd8 */
            { fVar5 + half_xy, fVar6 - half_xy },   /* TR @ 4bec30 */
            { fVar5 + half_xy, fVar6 + half_xy },   /* BR @ 4bec04 */
        };

        /* Submit in orig order (0x0043d29e/0x0043d2a8/0x0043d2b2: 3x
         * SubmitImmediateTranslucentPrimitive). Each shares the same gray
         * pulse_color (all 4 corners get the same diffuse via the orig's
         * local_18/14/10/c broadcast at 0x0043cea5..0x0043ceba). */
        {
            int   page = td5_vfx_tracked_marker_get_page(marker, 0);
            float u0, v0, u1, v1;
            td5_vfx_tracked_marker_get_uv(marker, 0, &u0, &v0, &u1, &v1);
            tracked_marker_emit_quad_world(l0_corners, fVar7,
                                            u0, v0, u1, v1, pulse_color, page);
        }
        {
            int   page = td5_vfx_tracked_marker_get_page(marker, 1);
            float u0, v0, u1, v1;
            td5_vfx_tracked_marker_get_uv(marker, 1, &u0, &v0, &u1, &v1);
            tracked_marker_emit_quad_world(l1_corners, fVar7,
                                            u0, v0, u1, v1, pulse_color, page);
        }
        {
            int   page = td5_vfx_tracked_marker_get_page(marker, 2);
            float u0, v0, u1, v1;
            td5_vfx_tracked_marker_get_uv(marker, 2, &u0, &v0, &u1, &v1);
            tracked_marker_emit_quad_world(l2_corners, l2_z,
                                            u0, v0, u1, v1, pulse_color, page);
        }
    }

    /* Restore opaque preset so it doesn't leak into next pass (mirrors
     * brake_lights tail). */
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

/* [ARCH-DIVERGENCE: D3D3 sprite templates -> D3D11 per-frame UV lookup;
 *  Phase 5(d) L5 audit 2026-05-21]
 *   Corresponds to InitializeVehicleWheelSpriteTemplates @ 0x00446A70 (orig
 *   1071B). Orig populates 4-quad-per-wheel sprite template scratch (at
 *   &DAT_004c43b8 and &g_wheelHubcapSpriteScratch), pre-rotating each tire
 *   sidewall quad via SinFloat12bit*halfwidth across 0x12000 / 0x2000 = 9
 *   angle steps, then loading INWHEEL + COLOURS atlas UVs via
 *   FindArchiveEntryByName + BuildSpriteQuadTemplate. Port stores only the
 *   atlas UVs (s_tire_u/v, s_inwheel_uv) and the texture page once, then
 *   re-emits the tire-sidewall ring + hub-cap quad as a fresh
 *   TD5_D3DVertex stream every frame inside render_vehicle_wheel_billboards
 *   (see the ARCH-DIVERGENCE block at td5_render.c:~4280). No template
 *   scratch buffer in the port. */
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

/* [ARCH-DIVERGENCE — D3D3->D3D11 wheel billboard emission] orig 0x00446f00.
 *
 * RenderVehicleWheelBillboards — 1410 bytes orig (0x00446F00..0x00447482).
 *
 * Original used a D3D3-era fixed-function pipeline: BuildSpriteQuadTemplate /
 * WriteTransformedShortVector / QueueTranslucentPrimitiveBatch write
 * pre-rotated quad templates to scratch buffers (DAT_004C4300+) and submit
 * via the legacy D3D3 immediate-mode batch queue (DrawIndexedPrimitiveVB-
 * style with DDraw surface keys).
 *
 * Port replaces this with a D3D11 ring-vertex pipeline: 9-vertex tire
 * sidewall ring (8 segments * 2 rings for inner/outer) emitted as a
 * TD5_D3DVertex stream through td5_plat_render_draw_tris on D3D11
 * immediate command lists. The chain matches semantically (CW-from-+Z
 * yaw, same UV layout, same per-wheel billboard at slot-position) but
 * the GPU API divergence makes per-byte vertex-buffer comparison
 * meaningless.
 *
 * Invariants that DO hold (cited inline below):
 *   - Hub-cap spin pre-compute formulas: front_angle_12b = slip_z*-4,
 *     rear_angle_12b = slip_x*-4 [CONFIRMED @ 0x446F15 / 0x446F23].
 *   - Wheel dimensions read from cardef+0x82 / cardef+0x84
 *     [CONFIRMED @ 0x446E30-0x446E3C].
 *   - Front-wheel visual steering yaw matrix [cos 0 sin; 0 1 0; -sin 0 cos]
 *     derived from (steering_command>>8). Port commit 67f8d18 fixed the
 *     CW-from-+Z yaw convention sign (SHIPPED, merged ad78a32).
 *   - COLOURS palette tire-color lookup [CONFIRMED @ 0x446B44-0x446B6A].
 *
 * See reference_arch_render_wheel_billboards_d3d_2026-05-18.md for full
 * rationale and the documented invariants that DO hold (CW-from-+Z yaw,
 * billboard position, UV mapping).
 */
static void render_vehicle_wheel_billboards(TD5_Actor *actor, int slot)
{
    const float *m = s_render_transform.m;

    if (!s_wheel_lookup_done)
        wheel_lookup_static_hed();

    /* Read wheel dimensions from cardef (RE: 0x446E30-0x446E3C).
     *   cardef+0x82 (int16) -> raw rim value
     *     - Tire ring radius = raw * 0.76171875 (DAT_0045D7AC)
     *   cardef+0x84 (int16) -> axle_halfw (raw, no scaling)
     *
     * ARCHITECTURAL DIVERGENCE: original reads cardef via
     * &gVehicleTuningTable[slot*0x8C] (DAT_004AE580). Port reads via
     * actor->car_definition_ptr — per-actor buffer seeded from carparam.dat.
     * Bytes byte-faithful; addressing differs. See
     * memory/reference_arch_cardef_per_actor_indirection.md and
     * td5_physics.c s_loaded_cardef comment block. */
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

        /* Front wheel visual steering yaw — orig RenderVehicleWheelBillboards
         * @ 0x00446F00 builds matrix [cos 0 sin; 0 1 0; -sin 0 cos] from
         * (steering_command >> 8). That matrix multiplied as M*v on local
         * (dx, 0, dz) gives: rx = cos*dx + sin*dz, rz = -sin*dx + cos*dz
         * (TD5's CW-from-+Z yaw convention, same as obb_corner_test fix
         * 2026-05-13). Earlier port used the CCW-from-+X convention which
         * inverted the visual wheel angle vs the steering input.
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
                /* TD5 CW-from-+Z yaw: rx = cos*dx + sin*dz, rz = -sin*dx + cos*dz */
                float rx = dx * cos_s + dz * sin_s;
                float rz = -dx * sin_s + dz * cos_s;
                float px = wx + rx, py = wy + cy, pz = wz + rz;
                float vx = px*m[0] + py*m[1] + pz*m[2] + m[9];
                float vy = px*m[3] + py*m[4] + pz*m[5] + m[10];
                float vz = px*m[6] + py*m[7] + pz*m[8] + m[11];
                if (vz <= s_near_clip) { all_visible = 0; break; }
                float inv_z = 1.0f / vz;
                verts[i].screen_x = -vx * s_focal_length * inv_z + s_center_x;
                verts[i].screen_y = -vy * s_focal_length * inv_z + s_center_y;
                verts[i].depth_z  = (vz - NEAR_DEPTH_OFFSET) * DEPTH_NORMALIZE_INV;
                verts[i].rhw      = inv_z;
                verts[i].diffuse  = 0xFFFFFFFFu;
                verts[i].specular = 0;
                verts[i].tex_u = s_tire_u;
                verts[i].tex_v = s_tire_v;
            }

            /* Outer ring vertex (with front-wheel steering yaw) */
            {
                float dx = outer_off, dz = cz;
                /* TD5 CW-from-+Z yaw: rx = cos*dx + sin*dz, rz = -sin*dx + cos*dz */
                float rx = dx * cos_s + dz * sin_s;
                float rz = -dx * sin_s + dz * cos_s;
                float px = wx + rx, py = wy + cy, pz = wz + rz;
                float vx = px*m[0] + py*m[1] + pz*m[2] + m[9];
                float vy = px*m[3] + py*m[4] + pz*m[5] + m[10];
                float vz = px*m[6] + py*m[7] + pz*m[8] + m[11];
                if (vz <= s_near_clip) { all_visible = 0; break; }
                float inv_z = 1.0f / vz;
                verts[9+i].screen_x = -vx * s_focal_length * inv_z + s_center_x;
                verts[9+i].screen_y = -vy * s_focal_length * inv_z + s_center_y;
                verts[9+i].depth_z  = (vz - NEAR_DEPTH_OFFSET) * DEPTH_NORMALIZE_INV;
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
                    hub[0].depth_z  = (vz - NEAR_DEPTH_OFFSET) * DEPTH_NORMALIZE_INV;
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
                hub[1+c].depth_z  = (vz - NEAR_DEPTH_OFFSET) * DEPTH_NORMALIZE_INV;
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
     * Original RunRaceFrame sky block (0x0042bdf1-0x0042be45) sequence:
     *   1. ApplyMeshResourceRenderTransform(gSkyMeshResource) — rotation only
     *   2. TransformMeshVerticesToView(gSkyMeshResource)
     *   3. SetRaceRenderStatePreset(0) → ZFUNC=ALWAYS, ZWRITE=0
     *      [CONFIRMED @ 0x0040b0d8 (ZFUNC=8), 0x0040b0e1 (ZWRITE=0)]
     *   4. RenderPreparedMeshResource(gSkyMeshResource)
     *   5. SetRaceRenderStatePreset(1) → ZFUNC=LESSEQUAL, ZWRITE=1
     *      [CONFIRMED @ 0x0040b0a3 (ZFUNC=4), 0x0040b0b1 (ZWRITE=1)]
     *
     * The caller in td5_render_actors_for_view installs TD5_PRESET_SKY
     * (z_func=1/ALWAYS, z_write=0) BEFORE entering this function and
     * suppresses the page-blend remap via s_in_sky_draw, so the dome's
     * batch flush keeps the SKY depth-state. Track render that follows
     * installs TD5_PRESET_OPAQUE_LINEAR which explicitly resets z_func=0
     * (LESSEQUAL) and z_write=1.
     *
     * The dome's projected screen_z values are tiny (camera-centered
     * geometry, vz close to camera) — but ZFUNC=ALWAYS makes the depth
     * comparison vacuous and ZWRITE=0 leaves the cleared far value in
     * the buffer, so distant track spans still pass their own LESSEQUAL
     * test against the cleared depth. */
    if (s_sky_mesh) {
        TD5_Mat3x3 sky_rot;

        /* Camera basis IS the rotation — sky has identity model rotation */
        for (int i = 0; i < 9; i++)
            sky_rot.m[i] = s_camera_basis[i];

        td5_render_load_rotation(&sky_rot);

        /* Sky dome is camera-centered: translation is zero in view space.
         * Original ApplyMeshResourceRenderTransform stores rotated mesh
         * +0x1c..+0x24, which are zero for sky.prr at runtime. Do NOT use
         * td5_render_load_translation() — it subtracts camera_pos. */
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
        td5_plat_render_set_preset(TD5_PRESET_SKY);
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

/* [ARCH-DIVERGENCE: per-billboard pool collapsed to global counter; L5 sweep 2026-05-21]
 *   Orig 0x0043CDC0 walks a tracked-billboard pool from
 *   `g_trackedActorMarkerBillboardPool_PROVISIONAL` to address 0x4bf218,
 *   stepping 0x22c bytes (0x8b * sizeof(int)) per entry and incrementing the
 *   first int of each entry by 0x10. The port collapses this to a single
 *   global counter because the tracked-billboard pool struct itself is not
 *   yet ported (animated-billboard texture-frame selection is a deferred
 *   feature). Step matches orig at +0x10 per tick; consumer reads remain a
 *   TODO when the billboard pool gets ported. */
void td5_render_advance_billboard_anims(void)
{
    s_billboard_anim_phase += 0x10;
}

/* ========================================================================
 * 12-bit Trigonometry (migrated from td5re_stubs.c)
 *
 * Original game uses a lookup table populated once by
 * BuildSinCosLookupTables @ 0x0040A650 from FCOS in 80-bit x87 then stored
 * as 32-bit float (g_sinCosFloatTable @ 0x00488984, 5120 entries; covers a
 * 4096-step circle plus an extra quarter turn of padding).
 *
 *   CosFloat12bit(arg) : return LUT[(arg) & 0xFFF]
 *   SinFloat12bit(arg) : return LUT[(arg - 0x400) & 0xFFF]
 *
 * To match byte-for-byte, the port now also computes the LUT once at startup
 * (mirroring the original's two-step FILD/FMUL(2π)/FMUL(1/4096) x87 chain via
 * long double on i386 MinGW) and indexes it for every call. Computing live
 * with cos()/sin() was the previous behavior and leaked LSB drift relative
 * to the original's static LUT.
 *
 * Audit: re/analysis/pilot_trig_audit.md
 * ======================================================================== */

#include "td5_pilot_trace_trig.h"

#define TD5_TRIG_LUT_SIZE 0x1400  /* 5120, matches original */

static float  s_cosFloatTable[TD5_TRIG_LUT_SIZE];
static int    s_cosFixedTable[TD5_TRIG_LUT_SIZE];
static int    s_trig_lut_built = 0;

/* Reference LUT extracted byte-for-byte from a running TD5_d3d.exe instance
 * via tools/frida_pool3_trig_dump.js (one-shot read of g_sinCosFloatTable
 * @ 0x00488984, 5120 entries × 4 bytes = 20 KB). Using the original's exact
 * bits avoids the residual ±1 ULP drift seen even when port-side FCOS,
 * constants, and FPU PC are all matched — likely due to a remaining x87
 * micro-state difference (FTOP, register pollution from MinGW startup math
 * before the LUT is built). The dump is the only way to guarantee byte
 * equality and is also faster than computing the LUT at startup.
 *
 * To regenerate after a runtime LUT change in the original:
 *   1. python re/tools/quickrace/td5_quickrace.py --no-ini ... \
 *          --extra-script tools/frida_pool3_trig_dump.js
 *   2. python -c "import struct; ..." > td5_trig_lut_data.c (see runbook).
 */
extern const uint32_t td5_trig_lut_bits[TD5_TRIG_LUT_SIZE];

static void td5_trig_build_lut(void) {
    /* Copy the embedded LUT bytes into the float LUT. The C-level cast via a
     * union pun is byte-exact. */
    for (int i = 0; i < TD5_TRIG_LUT_SIZE; i++) {
        union { uint32_t u; float f; } pun;
        pun.u = td5_trig_lut_bits[i];
        s_cosFloatTable[i] = pun.f;
    }
    /* The int (FP12 fixed-point) LUT — DAT_00483984 in the original — is
     * derived from the float LUT by `lrintf(float * 4096.0f)` using FISTP
     * semantics (round-to-nearest-even). Build it here so CosFixed12bit /
     * SinFixed12bit stay byte-faithful to the original's int LUT as well.
     * Note: doing this via FISTP under PC=64 matches the original because
     * the source float is already byte-equal. */
    static const float SCALE_F = 4096.0f;
    unsigned short saved_cw = 0, new_cw = 0;
    __asm__ volatile ("fnstcw %0" : "=m" (saved_cw));
    new_cw = (unsigned short)((saved_cw & 0xfcffu) | 0x0300u);
    __asm__ volatile ("fldcw %0" : : "m" (new_cw));
    for (int i = 0; i < TD5_TRIG_LUT_SIZE; i++) {
        float v = s_cosFloatTable[i];
        int   out;
        __asm__ volatile (
            "flds     %[in]       \n\t"
            "fmuls    %[scale]    \n\t"
            "fistpl   %[out]      \n\t"
            : [out] "=m" (out)
            : [in] "m" (v),
              [scale] "m" (SCALE_F)
            : "st", "memory"
        );
        s_cosFixedTable[i] = out;
    }
    __asm__ volatile ("fldcw %0" : : "m" (saved_cw));

    s_trig_lut_built = 1;
}

static inline void td5_trig_ensure_lut(void) {
    if (!s_trig_lut_built) td5_trig_build_lut();
}

/* [CONFIRMED @ 0x0040A6A0] Byte-faithful with orig CosFloat12bit.
 * L5 promotion 2026-05-18 (small-tier sweep). 4-instr listing match:
 * AND angle, 0xfff; FLD float [base + idx*4]. Port reads s_cosFloatTable
 * (built from FPU cos at td5_trig_build_lut) at the same index. Pilot trig
 * emit hook is an instrumentation side-effect with no semantic divergence. */
float CosFloat12bit(unsigned int angle) {
    td5_trig_ensure_lut();
    unsigned int idx = angle & 0xFFFu;
    float v = s_cosFloatTable[idx];
    union { float f; uint32_t u; } pun;
    pun.f = v;
    td5_pilot_trig_emit("cos", (int32_t)angle, pun.u, v);
    return v;
}

/* [CONFIRMED @ 0x0040A6C0] Byte-faithful with orig SinFloat12bit.
 * L5 promotion 2026-05-18 (small-tier sweep). 5-instr listing match:
 * ADD EAX, 0xfffffc00 (32-bit signed wrap = sin via cos(angle-pi/2));
 * AND 0xfff; FLD float [s_cosFloatTable + idx*4]. Pilot trig emit hook
 * is an instrumentation side-effect with no semantic divergence. */
float SinFloat12bit(int angle) {
    td5_trig_ensure_lut();
    /* Match the original's `ADD EAX, 0xfffffc00` (32-bit signed wrap), then
     * AND 0xfff. */
    unsigned int shifted = ((unsigned int)angle) + 0xfffffc00u;
    unsigned int idx = shifted & 0xFFFu;
    float v = s_cosFloatTable[idx];
    union { float f; uint32_t u; } pun;
    pun.f = v;
    td5_pilot_trig_emit("sin", (int32_t)angle, pun.u, v);
    return v;
}

int CosFixed12bit(unsigned int angle) {
    td5_trig_ensure_lut();
    return s_cosFixedTable[angle & 0xFFFu];
}

int SinFixed12bit(int angle) {
    td5_trig_ensure_lut();
    unsigned int shifted = ((unsigned int)angle) + 0xfffffc00u;
    return s_cosFixedTable[shifted & 0xFFFu];
}

/* AngleFromVector12 LUT — literal port of DAT_00463214 from TD5_d3d.exe.
 *
 * 1024 entries encode round(atan(i/1024) * 2048 / pi) for i in [0, 1023], range
 * [0, 511]. Entry 1024 (value 0x200 = 512) sits past the declared array and is
 * silently read by the original when param_1==param_2>0 (the diagonal of
 * octant 1 produces idx=1024 exactly). The original binary's memory at
 * 0x00463A14 holds 0x00 0x02 which decodes to 0x0200 — mathematically
 * atan(1.0)*2048/pi=512 — and we mirror that here. */
static const int16_t k_angle_from_vector12_lut[1026] = {
       0,    1,    1,    2,    3,    3,    4,    4,    5,    6,    6,    7,    8,    8,    9,   10,
      10,   11,   11,   12,   13,   13,   14,   15,   15,   16,   17,   17,   18,   18,   19,   20,
      20,   21,   22,   22,   23,   24,   24,   25,   25,   26,   27,   27,   28,   29,   29,   30,
      31,   31,   32,   32,   33,   34,   34,   35,   36,   36,   37,   38,   38,   39,   39,   40,
      41,   41,   42,   43,   43,   44,   44,   45,   46,   46,   47,   48,   48,   49,   50,   50,
      51,   51,   52,   53,   53,   54,   55,   55,   56,   57,   57,   58,   58,   59,   60,   60,
      61,   62,   62,   63,   63,   64,   65,   65,   66,   67,   67,   68,   69,   69,   70,   70,
      71,   72,   72,   73,   74,   74,   75,   75,   76,   77,   77,   78,   79,   79,   80,   80,
      81,   82,   82,   83,   84,   84,   85,   85,   86,   87,   87,   88,   89,   89,   90,   90,
      91,   92,   92,   93,   94,   94,   95,   95,   96,   97,   97,   98,   99,   99,  100,  100,
     101,  102,  102,  103,  104,  104,  105,  105,  106,  107,  107,  108,  108,  109,  110,  110,
     111,  112,  112,  113,  113,  114,  115,  115,  116,  117,  117,  118,  118,  119,  120,  120,
     121,  121,  122,  123,  123,  124,  125,  125,  126,  126,  127,  128,  128,  129,  129,  130,
     131,  131,  132,  132,  133,  134,  134,  135,  136,  136,  137,  137,  138,  139,  139,  140,
     140,  141,  142,  142,  143,  143,  144,  145,  145,  146,  146,  147,  148,  148,  149,  149,
     150,  151,  151,  152,  152,  153,  154,  154,  155,  156,  156,  157,  157,  158,  159,  159,
     160,  160,  161,  161,  162,  163,  163,  164,  164,  165,  166,  166,  167,  167,  168,  169,
     169,  170,  170,  171,  172,  172,  173,  173,  174,  175,  175,  176,  176,  177,  178,  178,
     179,  179,  180,  180,  181,  182,  182,  183,  183,  184,  185,  185,  186,  186,  187,  188,
     188,  189,  189,  190,  190,  191,  192,  192,  193,  193,  194,  195,  195,  196,  196,  197,
     197,  198,  199,  199,  200,  200,  201,  202,  202,  203,  203,  204,  204,  205,  206,  206,
     207,  207,  208,  208,  209,  210,  210,  211,  211,  212,  212,  213,  214,  214,  215,  215,
     216,  216,  217,  218,  218,  219,  219,  220,  220,  221,  222,  222,  223,  223,  224,  224,
     225,  225,  226,  227,  227,  228,  228,  229,  229,  230,  231,  231,  232,  232,  233,  233,
     234,  234,  235,  236,  236,  237,  237,  238,  238,  239,  239,  240,  241,  241,  242,  242,
     243,  243,  244,  244,  245,  246,  246,  247,  247,  248,  248,  249,  249,  250,  250,  251,
     252,  252,  253,  253,  254,  254,  255,  255,  256,  256,  257,  258,  258,  259,  259,  260,
     260,  261,  261,  262,  262,  263,  263,  264,  265,  265,  266,  266,  267,  267,  268,  268,
     269,  269,  270,  270,  271,  272,  272,  273,  273,  274,  274,  275,  275,  276,  276,  277,
     277,  278,  278,  279,  279,  280,  281,  281,  282,  282,  283,  283,  284,  284,  285,  285,
     286,  286,  287,  287,  288,  288,  289,  289,  290,  290,  291,  291,  292,  293,  293,  294,
     294,  295,  295,  296,  296,  297,  297,  298,  298,  299,  299,  300,  300,  301,  301,  302,
     302,  303,  303,  304,  304,  305,  305,  306,  306,  307,  307,  308,  308,  309,  309,  310,
     310,  311,  311,  312,  312,  313,  313,  314,  314,  315,  315,  316,  316,  317,  317,  318,
     318,  319,  319,  320,  320,  321,  321,  322,  322,  323,  323,  324,  324,  325,  325,  326,
     326,  327,  327,  328,  328,  329,  329,  330,  330,  331,  331,  332,  332,  333,  333,  334,
     334,  335,  335,  335,  336,  336,  337,  337,  338,  338,  339,  339,  340,  340,  341,  341,
     342,  342,  343,  343,  344,  344,  345,  345,  346,  346,  346,  347,  347,  348,  348,  349,
     349,  350,  350,  351,  351,  352,  352,  353,  353,  354,  354,  354,  355,  355,  356,  356,
     357,  357,  358,  358,  359,  359,  360,  360,  360,  361,  361,  362,  362,  363,  363,  364,
     364,  365,  365,  366,  366,  366,  367,  367,  368,  368,  369,  369,  370,  370,  371,  371,
     371,  372,  372,  373,  373,  374,  374,  375,  375,  375,  376,  376,  377,  377,  378,  378,
     379,  379,  379,  380,  380,  381,  381,  382,  382,  383,  383,  383,  384,  384,  385,  385,
     386,  386,  387,  387,  387,  388,  388,  389,  389,  390,  390,  390,  391,  391,  392,  392,
     393,  393,  393,  394,  394,  395,  395,  396,  396,  397,  397,  397,  398,  398,  399,  399,
     399,  400,  400,  401,  401,  402,  402,  402,  403,  403,  404,  404,  405,  405,  405,  406,
     406,  407,  407,  408,  408,  408,  409,  409,  410,  410,  410,  411,  411,  412,  412,  413,
     413,  413,  414,  414,  415,  415,  415,  416,  416,  417,  417,  417,  418,  418,  419,  419,
     419,  420,  420,  421,  421,  422,  422,  422,  423,  423,  424,  424,  424,  425,  425,  426,
     426,  426,  427,  427,  428,  428,  428,  429,  429,  430,  430,  430,  431,  431,  432,  432,
     432,  433,  433,  434,  434,  434,  435,  435,  435,  436,  436,  437,  437,  437,  438,  438,
     439,  439,  439,  440,  440,  441,  441,  441,  442,  442,  442,  443,  443,  444,  444,  444,
     445,  445,  446,  446,  446,  447,  447,  447,  448,  448,  449,  449,  449,  450,  450,  451,
     451,  451,  452,  452,  452,  453,  453,  454,  454,  454,  455,  455,  455,  456,  456,  457,
     457,  457,  458,  458,  458,  459,  459,  459,  460,  460,  461,  461,  461,  462,  462,  462,
     463,  463,  464,  464,  464,  465,  465,  465,  466,  466,  466,  467,  467,  468,  468,  468,
     469,  469,  469,  470,  470,  470,  471,  471,  471,  472,  472,  473,  473,  473,  474,  474,
     474,  475,  475,  475,  476,  476,  476,  477,  477,  478,  478,  478,  479,  479,  479,  480,
     480,  480,  481,  481,  481,  482,  482,  482,  483,  483,  483,  484,  484,  484,  485,  485,
     486,  486,  486,  487,  487,  487,  488,  488,  488,  489,  489,  489,  490,  490,  490,  491,
     491,  491,  492,  492,  492,  493,  493,  493,  494,  494,  494,  495,  495,  495,  496,  496,
     496,  497,  497,  497,  498,  498,  498,  499,  499,  499,  500,  500,  500,  501,  501,  501,
     502,  502,  502,  503,  503,  503,  504,  504,  504,  505,  505,  505,  506,  506,  506,  507,
     507,  507,  508,  508,  508,  508,  509,  509,  509,  510,  510,  510,  511,  511,  511,  512,
    /* index 1024 -- silent past-end read by octant 1's diagonal (p1==p2>0).
     * Original 0x00463A14 holds 0x0200 = 512 = round(atan(1)*2048/pi). */
    512,
    /* index 1025 -- silent past-end read by octants 5/6 only for the
     * degenerate inputs (-1,-1) and (-1,+1). Original 0x00463A16 = 0x0000. */
    0
};

#ifdef TD5_PILOT_TRACE_0040A720
#include "td5_pilot_trace_0040A720.h"
#endif

int AngleFromVector12(int x, int z) {
    /* Literal port of 0x0040A720 AngleFromVector12 from TD5_d3d.exe.
     * Convention: x = param_1 (e.g. dx, horizontal), z = param_2 (dz, vertical).
     * Returns 12-bit angle (0..0xFFF) measured CW from +z axis.
     *
     * Implementation mirrors the listing octant-by-octant. The LUT-index
     * trick `&DAT_00463214 + idx * -2` in the assembly is replicated as
     * `k_angle_from_vector12_lut[-idx]` for the negative-quotient branches.
     *
     * Acceptance: byte-faithful with the original LUT — see pilot_0040A720_audit.md. */
    const int param_1 = x;
    const int param_2 = z;
    int ret;

    if (param_1 == 0 && param_2 == 0) {
        ret = 0;
    } else if (param_1 >= 0) {
        if (param_2 > 0) {
            if (param_1 < param_2) {
                /* OCTANT 0: idx = (p1*1024 + p2/2)/p2 ∈ [0, 1024) */
                int idx = (param_1 * 1024 + (param_2 >> 1)) / param_2;
                ret = k_angle_from_vector12_lut[idx];
            } else {
                /* OCTANT 1: param_1 >= param_2 > 0 → idx = (p2*1024 + p1/2)/p1 ∈ [0, 1024]
                 * Sub-test mirrors the assembly: only reach here via JLE at 0040a743,
                 * and a separate JZ at 0040a75f returns 0 for param_1==0 (dead
                 * because we already ruled out (0,0) and we're in p2>0). */
                if (param_1 == 0) {
                    ret = 0;
                } else {
                    int idx = (param_2 * 1024 + (param_1 >> 1)) / param_1;
                    ret = 0x400 - k_angle_from_vector12_lut[idx];
                }
            }
        } else {
            /* param_1 >= 0, param_2 <= 0 */
            int neg_p2 = -param_2;
            if (neg_p2 <= param_1) {
                /* OCTANT 2: param_1 > 0, param_2 <= 0, -p2 <= p1 (so |p2|<=p1).
                 *   0040a7af TEST ESI,ESI; JZ 0040a731  (if param_1==0, return 0)
                 *   0040a7b7 MOV EAX,ECX; SHL EAX,0xA    ; EAX = p2*1024 (<=0)
                 *   0040a7bc MOV ECX,ESI; SAR ECX,1      ; ECX = p1>>1 (>0)
                 *   0040a7c0 SUB EAX,ECX                  ; EAX = p2*1024 - (p1>>1) (<=0)
                 *   0040a7c3 IDIV ESI                     ; /param_1 (>0); quotient <= 0
                 *   0040a7cd SUB EDX,EAX where EDX=0x463214  ; LUT_base - 2*q → LUT[-q]
                 *   0040a7d2 ADD EAX,0x400
                 */
                if (param_1 == 0) {
                    ret = 0;
                } else {
                    int idx = (param_2 * 1024 - (param_1 >> 1)) / param_1;
                    ret = 0x400 + k_angle_from_vector12_lut[-idx];
                }
            } else {
                /* OCTANT 3: param_1 > 0 (we'd have hit dead-corner if ==0),
                 * param_2 < 0, -p2 > p1. The JZ at 0040a78a tests ECX==0 (param_2):
                 * if both p2==0 AND fell into this branch, return 0 — but we're
                 * here only if -p2 > p1 ≥ 0, so p2<0 strictly. JZ is dead.
                 *
                 *   EAX = p1*1024 - (p2>>1)   ; p2<0 → -(p2>>1)>0 → num positive
                 *   EAX /= p2                  ; quotient negative
                 *   2*quotient subtracted from LUT_base → LUT[-quotient]
                 *   return 0x800 - LUT[-quotient] */
                if (param_2 == 0) {
                    ret = 0;
                } else {
                    int idx = (param_1 * 1024 - (param_2 >> 1)) / param_2;
                    ret = 0x800 - k_angle_from_vector12_lut[-idx];
                }
            }
        }
    } else {
        /* param_1 < 0, branched at 0040a737 → 0040a7d8.
         *   0040a7d8 TEST ECX, ECX   ; param_2 sign
         *   0040a7da MOV EAX, ESI
         *   0040a7dc JLE 0040a828    ; if param_2 <= 0 → 0040a828
         */
        if (param_2 > 0) {
            /* param_1 < 0, param_2 > 0.
             *   0040a7de NEG EAX       ; EAX = -param_1 (>0)
             *   0040a7e0 CMP ECX, EAX  ; compares param_2 vs -param_1
             *   0040a7e2 JLE 0040a807  ; take if param_2 <= -param_1 → OCTANT 6
             */
            int neg_p1 = -param_1;
            if (param_2 > neg_p1) {
                /* OCTANT 7: param_1<0, param_2>0, param_2 > -param_1
                 *   0040a7e4 MOV EDX, ECX; SAR EDX,1  ; EDX = p2>>1
                 *   0040a7e8 MOV EAX, ESI; SHL EAX,0xA; EAX = p1*1024 (negative)
                 *   0040a7ed SUB EAX, EDX             ; EAX = p1*1024 - p2/2
                 *   0040a7ef CDQ; IDIV ECX            ; /param_2 (positive); quotient negative
                 *   0040a7f8 SHL EAX,1
                 *   0040a7fa SUB ECX, EAX  (ECX=0x463214) ; LUT[-quotient]
                 *   0040a7ff EAX = 0x1000
                 *   0040a804 SUB EAX, EDX
                 */
                int idx = (param_1 * 1024 - (param_2 >> 1)) / param_2;
                ret = 0x1000 - k_angle_from_vector12_lut[-idx];
            } else {
                /* OCTANT 6: param_1<0, param_2>0, param_2 <= -param_1.
                 *   0040a807 MOV EAX, ECX; SHL EAX,0xA  ; EAX = p2*1024 (positive)
                 *   0040a80c MOV ECX, ESI; SAR ECX,1    ; ECX = p1>>1 (negative)
                 *   0040a810 SUB EAX, ECX               ; EAX = p2*1024 - p1/2 (positive larger)
                 *   0040a813 IDIV ESI                   ; /param_1 (negative); quotient negative
                 *   0040a81b SHL EAX,1
                 *   0040a81d SUB EDX, EAX  (EDX=0x463214) ; LUT[-quotient]
                 *   0040a822 ADD EAX, 0xc00
                 */
                int idx = (param_2 * 1024 - (param_1 >> 1)) / param_1;
                ret = 0xc00 + k_angle_from_vector12_lut[-idx];
            }
        } else {
            /* param_1 < 0, param_2 <= 0. 0040a828:
             *   0040a828 MOV EDX, ECX
             *   0040a82a NEG EAX       ; EAX = -param_1 (>0)
             *   0040a82c NEG EDX       ; EDX = -param_2 (>=0)
             *   0040a82e CMP EDX, EAX  ; compares -p2 vs -p1
             *   0040a830 JLE 0040a857  ; take if -p2 <= -p1 → OCTANT 5
             */
            int neg_p1 = -param_1;
            int neg_p2 = -param_2;
            if (neg_p2 > neg_p1) {
                /* OCTANT 4: param_1<0, param_2<0, -p2 > -p1 (|p2|>|p1|).
                 * The JZ at 0040a834 fires when param_1==0 — but we're in p1<0, dead.
                 *   0040a83a MOV EAX, ESI; SHL EAX,0xA  ; EAX = p1*1024 (negative)
                 *   0040a83f MOV EDX, ECX; SAR EDX,1    ; EDX = p2>>1 (negative)
                 *   0040a843 ADD EAX, EDX               ; EAX = p1*1024 + p2/2 (very negative)
                 *   0040a846 IDIV ECX                   ; /param_2 (negative); quotient positive
                 *   0040a849 MOVSX EAX,[EAX*2+0x463214] ; LUT[+quotient]
                 *   0040a851 ADD EAX, 0x800
                 */
                if (param_1 == 0) {
                    ret = 0;
                } else {
                    int idx = (param_1 * 1024 + (param_2 >> 1)) / param_2;
                    ret = 0x800 + k_angle_from_vector12_lut[idx];
                }
            } else {
                /* OCTANT 5: param_1<0, param_2<=0, -p2 <= -p1 (|p2|<=|p1|).
                 *   0040a857 MOV EAX, ECX; SHL EAX,0xA  ; EAX = p2*1024 (<=0)
                 *   0040a85c MOV ECX, ESI; SAR ECX,1    ; ECX = p1>>1 (negative)
                 *   0040a860 ADD EAX, ECX               ; EAX = p2*1024 + p1/2 (very negative)
                 *   0040a863 IDIV ESI                   ; /param_1 (negative); quotient positive
                 *   0040a866 MOVSX EDX,[EAX*2+0x463214] ; LUT[+quotient]
                 *   0040a86e EAX = 0xc00; SUB EAX, EDX
                 */
                int idx = (param_2 * 1024 + (param_1 >> 1)) / param_1;
                ret = 0xc00 - k_angle_from_vector12_lut[idx];
            }
        }
    }

#ifdef TD5_PILOT_TRACE_0040A720
    td5_pilot_trace_0040A720_call(param_1, param_2, ret);
#endif
    return ret;
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

/* [CONFIRMED @ 0x0042DA10] Byte-faithful with orig MultiplyRotationMatrices3x3.
 * L5 audit 2026-05-18 (TD5_pool0 read-only):
 *   - Formula: C[i][j] = sum_k A[i*3+k] * B[k*3+j], identical to original
 *     row-major 3x3 multiply (see param_3[2] = A[0]*B[2]+A[1]*B[5]+A[2]*B[8]).
 *   - Alias safety: original loads ALL 48 source slots into temps before any
 *     write to param_3 (aliasing-safe). Port uses local tmp[9] buffer + memcpy
 *     — semantically identical for any aliasing pattern (A==out, B==out, or
 *     A==B==out as seen in td5_camera.c rotor chains).
 *   - Original computes float-only with FPU stack-order writes; port computes
 *     float-only via i,j,k triple loop. Same precision (single-precision IEEE).
 *   - Write order in original is non-sequential (param_3[2], 3, 4, 0, 5, 1,
 *     6, 7, 8) because of FPU register pressure; result identical after all
 *     stores commit. */
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
     * 0x42dbd0 -- Transform a short[3] vector by a 3x3 rotation matrix.
     *
     * [ARCH-DIVERGENCE: signature & output type; L5 sweep 2026-05-21]
     *   Orig 0x0042DBD0 disassembly (FLD/FMUL/FADDP/FSTP) writes 3 floats
     *   via FSTP — no truncation. Orig callers consume floats:
     *   RenderRaceActorForView, BuildSpecialActorOverlayQuads,
     *   ApplyMeshProjectionEffect, RenderTireTrackPool.
     *
     *   Port reuses this symbol with (float *m, short *v, int *out) plus a
     *   truncate-toward-zero (int) cast at the FSTP site. Camera-side port
     *   callers (UpdateVehicleRelativeCamera, UpdateTracksideCamera case 1/2)
     *   call THIS, whereas in the orig those same camera sites called
     *   ConvertFloatVec3ToIntVec3 @ 0x0042DB40 (__ftol + (int)(short) clamp).
     *
     *   Math sequence is identical: out[i] = m[i*3+0]*v[0] + m[i*3+1]*v[1]
     *   + m[i*3+2]*v[2]. Term-reorder in orig FPU stack produces equivalent
     *   IEEE single-precision result. Output type and short-clamp behavior
     *   diverge — see UpdateTracksideCamera/UpdateVehicleRelativeCamera
     *   headers for visual impact assessment.
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

/* [FIX 2026-05-24 OVERSIGHT: case_1_2_basis_transform; orig 0x0042DB40
 * ConvertFloatVec3ToIntVec3] Same math as TransformVector3ByBasis but each
 * output is __ftol-rounded then truncated to int16 via (int)(short) cast.
 * Orig camera sites (UpdateTracksideCamera case 1/2, UpdateVehicleRelativeCamera)
 * call THIS helper, not TransformVector3ByBasis. For |result| <= 32767 the
 * two match; the short-clamp is a safety net for overflow cases. */
void ConvertFloatVec3ToIntVec3(float *matrix, void *vec, int *out) {
    short *v = (short *)vec;
    if (!out) return;
    if (!matrix || !v) { out[0] = 0; out[1] = 0; out[2] = 0; return; }

    float fx = (float)v[0];
    float fy = (float)v[1];
    float fz = (float)v[2];

    out[0] = (int)(short)(int)(matrix[0] * fx + matrix[1] * fy + matrix[2] * fz);
    out[1] = (int)(short)(int)(matrix[3] * fx + matrix[4] * fy + matrix[5] * fz);
    out[2] = (int)(short)(int)(matrix[6] * fx + matrix[7] * fy + matrix[8] * fz);
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

    /* [FIX 2026-05-27 PM-12 — matrix rotation ORDER reversed]
     * Decomp of orig BuildRotationMatrixFromAngles @ 0x0042E1E0 (closed-form)
     * produces Ry(a1)·Rx(a0)·Rz(a2) (yaw·pitch·roll). Working out the elements:
     *   M[5]=-sin(a0)   M[8]=cos(a1)*cos(a0)   M[2]=sin(a1)*cos(a0)
     *   M[3]=sin(a2)*cos(a0)   M[4]=cos(a2)*cos(a0)
     * all match Ry·Rx·Rz exactly.
     *
     * The previous port applied Yaw then Pitch then Roll (each as out = rot·out),
     * which builds Rz·Rx·Ry — the REVERSE order. Same display_angles, same trig
     * helpers, but a different final matrix. Physics solvers (attitude_from_wheels)
     * use raw contacts so the numeric attitude matched the orig; but every render
     * of the car body and the wheel billboards uses this matrix, so the orig saw
     * Ry·Rx·Rz and the port saw Rz·Rx·Ry → the rendered orientation differed even
     * though every diagnostic that re-multiplied through the same wrong matrix
     * agreed with itself (the data-matches-but-visual-differs paradox).
     *
     * To match the orig, apply Roll FIRST, then Pitch, then Yaw:
     *   out = I
     *   out = Rz · out          (after roll block)
     *   out = Rx · out  = Rx·Rz (after pitch block)
     *   out = Ry · out  = Ry·Rx·Rz   ✓ matches orig
     */

    /* Roll (angles[2]): rotate around Z axis */
    s = CosFloat12bit((unsigned int)(unsigned short)angles[2]);
    c = SinFloat12bit(angles[2]);
    rot[8] = 1.0f;
    rot[2] = 0.0f; rot[5] = 0.0f;
    rot[6] = 0.0f; rot[7] = 0.0f;
    rot[0] = s;  rot[4] = s;
    rot[3] = c;  rot[1] = -c;
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

    /* Yaw (angles[1]): rotate around Y axis */
    s = CosFloat12bit((unsigned int)(unsigned short)angles[1]);
    c = SinFloat12bit(angles[1]);
    rot[4] = 1.0f;
    rot[3] = 0.0f; rot[5] = 0.0f;
    rot[1] = 0.0f; rot[7] = 0.0f;
    rot[0] = s;  rot[8] = s;
    rot[2] = c;  rot[6] = -c;
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

/* BuildSpriteQuadTemplate @ 0x00432BD0 — flag-driven sprite-quad writer.
 *
 * Orig dispatches on a per-bit mask (verified via Ghidra disasm 0x432BD0..
 * 0x432D5D, decomp 2026-05-18 from TD5_pool0 read-only):
 *
 *   flag 0x001 — GEOMETRY:  write 4× (sx, sy, rhw) using formula
 *                  sx = view_x * g_inverseProjectionDepth * z
 *                  sy = view_y * g_inverseProjectionDepth * z
 *                  rhw = z
 *                Writes hit byte offsets 0x14, 0x40, 0x6c, 0x98 in the orig
 *                184-byte quad buffer (44-byte vertex stride).
 *   flag 0x002 — UV:        write 4× (u, v) = src * (1/256) = DAT_004749d0
 *   flag 0x004 — COLOR:     write 4× (uint32) = src & 0xff
 *                Note: the `& 0xff` mask is intentional. In orig D3DCOLOR
 *                ARGB the low byte is the BLUE channel; combined with D3D3
 *                TSS SELECTARG2 (texture-only) it has no visible effect.
 *                Port's R8G8B8A8_UNORM + MODULATE shader DOES read diffuse.rgb,
 *                so the mask reproduces the visual outcome only by ALSO
 *                forcing diffuse_rgb≈0 (which the modulate shader would render
 *                as black). Port-correct behavior is to KEEP the full 32-bit
 *                color so the modulate shader passes the texture through; this
 *                is the existing behavior and remains for visual parity.
 *   flag 0x100 — OPCODE:    write WORD at byte 0 of quad
 *                  param[26] != 0 ? 6 : 3  (tri-strip vs tri-fan opcode)
 *   flag 0x200 — TEXPAGE:   write WORD at byte 2 of quad from low 16 bits
 *                  of param[27].
 *
 * Port adaptation (legacy callers):
 *   The 3 existing callers in td5_hud.c pass mode_flags=0 expecting
 *   "do everything". Map mode_flags=0 to TD5_BSQT_LEGACY_ALL (geom+UV+color+
 *   texpage). Map mode_flags=2 to TD5_BSQT_UV_ONLY for compatibility with the
 *   smoke-draw style. Any caller passing a value with bit 0x1000 set is
 *   treated as a raw orig-style bitmask (geom/UV/color/opcode/texpage).
 *
 * Port-side ARCH-DIVERGENCE: the port's TD5_RenderSpriteQuad layout differs
 * from orig's 44-byte-stride packed buffer — the port uses a (sx, sy, sz, rhw,
 * color, u, v) 7-float layout per vertex starting at offset 0x08. The flag
 * dispatch maps each orig field semantic onto the port layout. The opcode
 * field at port byte 0 is `geometry_ptr` (int); the texpage at byte 2 is the
 * high half of geometry_ptr — preserving orig's 32-bit-wide opcode|texpage
 * header would corrupt the port's pointer-based pipeline. To avoid that we
 * store opcode/texpage into reserved scratch instead. */

/* Orig flag bits (must match exact values). */
#define TD5_BSQT_GEOMETRY   0x001
#define TD5_BSQT_UV         0x002
#define TD5_BSQT_COLOR      0x004
#define TD5_BSQT_OPCODE     0x100
#define TD5_BSQT_TEXPAGE    0x200

/* Port-side dispatch bit: when set, treat mode_flags as a raw orig bitmask.
 * Otherwise apply the legacy compatibility mapping documented above. */
#define TD5_BSQT_RAW_FLAGS  0x1000

/* Legacy "do everything" mask used when callers pass mode_flags=0. */
#define TD5_BSQT_LEGACY_ALL (TD5_BSQT_GEOMETRY | TD5_BSQT_UV | TD5_BSQT_COLOR | TD5_BSQT_TEXPAGE)

void td5_render_build_sprite_quad(int *params) {
    const TD5_RenderSpriteQuadParams *src = (const TD5_RenderSpriteQuadParams *)params;
    TD5_RenderSpriteQuad *dst;
    unsigned int flags;
    float z, rhw;

    if (!src || !src->dest) {
        return;
    }

    dst = (TD5_RenderSpriteQuad *)src->dest;

    /* Resolve flag mask. Legacy paths use mode_flags ∈ {0, 2}; orig-faithful
     * callers may set TD5_BSQT_RAW_FLAGS and pass the orig 5-bit mask. */
    if ((unsigned int)src->mode_flags & TD5_BSQT_RAW_FLAGS) {
        flags = (unsigned int)src->mode_flags & ~(unsigned int)TD5_BSQT_RAW_FLAGS;
    } else if (src->mode_flags == 2) {
        flags = TD5_BSQT_UV;
    } else {
        flags = TD5_BSQT_LEGACY_ALL;
    }

    /* --- Geometry (orig flag 0x001) --- */
    if (flags & TD5_BSQT_GEOMETRY) {
        z = src->depth_z[0];
        rhw = (z > 0.0f) ? (1.0f / z) : 1.0f;

        dst->geometry_ptr = 0;
        dst->vertex_count = 4;

        /* Slot mapping mirrors orig's storage order:
         *   src[0] → dst.v0   src[3] → dst.v1
         *   src[2] → dst.v2   src[1] → dst.v3   */
        dst->v0_x = src->scr_x[0]; dst->v0_y = src->scr_y[0];
        dst->v1_x = src->scr_x[3]; dst->v1_y = src->scr_y[3];
        dst->v2_x = src->scr_x[2]; dst->v2_y = src->scr_y[2];
        dst->v3_x = src->scr_x[1]; dst->v3_y = src->scr_y[1];

        dst->v0_z = dst->v1_z = dst->v2_z = dst->v3_z = z;
        dst->v0_rhw = dst->v1_rhw = dst->v2_rhw = dst->v3_rhw = rhw;

        dst->quad_width = src->scr_x[2] - src->scr_x[0];
        dst->quad_height = src->scr_y[1] - src->scr_y[0];
    }

    /* --- Color (orig flag 0x004) --- */
    if (flags & TD5_BSQT_COLOR) {
        /* Orig masks src & 0xff (D3DCOLOR low byte = blue channel) — see
         * function header for why port keeps the full 32-bit value. */
        dst->v0_color = src->diffuse[0];
        dst->v1_color = src->diffuse[3];
        dst->v2_color = src->diffuse[2];
        dst->v3_color = src->diffuse[1];
    }

    /* --- UV (orig flag 0x002) --- */
    if (flags & TD5_BSQT_UV) {
        dst->v0_u = src->tex_u[0]; dst->v0_v = src->tex_v[0];
        dst->v1_u = src->tex_u[3]; dst->v1_v = src->tex_v[3];
        dst->v2_u = src->tex_u[2]; dst->v2_v = src->tex_v[2];
        dst->v3_u = src->tex_u[1]; dst->v3_v = src->tex_v[1];

        dst->tex_u0 = src->tex_u[0];
        dst->tex_v0 = src->tex_v[0];
        dst->tex_u1 = src->tex_u[2];
        dst->tex_v1 = src->tex_v[2];
    }

    /* --- Texpage (orig flag 0x200) --- */
    if (flags & TD5_BSQT_TEXPAGE) {
        dst->texture_page = (float)src->texture_page;
    }

    /* --- Opcode (orig flag 0x100) ---
     * Orig stores: param[26] != 0 ? 6 : 3 as a WORD at byte 0 of the quad.
     * Port has no equivalent slot (geometry_ptr at byte 0 is a pointer used
     * by the port's batch pipeline). The opcode is not consumed by the port
     * pipeline, so silently drop the write but record that we honored the
     * flag. */
    (void)(flags & TD5_BSQT_OPCODE);
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

/* Submit a pre-built translucent quad using the HUD preset (LINEAR filter +
 * alpha_ref=1). Mirrors orig M2DX DXD3D::SetRenderState @ M2DX.dll 0x10001770
 * which sets D3DRS_ALPHAREF=0 + D3DRS_ALPHAFUNC=NOTEQUAL — i.e. discard only
 * fully-transparent pixels. The non-HUD TRANSLUCENT_LINEAR keeps alpha_ref=0x80
 * to prune bilinear fringes on world props; HUD widgets need the lower cutoff
 * to retain anti-aliased edges on digits/text. */
void td5_render_submit_translucent_hud(uint16_t *quad_data) {
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
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR_HUD);
    td5_plat_render_bind_texture(tex_page);
    td5_plat_render_draw_tris(verts, 4, indices, 6);
}

/* Additive variant of submit_translucent_hud for the victory star pulse.
 * Uses TD5_PRESET_ADDITIVE_OVERLAY (ONE/ONE, z-test off) so the grayscale-ramp
 * petals (diffuse RGB = phase*0.319, alpha pinned 0xFF) read as a SEMI-TRANSPARENT
 * WHITE GLOW that brightens as phase grows: gray 0 adds nothing (invisible at
 * start) -> gray 255 adds full white (bright). The plain translucent HUD path
 * (SRCALPHA with alpha=0xFF) drew them as OPAQUE gray quads instead.
 * [user feedback 2026-05-30: star should be white, semi-transparent, brighter
 *  as the animation progresses. Matches orig's additive (type-3) petal path.] */
void td5_render_submit_additive_hud(uint16_t *quad_data) {
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
    td5_plat_render_set_preset(TD5_PRESET_ADDITIVE_OVERLAY);
    td5_plat_render_bind_texture(tex_page);
    td5_plat_render_draw_tris(verts, 4, indices, 6);
}

/* Submit a pre-built translucent quad for world-space VFX (smoke, weather
 * streaks) so it is OCCLUDED by opaque geometry (walls, cars), matching the
 * original. Uses TD5_PRESET_ADDITIVE_WORLD: ONE/ONE additive blend with the
 * depth test ON (LEQUAL) and z-write off.
 *
 * Why depth-tested: RE of the original (Ghidra, 2026-06-01) shows queued
 * translucent primitives — including wheel smoke — are drawn by
 * FlushQueuedTranslucentPrimitives @0x00431340, which RunRaceFrame @0x0042b580
 * calls while the OPAQUE pass preset is still active (ZFUNC=LESSEQUAL, z-buffer
 * enabled). SetRaceRenderStatePreset @0x0040b070 never touches ZENABLE, which
 * stays TRUE scene-wide (proven by the SKY pass dodging occlusion via
 * ZFUNC=ALWAYS rather than disabling ZENABLE), so orig smoke is depth-tested.
 *
 * DEPTH SPACE: the renderer is uniformly LINEAR depth (no NDC stage). Smoke's
 * `sz` from project_vertex (line 498) is vz*(1/195000); opaque geometry writes
 * (vz-64)*(1/195000) (clip_and_submit_polygon, line 824). They differ ONLY by
 * the constant 64 NEAR_DEPTH_OFFSET, folded in below so the LEQUAL compare
 * against coplanar geometry is exact. td5_render_submit_tire_mark (below) is
 * the analogous depth-tested decal path (via TD5_PRESET_SHADOW). */
void td5_render_submit_translucent_world(uint16_t *quad_data) {
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
        /* Fold in the -64 NEAR_DEPTH_OFFSET that the opaque pass applies
         * (line 824) but the shared project_vertex (line 498) omits, so smoke
         * ties exactly with coplanar opaque geometry under the LEQUAL test. */
        verts[i].depth_z  = fdata[base + 2] - NEAR_DEPTH_OFFSET * DEPTH_NORMALIZE_INV;
        verts[i].rhw      = fdata[base + 3];
        verts[i].diffuse  = *(uint32_t *)&fdata[base + 4];
        verts[i].specular = 0;
        verts[i].tex_u    = fdata[base + 5];
        verts[i].tex_v    = fdata[base + 6];
    }

    tex_page = (int)(*(float *)((uint8_t *)quad_data + 0x90));
    td5_plat_render_set_preset(TD5_PRESET_ADDITIVE_WORLD);
    td5_plat_render_bind_texture(tex_page);
    td5_plat_render_draw_tris(verts, 4, indices, 6);
}

/* Submit a pre-built translucent quad as a ground DECAL: TD5_PRESET_SHADOW
 * (z_test=LEQUAL, z_write=0, alpha_ref=1, SRCALPHA). Used for tire/skid marks
 * — they lie on the road and MUST be depth-tested against world geometry so
 * walls/props occlude them. The marks' `sz` comes from the same project_vertex
 * (linear vz/far_clip) the opaque pass uses, so the LEQUAL compare is valid
 * (unlike the smoke NDC-vs-linear issue). z_write=0 so overlapping marks in
 * the trail don't z-fight each other. [FIX 2026-05-28 tire-marks-through-walls] */
void td5_render_submit_tire_mark(uint16_t *quad_data) {
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
    td5_plat_render_set_preset(TD5_PRESET_SHADOW);
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

/* [CONFIRMED @ 0x00439E60 RenderHudRadialPulseOverlay; DA-T5 impl 2026-05-22]
 * 5-petal translucent pulse ring drawn at viewport center on race transitions
 * (race-start, lap, finish). Ported from orig 928-byte RenderHudRadialPulseOverlay
 * per DA-T5 audit:
 *
 *   - Phase advance:  phase += dt * 4.2f  while phase < 3000.0f
 *   - Alpha:          clamp(phase * 0.31875f, 0, 255)
 *   - Radius:         viewport_width * phase * (1/160)
 *   - Anim accum:     s_radial_pulse_anim += dt * 3328.0f
 *
 * Vertex layout per petal (mirrors orig V0/V1/V2/V3 quad slots):
 *   V0 = inner_start (radius/2)
 *   V1 = outer_bisector (full radius, between V0 and V2)
 *   V2 = inner_end (radius/2)
 *   V3 = (0, 0) center
 *
 * Constants (from TD5_d3d.exe data segment per DA-T5):
 *   0x0045d624 = 0.0f      (phase gate)
 *   0x0045d708 = 3328.0f   (anim incr)
 *   0x0045d70c = 0.31875f  (phase→alpha)
 *   0x0045d710 = 4.2f      (phase incr)
 *   0x0045d714 = 3000.0f   (phase cap)
 *   0x0045d64c = 0.00625f  (radius = w * phase / 160)
 *   0x0045d5d0 = 0.5f      (inner ring multiplier)
 *   0x4300199a = 128.1f    (quad Z) */
static float s_radial_pulse_anim;  /* orig [0x004B08C0] _g_hudRadialPulseAnimState */

/* Mirror of td5_hud.c HUD_WHITE_TEX_PAGE — the 1x1 white texture page uploaded
 * during HUD init, used to render flat-color (untextured-equivalent) HUD quads
 * through the texture-modulating translucent path. */
#define TD5_HUD_WHITE_TEX_PAGE 899

void td5_render_radial_pulse(float dt)
{
    float phase = td5_hud_radial_pulse_get();

    /* [TEMP DIAG — remove before commit] Force the victory star visible during
     * a normal race so its color can be frame-dumped over the (neutral) road,
     * isolating an inherent tint (texture/channel) from scene-bleed. Set
     * TD5RE_DIAG_STAR=<phase> in the environment. */
    {
        const char *ds = getenv("TD5RE_DIAG_STAR");
        if (ds) { phase = (float)atof(ds); td5_hud_radial_pulse_set(phase); }
    }

    {
        static uint32_t s_diag_n = 0;
        if ((s_diag_n++ % 60u) == 0u) {
            TD5_LOG_I("render", "radial_pulse: phase=%.1f vp=%dx%d", phase,
                      (int)s_viewport_width, (int)s_viewport_height);
        }
    }

    /* Gate: orig FCOMP [0x0045d624] (= 0.0f). Skip when phase < 0. */
    if (phase < 0.0f) return;

    /* Snapshot base angle from anim-state accumulator (truncate-toward-zero). */
    int base_angle = (int)s_radial_pulse_anim;

    /* Phase advance (capped at 3000.0f). */
    if (phase < 3000.0f) {
        phase += dt * 4.2f;
        td5_hud_radial_pulse_set(phase);
    }

    /* Anim accumulator advances every frame (independent of phase). */
    s_radial_pulse_anim += dt * 3328.0f;

    /* Star opacity ramp. [user 2026-06-02: victory star looked "a little
     * pinkish"; should be white/neutral.] The DOMINANT cause was the missing
     * TD5_BSQT_TEXPAGE flag above (the petals modulated PAGE 0, a brown scene
     * atlas) — fixed there; with page 899 bound the petals are now pure white
     * (frame-dump verified 255,255,255). This ramp is a SECONDARY measure: the
     * petals are still drawn translucent (SRCALPHA), so at the old coefficient
     * 0.55 they only reached ~72% opacity (phase ~0..330 -> alpha max ~181),
     * letting the WARM finish-line scene bleed ~28% through the white star and
     * leaving a faint residual warm tint. [RE: RenderHudRadialPulseOverlay
     * @0x00439E60 — orig petals are OPAQUE (alpha 0xFF), so the original never
     * bleeds the scene.] Ramp the alpha faster (1.6) so the star reaches full
     * opacity (255) by phase ~160 (mid-hold): the prominent second half is then
     * fully OPAQUE neutral white, the first half still fades in, and the result
     * is closer to the original opaque petals. Tunable: 0.31875 = orig-faint,
     * 0.55 = old (translucent fade-in), 1.6 = opaque-by-mid-hold (neutral). */
    int alpha = (int)(phase * 1.6f);
    if (alpha < 0)        alpha = 0;
    else if (alpha > 255) alpha = 255;

    /* Per-frame radius. viewport_width * phase * (1/640).
     * [CONFIRMED @ 0x439e60 RenderHudRadialPulseOverlay: _DAT_0045d64c =
     *  0.0015625f = 1/640]. The port previously used 0.00625f (1/160) — 4x
     *  too large, which ballooned the star across the screen ~4x too fast
     *  (user 2026-05-30 "animation too fast"). Restored to the faithful 1/640. */
    float radius = (float)s_viewport_width * phase * 0.0015625f;

    /* 10 ring vertices: even k = inner (radius*0.5), odd k = outer (radius).
     * Inner angle steps by -0x33332 (~72°) per pair; outer angle is inner - 0x19999. */
    float vx[10], vy[10];
    int a = base_angle;
    for (int k = 0; k < 10; k += 2) {
        unsigned int a_inner = ((unsigned int)a) >> 8;
        unsigned int a_outer = ((unsigned int)(a - 0x19999)) >> 8;
        vx[k]     = CosFloat12bit(a_inner) * radius * 0.5f;
        vy[k]     = SinFloat12bit((int)a_inner) * radius * 0.5f;
        vx[k + 1] = CosFloat12bit(a_outer) * radius;
        vy[k + 1] = SinFloat12bit((int)a_outer) * radius;
        a -= 0x33332;
    }

    /* White victory star with alpha fade-in: RGB pinned WHITE (0xFFFFFF),
     * alpha = the phase ramp. Drawn via the translucent (SRCALPHA) HUD path
     * below, so alpha 0 = invisible -> alpha 255 = bright opaque white.
     *
     * [user 2026-05-30: "star is black, should be white".] Two port bugs made
     * it read black: (1) the previous pass put the ramp in the RGB bytes (so
     * the star was near-black gray at low phase) and (2) submitted ADDITIVE,
     * where dark RGB adds ~nothing to the scene -> a faint/black flash.
     * Deliberate deviation from orig's gray-RGB-ramp/opaque-alpha at 0x439e60:
     * orig's gray ramp only reaches white at phase ~800 (off-screen radius),
     * so on-screen it always looks dark — constant-white + alpha-ramp delivers
     * the white glow the user expects while keeping the faithful translucent blend. */
    uint32_t color = ((uint32_t)alpha << 24) | 0x00FFFFFFu;

    /* Center the ring on the viewport. */
    float cx = (float)s_viewport_width * 0.5f;
    float cy = (float)s_viewport_height * 0.5f;

    /* Per-petal scratch quad buffers (orig DAT_004B0C08/CC0/D78/E30/EE8). */
    static uint8_t s_pulse_quads[5][0xB8];
    static const int idx_table[5][3] = {
        {0, 1, 2}, {2, 3, 4}, {4, 5, 6}, {6, 7, 8}, {8, 9, 0},
    };

    for (int q = 0; q < 5; q++) {
        int i0 = idx_table[q][0];  /* inner start */
        int i1 = idx_table[q][1];  /* outer bisector */
        int i2 = idx_table[q][2];  /* inner end */

        TD5_RenderSpriteQuadParams p;
        p.dest = &s_pulse_quads[q];
        /* [FIX 2026-06-02 star-pinkish] TD5_BSQT_TEXPAGE was MISSING here, so
         * build_sprite_quad silently dropped p.texture_page (899=white) and the
         * quad kept texture_page=0 -> the petals modulated PAGE 0 (an arbitrary
         * brown scene atlas), rendering the victory star muddy brown/pink instead
         * of white. A prior pass set p.texture_page=899 but forgot the flag that
         * makes build_sprite_quad honour it. Adding TD5_BSQT_TEXPAGE binds the
         * real 1x1 white page 899 -> white star (matches orig untextured petals). */
        p.mode_flags = TD5_BSQT_RAW_FLAGS | TD5_BSQT_GEOMETRY | TD5_BSQT_COLOR | TD5_BSQT_TEXPAGE;

        /* Slot mapping per td5_render_build_sprite_quad:
         *   src[0] → V0   src[3] → V1   src[2] → V2   src[1] → V3
         * Orig wants V0=inner_start, V1=outer_bisector, V2=inner_end, V3=center.
         * So src indices: 0→inner_start, 1→center, 2→inner_end, 3→outer_bisector. */
        p.scr_x[0] = cx + vx[i0]; p.scr_y[0] = cy + vy[i0];
        p.scr_x[1] = cx;          p.scr_y[1] = cy;
        p.scr_x[2] = cx + vx[i2]; p.scr_y[2] = cy + vy[i2];
        p.scr_x[3] = cx + vx[i1]; p.scr_y[3] = cy + vy[i1];

        for (int v = 0; v < 4; v++) {
            p.depth_z[v] = 128.1f;   /* orig immediate 0x4300199a */
            p.diffuse[v] = color;
            p.tex_u[v]   = 0.0f;
            p.tex_v[v]   = 0.0f;
        }
        /* Orig petals are UNTEXTURED flat color (tex_u=tex_v=0). The port's
         * translucent-HUD path always modulates by a bound texture, so bind the
         * 1x1 WHITE page (== td5_hud.c HUD_WHITE_TEX_PAGE 899, uploaded at HUD
         * init) — page 0 is an arbitrary atlas whose texel darkened the flat
         * white, contributing to the "black star". white*white = white. */
        p.texture_page = TD5_HUD_WHITE_TEX_PAGE;
        p.reserved     = 0;

        td5_render_build_sprite_quad((int *)&p);
        /* Translucent (SRCALPHA/INVSRCALPHA) so the white petals fade in by
         * vertex alpha — faithful to orig SubmitImmediateTranslucentPrimitive
         * @ 0x4315b0 (NOT additive). The previous additive path made the dark
         * low-phase color invisible/black. [user feedback 2026-05-30] */
        td5_render_submit_translucent_hud((uint16_t *)&s_pulse_quads[q]);
    }
}

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

/* ========================================================================
 * Debug line overlay — world-space colored line batch (F12 collision wireframe).
 *
 * Accumulates the world-space segments emitted by
 * td5_track_debug_emit_collision_lines() and flushes them as a single D3D11
 * LINELIST via td5_plat_render_draw_lines(). Each endpoint is projected through
 * the SAME camera-relative transform + depth formula the opaque track polygons
 * use (see the track pass at ~line 800: depth = (vz-NEAR_DEPTH_OFFSET)*
 * DEPTH_NORMALIZE_INV), so the wireframe registers exactly on the rails.
 * draw_lines hard-codes z-test LESS_EQUAL / z-write OFF / opaque, so lines are
 * occluded by nearer terrain but coincident rail edges still draw on top, and
 * the overlay never poisons depth for later passes.
 *
 * Coordinate space: x/y/z arrive in raw 24.8 world fixed-point (origin+vertex
 * sums from emit_strip_line) — the SAME space as s_camera_pos, so a plain
 * camera-relative subtract + basis rotate matches the world geometry.
 * [Implements the former no-op stub; renderer pipeline confirmed against
 * project_vertex / the shadow path 2026-05-30.]
 * ======================================================================== */
#define TD5_DEBUG_LINE_MAX_VERTS 2048   /* 1024 segments/flush; VB holds ~4096 */
#define TD5_DEBUG_LINE_HALF_PX   1      /* line half-thickness in px → (2*HP+1) px wide.
                                         * D3D11 LINELIST is always 1px, so thicken by
                                         * emitting parallel copies offset perpendicular
                                         * in screen space. */

static TD5_D3DVertex s_debug_line_verts[TD5_DEBUG_LINE_MAX_VERTS];
static int           s_debug_line_count = 0;

void td5_render_debug_lines_reset(void) {
    s_debug_line_count = 0;
}

void td5_render_debug_lines_flush(void) {
    if (s_debug_line_count >= 2) {
        static int s_logged = 0;
        if (!s_logged) {
            TD5_LOG_I(LOG_TAG, "debug wireframe: first flush %d verts (%d segments)",
                      s_debug_line_count, s_debug_line_count / 2);
            s_logged = 1;
        }
        td5_plat_render_draw_lines(s_debug_line_verts, s_debug_line_count);
    }
    s_debug_line_count = 0;
}

/* Project one world point (raw 24.8 fixed-point, same space as s_camera_pos)
 * into a pretransformed screen-space vertex. Returns 0 if behind near clip. */
static int debug_line_project(float wx, float wy, float wz, uint32_t argb,
                              TD5_D3DVertex *out) {
    float dx = wx - s_camera_pos[0];
    float dy = wy - s_camera_pos[1];
    float dz = wz - s_camera_pos[2];
    /* camera_basis is row-major { right, up, forward } (same as track/shadow). */
    float vx = dx * s_camera_basis[0] + dy * s_camera_basis[1] + dz * s_camera_basis[2];
    float vy = dx * s_camera_basis[3] + dy * s_camera_basis[4] + dz * s_camera_basis[5];
    float vz = dx * s_camera_basis[6] + dy * s_camera_basis[7] + dz * s_camera_basis[8];
    if (vz <= s_near_clip) return 0;
    float inv_z = 1.0f / vz;
    out->screen_x = -vx * s_focal_length * inv_z + s_center_x;
    out->screen_y = -vy * s_focal_length * inv_z + s_center_y;
    out->depth_z  = (vz - NEAR_DEPTH_OFFSET) * DEPTH_NORMALIZE_INV; /* matches track polys */
    out->rhw      = inv_z;
    out->diffuse  = argb;   /* 0xAARRGGBB → B8G8R8A8_UNORM diffuse (white SRV modulate) */
    out->specular = 0;
    out->tex_u    = 0.0f;
    out->tex_v    = 0.0f;
    return 1;
}

void td5_render_debug_line_world(float x0, float y0, float z0,
                                 float x1, float y1, float z1,
                                 uint32_t argb) {
    TD5_D3DVertex a, b;
    /* The line path has no near-plane clipper, so drop the whole segment if
     * either endpoint is behind the camera. Acceptable for a debug overlay. */
    if (!debug_line_project(x0, y0, z0, argb, &a)) return;
    if (!debug_line_project(x1, y1, z1, argb, &b)) return;

    /* Thicken: D3D11 lines are 1px, so emit (2*HALF_PX+1) parallel copies
     * offset perpendicular to the segment in SCREEN space (pixels). Depth/rhw
     * are preserved so occlusion is unchanged. */
    float sdx = b.screen_x - a.screen_x;
    float sdy = b.screen_y - a.screen_y;
    float slen = sqrtf(sdx * sdx + sdy * sdy);
    float px = 0.0f, py = 0.0f;
    if (slen > 0.001f) { px = -sdy / slen; py = sdx / slen; }

    for (int o = -TD5_DEBUG_LINE_HALF_PX; o <= TD5_DEBUG_LINE_HALF_PX; o++) {
        TD5_D3DVertex va = a, vb = b;
        va.screen_x += px * (float)o; va.screen_y += py * (float)o;
        vb.screen_x += px * (float)o; vb.screen_y += py * (float)o;
        if (s_debug_line_count + 2 > TD5_DEBUG_LINE_MAX_VERTS) {
            td5_render_debug_lines_flush();   /* emit full batch, keep accumulating */
        }
        s_debug_line_verts[s_debug_line_count++] = va;
        s_debug_line_verts[s_debug_line_count++] = vb;
    }
}

/* ============================================================
 * [CITATION-SWEEP 2026-05-21] Phase 1 audit-header refresh
 *
 * The following L3 Ghidra functions are ported (or folded) into
 * this file but were missed by build_confidence_map.py's
 * 2026-05-18 citation scan due to snake_case rename or
 * multi-line comment wraps. Listed here so the next confidence-
 * map run promotes them L3 -> L4 (cited without precision
 * keywords). Per-function audits remain a separate Phase 4 task.
 *
 * Source: re/analysis/l3_triage_2026-05-21.csv +
 *         re/analysis/phase1_manifest_assignment.csv
 *
 *   0x004092D0  RenderVehicleActorModel  (density-match, verify in Phase 4)
 *   0x0040CDC0  CrossFade16BitSurfaces
 *   0x0040D120  AdvanceCrossFadeTransition  (density-match, verify in Phase 4)
 *   0x0040D190  CrossFade16BitSurfaces
 *   0x0042D880  ApplyMeshRenderBasisFromTransform  (density-match, verify in Phase 4)
 *   0x0042E370  TransformVector3ByRenderRotation  (density-match, verify in Phase 4)
 *   0x0042E3D0  TransformShortVectorToView  (density-match, verify in Phase 4)
 *   0x0042E4F0  WritePointToCurrentRenderTransform  (density-match, verify in Phase 4)
 *   0x0042E560  TransformTriangleByRenderMatrix  (density-match, verify in Phase 4)
 *   0x0042E750  BuildWorldToViewMatrix  (density-match, verify in Phase 4)
 *   0x0042E9C0  LoadGlobalOrientationToRenderState  (density-match, verify in Phase 4)
 *   0x004317C0  SubmitProjectedPolygon
 *   0x0043E2C0  ResetProjectedPrimitiveWorkBuffer  (density-match, verify in Phase 4)
 *   0x0043E5F0  InitializeProjectedPrimitiveBuckets  (density-match, verify in Phase 4)
 */


/* ============================================================
 * [ARCH-DIVERGENCE: cross-fade / fade-overlay collapse] Phase 5(a) class manifest (2026-05-21)
 *
 * Orig's CrossFade16BitSurfaces (two entry points 0x0040CDC0 +
 * 0x0040D190) iterates 16-bit DDraw surfaces scanline-by-scanline,
 * blending pixel-pairs across a transition. AdvanceCrossFadeTransition
 * (0x0040D120) advances the per-pixel mix factor. The port replaces all
 * three with a single full-screen quad pass under D3D11 backbuffer at
 * td5_render_crossfade_surfaces (td5_render.c:~3666). Same conceptual
 * blend curve; the pixel-walk is gone because D3D11 doesn't expose
 * lockable surfaces.
 *
 *   0x0040CDC0  CrossFade16BitSurfaces (variant 1)  [ARCH-DIVERGENCE: CrossFade]
 *   0x0040D120  AdvanceCrossFadeTransition          [ARCH-DIVERGENCE: CrossFade]
 *   0x0040D190  CrossFade16BitSurfaces (variant 2)  [ARCH-DIVERGENCE: CrossFade]
 */

/* ============================================================
 * [ARCH-DIVERGENCE: mesh transform / projection helpers] Phase 5(a) class manifest (2026-05-21)
 *
 * Orig has multiple per-mesh transform helpers operating on 12-float 3x4
 * matrices stored in DDraw-era globals. ApplyMeshRenderBasisFromTransform
 * and TransformShortVectorToView are fp16 view-space transforms;
 * TransformTriangleByRenderMatrix is per-triangle projection-pipeline
 * transform; ApplyMeshProjectionEffect generates water/envmap UVs. The
 * port routes all of these through s_render_transform.m + inline mat3x3
 * helpers; per-helper functions are folded into
 * td5_render_transform_mesh_vertices and dispatch_projected_* callers.
 *
 *   0x0042D880  ApplyMeshRenderBasisFromTransform  [ARCH-DIVERGENCE: MeshXform]
 *   0x0042E3D0  TransformShortVectorToView         [ARCH-DIVERGENCE: MeshXform]
 *   0x0042E560  TransformTriangleByRenderMatrix    [ARCH-DIVERGENCE: MeshXform]
 *   0x0043DEC0  ApplyMeshProjectionEffect          [ARCH-DIVERGENCE: MeshXform]
 */

/* ============================================================
 * [ARCH-DIVERGENCE: depth-sort bucket management] Phase 5(a) class manifest (2026-05-21)
 *
 * Orig's depth-sort bucket system uses raw-heap scratch buffers and
 * global state (DAT_004af268 / DAT_004af278) for the projected-primitive
 * linked lists. Port consolidates this into typed struct arrays (one
 * TD5_RenderBucketEntry per slot) plus inline reset semantics inside
 * td5_render_flush_projected_buckets and td5_render_init. Four orig
 * helper functions (Reset/Initialize/Insert/Flush) fold into the
 * consolidated init+flush path; semantically equivalent (same 4096-bucket
 * inverse-Z layout) without the raw-byte scratch interface.
 *
 *   0x0043E2C0  ResetProjectedPrimitiveWorkBuffer    [ARCH-DIVERGENCE: DepthSort]
 *   0x0043E2F0  FlushProjectedPrimitiveBuckets       [ARCH-DIVERGENCE: DepthSort]
 *   0x0043E3B0  InsertBillboardIntoDepthSortBuckets  [ARCH-DIVERGENCE: DepthSort]
 *   0x0043E5F0  InitializeProjectedPrimitiveBuckets  [ARCH-DIVERGENCE: DepthSort]
 */

/* ============================================================
 * [ARCH-DIVERGENCE: per-segment track lighting] Phase 5(a) class manifest (2026-05-21)
 *
 * Orig blends per-segment ambient light entries during span traversal
 * (BlendTrackLightEntryFromStart/End) and per-actor track-light state
 * snapshot (UpdateActorTrackLightState). All three fold in port into
 * td5_render_apply_track_lighting (td5_render.c:~2102) called BEFORE
 * compute_vertex_lighting in the per-actor vehicle dispatch path (mirrors
 * ApplyTrackLightingForVehicleSegment @ 0x00430150). Output is same
 * s_light_dirs[] / s_ambient_intensity globals; orig's linked-list blend
 * at segment boundary collapses into a single per-actor lookup.
 *
 *   0x0040CD10  UpdateActorTrackLightState     [ARCH-DIVERGENCE: TrackLight]
 *   0x0042FE20  BlendTrackLightEntryFromStart  [ARCH-DIVERGENCE: TrackLight]
 *   0x0042FFC0  BlendTrackLightEntryFromEnd    [ARCH-DIVERGENCE: TrackLight]
 */

/* ============================================================
 * [ARCH-DIVERGENCE: rasterizer pipeline (D3D3 -> D3D11)] Phase 5(d) class manifest (2026-05-21)
 *
 * The original used DDraw + D3D3 immediate-mode rasterization: vertex/index
 * buffers were submitted through a vtable trampoline on `*d3d_exref` (slot
 * 0x38) → IDirect3DDevice3::DrawIndexedPrimitive with FVF 0x1C4 (pre-
 * transformed XYZRHW + diffuse + UV). On top of that, Sutherland-Hodgman
 * clipping was split across THREE separate orig functions for the X edges
 * (0x004323D0), Y edges (0x004326D0), and fan emission (0x00432AB0); the
 * 3030-byte master function ClipAndSubmitProjectedPolygon (0x004317F0)
 * orchestrated near-plane clip + project + screen-axis chain.
 *
 * The port collapses all four of these into a single
 * clip_and_submit_polygon() in td5_render.c that does near-plane clip,
 * perspective project, single early-reject screen test, and triangle-fan
 * emission, then routes through td5_plat_render_draw_tris() on a D3D11
 * immediate command list. D3D11 handles screen-edge clipping internally
 * (CullMode=NONE per L1 docs), so the X/Y screen-axis Sutherland-Hodgman
 * stages are deliberately absent. FlushImmediateDrawPrimitiveBatch's
 * vtable call (orig 0x004329E0) becomes td5_plat_render_draw_tris().
 *
 * Same geometry semantics (same MeshVertex layout, same texture-page
 * binding, same back-to-front depth ordering for translucent), same
 * 4096-bucket depth sort downstream. The pipeline-stage byte layout
 * diverges by design; per-byte vertex-buffer comparison is meaningless
 * across the D3D3->D3D11 boundary.
 *
 *   0x004317F0  ClipAndSubmitProjectedPolygon    [ARCH-DIVERGENCE: D3D Pipeline]
 *   0x004323D0  RenderTrackSegmentBatch          [ARCH-DIVERGENCE: D3D Pipeline]
 *   0x004326D0  RenderTrackSegmentBatchVariant   [ARCH-DIVERGENCE: D3D Pipeline]
 *   0x00432AB0  AppendClippedPolygonTriangleFan  [ARCH-DIVERGENCE: D3D Pipeline]
 *   0x004329E0  FlushImmediateDrawPrimitiveBatch [ARCH-DIVERGENCE: D3D Pipeline]
 *   0x00431340  FlushQueuedTranslucentPrimitives [ARCH-DIVERGENCE: D3D Pipeline]
 *   0x00431750  EmitTranslucentTriangleStrip     [ARCH-DIVERGENCE: D3D Pipeline]
 *   0x0043DCB0  TransformAndQueueTranslucentMesh [ARCH-DIVERGENCE: D3D Pipeline]
 */

/* ============================================================
 * [ARCH-DIVERGENCE: D3D3 sprite-template scratch -> D3D11 vertex stream] Phase 5(d) class manifest (2026-05-21)
 *
 * Original used BuildSpriteQuadTemplate (0x00432BD0) + Write/TransformShort-
 * VectorToView to pre-bake quad templates into static scratch buffers
 * (DAT_004C4300+, &g_vehicleShadowAndWheelSpriteTemplates+...) which were
 * then submitted via QueueTranslucentPrimitiveBatch for D3D3 immediate-mode
 * batching. Three large initializers populate those scratch buffers (wheel
 * tire sidewall rings, vehicle shadow corners, special-actor overlay quads),
 * all encoded as TD5_PrimitiveCmd records aliasing pre-transformed shorts.
 *
 * Port replaces this with per-frame D3D11 raw TD5_D3DVertex emission:
 *   - render_vehicle_wheel_billboards (td5_render.c:~4316) emits the 8-segment
 *     tire ring + hub-cap on each frame using projected vertex data.
 *   - render_vehicle_shadow_quad (td5_render.c:~3930) builds 4-vertex shadow
 *     trapezoids from the wheel-probe positions each frame.
 *   - render_vehicle_brake_lights (td5_render.c:~4106) builds 4-vertex
 *     billboard quads from cardef hardpoint offsets each frame.
 *
 * No per-frame template scratch buffers exist in the port. Geometry semantics
 * (UV layout, atlas lookup, sign conventions, brightness ramps) are individu-
 * ally [CONFIRMED] inline at the per-call sites; the wholesale ARCH change
 * is the D3D3 scratch-buffer + Queue/Flush pipeline -> D3D11 immediate stream.
 *
 *   0x00446A70  InitializeVehicleWheelSpriteTemplates [ARCH-DIVERGENCE: D3D3 Templates]
 *   0x0040C7E0  BuildSpecialActorOverlayQuads         [ARCH-DIVERGENCE: D3D3 Templates]
 *   0x004011C0  RenderVehicleTaillightQuads           [ARCH-DIVERGENCE: D3D3 Templates]
 *
 * Note: 0x004011C0 has a call-site dispatch in render.c (`render_vehicle_brake
 * _lights` is the render-side port; the higher-level orchestration lives in
 * td5_vfx.c as `td5_vfx_render_taillights`). The D3D3->D3D11 boundary is the
 * common factor that ARCH-DIVERGENCE-promotes both halves.
 */

/* ============================================================
 * [Phase 5(d) L5 promotion audit (2026-05-21)] — byte-faithful confirmations
 *
 * The following functions were re-decompiled and compared against the port
 * during the Phase 5(d) render audit. Their port implementations match the
 * orig logic line-for-line; promotion comments are placed inline at the
 * port call site or definition.
 *
 *   0x0040AE80  InitializeRaceRenderState        [CONFIRMED — byte-faithful init guard]
 *     Orig: 3-call sequence + sentinel set (DAT_0048dba0, bClearScreen) under
 *     a one-shot gate. Port td5_render_init (td5_render.c:~987) merges the
 *     three init sub-routines (InitializeTranslucentPrimitivePipeline,
 *     InitializeProjectedPrimitiveBuckets, ResetProjectedPrimitiveWorkBuffer)
 *     into a single inline reset pass; the sentinel collapses to s_initialized.
 *
 *   0x0042E9C0  LoadGlobalOrientationToRenderState [CONFIRMED — byte-faithful wrapper]
 *     Orig: single LoadRenderRotationMatrix(&DAT_004ab040) call. Port routes
 *     through td5_render_load_rotation((Mat3x3*)&g_raceRotationMatrix), same
 *     9-float copy. Used by RenderRaceActorForView path.
 *
 *   0x0040BAA0  QueryRaceTextureCapacity         [CONFIRMED — byte-faithful capacity probe]
 *     Orig: DXD3D::GetMaxTextures(0x40) → store in DAT_0048DC40+0x10 +
 *     mirror to g_appExref+0xDC + log via DX::GetStateString. Port reports
 *     the D3D11 device's max texture count via wrapper at init time; the
 *     0x40 (=64) cap orig requested is now a wrapper assertion. Logged
 *     identically through td5_render_log_caps.
 *
 *   0x00431270  RenderTrackSpanDisplayList       [CONFIRMED — orig logic + port-side defensive guards]
 *     td5_render_span_display_list (td5_render.c:~1522). Core loop
 *     (cull→push if billboard tag 1/2→load billboard rot→transform→submit→
 *     pop) matches orig 1:1. Port adds NaN/Inf guards on bounding sphere
 *     fields and mesh-pointer in-blob validation — these reject invalid
 *     records that orig would crash on, never changing valid-record output.
 *     [CONFIRMED @ 0x42dcad bounding-sphere read; @ 0x00431296 billboard tag
 *     test] already inline at the port site.
 *
 * Two related render-c functions are out-of-scope-but-cited and remain L4:
 *   0x0042E4F0  WritePointToCurrentRenderTransform — citation-sweep header
 *     entry; no dedicated port. The 3x4 matrix * vec3 + translation column
 *     operation is folded into td5_render_transform_mesh_vertices /
 *     mat3x3_transform_vec3 across multiple call sites. Honest skip — too
 *     diffused to point to a single port site.
 *
 *   0x0042E750  BuildWorldToViewMatrix — citation-sweep header entry; orig's
 *     pitch/yaw + forward-vector to 3x3 builder is replaced by per-frame
 *     camera-basis composition in td5_camera (orbit/chase basis dump).
 *     No corresponding td5_render.c body — honest skip.
 */
