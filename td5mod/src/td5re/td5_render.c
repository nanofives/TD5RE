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
#include "td5_rcmd.h"   /* Phase B render-transform: per-pane CPU command recording */
#include "td5_profile.h"
#include "td5_track.h"
#include "td5_game.h"
#include "td5_asset.h"
#include "td5_save.h"
#include "td5_vfx.h"
#include "td5_arcade.h"   /* ARCADE power-up pad / hazard world billboards */
#include "td5_damage.h"   /* [CAR DAMAGE] per-vertex deformation deltas */
#include "td5_ai.h"
#include "td5_light.h"    /* [DYNAMIC LIGHTS] world-space point-light registry */
#include "td5_config.h"   /* shared TD5RE_* env-knob accessors */
#include "td5re.h"

#include "../../../re/include/td5_actor_struct.h"

#include "td5_render_internal.h"  /* PRIVATE core<->effects seam */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <string.h>

extern uint32_t g_tick_counter;

/* [P1-C] Constants, pool typedefs, the RenderScratch struct and the s_* field
 * shims moved VERBATIM to td5_render_internal.h (shared with
 * td5_render_effects.c). The static variable definitions stay here: */
static uint32_t s_fog_color;
static int      s_fog_enabled;
TextureCacheSlot s_texture_cache[TEXTURE_CACHE_SLOTS];
int              s_texture_cache_active_count;
static uint32_t s_color_lut[1024];
static RenderScratch        g_rs_default;
__thread RenderScratch *g_rs = &g_rs_default;   /* extern decl in td5_render_internal.h */

/* [Phase B Stage 2b] Per-pane RenderScratch pool. Workers bind their pane's
 * instance (this thread's g_rs); the serial/main path uses g_rs_default. The
 * pool is allocated once, lazily, and never freed (bounded, reused each race). */
static RenderScratch *s_rs_pool[TD5_MAX_VIEWPORTS];

/* A calloc'd RenderScratch is all-zero, but several of its structures are
 * linked lists / caches that MUST start -1-terminated (a zero free-list is a
 * self-referential cycle -> infinite loop on first traversal). Mirror the
 * one-time startup init that g_rs_default received. Runs on the main thread
 * during pool creation (single-threaded), temporarily pointing g_rs at the
 * instance so the field shims address it. */
static void rs_init_instance(RenderScratch *rs)
{
    RenderScratch *save = g_rs;
    g_rs = rs;

    /* (texture cache is shared, not per-instance — initialized elsewhere) */

    /* Translucent pipeline: builds the -1-terminated free list + empties the
     * sorted list (head = -1). */
    td5_render_init_translucent_pipeline();

    /* Depth-sort buckets: -1 = empty bucket. */
    for (int i = 0; i < DEPTH_BUCKET_COUNT; i++)
        s_depth_buckets[i] = -1;
    s_depth_entry_count = 0;

    /* Texture-page override: -1 = none (0 is a valid page, so calloc-zero
     * would silently force every command onto page 0). */
    g_rs->tex_page_override = -1;

    g_rs = save;
}

int td5_render_scratch_pool_ensure(int count)
{
    if (count > TD5_MAX_VIEWPORTS) count = TD5_MAX_VIEWPORTS;
    for (int i = 0; i < count; i++) {
        if (!s_rs_pool[i]) {
            s_rs_pool[i] = (RenderScratch *)calloc(1, sizeof(RenderScratch));
            if (!s_rs_pool[i]) return 0;
            rs_init_instance(s_rs_pool[i]);
        }
    }
    return 1;
}

void td5_render_scratch_bind(int index)
{
    g_rs = (index >= 0 && index < TD5_MAX_VIEWPORTS && s_rs_pool[index])
           ? s_rs_pool[index] : &g_rs_default;
}

void td5_render_scratch_unbind(void)
{
    g_rs = &g_rs_default;
}

/* --- [Phase B parallel-build] per-pane vertex workspace helpers --- */

/* Grow this pane's workspace to hold `count` vertices. Returns 0 on alloc
 * failure (caller falls back to legacy in-place blob writes — correct for the
 * serial path, never expected to fail in practice). */
static int rs_vtx_workspace_ensure(int count)
{
    if (g_rs->vtx_work && count <= g_rs->vtx_work_cap) return 1;
    int cap = g_rs->vtx_work_cap > 0 ? g_rs->vtx_work_cap : 1024;
    while (cap < count) cap *= 2;
    TD5_MeshVertex *nw = (TD5_MeshVertex *)malloc((size_t)cap * sizeof(TD5_MeshVertex));
    if (!nw) return 0;
    free(g_rs->vtx_work);
    g_rs->vtx_work     = nw;
    g_rs->vtx_work_cap = cap;
    return 1;
}

/* Translate a vertex pointer derived from the CURRENT mesh's blob range into
 * this pane's workspace copy. Pointers outside the range (a mesh that never
 * went through transform_mesh_vertices, or the alloc-failure fallback) pass
 * through unchanged — identical to the legacy in-place behavior. */
/* rs_vtx_rebase moved to td5_render_internal.h (static inline) */

/* [Phase B Stage 2b] When set, render_actors_for_view / configure_projection do
 * NOT refresh the render camera from the camera module's single "current"
 * snapshot (td5_camera_get_basis/position) — the per-pane camera was already
 * baked into each g_rs[vp] by the serial Phase-1 pass. Without this, every pane
 * would re-read the SAME current camera (the last pane applied) and show an
 * identical view. Read-only on worker threads during the render phase. */
int s_camera_prebaked = 0;
void td5_render_set_camera_prebaked(int on) { s_camera_prebaked = on ? 1 : 0; }

/* ========================================================================
 * Sky Rotation (12-bit fixed angle, +0x400 per tick)
 *
 * Original: DAT_004bf500
 * ======================================================================== */

int32_t s_sky_rotation_angle;          /* extern: effects TU draws/rotates the sky (seam header) */

/* ========================================================================
 * Billboard Animation State
 * ======================================================================== */

int32_t s_billboard_anim_phase;        /* extern: effects TU advances billboard anims (seam header) */

/* ========================================================================
 * Race render initialized flags
 * ======================================================================== */

static int s_globals_initialized;   /* DAT_0048dba8 */
static int s_state_active;          /* DAT_0048dba0 */
/* s_scene_has_renderer_geometry moved to RenderScratch (Phase B Stage 1). */
int s_debug_fallback_log_count;
static int s_debug_clip_log_count;
int s_debug_clip_near_rejects;
int s_debug_clip_backface_rejects;
int s_debug_clip_screen_rejects;
int s_debug_clip_emitted_tris;
int s_debug_prepared_mesh_calls;
static int s_debug_append_calls;
static int s_debug_flush_calls;
static int s_debug_flush_submitted_tris;
int s_debug_texture_bind_calls;
int s_debug_texture_cache_hits;
int s_debug_texture_cache_misses;
int s_debug_texture_cache_evictions;
static int s_debug_scene_draw_calls;
int s_debug_span_meshes_submitted;
TD5_MeshHeader *s_vehicle_meshes[TD5_ACTOR_MAX_TOTAL_SLOTS];  /* extern: effects TU (wheels/shadow) reads it (seam header) */

/* Per-slot paint TINT (0xRRGGBB). 0 = unset = white = identity (TD5 cars and AI
 * are unaffected, byte-for-byte). A non-white tint is applied by
 * td5_render_compute_vertex_lighting to color a GRAYSCALE body (ported TD6 cars):
 * the chosen color multiplies the per-vertex luminance. Reset on every mesh load;
 * the game sets the player's selected color after loading a TD6 car. */
uint32_t s_vehicle_tint[TD5_ACTOR_MAX_TOTAL_SLOTS];

void td5_render_set_vehicle_tint(int slot, uint32_t rgb)
{
    if (slot < 0 || slot >= TD5_ACTOR_MAX_TOTAL_SLOTS) return;
    s_vehicle_tint[slot] = rgb & 0x00FFFFFFu;
}

/* [dynamic-traffic] Whole-actor draw fade (0..255, 255 = opaque/normal).
 * Set per actor around its draws in td5_render_actors_for_view; consumed by
 * flush_immediate_internal (vertex alpha scale + preset remap) and by the
 * direct-draw car accessories (shadow, wheels, brake lights, reflection).
 * MUST flush the pending immediate batch on change — the batch can otherwise
 * carry one actor's triangles into the next actor's alpha. */
int s_actor_draw_alpha = 255;   /* extern: effects TU emitters read it (seam header) */

void td5_render_set_actor_draw_alpha(int alpha)
{
    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    if (alpha == s_actor_draw_alpha) return;
    flush_immediate_internal();
    s_actor_draw_alpha = alpha;
}

/* [ARCADE 2026-06-26] Per-actor effect-glow tint (0 = none). When set, the
 * actor's body vertices are ADDITIVELY brightened toward this ARGB colour in
 * flush_immediate_internal, so the car SILHOUETTE glows in the power-up colour
 * (the alpha byte is the glow intensity 0..255). Bracket the body draw with it
 * like s_actor_draw_alpha; MUST flush on change so the tint can't bleed into the
 * next actor's triangles. */
static uint32_t s_actor_effect_tint = 0;
void td5_render_set_actor_effect_tint(uint32_t argb)
{
    if (argb == s_actor_effect_tint) return;
    flush_immediate_internal();
    s_actor_effect_tint = argb;
}

/* [MP GAME MODES: TIME TRIAL 2026-06-22] Non-owner players render translucent
 * ("ghost") so the player pass-through reads visually. Knob TD5RE_TT_GHOST=0
 * keeps opponents fully opaque. */
/* #define TT_GHOST_ALPHA moved to td5_render_internal.h */
int tt_ghost_enabled(void)
{
    static int knob = -1;
    if (knob < 0) {
        knob = td5_env_flag_on("TD5RE_TT_GHOST");   /* default ON */
    }
    return knob;
}

/* Per-slot "this is a ported TD6 car" flag. TD6 cars have a grayscale body and
 * no meaningful env-map reflection mesh (envmodel.dat is unused), so the
 * TD5-faithful chrome/projection reflection overlay must NOT run on them: in a
 * planar-scroll light zone (e.g. the Australia bridge/tunnel) it paints the
 * scrolling environs "lights" texture onto the grayscale body — the user's
 * "lights shader over new cars". Reset on every mesh swap. */
int s_vehicle_is_td6[TD5_ACTOR_MAX_TOTAL_SLOTS];

