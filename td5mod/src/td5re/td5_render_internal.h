/* ========================================================================
 * td5_render_internal.h — PRIVATE shared internals of the render module
 *
 * Included ONLY by td5_render.c (core pipeline) and td5_render_effects.c
 * (per-actor effects split out of the core, P1-C 2026-07-02). Nothing outside
 * the render TUs may include this header — the public surface stays
 * td5_render.h.
 *
 * NOTE: include this AFTER td5_render.h / td5_platform.h / the actor-struct
 * header — the declarations below use their types (TD5_Actor, TD5_D3DVertex,
 * TD5_Mat3x3, TD5_Vec3f) without re-including them.
 * ======================================================================== */
#ifndef TD5_RENDER_INTERNAL_H
#define TD5_RENDER_INTERNAL_H

/* ------------------------------------------------------------------------
 * Render state vocabulary hoisted VERBATIM from td5_render.c (P1-C):
 * constants, pool typedefs, the RenderScratch re-entrancy struct and the
 * historical s_* field shims. Static variable definitions stayed in
 * td5_render.c (marked inline below).
 * ------------------------------------------------------------------------ */
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
#define TIRE_DECAL_BIAS      40.0f                /* tire-mark decal pull toward camera (view units); 40 clears z-fight speckle on bumpy cobblestone while the car (far closer) still occludes */

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

/* s_render_transform / s_transform_stack[_depth] moved to RenderScratch (Phase B Stage 1). */

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

/* projection params (focal/frustum/clip/center/viewport) moved to RenderScratch (Phase B Stage 1). */

/* ========================================================================
 * Camera Basis (set externally by td5_camera module)
 *
 * Original: DAT_004aafa0..b4 = right/up/forward (3x3)
 *           DAT_004aafc4..cc = camera world position
 * ======================================================================== */

/* s_camera_basis / s_camera_pos moved to RenderScratch (Phase B Stage 1). */

/* ========================================================================
 * Lighting State
 *
 * 3 directional lights (9 floats total) + ambient intensity.
 * Original: DAT_004ab0d0..f0 = light directions, DAT_004bf6a8 = ambient
 * ======================================================================== */

/* s_light_dirs / s_ambient_intensity moved to RenderScratch (Phase B Stage 1). */

/* ========================================================================
 * Fog State
 *
 * Original: d3d_exref+0x18 = color, d3d_exref+0xa34 = enable flag
 * ======================================================================== */

/* (static var stays in td5_render.c: static uint32_t s_fog_color) */
/* (static var stays in td5_render.c: static int      s_fog_enabled) */

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

/* texture-bind cache: SHARED (manages the shared 1024-page GPU texture table).
 * Intentionally NOT per-pane — see the note in RenderScratch. */
/* (static var stays in td5_render.c: static TextureCacheSlot s_texture_cache[TEXTURE_CACHE_SLOTS]) */
/* (static var stays in td5_render.c: static int              s_texture_cache_active_count) */
/* s_current_texture_page / s_previous_texture_page are PER-PANE (RenderScratch):
 * the "currently bound page" is per-pane render state (each pane binds its own
 * sequence), so the parallel build records the right page per pane. The LRU
 * cache + active_count above stay SHARED (GPU residency manager, touched only on
 * the serial replay/live path). */

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

/* translucent batch pool (pool/head/free/count) moved to RenderScratch (Phase B Stage 1). */

/* Luminance-to-ARGB color LUT (1024 entries).
 * Original: DAT_004aee68, init as i * 0x10101 - 0x1000000 */
/* (static var stays in td5_render.c: static uint32_t s_color_lut[1024]) */

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

/* depth-sort buckets/entries/count moved to RenderScratch (Phase B Stage 1). */

/* ========================================================================
 * Immediate Draw Buffers
 *
 * Accumulated pre-transformed vertices/indices for batched DrawPrimitive.
 * Original: DAT_004afb14 (vertex buf), DAT_004af314 (index buf)
 * ======================================================================== */

/* immediate vert/index buffers + counts moved to RenderScratch (Phase B Stage 1). */

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