void td5_render_set_vehicle_is_td6(int slot, int is_td6)
{
    if (slot < 0 || slot >= TD5_ACTOR_MAX_TOTAL_SLOTS) return;
    s_vehicle_is_td6[slot] = is_td6 ? 1 : 0;
}

/* [task#21] TD6 car body z-fight fix. Ported TD6 cars are transcoded to a SINGLE
 * de-indexed triangle list (td5_asset_transcode_td6_mesh: one dispatch_type=0
 * command). The artist mesh has interior trim / glass surrounds modelled
 * coplanar with the painted outer shell, and the engine draws them all in one
 * opaque pass with CullMode=NONE. Those coplanar faces tie on the depth test;
 * with sub-tick camera/body interpolation jittering each face's projected depth
 * by a different sub-LSB amount every frame, the LEQUAL winner flips frame to
 * frame -> the reported "internal geometry pokes through the body and flickers".
 *
 * Render-side fix (no per-face metadata needed, order-independent): while a TD6
 * car body is drawn we (1) SNAP each vertex depth to a fine grid so faces whose
 * true depths are within one grid step collapse to the SAME depth and resolve by
 * stable submission order (deterministic across frames -> no flicker), and (2)
 * pull the whole body a hair toward the camera so the shell wins its tie against
 * the geometry immediately behind it. The grid step is tiny vs the car's depth
 * extent (a car spans ~2.5e-5 in normalized depth in a chase cam, the step is
 * ~1.5e-6) so legitimately-separated faces stay distinct, and the body-vs-ground
 * depth gap is far larger than the step+pull so the body never sinks into the
 * road. A/B: TD5RE_TD6_CAR_ZFIX=0 disables it. */
/* #define TD6_CAR_ZFIX_PULL_VIEWZ moved to td5_render_internal.h */
#define TD6_CAR_ZFIX_SNAP_VIEWZ   (0.20f)  /* depth snap grid, view-z units      */
int td6_car_zfix_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("TD5RE_TD6_CAR_ZFIX");
        cached = (e && e[0] == '0' && e[1] == '\0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "TD6 car body z-fix %s (pull=%.1f snap=%.2f view-z)",
                  cached ? "ON" : "OFF",
                  (double)TD6_CAR_ZFIX_PULL_VIEWZ, (double)TD6_CAR_ZFIX_SNAP_VIEWZ);
    }
    return cached;
}

/* [TRAFFIC CRASH SMOKE 2026-06-21] Per user spec: a TRAFFIC/cop car must smoke
 * ONLY when it has taken a FATAL hit (it becomes a broken-down wreck). Default
 * OFF: traffic does NOT emit the incidental engine-rev puff + wheelspin smoke.
 * Those two emitters fire whenever a car's wheels slip or its RPM sits in the
 * puff window — which a traffic car hits while tumbling through a NON-fatal
 * crash-spin, then drops the instant it recovers and grips again, producing the
 * reported "smoke during the collision, gone after recovery". With this off a
 * recovering traffic car shows nothing; only a totalled one keeps its wreck
 * plume. Set TD5RE_TRAFFIC_RECOVER_SMOKE=1 to restore the old always-on
 * incidental traffic smoke for A/B. Racers (player + AI opponents) are never
 * gated by this — their incidental smoke stays faithful. */
int traffic_recover_smoke_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("TD5RE_TRAFFIC_RECOVER_SMOKE");
        cached = (e && e[0] == '1' && e[1] == '\0') ? 1 : 0;
        TD5_LOG_I(LOG_TAG, "traffic recover-smoke %s -> traffic smokes only-when-fatal=%s",
                  cached ? "ON (legacy incidental smoke)" : "OFF",
                  cached ? "no" : "yes");
    }
    return cached;
}

/* [S23] Per-slot authored rear/brake-light positions (model space, int16[3] ×2).
 * Ported TD6 cars carry WRONG taillight values in the binary carparam.dat at
 * +0x60/+0x68 — that is NOT TD6's brake-light field. TD6.exe instead reads the
 * authored :CAR_LIGHTS0/1: positions from each car's param.scr (CONFIRMED:
 * TD6.exe contains the "CAR_LIGHTS"/"param.scr" parser strings; .scr values
 * differ from the binary +0x60 in 28/39 cars). The asset loader installs those
 * authored positions here when it loads a TD6 car; render_vehicle_brake_lights
 * uses them in preference to the cardef hardpoint. valid=0 → fall back to cardef
 * (TD5 cars + donor-param TD6 cars aud/pro/xjr that have no .scr). */
/* (arrays shared with td5_render_effects.c via td5_render_internal.h) */
int16_t g_vehicle_taillight[TD5_ACTOR_MAX_TOTAL_SLOTS][2][3];
int     g_vehicle_taillight_valid[TD5_ACTOR_MAX_TOTAL_SLOTS];

void td5_render_set_vehicle_taillights(int slot, const int16_t *l0, const int16_t *l1)
{
    if (slot < 0 || slot >= TD5_ACTOR_MAX_TOTAL_SLOTS) return;
    if (!l0 || !l1) { g_vehicle_taillight_valid[slot] = 0; return; }
    for (int i = 0; i < 3; i++) {
        g_vehicle_taillight[slot][0][i] = l0[i];
        g_vehicle_taillight[slot][1][i] = l1[i];
    }
    g_vehicle_taillight_valid[slot] = 1;
}

/* Photo-booth mode: render ONLY the player car (skip sky + track spans here, and
 * VFX/HUD/clear in the game frame) over a chroma background, for offline preview
 * (carpic) generation. Reuses the normal chase camera (frozen car at spawn). */
int s_photobooth_active = 0;
void td5_render_set_photobooth(int on) { s_photobooth_active = on ? 1 : 0; }
int  td5_render_photobooth_active(void) { return s_photobooth_active; }

/* ========================================================================
 * Vehicle Projection Effect / Chrome Reflection (0x43DEC0 / 0x40CBD0)
 *
 * Original renders a second pass on the player car mesh with heading-
 * rotated UV coordinates sampling environment textures (environs.zip).
 * Mode 2 = chrome/specular reflection on car bodies.
 *
 * Effect state per-slot: heading cos/sin, sub-mode, texture page.
 * ======================================================================== */

/* D3D page IDs for environs textures. MUST NOT overlap the frontend surface
 * pool (FE_SURFACE_PAGE_BASE=900, 31 pages → 900-930): they did, so loading a
 * level's environs/projection textures for a race clobbered the menu background
 * surface, and only the (now-removed) per-frame frontend surface recovery hid
 * it. Moved to 990-993, clear of the frontend pool and within MAX_TEXTURE_PAGES
 * (1024). */
/* #define ENVMAP_TEXTURE_PAGE_BASE moved to td5_render_internal.h */
/* #define ENVMAP_MAX_PAGES moved to td5_render_internal.h */

/* ProjectionEffectState typedef moved to td5_render_internal.h */

ProjectionEffectState s_proj_effect[TD5_ACTOR_MAX_TOTAL_SLOTS];
int  s_proj_effect_mode;   /* 0=disabled, 2=enabled (g_vehicleProjectionEffectMode @ 0x4C3D44) */
int  s_envmap_page_count;  /* number of uploaded environs textures */
int  s_envmap_pages[ENVMAP_MAX_PAGES]; /* D3D page IDs, indexed 0..count-1 by entry */
int  s_environs_level;     /* level_number used to key the per-track tables */
/* [unified car look] The chrome/env-map "mode 2" reflection overlay is disabled
 * for ALL cars (it painted scrolling environs "lights" onto the TD6 grayscale
 * bodies). File-scope so the loader also skips fetching the now-never-drawn
 * environs reflection textures. Flip to 1 AND restore re/assets/environs/ (re-
 * extractable from original/) to revert the all-racer chrome. */
const int s_vehicle_reflection_overlay_enabled = 0;

/* Per-actor light-zone index, mirroring actor->field_0x377 in the original.
 * ApplyTrackLightingForVehicleSegment @ 0x00430150 walks the per-track zone
 * array forward/backward each frame based on the actor's track_span_raw, so
 * we persist the last-known index per slot across frames. */
/* s_actor_light_zone moved to RenderScratch (per-pane): update_actor_light_zone
 * writes it every frame during the actor render, so under the threaded build all
 * panes would write the same slot concurrently (flicker). Per-pane copies start
 * at 0 (calloc), matching the level-load reset; hysteresis stays per-pane. */

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
/* [P1-C SPLIT step 1, 2026-07-02] shadow/wheel/brake-light/headlight/marker
 * renderers moved to td5_render_effects.c (decls in td5_render_internal.h). */
/* fwd decl removed: render_vehicle_reflection_overlay now extern (td5_render_internal.h) */

/** 7-entry dispatch table matching original at 0x473b9c */
/* PrimDispatchFn typedef moved to td5_render_internal.h */