/* ========================================================================
 * [Phase B / Stage 1 — render re-entrancy, 2026-06-08]
 *
 * All per-pane MUTABLE render scratch (transform stack, projection, camera,
 * lighting, texture-bind cache, translucent/depth/immediate/deferred-additive
 * buffers, and the nested-draw guards) is bundled into RenderScratch and
 * reached through a THREAD-LOCAL pointer g_rs that defaults to a single static
 * instance g_rs_default. The existing `s_*` access sites are redirected by the
 * #define shims below, so the serial/main-thread path (g_rs == &g_rs_default)
 * is byte-identical to the previous single-instance code. Stage 2 points each
 * worker thread's g_rs at its own instance so panes record concurrently
 * without clobbering each other's scratch.
 *
 * Deliberately LEFT SHARED (read-only after init, or benign): the trig/color
 * LUTs, per-frame anim counters (sky rotation, billboard phase), the per-slot
 * vehicle prep arrays (built once per frame before the pane loop), the lazy
 * atlas-lookup caches (idempotent), the diagnostic debug counters, and the
 * dev-only F12 debug-line overlay buffer.
 * ======================================================================== */

/* Relocated from its original site (~0x42E130 lighting block) so it can live
 * inside RenderScratch, which is defined above the first use of the shims. */
typedef struct {
    int   enabled;       /* 0 = slot disabled, 1 = active */
    float vec_world[3];  /* world-frame contribution dir scaled by intensity */
} TL_Contribution;

typedef struct RenderScratch {
    /* transform stack (mesh dispatch) */
    TD5_Mat3x4 render_transform;
    TD5_Mat3x4 transform_stack[TRANSFORM_STACK_MAX];
    int        transform_stack_depth;
    /* projection params (per pane via td5_render_configure_projection) */
    float focal_length, inv_focal;
    float frustum_h_cos, frustum_h_sin;
    float frustum_v_cos, frustum_v_sin;
    float near_clip, far_clip, far_cull;
    float center_x, center_y;
    int   viewport_width, viewport_height;
    /* camera basis/pos (per pane) */
    float camera_basis[9];
    float camera_pos[3];
    float camera_secondary[9];   /* g_cameraSecondaryUnscaled snapshot (billboard basis) */
    /* lighting (written during actor pass) */
    float light_dirs[9];
    float ambient_intensity;
    TL_Contribution tl_contrib[3];
    int   tl_ambient;
    /* [LIGHT2] Authored per-channel zone color the classic path averages to
     * gray: tl_chroma[slot] = weight_rgb / gray-average (all-zero = "not yet
     * captured", treated as neutral); tl_amb_rgb = raw ambient RGB (all-zero =
     * fall back to the gray ambient_intensity). Written by tl_set_contrib /
     * tl_set_depth, consumed by the Mode>=1 colored vertex lighting. */
    float tl_chroma[3][3];
    float tl_amb_rgb[3];
    /* [DYNAMIC LIGHTS] model->world basis for the mesh whose vertex lighting is
     * about to be computed: origin (world units) + body->world rotation (9
     * floats, row-major). has_rot=0 means identity rotation (track geometry,
     * vertices already in world-offset space). Set per-mesh right before
     * td5_render_compute_vertex_lighting; per-pane so threaded panes can't race. */
    float light_basis_origin[3];
    float light_basis_rot[9];
    int   light_basis_has_rot;
    /* "currently/previously bound page" — per-pane render state (the LRU cache
     * array itself stays shared; see note at its declaration). */
    int current_texture_page;
    int previous_texture_page;
    /* NOTE: the texture-bind cache (TextureCacheSlot[600] + active_count) is NOT
     * here — it manages the SHARED 1024-page GPU texture table, so it's a single
     * shared instance (per-pane copies would diverge their page->slot mapping and
     * bind/evict the wrong GPU pages). Kept as shared file statics. */
    /* translucent batch pipeline */
    TranslucentBatchEntry translucent_pool[TRANSLUCENT_POOL_SIZE];
    int translucent_head, translucent_free, translucent_count;
    /* depth-sorted projected bucket pipeline */
    int depth_buckets[DEPTH_BUCKET_COUNT];
    DepthBucketEntry depth_entries[DEPTH_ENTRY_POOL];
    int depth_entry_count;
    /* immediate draw accumulation */
    TD5_D3DVertex imm_verts[IMMEDIATE_MAX_VERTS];
    uint16_t imm_indices[IMMEDIATE_MAX_INDICES];
    int imm_vert_count, imm_index_count;
    /* deferred additive pass */
    TD5_D3DVertex deferred_add_verts[DEFERRED_ADD_MAX_VERTS];
    uint16_t deferred_add_indices[DEFERRED_ADD_MAX_INDICES];
    int deferred_add_vert_count, deferred_add_index_count;
    DeferredAdditiveBatch deferred_add_batches[DEFERRED_ADD_MAX_BATCHES];
    int deferred_add_batch_count;
    int deferred_add_active;
    /* misc per-pane state */
    int scene_has_renderer_geometry;
    int in_sky_draw;
    int in_reflection_overlay;
    /* [task#21] TD6 car body z-fix. When >0, clip_and_submit_polygon snaps each
     * vertex depth to a fine grid (so coincident/near-coplanar faces of the
     * single de-indexed TD6 body mesh resolve to the SAME depth and tie stably by
     * submission order instead of flickering) and pulls the body a hair toward
     * the camera (so the painted shell wins the tie against the geometry behind
     * it). Set only around a ported-TD6 car body's mesh draw; 0 otherwise. */
    float td6_car_zbias;
    uint8_t actor_light_zone[TD5_ACTOR_MAX_TOTAL_SLOTS];  /* per-pane light-zone cache */
    /* [Phase B parallel-build] Per-pane mesh-vertex WORKSPACE. The mesh
     * transform used to write view coords IN-PLACE into the SHARED mesh blob
     * (lighting + proj-UV writers likewise) — fatal for concurrent pane
     * builds: every pane transforms the SAME blobs with its own camera.
     * td5_render_transform_mesh_vertices now copies the mesh's vertex array
     * here and writes the runtime fields in the copy; dispatch rebases
     * blob-derived vertex pointers via rs_vtx_rebase(). The blob is never
     * written by the render path. The workspace holds ONE mesh at a time
     * (transform -> dispatch is one uninterrupted sequence per mesh), so
     * depth-bucket prims — consumed at flush, after the workspace has been
     * reused — are copied into prim_copy at queue time (entry idx*4 slots). */
    TD5_MeshVertex *vtx_work;        /* malloc'd, grown on demand            */
    int             vtx_work_cap;
    /* [LIGHT2] Parallel per-workspace-vertex packed WORLD normal (biased
     * 8:8:8 in bits 23..0; 0 = "no normal"). Cleared per mesh in
     * transform_mesh_vertices, filled by compute_vertex_lighting, consumed by
     * clip_and_submit_polygon which adds the material id in bits 31..24 and
     * emits the result as the D3D vertex's COLOR1 (G-buffer feed). */
    uint32_t       *vtx_pack;
    int             vtx_pack_cap;
    const uint8_t  *vtx_src_base;    /* blob range of the current mesh       */
    const uint8_t  *vtx_src_end;
    int             tex_page_override; /* -1 = none; reflection overlay page */
    TD5_MeshVertex  prim_copy[DEPTH_ENTRY_POOL * 4];
} RenderScratch;

extern __thread RenderScratch *g_rs;   /* defined in td5_render.c (default -> g_rs_default) */

/* Field shims — redirect the historical static names to the thread-local
 * instance. Defined here (after all member typedefs, before the first function
 * that uses them) so every access site compiles unchanged. */
#define s_render_transform          (g_rs->render_transform)
#define s_transform_stack           (g_rs->transform_stack)
#define s_transform_stack_depth     (g_rs->transform_stack_depth)
#define s_focal_length              (g_rs->focal_length)
#define s_inv_focal                 (g_rs->inv_focal)
#define s_frustum_h_cos             (g_rs->frustum_h_cos)
#define s_frustum_h_sin             (g_rs->frustum_h_sin)
#define s_frustum_v_cos             (g_rs->frustum_v_cos)
#define s_frustum_v_sin             (g_rs->frustum_v_sin)
#define s_near_clip                 (g_rs->near_clip)
#define s_far_clip                  (g_rs->far_clip)
#define s_far_cull                  (g_rs->far_cull)
#define s_center_x                  (g_rs->center_x)
#define s_center_y                  (g_rs->center_y)
#define s_viewport_width            (g_rs->viewport_width)
#define s_viewport_height           (g_rs->viewport_height)
#define s_camera_basis              (g_rs->camera_basis)
#define s_camera_pos                (g_rs->camera_pos)
#define s_camera_secondary          (g_rs->camera_secondary)
#define s_light_dirs                (g_rs->light_dirs)
#define s_ambient_intensity         (g_rs->ambient_intensity)
#define s_tl_contrib                (g_rs->tl_contrib)
#define s_tl_ambient                (g_rs->tl_ambient)
#define s_tl_chroma                 (g_rs->tl_chroma)
#define s_tl_amb_rgb                (g_rs->tl_amb_rgb)
#define s_light_basis_origin        (g_rs->light_basis_origin)
#define s_light_basis_rot           (g_rs->light_basis_rot)
#define s_light_basis_has_rot       (g_rs->light_basis_has_rot)
/* s_texture_cache / _active_count are SHARED statics (declared at their original
 * site), NOT g_rs fields. s_current/_previous_texture_page ARE per-pane g_rs. */