const PrimDispatchFn s_dispatch_table[7] = {
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

void mat3x3_mul(const float *a, const float *b, float *out)
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
/* clampi moved to td5_render_internal.h (static inline) */

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
/* fwd decl removed: td5_render_apply_page_blend_preset now extern (seam header) */

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
void flush_immediate_internal(void)
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

    /* [dynamic-traffic] Whole-actor fade: scale every vertex's diffuse alpha
     * by the current actor fade (the VEHICLE_FADE preset blends on it). For
     * type-3 (additive) pages alpha is meaningless (ONE/ONE) — scale RGB
     * instead so glows dim with the body. Runs BEFORE the deferred-additive
     * stash below so deferred batches carry the faded colors. */
    if (s_actor_draw_alpha < 255) {
        uint32_t fade = (uint32_t)s_actor_draw_alpha;
        int additive = (s_current_texture_page >= 0 &&
                        td5_asset_get_page_transparency(s_current_texture_page) == 3);
        for (int i = 0; i < s_imm_vert_count; i++) {
            uint32_t d = s_imm_verts[i].diffuse;
            if (additive) {
                uint32_t r = (((d >> 16) & 0xFFu) * fade) >> 8;
                uint32_t g = (((d >>  8) & 0xFFu) * fade) >> 8;
                uint32_t b = (((d      ) & 0xFFu) * fade) >> 8;
                s_imm_verts[i].diffuse = (d & 0xFF000000u) | (r << 16) | (g << 8) | b;
            } else {
                uint32_t a = (((d >> 24) & 0xFFu) * fade) >> 8;
                s_imm_verts[i].diffuse = (d & 0x00FFFFFFu) | (a << 24);
            }
        }
    }

    /* [ARCADE] effect-glow tint: make the car SILHOUETTE clearly glow in the
     * power-up colour. Two steps per body vertex: (1) COLORIZE — replace the body
     * colour with the effect hue, scaled by the vertex's own brightness (so the
     * car's shape/shading is kept) with a floor so dark panels still read the
     * colour; (2) add a pulsing GLOW on top (the tint's alpha is the strength).
     * Runs after fade, only set around a car's body draw, so nothing else tints. */
    if (s_actor_effect_tint & 0xFF000000u) {
        uint32_t inten = (s_actor_effect_tint >> 24) & 0xFFu;
        uint32_t tr = (s_actor_effect_tint >> 16) & 0xFFu;
        uint32_t tg = (s_actor_effect_tint >>  8) & 0xFFu;
        uint32_t tb =  s_actor_effect_tint        & 0xFFu;
        for (int i = 0; i < s_imm_vert_count; i++) {
            uint32_t d = s_imm_verts[i].diffuse;
            uint32_t a = (d >> 24) & 0xFFu;
            uint32_t r = (d >> 16) & 0xFFu, g = (d >> 8) & 0xFFu, b = d & 0xFFu;
            uint32_t luma = (r * 77u + g * 150u + b * 29u) >> 8;   /* 0..255 */
            uint32_t base = 96u + (luma * 159u) / 255u;            /* 96..255 */
            uint32_t cr = (tr * base) / 255u + (tr * inten) / 255u;
            uint32_t cg = (tg * base) / 255u + (tg * inten) / 255u;
            uint32_t cb = (tb * base) / 255u + (tb * inten) / 255u;
            if (cr > 255u) cr = 255u;
            if (cg > 255u) cg = 255u;
            if (cb > 255u) cb = 255u;
            s_imm_verts[i].diffuse = (a << 24) | (cr << 16) | (cg << 8) | cb;
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

#ifndef TD5RE_RELEASE
/* ------------------------------------------------------------------------
 * Debug INSPECTION CAMERA (dev builds only) — reusable testing override.
 *
 * Replaces the gameplay chase/trackside view with a fixed-angle orbit that
 * always frames a chosen actor (default: the player car), so wheels / body /
 * shadow can be inspected from a clean side or top-down angle no matter how
 * the normal camera would frame them. Updated every frame so the view tracks
 * the car while it drives; works while stationary too (e.g. on the grid).
 *
 * Enable + tune entirely via environment variables — no rebuild to retune:
 *   TD5RE_INSPECT_CAM  = 1        enable the override
 *   TD5RE_INSPECT_AZ   = <deg>    azimuth around the car, WORLD-relative
 *                                 (rotates which side faces you)   [def 90]
 *   TD5RE_INSPECT_EL   = <deg>    elevation: 0 = level side view,
 *                                 90 = straight-down top view       [def 30]
 *   TD5RE_INSPECT_DIST = <units>  camera distance in render units   [def 1600]
 *   TD5RE_INSPECT_SLOT = <0..5>   which actor to frame              [def 0]
 *
 * Render units == world_pos/256 (the same space as s_camera_pos). The player
 * car body is ~300-500 units long; wheel rim radius is ~115.
 *
 * Handedness: the projection is screen = -(basis·delta)*focal/vz, so building
 * right = up_ref x forward and up = forward x right yields a correct,
 * non-mirrored, right-side-up image (verified by capture).
 * ------------------------------------------------------------------------ */
static int   s_inspect_loaded  = 0;
static int   s_inspect_enabled  = 0;
static float s_inspect_az_deg   = 90.0f;
static float s_inspect_el_deg   = 30.0f;
static float s_inspect_dist     = 1600.0f;
static int   s_inspect_slot     = 0;

static void inspect_cam_load_env(void)
{
    s_inspect_loaded = 1;
    const char *e = getenv("TD5RE_INSPECT_CAM");
    s_inspect_enabled = (e && e[0] && e[0] != '0');
    if (!s_inspect_enabled)
        return;
    const char *az = getenv("TD5RE_INSPECT_AZ");
    const char *el = getenv("TD5RE_INSPECT_EL");
    const char *ds = getenv("TD5RE_INSPECT_DIST");
    const char *sl = getenv("TD5RE_INSPECT_SLOT");
    if (az && az[0]) s_inspect_az_deg = (float)atof(az);
    if (el && el[0]) s_inspect_el_deg = (float)atof(el);
    if (ds && ds[0]) s_inspect_dist  = (float)atof(ds);
    if (sl && sl[0]) s_inspect_slot  = atoi(sl);
    /* [wheel-overhaul dev] allow framing traffic slots too (was capped at 5). */
    if (s_inspect_slot < 0 || s_inspect_slot >= TD5_ACTOR_MAX_TOTAL_SLOTS)
        s_inspect_slot = 0;
    TD5_LOG_W(LOG_TAG, "InspectCam ON: slot=%d az=%.1f el=%.1f dist=%.1f",
              s_inspect_slot, s_inspect_az_deg, s_inspect_el_deg, s_inspect_dist);
}

static void apply_inspection_camera(void)
{
    TD5_Actor *a = td5_game_get_actor(s_inspect_slot);
    if (!a)
        return;

    /* Target the SAME sub-tick-extrapolated position the body mesh is drawn at
     * (td5_render.c:2479, faithful to orig 0x40C164):
     *   render = (world_pos + linear_velocity * g_subTickFraction) / 256
     * Using raw world_pos here would snap the camera to the 30 Hz sim tick
     * while the car body slides forward each render frame, producing the
     * speed-dependent "sawtooth" shake the faithful chase camera avoids the
     * same way (td5_camera.c:1236-1295, td5_game.c:3587-3596). */
    extern float g_subTickFraction;
    float frac = g_subTickFraction;
    /* X/Z: velocity-extrapolate to match the body mesh (keeps the car centred
     * horizontally, lag-free). Y: sub-tick INTERPOLATE between sim ticks rather
     * than velocity-extrapolate, so the suspension-settle snap + per-tick velY
     * jitter don't bob the inspection view at high FPS — same rationale as the
     * chase-cam fix in td5_camera.c finalize_chase_pos. */
    float tx = ((float)a->world_pos.x + (float)a->linear_velocity_x * frac) * (1.0f / 256.0f);
    float tz = ((float)a->world_pos.z + (float)a->linear_velocity_z * frac) * (1.0f / 256.0f);
    static int      s_iy_init     = 0;
    static int      s_iy_prev     = 0;
    static int      s_iy_cur      = 0;
    static uint32_t s_iy_lastTick = 0;
    int wy = a->world_pos.y;
    if (!s_iy_init) {
        s_iy_init = 1; s_iy_prev = wy; s_iy_cur = wy;
        s_iy_lastTick = (uint32_t)g_td5.simulation_tick_counter;
    } else if ((uint32_t)g_td5.simulation_tick_counter != s_iy_lastTick) {
        s_iy_lastTick = (uint32_t)g_td5.simulation_tick_counter;
        s_iy_prev = s_iy_cur; s_iy_cur = wy;
    }
    { int dyj = wy - s_iy_prev; if (dyj > 0x40000 || dyj < -0x40000) { s_iy_prev = wy; s_iy_cur = wy; } }
    float ty = ((float)s_iy_prev + (float)(wy - s_iy_prev) * frac) * (1.0f / 256.0f);

    float az = s_inspect_az_deg * ((float)M_PI / 180.0f);
    float el = s_inspect_el_deg * ((float)M_PI / 180.0f);
    float ce = cosf(el), se = sinf(el);

    /* Unit offset from the target to the camera (world space, +Y up). */
    float ox = ce * sinf(az);
    float oy = se;
    float oz = ce * cosf(az);

    s_camera_pos[0] = tx + ox * s_inspect_dist;
    s_camera_pos[1] = ty + oy * s_inspect_dist;
    s_camera_pos[2] = tz + oz * s_inspect_dist;

    /* forward = camera -> target (points into the screen, +vz). */
    float fx = -ox, fy = -oy, fz = -oz;
    float fl = sqrtf(fx * fx + fy * fy + fz * fz);
    if (fl < 1e-6f)
        return;
    fx /= fl; fy /= fl; fz /= fl;

    /* Up reference; swap to world +Z when looking near-vertical (top-down)
     * so the cross product stays well-conditioned. */
    float ux = 0.0f, uy = 1.0f, uz = 0.0f;
    if (fy > 0.99f || fy < -0.99f) { ux = 0.0f; uy = 0.0f; uz = 1.0f; }

    /* right = up_ref x forward */
    float rx = uy * fz - uz * fy;
    float ry = uz * fx - ux * fz;
    float rz = ux * fy - uy * fx;
    float rl = sqrtf(rx * rx + ry * ry + rz * rz);
    if (rl < 1e-6f)
        return;
    rx /= rl; ry /= rl; rz /= rl;

    /* up = forward x right (already unit length) */
    float upx = fy * rz - fz * ry;
    float upy = fz * rx - fx * rz;
    float upz = fx * ry - fy * rx;

    s_camera_basis[0] = rx;  s_camera_basis[1] = ry;  s_camera_basis[2] = rz;
    s_camera_basis[3] = upx; s_camera_basis[4] = upy; s_camera_basis[5] = upz;
    s_camera_basis[6] = fx;  s_camera_basis[7] = fy;  s_camera_basis[8] = fz;
}
#endif /* TD5RE_RELEASE */

void update_render_camera_from_game(void)
{
    td5_camera_get_basis(&s_camera_basis[0], &s_camera_basis[3], &s_camera_basis[6]);
    td5_camera_get_position(&s_camera_pos[0], &s_camera_pos[1], &s_camera_pos[2]);

    /* [billboard-tree fix 2026-06-15] Snapshot the yaw-stripped camera-secondary
     * basis into this g_rs so the camera-facing billboard branch in
     * td5_render_span_display_list has a valid rotation.
     *
     * ROOT CAUSE: g_rs->camera_secondary (RenderScratch) is zero-initialized and
     * was ONLY populated by td5_render_bake_camera(), which the renderer calls
     * exclusively on the THREADED multi-pane path (td5_game.c, mt_threaded,
     * viewport_count>2). On the SERIAL / single- or double-viewport path (the
     * common case — e.g. Moscow solo) the renderer refreshes the camera via THIS
     * function instead, which never copied the secondary basis. The billboard
     * branch then loaded an all-zero 3x3 into the transform rotation, collapsing
     * every tree quad's 4 vertices onto the single translation point → zero-area
     * quad → rejected by the degenerate-triangle test in clip_and_submit_polygon
     * → trees render INVISIBLE in-scene (while the regular fence/grass meshes,
     * which use s_camera_basis populated above, render fine). Copying the basis
     * here fixes both paths (bake_camera's own copy below becomes redundant but
     * harmless). g_rs-local, so threaded panes still each get their own.
     *
     * Gated by TD5RE_BILLBOARD_TREE_FIX (default ON); set =0 to reproduce the
     * original invisible-tree behaviour for A/B. */
    {
        extern float g_cameraSecondaryUnscaled[9];   /* td5_camera.c (billboard basis) */
        static int s_bb_fix = -1;      /* -1 unread, 0 off, 1 on */
        if (s_bb_fix < 0) {
            s_bb_fix = td5_env_flag_on("TD5RE_BILLBOARD_TREE_FIX");
            TD5_LOG_I(LOG_TAG,
                      "billboard-tree fix: %s (TD5RE_BILLBOARD_TREE_FIX; serial-path "
                      "camera_secondary bake — fixes invisible in-scene trees)",
                      s_bb_fix ? "ON" : "OFF");
        }
        if (s_bb_fix)
            memcpy(s_camera_secondary, g_cameraSecondaryUnscaled, 9 * sizeof(float));
    }

#ifndef TD5RE_RELEASE
    /* Optional debug inspection-camera override (env-gated, dev only). */
    if (!s_inspect_loaded)
        inspect_cam_load_env();
    if (s_inspect_enabled)
        apply_inspection_camera();
#endif
}

/* [Phase B Stage 2b] Public: bake the camera module's CURRENT basis/position into
 * the bound g_rs. The threaded path calls this in the serial camera pass (after
 * td5_camera_apply_view(vp), with g_rs[vp] bound) so each pane's camera is stored
 * per-pane EVERY frame; render_actors then skips its own refresh (s_camera_prebaked)
 * and uses the baked camera, so panes don't all inherit the last applied view. */
void td5_render_bake_camera(void)
{
    extern float g_cameraSecondaryUnscaled[9];   /* td5_camera.c (billboard basis) */
    update_render_camera_from_game();
    /* Snapshot the shared camera-secondary basis (used to orient billboards) into
     * this pane's g_rs so the threaded build doesn't read another pane's value. */
    memcpy(s_camera_secondary, g_cameraSecondaryUnscaled, 9 * sizeof(float));
}

/* [diag] log the bound g_rs's projection inputs (per-pane bake verification). */
void td5_render_log_pane_proj(int vp)
{
    static int s_n = 0;
    if (s_n < 12) {
        TD5_LOG_W(LOG_TAG, "pane %d proj: center=(%.0f,%.0f) vpwh=(%d,%d) focal=%.0f cam=(%.0f,%.0f,%.0f)",
                  vp, s_center_x, s_center_y, s_viewport_width, s_viewport_height, s_focal_length,
                  s_camera_pos[0], s_camera_pos[1], s_camera_pos[2]);
        s_n++;
    }
}

/* Programmatic inspection-camera control — drives the same fixed-angle camera
 * the photo booth uses to frame a car (dev only; no-op in release). Locks out
 * the env loader so the booth's angles aren't overridden. */
void td5_render_set_inspect_cam(int on, float az_deg, float el_deg, float dist, int slot)
{
#ifndef TD5RE_RELEASE
    s_inspect_loaded  = 1;
    s_inspect_enabled = on ? 1 : 0;
    s_inspect_az_deg  = az_deg;
    s_inspect_el_deg  = el_deg;
    s_inspect_dist    = dist;
    s_inspect_slot    = (slot < 0 || slot > 5) ? 0 : slot;
#else
    (void)on; (void)az_deg; (void)el_deg; (void)dist; (void)slot;
#endif
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
/* [#20 HK reverse] When >0, drop any polygon with a vertex above this model-space Y
 * — set ONLY while drawing the in-road HK building submeshes (entry 509 sub 8/10/11)
 * so their WALLS/ROOF go but their road-level faces stay (the submesh bundles the
 * road surface with the building, so dropping the whole submesh holed the road). */
float s_hk_clip_y = 0.0f;

/* [banners] Track SIGN panels (START/FINISH + the numbered 1..N gantries) are
 * flat double-sided meshes: with the global CullMode=NONE both the front and
 * the back face draw back-to-back and z-fight ("clipping"). Cull the
 * away-facing side so only the banner facing the car shows. Scoped to banner
 * TEXTURE PAGES during the level-geometry pass, so road, walls, tunnel
 * interiors, buildings, billboards and cars are never affected (the existing
 * CullMode=NONE behaviour is preserved everywhere else — track and car meshes
 * have opposite winding conventions, so a global cull can't pick one sign).
 * TD6 banner pages are the textures.dir indices == runtime cmd->texture_page_id
 * (the converter preserves them; see k_td6_rev_banner). The screen-winding
 * sign that means "facing away" is set empirically and flippable via env knob
 * (TD5RE_BANNER_CULL=0 disables; TD5RE_BANNER_CULL_FLIP=1 swaps the kept side
 * if banners end up showing their back). */
static const struct { short level, page; } k_td6_banner_pages[] = {
    /* Paris 8 */    {8,164},{8,218},{8,163},{8,188},{8,192},{8,205},
    /* NewYork 9 */  {9,316},{9,317},{9,327},{9,306},{9,312},{9,271},{9,274},{9,303},
    /* Rome 10 */    {10,342},{10,343},{10,353},{10,354},{10,355},{10,357},{10,358},{10,365},{10,366},
    /* HongKong 11 */{11,216},{11,235},{11,215},{11,192},{11,228},{11,236},
    /* London 12 */  {12,59},{12,60},{12,57},{12,58},{12,102},{12,157},{12,158},{12,188},
};
int s_level_pass_active   = 0;  /* set only while drawing level geometry */
int s_banner_cull         = -1; /* -1 uninit; 0 off; 1 on (env TD5RE_BANNER_CULL) */
int s_banner_cull_keep_pos = 0; /* which screen-winding sign faces the camera */
int s_banner_cull_revflip  = -1; /* [#9] auto-flip kept side in reverse (env TD5RE_TD6_BANNER_REVFLIP) */
int s_banner_align         = -1; /* [START-banner align] -1 uninit; 0 off; 1 on (env TD5RE_BANNER_ALIGN) */
int s_banner_align_log     = 0;  /* throttle the per-mesh align log */
/* [START-banner align] Pending world-space X shift applied by
 * td5_render_transform_mesh_vertices to OVERHEAD banner verts only (pos_y above
 * the threshold), so the gantry's ground start-plaza (pos_y~0) is NOT dragged
 * along. Set per banner mesh in the level draw loop, reset to 0 after. */
float s_banner_vshift_x    = 0.0f;
#define TD6_BANNER_OVERHEAD_Y 1250.0f   /* banner panels sit at Y~2500-4000; ground plaza at Y~0 */

/* [DRAG WIDE ROAD 2026-06-28] Lateral (X) scale applied to LEVEL geometry verts
 * during the drag race, so the visible asphalt / borders / start+finish banner
 * physically widen with the field. The drag strip's road meshes are centred at
 * model X=0 (origin (0,0,0), bounding-centre X~0), so scaling pos_x about 0
 * widens the road about its centreline. Scale = field/4 makes the asphalt width
 * match the N-lane navigation strip exactly, so cars sit one-per-lane on real
 * road. 1.0 = no scaling (every non-drag track, and the reset state). Set at the
 * top of the level pass, applied in td5_render_transform_mesh_vertices, reset to
 * 1.0 after the pass so it never leaks into car / sky / HUD transforms. */
float s_drag_road_scale    = 1.0f;

/* [DRAG STADIUM EXTEND] When non-zero, td5_render_span_display_list draws the
 * block shifted by this much render-float Z (added to BOTH the frustum-cull
 * centre and the origin translation). Used by td5_render_drag_stadium_extension()
 * to TILE a clean mid-stadium block (road+stands) down the extended drag straight,
 * since the baked stadium scenery ends at ~span 240. Set→render→reset synchronously
 * around each copy; single-screen drag only (a concurrent split-screen pane could
 * race it — acceptable for now). */
float s_dl_z_offset        = 0.0f;
/* [DRAG FINISH GANTRY] level030's finish gantry mesh (MODELS.DAT dl 26 / sub 0) and
 * its rendered-Z centroid, resolved once per level. Suppressed at its original
 * position and re-drawn at the relocated finish. Defined near the finish helpers. */
TD5_MeshHeader *s_drag_gantry_mesh = NULL;
static float           s_drag_gantry_z    = 0.0f;
/* fwd decl removed: td5_render_drag_gantry now extern (td5_render_internal.h) */

static int td6_is_banner_page(int page)
{
    int lvl = g_active_td6_level;
    size_t i;
    if (lvl <= 0) return 0;
    for (i = 0; i < sizeof(k_td6_banner_pages) / sizeof(k_td6_banner_pages[0]); i++)
        if (k_td6_banner_pages[i].level == lvl && k_td6_banner_pages[i].page == page)
            return 1;
    return 0;
}

/* [LONDON START BANNER 2026-06-23 — road-centre align]
 * TD6 START/FINISH/checkpoint banner gantries are baked into the level geometry
 * as flat meshes with absolute verts (origin=0). London's START gantry (pages
 * 59/60) was authored with its text panels centred at world X=0, but the road
 * centreline at the start line sits well off to one side, so the gantry hangs
 * beside the road and "doesn't read START". FINISH (57/58) is authored over its
 * own road segment and renders fine. Instead of baking a per-track magic offset,
 * shift a banner mesh laterally so its centre lines up with the actual road
 * centre at its longitudinal (Z) position, derived from the engine's own strip
 * span data — self-correcting, so an already-aligned banner resolves to ~0.
 * Only VISIBLE banner meshes reach this (post-frustum-cull) and there are only a
 * handful of gantries on screen, so the per-span scan is cheap. */

/* Does this mesh draw any TD6 banner texture page? Gated to TD6 levels; the
 * <=512-command guard skips the large road/building meshes cheaply (the banner
 * gantries themselves are small meshes). */
int td6_mesh_uses_banner_page(const TD5_MeshHeader *mesh)
{
    const TD5_PrimitiveCmd *cmds;
    int c, n;
    if (g_active_td6_level <= 0) return 0;
    n = mesh->command_count;
    if (n <= 0 || n > 512) return 0;
    cmds = (const TD5_PrimitiveCmd *)(uintptr_t)mesh->commands_offset;
    if (!cmds) return 0;
    for (c = 0; c < n; c++)
        if (td6_is_banner_page(cmds[c].texture_page_id))
            return 1;
    return 0;
}

/* Absolute road-centre X (world units) at the span nearest (ref_x, ref_z). The
 * match is 2D (X AND Z): a START gantry's road span and a far-away doubled-back
 * span can share a Z (London's strip returns to Z~121500 at span 1350, X~-977000),
 * so a Z-only match would pick the wrong one. Both mesh->bounding_center_* and
 * td5_track_get_span_center_world() are in the same large-magnitude WORLD-unit
 * space (NOT integer-coord/256 — verified at runtime: a START gantry reports
 * bc=(-2750,121500), matching the strip span centres). Returns 0 (no match) or 1. */
int td6_banner_roadcenter_x(float ref_x, float ref_z, float *out_rx)
{
    int n = td5_track_get_span_count();
    int s, best = -1;
    float best_d2 = 0.0f;
    int rx, ry, rz;
    for (s = 0; s < n; s++) {
        int ix, iy, iz;
        float dx, dz, d2;
        if (!td5_track_get_span_center_world(s, &ix, &iy, &iz))
            continue;                       /* skips junction span types 9/10 */
        dx = (float)ix - ref_x;
        dz = (float)iz - ref_z;
        d2 = dx * dx + dz * dz;
        if (best < 0 || d2 < best_d2) { best_d2 = d2; best = s; }
    }
    if (best < 0) return 0;
    if (!td5_track_get_span_center_world(best, &rx, &ry, &rz)) return 0;
    if (out_rx) *out_rx = (float)rx;
    return 1;
}

/* Average X (world units) of every banner mesh in `block` sharing this gantry's
 * longitudinal line (Z within TOL of my_z). A START gantry is two half-panels
 * ("STA" at X=-2750 and "ART" at X=+2750, combined centre 0); the whole gantry
 * must shift by ONE common delta (road_centre - group_centre) or the halves
 * collapse onto each other. Falls back to my_x for a single-mesh gantry. */
float td6_banner_group_center_x(uint32_t *block, int count,
                                       float my_z, float my_x)
{
    float sum = 0.0f;
    int nb = 0, j;
    for (j = 0; j < count; j++) {
        TD5_MeshHeader *m = (TD5_MeshHeader *)(uintptr_t)block[j + 1];
        float dz;
        if (!m || (uintptr_t)m < 0x100000u || !td5_track_is_valid_mesh_ptr(m))
            continue;
        if (!td6_mesh_uses_banner_page(m))
            continue;
        dz = m->bounding_center_z - my_z;
        if (dz < 0.0f) dz = -dz;
        if (dz > 2000.0f) continue;         /* different gantry (different Z line) */
        sum += m->bounding_center_x;
        nb++;
    }
    return (nb > 0) ? (sum / (float)nb) : my_x;
}

void clip_and_submit_polygon(TD5_MeshVertex *vert_data, int vert_count,
                                    int tex_page)
{
    /* Working buffer for clipped polygon (max 8 verts after clipping) */
    TD5_D3DVertex clipped[8];
    int clipped_count = 0;

    if (s_hk_clip_y > 0.0f) {
        int hk;
        for (hk = 0; hk < vert_count; hk++)
            if (vert_data[hk].pos_y > s_hk_clip_y) return;
    }

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
    /* [task#21] TD6 car body z-fix: snap depth to a fine grid + a toward-camera
     * pull so coplanar interior/shell faces resolve stably (no per-frame flicker)
     * and the shell wins. Active only while a ported-TD6 car body is drawn
     * (s_td6_car_zbias>0); screen position still uses RAW vz so geometry is
     * unmoved — only the depth COMPARE value changes. */
    const float zfix = s_td6_car_zbias;   /* 0 = inactive */
    for (int i = 0; i < out_count; i++) {
        float inv_z = 1.0f / out_vz[i];
        clipped[i].screen_x = -out_vx[i] * s_focal_length * inv_z + s_center_x;
        clipped[i].screen_y = -out_vy[i] * s_focal_length * inv_z + s_center_y;
        /* [DA-T1 fix #4 2026-05-22] orig 0x00432362 region:
         *   v[2] -= 64.0f;
         *   v[2] *= 1.5278e-5f;  (= 1/65479)
         * Port previously did vz/65536 with no near offset → ~64 z-unit depth
         * bias near camera. Now matches orig. */
        float dvz = out_vz[i];
        if (zfix > 0.0f) {
            /* snap to the grid (collapses sub-grid coplanar differences) then
             * pull toward the camera by the per-body bias. */
            dvz = floorf(dvz / TD6_CAR_ZFIX_SNAP_VIEWZ + 0.5f) * TD6_CAR_ZFIX_SNAP_VIEWZ
                  - zfix;
        }
        clipped[i].depth_z  = (dvz - NEAR_DEPTH_OFFSET) * DEPTH_NORMALIZE_INV;
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
        /* [banners] one-sided cull for track sign panels only (see
         * k_td6_banner_pages). Everything else keeps CullMode=NONE. */
        if (s_level_pass_active && s_banner_cull && td6_is_banner_page(tex_page)) {
            /* [#9 2026-06-19] Auto-flip the kept (camera-facing) winding sign in
             * reverse — each P2P banner panel's screen winding flips when driven
             * backward, so the forward keep-side would cull the now-camera-facing
             * face and the banners disappear. Scoped to banner pages in the level
             * pass only (never other geometry). TD5RE_TD6_BANNER_REVFLIP=0 reverts. */
            int keep_pos = s_banner_cull_keep_pos;
            if (s_banner_cull_revflip == 1 && g_td5.reverse_direction) keep_pos = !keep_pos;
            int facing_away = keep_pos ? (cross < 0.0f) : (cross > 0.0f);
            if (facing_away) {
                s_debug_clip_backface_rejects++;
                return;
            }
            /* [#R3-6 2026-06-19] In reverse the kept face is the BACK of the sign,
             * so its texture reads MIRRORED (Paris/NY-backwards "banner flipped
             * horizontally"). Un-mirror by flipping U about the triangle's U
             * extent — each banner triangle spans the full quad U-range, so
             * (umin+umax - u) mirrors the whole sign. Same gate as the revflip. */
            if (s_banner_cull_revflip == 1 && g_td5.reverse_direction) {
                float umin = clipped[0].tex_u, umax = clipped[0].tex_u;
                for (int i = 1; i < clipped_count; i++) {
                    if (clipped[i].tex_u < umin) umin = clipped[i].tex_u;
                    if (clipped[i].tex_u > umax) umax = clipped[i].tex_u;
                }
                float usum = umin + umax;
                for (int i = 0; i < clipped_count; i++)
                    clipped[i].tex_u = usum - clipped[i].tex_u;
            }
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

    /* [parallel-build] g_rs_default doesn't go through rs_init_instance:
     * arm the no-override sentinel here (0 would force texture page 0). */
    g_rs->tex_page_override = -1;

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

    /* [2026-06-08 split-screen diagnostic] Per-frame draw-submission counters,
     * emitted at WARN once/~second when the profiler is on (so a split-screen
     * pane sweep captures them in engine.log). Lets us tell a draw-call /
     * triangle COUNT explosion (CPU-submission bound) apart from a per-draw cost
     * jump (GPU / Map-DISCARD stall): if these scale ~linearly with the pane
     * count while the render-ms cliffs, the bottleneck is GPU/driver side, not
     * the submission count. Whole-frame totals (all panes), reset at begin-scene. */
    if (td5_profile_enabled() && (g_tick_counter % 60u) == 0u) {
        TD5_LOG_W(LOG_TAG,
                  "RENDERSTAT views=%d draws=%d tris=%d binds=%d texmiss=%d texevict=%d spanmesh=%d",
                  g_td5.viewport_count,
                  s_debug_scene_draw_calls,
                  s_debug_flush_submitted_tris,
                  s_debug_texture_bind_calls,
                  s_debug_texture_cache_misses,
                  s_debug_texture_cache_evictions,
                  s_debug_span_meshes_submitted);
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

/* [CAR DAMAGE 2026-06-28] Active per-slot model-space deformation deltas for the
 * vehicle currently being transformed. Set by the per-vehicle draw block (where
 * the slot + mesh are in scope) right before td5_render_transform_mesh_vertices,
 * and cleared immediately after, so ONLY the vehicle body picks up the dents
 * (track/sky/etc. transform with these NULL). The deltas are added to the LOCAL
 * model pos used for the world->view multiply only — the shared mesh blob AND the
 * per-pane workspace model pos are left untouched, so no GPU re-upload is needed
 * and the fallback (in-place) transform path can never permanently dent the
 * shared mesh. Indexed by mesh vertex; valid for s_deform_count verts. */
const float *s_deform_dx = NULL;
const float *s_deform_dy = NULL;
const float *s_deform_dz = NULL;
int          s_deform_count = 0;

void td5_render_transform_mesh_vertices(TD5_MeshHeader *mesh)
{
    if (!mesh) return;

    const float *m = s_render_transform.m;
    int count = mesh->total_vertex_count;
    TD5_MeshVertex *src = (TD5_MeshVertex *)(uintptr_t)mesh->vertices_offset;
    if (!src || count <= 0) return;

    /* [Phase B parallel-build] Copy the mesh's vertices into this pane's
     * workspace and write the view coords THERE — the shared blob stays
     * read-only so concurrent pane builds can transform the same mesh with
     * different cameras. Downstream readers rebase via rs_vtx_rebase(). */
    TD5_MeshVertex *verts;
    if (rs_vtx_workspace_ensure(count)) {
        verts = g_rs->vtx_work;
        memcpy(verts, src, (size_t)count * sizeof(TD5_MeshVertex));
        g_rs->vtx_src_base = (const uint8_t *)src;
        g_rs->vtx_src_end  = (const uint8_t *)(src + count);
    } else {
        verts = src;   /* legacy in-place fallback (serial-only correct) */
        g_rs->vtx_src_base = g_rs->vtx_src_end = NULL;
    }

    /*
     * Software world->view transform (0x43DD60):
     *   view_x = pos_x * M[0] + pos_y * M[1] + pos_z * M[2] + M[9]
     *   view_y = pos_x * M[3] + pos_y * M[4] + pos_z * M[5] + M[10]
     *   view_z = pos_x * M[6] + pos_y * M[7] + pos_z * M[8] + M[11]
     *
     * Vertex stride: 0x2C (44 bytes). Input XYZ at +0x00, output at +0x0C.
     */
    /* [START-banner align] When a banner gantry needs lateral re-centring, shift
     * ONLY its overhead panel verts (pos_y above the threshold) in world X — the
     * gantry's ground start-plaza (pos_y~0) stays put so it isn't dragged off the
     * road. Zero for every non-banner mesh (no per-vertex branch cost there). */
    float bvx = s_banner_vshift_x;
    const float *ddx = s_deform_dx, *ddy = s_deform_dy, *ddz = s_deform_dz;
    int dcount = (ddx && ddy && ddz) ? s_deform_count : 0;
    float dsx = s_drag_road_scale;   /* [DRAG WIDE ROAD] lateral widen of level geom */
    for (int i = 0; i < count; i++) {
        float px = verts[i].pos_x;
        float py = verts[i].pos_y;
        float pz = verts[i].pos_z;

        if (bvx != 0.0f && py >= TD6_BANNER_OVERHEAD_Y)
            px += bvx;
        /* [DRAG WIDE ROAD] widen the road about its centreline (model X=0). */
        if (dsx != 1.0f)
            px *= dsx;

        /* [CAR DAMAGE] add this slot's accumulated model-space dent (local only). */
        if (i < dcount) {
            px += ddx[i];
            py += ddy[i];
            pz += ddz[i];
        }

        verts[i].view_x = px * m[0] + py * m[1] + pz * m[2] + m[9];
        verts[i].view_y = px * m[3] + py * m[4] + pz * m[5] + m[10];
        verts[i].view_z = px * m[6] + py * m[7] + pz * m[8] + m[11];
    }
}

/* ===================== TD6 PER-AREA LIGHTING ZONES (#21) — DEFERRED ==========
 * REMOVED 2026-06-19 at user request: the per-area ambient+sun zone modulation
 * (the "different shading per area" / tunnels darkening) produced distracting
 * brightness changes while driving and is being set aside in favour of a future
 * "smarter shadows" approach. The #13 BAKED per-vertex grey below is UNAFFECTED
 * and still shades faces like the original TD6 engine.
 *
 * The reverse-engineering is preserved for the revisit: the per-track zone
 * tables, format, and resolution chain are fully documented and dumpable via
 * re/tools/extract_td6_lightzones.py (-> LIGHTZONES.BIN). To bring the runtime
 * back, re-add a loader + a per-view selector + the prelit-path modulation. */

/* [P1-C SPLIT step 2, 2026-07-02] The scene/mesh region (dynamic point
 * lights + vertex lighting, frustum tests, span display list, prepared mesh
 * + vehicle mesh registry, actor rendering, projection config, translucent/
 * projected/texture-cache pipelines, environs) moved to td5_render_mesh.c.
 * Shared seam: td5_render_internal.h. */

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

/* TL_Contribution typedef relocated above into the RenderScratch region;
 * s_tl_contrib[3] / s_tl_ambient moved to RenderScratch (Phase B Stage 1). */

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
    /* [TD6 CAR LIGHTING — track-scoped] Migrated TD6 tracks have no real light
     * zones; without this they borrow the TD5 level-number's zone table, whose
     * tunnel/dark zones darken the car as if it were in a tunnel (seen on the
     * London opening). Apply the same flat daylight basis the track geometry
     * uses so the car stays correctly lit everywhere. Faithful TD5 tracks fall
     * through to the byte-faithful per-zone path below. */
    if (g_active_td6_level > 0) {
        td5_render_set_override_daylight();
        return;
    }
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
void render_vehicle_reflection_overlay(TD5_MeshHeader *mesh, int slot)
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

    /* [parallel-build] Operate on the pane workspace copy (the body render
     * just transformed this mesh, so the workspace holds it): the lighting/UV
     * overrides below must not touch the SHARED blob. */
    verts = rs_vtx_rebase((void *)(uintptr_t)mesh->vertices_offset);
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

    /* Override every command's texture page with the environs page via the
     * per-pane dispatch override. [parallel-build] This used to save/patch/
     * restore texture_page_id in the SHARED blob command list — a concurrent
     * pane dispatching the same car mesh mid-patch would bind the env page
     * for the opaque body. */
    g_rs->tex_page_override = pe->texture_page;

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

    g_rs->tex_page_override = -1;

    /* Restore original UVs + lighting. On the workspace path this is moot
     * (the workspace is rewritten by the next mesh transform anyway) but it
     * keeps the rs_vtx alloc-failure fallback — which writes the blob
     * in place — from leaking the reflection overrides into later frames. */
    for (i = 0; i < save_count; i++) {
        verts[i].tex_u = saved_uv[i][0];
        verts[i].tex_v = saved_uv[i][1];
        verts[i].lighting = saved_lighting[i];
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

/* [P1-C SPLIT step 1, 2026-07-02] The per-actor effects region (vehicle shadow
 * projection + raycast overhaul, TD6 props, brake lights, headlights, tracked
 * marker, ARCADE pads/billboards, sky, unified procedural wheels) moved to
 * td5_render_effects.c. Shared seam: td5_render_internal.h. */

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
 * (built from FPU cos at td5_trig_build_lut) at the same index. */
float CosFloat12bit(unsigned int angle) {
    td5_trig_ensure_lut();
    unsigned int idx = angle & 0xFFFu;
    float v = s_cosFloatTable[idx];
    return v;
}

/* [CONFIRMED @ 0x0040A6C0] Byte-faithful with orig SinFloat12bit.
 * L5 promotion 2026-05-18 (small-tier sweep). 5-instr listing match:
 * ADD EAX, 0xfffffc00 (32-bit signed wrap = sin via cos(angle-pi/2));
 * AND 0xfff; FLD float [s_cosFloatTable + idx*4]. */
float SinFloat12bit(int angle) {
    td5_trig_ensure_lut();
    /* Match the original's `ADD EAX, 0xfffffc00` (32-bit signed wrap), then
     * AND 0xfff. */
    unsigned int shifted = ((unsigned int)angle) + 0xfffffc00u;
    unsigned int idx = shifted & 0xFFFu;
    float v = s_cosFloatTable[idx];
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
/* [2026-06-08 procedural FX] When the VFX layer is rendering tire marks through
 * the procedural ps_fx_decal shader, it sets this so the immediate submit below
 * uses the low-alpha-ref depth-tested preset (TRANSLUCENT_POINT_ZTEST, alpha_ref=1)
 * instead of TRANSLUCENT_ANISO (alpha_ref=0x80) — otherwise the decal's feathered
 * edges (alpha < 0.5) get alpha-tested away. Default 0 keeps the legacy textured
 * path byte-identical. */
static int s_tire_mark_fx_preset = 0;
void td5_render_set_tire_mark_fx_preset(int on) { s_tire_mark_fx_preset = on ? 1 : 0; }

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
        /* [FIX 2026-06-02 tire-through-car] Put the decal in the SAME depth space
         * as the opaque road/car (which write (vz-NEAR_DEPTH_OFFSET)*INV) and add a
         * small extra bias toward the camera so the mark wins the coplanar road
         * without z-fighting, while the car body (genuinely much closer) still
         * occludes it. The raw projected sz (fdata[base+2]) omitted the -64 the
         * opaque pass applies, so marks were depth-inconsistent and showed through
         * the car. */
        verts[i].depth_z  = fdata[base + 2]
                            - (NEAR_DEPTH_OFFSET + TIRE_DECAL_BIAS) * DEPTH_NORMALIZE_INV;
        verts[i].rhw      = fdata[base + 3];
        verts[i].diffuse  = *(uint32_t *)&fdata[base + 4];
        verts[i].specular = 0;
        verts[i].tex_u    = fdata[base + 5];
        verts[i].tex_v    = fdata[base + 6];
    }

    tex_page = (int)(*(float *)((uint8_t *)quad_data + 0x90));
    /* [FIX 2026-06-02 tire-through-car] Use a depth-tested translucent preset WITHOUT
     * the SHADOW preset's polygon_offset. That rasterizer DepthBias pulls decals
     * toward the camera (needed for the car's OWN shadow, coplanar under the car) and
     * was shoving the tire marks IN FRONT of the car body -> see-through. The marks
     * trail behind the car, so they only need to (a) win the coplanar road, handled by
     * the small vertex bias above, and (b) lose to the car, handled by the normal
     * LEQUAL test now that no rasterizer pull over-biases them. */
    td5_plat_render_set_preset(s_tire_mark_fx_preset ? TD5_PRESET_TRANSLUCENT_POINT_ZTEST
                                                     : TD5_PRESET_TRANSLUCENT_ANISO);
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
     * bleeds the scene.]
     *
     * [S26 2026-06-05 FINAL — user wants a SUBTLE star: "appear just like now at
     * the beginning, then as it's rotating it gets just a little bit more opaque
     * (like 20%) and just that".] So this is a deliberate, documented deviation
     * from the original's opaque petals: the star stays TRANSLUCENT the whole
     * time. It fades in faint (same early look as the prior ramp) and rises
     * GENTLY to a low ~20% peak (alpha ~51 = 0.20*255) by the end of the ~2.5s
     * victory hold, then holds there — never approaching the full-white-out the
     * user rejected, and with NO fade-out at the end (also rejected).
     *   alpha = phase * 0.16  (reaches ~51 at phase ~315 = end of hold)
     *   capped at 51 so it never exceeds ~20% opacity.
     * Tunable: STAR_ALPHA_SLOPE sets how quickly it intensifies; STAR_ALPHA_MAX
     * sets the peak opacity (51 = 20%, 64 = 25%, 255 = fully opaque). */
    const float STAR_ALPHA_SLOPE = 0.16f;   /* opacity gained per phase unit */
    const int   STAR_ALPHA_MAX   = 51;      /* peak alpha (~20% of 255) */
    int alpha = (int)(phase * STAR_ALPHA_SLOPE);
    if (alpha < 0)                 alpha = 0;
    else if (alpha > STAR_ALPHA_MAX) alpha = STAR_ALPHA_MAX;

    /* Per-frame radius. viewport_width * phase * coeff.
     * Orig coeff [CONFIRMED @ 0x439e60 RenderHudRadialPulseOverlay: _DAT_0045d64c
     *  = 0.0015625f = 1/640]. A prior port bug used 0.00625f (1/160) — 4x too
     *  large, ballooning the star ~4x too fast (user 2026-05-30 "animation too
     *  fast") — which was restored to the faithful 1/640.
     *
     * [S26 2026-06-05 — "make the star bigger ... covers more of the screen"]
     *  Measured end radius/viewport_width = 0.752 at 1/640; enlarge the linear
     *  coefficient to 0.0024f (= 1/417, ~1.54x the faithful value) so the star
     *  grows clearly larger while the growth stays LINEAR in phase (smooth, no
     *  jump). Deliberate, documented deviation from the faithful 1/640 per user
     *  request. Tunable: 0.0015625 = faithful, 0.0024 = enlarged (current).
     *
     * [S26 2026-06-05 FINAL — "the star stops growing at some point, otherwise it
     *  looks fine"] A radius CAP was briefly added to stop a full white-out, but
     *  the white-out came from the opacity (since dialed down to a translucent
     *  ~20% peak above), not the size. At ~20% opacity a large star just lets the
     *  scene show through, so the cap is unnecessary AND the user saw the growth
     *  visibly stop when it clamped. Cap removed: the radius now grows linearly
     *  with phase for the whole animation, never stalling. */
    float radius = (float)s_viewport_width * phase * 0.0024f;

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
int debug_line_project(float wx, float wy, float wz, uint32_t argb,
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

/* --- Near-plane clip + project helpers for the custom-track ribbon fallback.
 * The debug-line projector DROPS any primitive that has a vertex behind the
 * near plane. With a chase camera that silently deletes the very span the
 * player is driving over (its near edge sits behind the camera) plus the next
 * span or two -> the road "vanishes" around the car as you drive (observed:
 * the car floats over a void with only the far curve drawn). These helpers
 * clip each road triangle to the near plane instead, so the partial span still
 * draws. Use the same file-static camera state as debug_line_project. ------- */
typedef struct { float vx, vy, vz, u, v; } TD5_RibbonCamV;  /* u,v carry the road UV through near-clip */

static TD5_RibbonCamV ribbon_world_to_cam(float wx, float wy, float wz)
{
    float dx = wx - s_camera_pos[0];
    float dy = wy - s_camera_pos[1];
    float dz = wz - s_camera_pos[2];
    TD5_RibbonCamV v;
    v.vx = dx * s_camera_basis[0] + dy * s_camera_basis[1] + dz * s_camera_basis[2];
    v.vy = dx * s_camera_basis[3] + dy * s_camera_basis[4] + dz * s_camera_basis[5];
    v.vz = dx * s_camera_basis[6] + dy * s_camera_basis[7] + dz * s_camera_basis[8];
    v.u = 0.0f; v.v = 0.0f;   /* caller sets per-corner UV after this */
    return v;
}

static void ribbon_cam_to_vertex(TD5_RibbonCamV v, uint32_t argb, TD5_D3DVertex *out)
{
    float inv_z = 1.0f / v.vz;
    out->screen_x = -v.vx * s_focal_length * inv_z + s_center_x;
    out->screen_y = -v.vy * s_focal_length * inv_z + s_center_y;
    out->depth_z  = (v.vz - NEAR_DEPTH_OFFSET) * DEPTH_NORMALIZE_INV;
    out->rhw      = inv_z;
    out->diffuse  = argb;
    out->specular = 0;
    out->tex_u    = v.u;
    out->tex_v    = v.v;
}

/* Clip a 3-vertex camera-space triangle to vz >= nearz (Sutherland-Hodgman,
 * one plane). Writes up to 4 output verts (convex) and returns the count
 * (0 = fully behind the near plane). */
static int ribbon_clip_near(const TD5_RibbonCamV *tri, float nearz, TD5_RibbonCamV *out)
{
    int n = 0, i;
    for (i = 0; i < 3; i++) {
        TD5_RibbonCamV a = tri[i];
        TD5_RibbonCamV b = tri[(i + 1) % 3];
        int a_in = (a.vz >= nearz);
        int b_in = (b.vz >= nearz);
        if (a_in) out[n++] = a;
        if (a_in != b_in) {
            float t = (nearz - a.vz) / (b.vz - a.vz);
            TD5_RibbonCamV c;
            c.vx = a.vx + (b.vx - a.vx) * t;
            c.vy = a.vy + (b.vy - a.vy) * t;
            c.vz = nearz;
            c.u  = a.u + (b.u - a.u) * t;   /* interpolate UV at the near-plane cut */
            c.v  = a.v + (b.v - a.v) * t;
            out[n++] = c;
        }
    }
    return n;
}

/* Emit one world-space triangle (3 integer-coord points) into the caller's vb/ib
 * as a near-clipped, optionally two-sided primitive, reusing the ribbon helpers.
 * Each surviving clipped sub-tri writes 3 verts + (two_sided?6:3) indices. */
/* [DRAG FINISH GANTRY 2026-06-29] Resolve level030's real FINISH gantry mesh.
 * Per-primitive dump (drag dev) showed it is MODELS.DAT display-list entry 26,
 * sub-mesh 0: a road-spanning overhead arch at world-Z ~-141000 (the original
 * finish, ~span 204), x±3300, up to y≈2664, built from 46 richly-textured
 * primitives — and crucially it carries NO road slab (every primitive is overhead),
 * so it can be relocated cleanly. Its header bounding_center_z (~-144449) is
 * UNRELIABLE (the base vertex array differs from the drawn command geometry), so we
 * derive the true rendered-Z centroid from the command vertex blocks. Caches the
 * mesh pointer + centroid; recomputes if MODELS.DAT reloads (pointer changes). */
TD5_MeshHeader *td5_render_drag_gantry(void)
{
    void *e = td5_track_get_display_list_entry(26);
    uint32_t *b;
    TD5_MeshHeader *m;
    if (!e) return NULL;
    b = (uint32_t *)e;
    if ((int)b[0] < 1) return NULL;
    m = (TD5_MeshHeader *)(uintptr_t)b[1];
    if (!m || !td5_track_is_valid_mesh_ptr(m)) return NULL;
    /* Sanity: only level030's gantry sits in the original-finish Z band. Anything
     * else means this isn't the drag strip — don't grab it. */
    if (m->bounding_center_z < -150000.0f || m->bounding_center_z > -134000.0f) return NULL;
    if (m != s_drag_gantry_mesh) {
        const TD5_PrimitiveCmd *cmds = (const TD5_PrimitiveCmd *)(uintptr_t)m->commands_offset;
        int nc = m->command_count, c;
        float minz = 1e30f, maxz = -1e30f;
        if (cmds && td5_track_is_valid_mesh_ptr((void *)cmds) && nc > 0 && nc <= 4096) {
            for (c = 0; c < nc; c++) {
                int nv = cmds[c].triangle_count * 3 + cmds[c].quad_count * 4, v;
                TD5_MeshVertex *vp = (TD5_MeshVertex *)(uintptr_t)cmds[c].vertex_data_ptr;
                if (!vp) vp = (TD5_MeshVertex *)(uintptr_t)m->vertices_offset;
                if (!vp || !td5_track_is_valid_mesh_ptr(vp) || nv < 1 || nv > 8192) continue;
                for (v = 0; v < nv; v++) {
                    float z = vp[v].pos_z;
                    if (z < minz) minz = z; if (z > maxz) maxz = z;
                }
            }
        }
        s_drag_gantry_z   = (minz <= maxz) ? (minz + maxz) * 0.5f : m->bounding_center_z;
        s_drag_gantry_mesh = m;
    }
    return m;
}

/* [DRAG FINISH GANTRY 2026-06-29] Re-render the REAL finish gantry (dl 26 sub 0) at
 * the relocated finish span. The original at ~span 204 is suppressed in the normal
 * walk (see td5_render_span_display_list, mesh == s_drag_gantry_mesh), and here we
 * draw that same mesh translated in Z (s_dl_z_offset) to the real finish (the last
 * checkpoint span, ~278 for EPIC) via a 1-entry display block. Drag-only. */
void td5_render_drag_finish_line(void)   /* extern: mesh TU calls it (seam header) */
{
    TD5_MeshHeader *g;
    int fspan, lanes, fx, fy, fz;
    float finish_z;
    uint32_t fake[2];

    if (!g_td5.drag_race_enabled) return;
    g = td5_render_drag_gantry();
    if (!g) return;
    {
        int cpc = td5_game_get_minimap_checkpoint_count();
        if (cpc <= 0) return;
        fspan = td5_game_get_minimap_checkpoint_span(cpc - 1);
    }
    if (fspan < 1) return;
    lanes = td5_track_span_lane_count_at(fspan);
    if (lanes < 1) lanes = 1;
    if (!td5_track_get_span_lane_world(fspan, lanes / 2, &fx, &fy, &fz)) return;
    finish_z = (float)fz / 256.0f;

    fake[0] = 1;
    fake[1] = (uint32_t)(uintptr_t)g;            /* 1-entry block: just the gantry */
    s_dl_z_offset = finish_z - s_drag_gantry_z;  /* slide it to the real finish */
    td5_render_span_display_list((void *)fake);
    s_dl_z_offset = 0.0f;

    {
        static int s_log = 0;
        if ((s_log++ % 120) == 0)
            TD5_LOG_I(LOG_TAG,
                "drag finish gantry: dl26/sub0 gantry_z=%.0f finish_span=%d finish_z=%.0f off=%.0f",
                s_drag_gantry_z, fspan, finish_z, finish_z - s_drag_gantry_z);
    }
}

/* [DRAG RIBBON TEXTURE 2026-07-01] Find the asphalt texture page the original drag
 * road uses, so the procedural ribbon (which paints the inserted spans) can texture
 * them to match instead of flat grey. Heuristic: scan the stadium display-list blocks
 * for the FLATTEST + WIDEST primitive (large x-range, small y-range = the horizontal
 * road slab vs the tall vertical walls) and take its texture page. Cached; -1 = none
 * found (fall back to flat grey). */
static int s_drag_road_tex_page = -2;   /* -2 unresolved, -1 none, >=0 page */
static int td5_render_drag_road_texture_page(void)
{
    int dl;
    float best_x = 0.0f;
    if (s_drag_road_tex_page != -2) return s_drag_road_tex_page;
    s_drag_road_tex_page = -1;
    for (dl = 0; dl <= 35; dl++) {
        void *e = td5_track_get_display_list_entry(dl);
        uint32_t *b;
        TD5_MeshHeader *m;
        const TD5_PrimitiveCmd *cmds;
        int nc, c;
        if (!e) continue;
        b = (uint32_t *)e;
        if ((int)b[0] < 1) continue;
        m = (TD5_MeshHeader *)(uintptr_t)b[1];
        if (!m || !td5_track_is_valid_mesh_ptr(m)) continue;
        cmds = (const TD5_PrimitiveCmd *)(uintptr_t)m->commands_offset;
        nc = m->command_count;
        if (!cmds || !td5_track_is_valid_mesh_ptr((void *)cmds) || nc < 1 || nc > 4096) continue;
        for (c = 0; c < nc; c++) {
            int nv = cmds[c].triangle_count * 3 + cmds[c].quad_count * 4, v;
            TD5_MeshVertex *vp = (TD5_MeshVertex *)(uintptr_t)cmds[c].vertex_data_ptr;
            float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f, xr, yr;
            if (!vp) vp = (TD5_MeshVertex *)(uintptr_t)m->vertices_offset;
            if (!vp || !td5_track_is_valid_mesh_ptr(vp) || nv < 3 || nv > 8192) continue;
            for (v = 0; v < nv; v++) {
                float x = (float)vp[v].pos_x, y = (float)vp[v].pos_y;
                if (x < minx) minx = x; if (x > maxx) maxx = x;
                if (y < miny) miny = y; if (y > maxy) maxy = y;
            }
            xr = maxx - minx; yr = maxy - miny;
            /* road = FLAT (small y span) at the DRIVABLE-ROAD WIDTH (~6438). Ruled out
             * in-game: page 4=concrete streaks, 5=start stripes, 7=crowd, 28=concrete,
             * 80=infield. The remaining road-width layer is 75/76 — trying 75 (the
             * asphalt surface). NOTE: render page IDs are REMAPPED, so they do NOT match
             * the extracted page_NNN.png filenames — identify visually only. */
            if (yr < 900.0f && xr > 5000.0f && xr < 9000.0f && cmds[c].texture_page_id == 75) {
                best_x = xr;
                s_drag_road_tex_page = 75;
            }
        }
    }
    TD5_LOG_I(LOG_TAG, "drag road texture page = %d (widest flat prim x-range=%.0f)",
              s_drag_road_tex_page, best_x);
    return s_drag_road_tex_page;
}

/* Custom-track STRIP-ribbon render fallback.
 *
 * A level built by re/tools/td5_trackgen.py ships only the collision/logic
 * ribbon (strip.json) and no MODELS.DAT mesh, so the normal display-list walk
 * draws nothing. This paints each span in the player's view window as a solid
 * road surface (asphalt grey) with brighter rail edge cues, near-plane clipped
 * so the span around the car never drops out. Both tri windings are emitted so
 * the road shows regardless of the rasterizer's cull mode.
 *
 * Span quad corners (each span owns near + far rows of (lanes+1) vertices):
 *   0=near-left  = left_vertex_index + 0     1=near-right = left_vertex_index  + lanes
 *   2=far-left   = right_vertex_index + 0     3=far-right  = right_vertex_index + lanes
 */
void td5_render_fallback_strip_ribbon(int center_span, int window,
                                             int ring, int total_spans,
                                             int is_circuit, int min_span, int max_span)
{
    enum { RIBBON_TRI_CAP = 128 };            /* triangles per flat-tri flush */
    TD5_D3DVertex vb[RIBBON_TRI_CAP * 3];
    uint16_t      ib[RIBBON_TRI_CAP * 6];     /* front + reversed winding */
    int tri_n = 0;
    const uint32_t road_col = 0xFF808488u;    /* plain tarmac grey (ARGB), matched to the
                                               * original road — the drag road is a BLEND of
                                               * layered textures (concrete/stripes/crowd/
                                               * fence/infield), no single asphalt page, so
                                               * a flat matched grey is the clean match. */
    const uint32_t edge_col = 0xFFB0B0B8u;    /* rail edge cue */
    const float nearz = s_near_clip + 1.0f;
    static const int tri_idx[2][3] = { {0, 1, 2}, {1, 3, 2} };
    int span, ring_iters, branch_base, total_iters;
    int drawn = 0;
    /* [DRAG RIBBON TEXTURE 2026-07-01] The drag road is a BLEND of several layered
     * textures (no single asphalt page — every road-width layer is concrete/stripes/
     * crowd/fence/infield), which one ribbon texture can't reproduce, so the inserted
     * road uses the flat matched tarmac grey (road_col) rather than a wrong single page.
     * (td5_render_drag_road_texture_page + the UV code are kept for a future real
     * road-mesh tiling pass.) */
    int      road_page = -1;
    int      textured  = (road_page >= 0);
    uint32_t fill_col  = textured ? 0xFFFFFFFFu : road_col;

    if (window < 1) window = 1;
    if (window > 256) window = 256;           /* bound the per-frame work */

    /* Iterate the player-centred ring window first, then EVERY branch-corridor
     * span [ring, total_spans) so forks / shortcuts (which live past the main
     * ring) are drawn too -- otherwise a branch road would be invisible. */
    ring_iters  = window * 2 + 1;
    branch_base = (ring > 0 && total_spans > ring) ? ring : total_spans;
    total_iters = ring_iters + (total_spans - branch_base);

    for (span = 0; span < total_iters; span++) {
        int si;
        TD5_StripSpan *sp;
        TD5_StripVertex *nl, *nr, *fl, *fr;
        int lanes, li, ri, ti;
        TD5_RibbonCamV corner[4];

        if (span < ring_iters) {
            si = center_span - window + span;       /* ring window (player-centred) */
            if (is_circuit && ring > 0) {
                while (si < 0)      si += ring;
                while (si >= ring)  si -= ring;
            } else if (si < 0 || si >= total_spans) {
                continue;
            }
        } else {
            si = branch_base + (span - ring_iters); /* branch corridor spans */
            if (si < 0 || si >= total_spans) continue;
        }
        if (si < min_span) continue;            /* drag: only from the insertion point */
        if (max_span > 0 && si >= max_span) continue;  /* drag: not the shifted junk tail */
        sp = td5_track_get_span(si);
        if (!sp) continue;

        lanes = sp->pad_02[1] & 0x0F;          /* low nibble of the packed lane byte */
        if (lanes < 1) lanes = 1;
        li = (int)sp->left_vertex_index;
        ri = (int)sp->right_vertex_index;
        nl = td5_track_get_vertex(li);
        nr = td5_track_get_vertex(li + lanes);
        fl = td5_track_get_vertex(ri);
        fr = td5_track_get_vertex(ri + lanes);
        if (!nl || !nr || !fl || !fr) continue;

        /* world = span origin + local int16 vertex, then into camera space */
        corner[0] = ribbon_world_to_cam((float)(sp->origin_x + nl->x), (float)(sp->origin_y + nl->y), (float)(sp->origin_z + nl->z));
        corner[1] = ribbon_world_to_cam((float)(sp->origin_x + nr->x), (float)(sp->origin_y + nr->y), (float)(sp->origin_z + nr->z));
        corner[2] = ribbon_world_to_cam((float)(sp->origin_x + fl->x), (float)(sp->origin_y + fl->y), (float)(sp->origin_z + fl->z));
        corner[3] = ribbon_world_to_cam((float)(sp->origin_x + fr->x), (float)(sp->origin_y + fr->y), (float)(sp->origin_z + fr->z));

        /* Asphalt UV. v must be CONTINUOUS down-track (v = span index, so far-of-span-N
         * == near-of-span-N+1) — a per-span 0->1 reset put a texture SEAM at every span
         * boundary, which showed as the perpendicular bands across the road. u tiles a
         * few times across the ~6.4k-wide road so the tarmac grain stays square rather
         * than stretched. Corner order: 0=near-left 1=near-right 2=far-left 3=far-right. */
        {
            float uw = 4.0f;                        /* ~4 tarmac tiles across the road width */
            float v0 = (float)si, v1 = (float)(si + 1);
            corner[0].u = 0.0f; corner[0].v = v0;
            corner[1].u = uw;   corner[1].v = v0;
            corner[2].u = 0.0f; corner[2].v = v1;
            corner[3].u = uw;   corner[3].v = v1;
        }

        /* whole span behind the near plane -> nothing to draw */
        if (corner[0].vz < nearz && corner[1].vz < nearz &&
            corner[2].vz < nearz && corner[3].vz < nearz)
            continue;
        drawn++;

        for (ti = 0; ti < 2; ti++) {
            TD5_RibbonCamV in[3], poly[4];
            int m, f;
            in[0] = corner[tri_idx[ti][0]];
            in[1] = corner[tri_idx[ti][1]];
            in[2] = corner[tri_idx[ti][2]];
            m = ribbon_clip_near(in, nearz, poly);     /* 0, 3 or 4 verts */
            for (f = 1; f + 1 < m; f++) {              /* fan into (m-2) tris */
                int base;
                if (tri_n >= RIBBON_TRI_CAP) {
                    if (textured) { td5_plat_render_bind_texture(road_page);
                                    td5_plat_render_draw_tris(vb, tri_n * 3, ib, tri_n * 6); }
                    else            td5_plat_render_draw_tris_flat(vb, tri_n * 3, ib, tri_n * 6);
                    tri_n = 0;
                }
                base = tri_n * 3;
                ribbon_cam_to_vertex(poly[0],     fill_col, &vb[base + 0]);
                ribbon_cam_to_vertex(poly[f],     fill_col, &vb[base + 1]);
                ribbon_cam_to_vertex(poly[f + 1], fill_col, &vb[base + 2]);
                ib[tri_n * 6 + 0] = (uint16_t)(base + 0);
                ib[tri_n * 6 + 1] = (uint16_t)(base + 1);
                ib[tri_n * 6 + 2] = (uint16_t)(base + 2);
                ib[tri_n * 6 + 3] = (uint16_t)(base + 0);   /* reversed winding */
                ib[tri_n * 6 + 4] = (uint16_t)(base + 2);
                ib[tri_n * 6 + 5] = (uint16_t)(base + 1);
                tri_n++;
            }
        }

        /* near->far rail edge cues (lines self-clip; thin cue, fine to drop) */
        td5_render_debug_line_world((float)(sp->origin_x + nl->x), (float)(sp->origin_y + nl->y), (float)(sp->origin_z + nl->z),
                                    (float)(sp->origin_x + fl->x), (float)(sp->origin_y + fl->y), (float)(sp->origin_z + fl->z), edge_col);
        td5_render_debug_line_world((float)(sp->origin_x + nr->x), (float)(sp->origin_y + nr->y), (float)(sp->origin_z + nr->z),
                                    (float)(sp->origin_x + fr->x), (float)(sp->origin_y + fr->y), (float)(sp->origin_z + fr->z), edge_col);
    }
    if (tri_n > 0) {
        if (textured) { td5_plat_render_bind_texture(road_page);
                        td5_plat_render_draw_tris(vb, tri_n * 3, ib, tri_n * 6); }
        else            td5_plat_render_draw_tris_flat(vb, tri_n * 3, ib, tri_n * 6);
    }
    td5_render_debug_lines_flush();

    if (min_span > 0) {                        /* [DRAG TAIL diag] */
        static int s_t = 0;
        if ((s_t++ % 30) == 0)
            TD5_LOG_I(LOG_TAG, "drag tail ribbon: center=%d min_span=%d total_spans=%d drew=%d",
                      center_span, min_span, total_spans, drawn);
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