#define s_current_texture_page      (g_rs->current_texture_page)
#define s_previous_texture_page     (g_rs->previous_texture_page)
#define s_translucent_pool          (g_rs->translucent_pool)
#define s_translucent_head          (g_rs->translucent_head)
#define s_translucent_free          (g_rs->translucent_free)
#define s_translucent_count         (g_rs->translucent_count)
#define s_depth_buckets             (g_rs->depth_buckets)
#define s_depth_entries             (g_rs->depth_entries)
#define s_depth_entry_count         (g_rs->depth_entry_count)
#define s_imm_verts                 (g_rs->imm_verts)
#define s_imm_indices               (g_rs->imm_indices)
#define s_imm_vert_count            (g_rs->imm_vert_count)
#define s_imm_index_count           (g_rs->imm_index_count)
#define s_deferred_add_verts        (g_rs->deferred_add_verts)
#define s_deferred_add_indices      (g_rs->deferred_add_indices)
#define s_deferred_add_vert_count   (g_rs->deferred_add_vert_count)
#define s_deferred_add_index_count  (g_rs->deferred_add_index_count)
#define s_deferred_add_batches      (g_rs->deferred_add_batches)
#define s_deferred_add_batch_count  (g_rs->deferred_add_batch_count)
#define s_deferred_add_active       (g_rs->deferred_add_active)
#define s_scene_has_renderer_geometry (g_rs->scene_has_renderer_geometry)
#define s_in_sky_draw               (g_rs->in_sky_draw)
#define s_in_reflection_overlay     (g_rs->in_reflection_overlay)
#define s_td6_car_zbias             (g_rs->td6_car_zbias)
#define s_actor_light_zone          (g_rs->actor_light_zone)

/* ------------------------------------------------------------------------
 * Shared state (defined in td5_render.c unless noted).
 * ------------------------------------------------------------------------ */
/* TD6 authored :CAR_LIGHTS0/1: taillight hardpoints. Written by the asset
 * loader via td5_render_set_vehicle_taillights + invalidated in mesh prepare
 * (core); read by render_vehicle_brake_lights (effects TU). */
extern int16_t g_vehicle_taillight[TD5_ACTOR_MAX_TOTAL_SLOTS][2][3];
extern int     g_vehicle_taillight_valid[TD5_ACTOR_MAX_TOTAL_SLOTS];
/* Sky/billboard animation state + the per-slot vehicle mesh table: written by
 * the core (init/reset, mesh registration), read/advanced by the effects TU. */
extern int32_t s_sky_rotation_angle;
extern int32_t s_billboard_anim_phase;
extern TD5_MeshHeader *s_vehicle_meshes[TD5_ACTOR_MAX_TOTAL_SLOTS];

/* ------------------------------------------------------------------------
 * Core-provided helpers the effects TU calls.
 * ------------------------------------------------------------------------ */
/* Project one world point (raw 24.8 fixed-point, same space as s_camera_pos)
 * into a pretransformed screen-space vertex. Returns 0 if behind near clip.
 * (Defined next to the debug line overlay in td5_render.c.) */
int debug_line_project(float wx, float wy, float wz, uint32_t argb,
                       TD5_D3DVertex *out);
/* Clip + submit a polygon through the projection pipeline (core). */
void clip_and_submit_polygon(TD5_MeshVertex *vert_data, int vert_count,
                             int tex_page);
/* TD6 banner sibling-group screen-center X (banner align helper, core). */
float td6_banner_group_center_x(uint32_t *block, int count,
                                float my_z, float my_x);
/* Daylight lighting override (mesh TU; track-lighting fallback calls it). */
void td5_render_set_override_daylight(void);
/* Flush the pending immediate batch (per-actor alpha changes need it). */
void flush_immediate_internal(void);
/* Fallback collision-ribbon road + relocated drag finish gantry: defined in
 * td5_render.c, called from the span display-list walk (mesh TU). */
void td5_render_fallback_strip_ribbon(int center_span, int window,
                                      int ring, int total_spans,
                                      int is_circuit, int min_span, int max_span);
void td5_render_drag_finish_line(void);

/* Mesh-TU API consumed by the core / effects TUs (td5_render_mesh.c). */
void td5_render_drag_stadium_extension(void);
void td5_render_apply_page_blend_preset(int page_id);
/* Per-actor draw alpha applied by the immediate emitters. */
extern int s_actor_draw_alpha;

/* ------------------------------------------------------------------------
 * Mesh-TU seam (P1-C step 2): scene/texture-cache/banner/deform/projection-
 * effect state + helpers shared between td5_render.c and td5_render_mesh.c.
 * All defined in td5_render.c.
 * ------------------------------------------------------------------------ */
#define TT_GHOST_ALPHA 130
#define TD6_CAR_ZFIX_PULL_VIEWZ   (3.0f)   /* toward-camera, view-z units        */
#define ENVMAP_TEXTURE_PAGE_BASE 990
#define ENVMAP_MAX_PAGES         4
typedef void (*PrimDispatchFn)(TD5_PrimitiveCmd *cmd, TD5_MeshVertex *base_verts);
extern const PrimDispatchFn s_dispatch_table[7];

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

extern TextureCacheSlot s_texture_cache[TEXTURE_CACHE_SLOTS];
extern int              s_texture_cache_active_count;
extern int s_camera_prebaked;
extern int s_debug_fallback_log_count;
extern int s_debug_clip_near_rejects;
extern int s_debug_clip_backface_rejects;
extern int s_debug_clip_screen_rejects;
extern int s_debug_clip_emitted_tris;
extern int s_debug_prepared_mesh_calls;
extern int s_debug_texture_bind_calls;
extern int s_debug_texture_cache_hits;
extern int s_debug_texture_cache_misses;
extern int s_debug_texture_cache_evictions;
extern int s_debug_span_meshes_submitted;
extern uint32_t s_vehicle_tint[TD5_ACTOR_MAX_TOTAL_SLOTS];
extern int s_vehicle_is_td6[TD5_ACTOR_MAX_TOTAL_SLOTS];
extern int s_photobooth_active;
extern ProjectionEffectState s_proj_effect[TD5_ACTOR_MAX_TOTAL_SLOTS];
extern int  s_proj_effect_mode;
extern int  s_envmap_page_count;
extern int  s_envmap_pages[ENVMAP_MAX_PAGES];
extern int  s_environs_level;
extern const int s_vehicle_reflection_overlay_enabled;
extern float s_hk_clip_y;
extern int s_level_pass_active;
extern int s_banner_cull;
extern int s_banner_cull_keep_pos;
extern int s_banner_cull_revflip;
extern int s_native_banner_keep_pos;  /* [NATIVE BANNERS] kept winding sign for detected native banner pages */
extern int s_banner_align;
extern int s_banner_align_log;
extern float s_banner_vshift_x;
extern float s_drag_road_scale;
extern float s_dl_z_offset;
extern TD5_MeshHeader *s_drag_gantry_mesh;
extern const float *s_deform_dx;
extern const float *s_deform_dy;
extern const float *s_deform_dz;
extern int          s_deform_count;

int td6_car_zfix_enabled(void);
int traffic_recover_smoke_enabled(void);
void render_vehicle_reflection_overlay(TD5_MeshHeader *mesh, int slot);
void mat3x3_mul(const float *a, const float *b, float *out);
int td6_mesh_uses_banner_page(const TD5_MeshHeader *mesh);
int td6_banner_roadcenter_x(float ref_x, float ref_z, float *out_rx);
TD5_MeshHeader *td5_render_drag_gantry(void);
void update_render_camera_from_game(void);
void td5_render_set_actor_effect_tint(uint32_t argb);

static inline TD5_MeshVertex *rs_vtx_rebase(void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    if (b >= g_rs->vtx_src_base && b < g_rs->vtx_src_end)
        return (TD5_MeshVertex *)((uint8_t *)g_rs->vtx_work + (b - g_rs->vtx_src_base));
    return (TD5_MeshVertex *)p;
}

static inline int clampi(int x, int lo, int hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}


/* ------------------------------------------------------------------------
 * Effects-TU API consumed by the core dispatch (td5_render_effects.c). The
 * td5_render_* sky/billboard/arcade/prop entry points stay public in
 * td5_render.h.
 * ------------------------------------------------------------------------ */
void render_vehicle_shadow_quad(const TD5_Actor *actor);
void render_vehicle_wheel_billboards(TD5_Actor *actor, int slot);
void render_vehicle_wheels_unified(TD5_Actor *actor, int slot);  /* wheel overhaul */
int  wheel_overhaul_enabled(void);
int  wheel_traffic_enabled(void);
void render_vehicle_brake_lights(const TD5_Actor *actor, int slot);
void render_vehicle_headlights(const TD5_Actor *actor, int slot);
void render_tracked_actor_marker(const TD5_Actor *actor,
                                 const TD5_Mat3x3 *body_rot,
                                 const TD5_Vec3f *body_pos,
                                 int32_t intensity);

#endif /* TD5_RENDER_INTERNAL_H */
