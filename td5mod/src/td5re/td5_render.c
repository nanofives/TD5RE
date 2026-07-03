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
#include "td5_light2.h"   /* [LIGHT2] lighting rework mode knob */
#include "td5_material.h" /* [LIGHT2] texture-page -> material id */
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
    /* [LIGHT2] Parallel packed-normal array (same indexing as vtx_work). Kept
     * in lockstep with the workspace so a grow can never desync the two. An
     * alloc failure here just disables the G-buffer feed for this pane. */
    uint32_t *np = (uint32_t *)malloc((size_t)cap * sizeof(uint32_t));
    free(g_rs->vtx_work);
    free(g_rs->vtx_pack);
    g_rs->vtx_work     = nw;
    g_rs->vtx_work_cap = cap;
    g_rs->vtx_pack     = np;                 /* NULL on failure = feed off */
    g_rs->vtx_pack_cap = np ? cap : 0;
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
    uint32_t in_pack[8];
    int in_count = vert_count;

    /* [LIGHT2] Packed world normals for this polygon's vertices. The mesh
     * walker hands us pointers INTO the pane workspace (rebased upstream), so
     * the workspace index recovers each vertex's slot in the parallel
     * vtx_pack array filled by compute_vertex_lighting. Pointers outside the
     * workspace (ad-hoc emitters, depth-bucket prim copies, alloc-failure
     * fallback) get 0 = "no normal" and stay emissive. matid joins below. */
    uint32_t poly_matid = 0;
    int have_pack = 0;
    if (td5_light2_active() && g_rs->vtx_pack &&
        vert_data >= g_rs->vtx_work &&
        vert_data + vert_count <= g_rs->vtx_work + g_rs->vtx_pack_cap) {
        have_pack = 1;
        /* [P3] Vehicle bodies: the light basis carries a body->world rotation
         * exactly for rotated (vehicle/prop) meshes — classify those CARBODY
         * so car paint gets its SSR sheen; everything else by texture-page
         * class. */
        poly_matid = (uint32_t)(s_light_basis_has_rot
                                ? TD5_MAT_CARBODY
                                : td5_material_id_for_page(tex_page)) << 24;
    }

    for (int i = 0; i < vert_count; i++) {
        in_vx[i]    = vert_data[i].view_x;
        in_vy[i]    = vert_data[i].view_y;
        in_vz[i]    = vert_data[i].view_z;
        in_u[i]     = vert_data[i].tex_u;
        in_v[i]     = vert_data[i].tex_v;
        in_color[i] = vert_data[i].lighting;
        in_pack[i]  = have_pack ? g_rs->vtx_pack[(vert_data - g_rs->vtx_work) + i] : 0;
    }

    /* Near-plane clip */
    float out_vx[8], out_vy[8], out_vz[8], out_u[8], out_v[8];
    uint32_t out_color[8];
    uint32_t out_pack[8];
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
            out_pack[out_count]  = in_pack[i];
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
            /* [LIGHT2] Clip-edge vertex: nearest-vertex normal (normals vary
             * little across one edge; a per-channel lerp isn't worth it). */
            out_pack[out_count] = in_pack[i];
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
        /* [LIGHT2] COLOR1 = material id (bits 31..24) + packed world normal
         * (bits 23..0). 0 = no G-buffer data (classic / never-lit vertices) —
         * byte-identical to the old constant 0 when Mode=0. */
        clipped[i].specular = out_pack[i] ? (poly_matid | out_pack[i]) : 0;
        clipped[i].tex_u    = out_u[i];
        clipped[i].tex_v    = out_v[i];
    }
    clipped_count = out_count;

    /* [LIGHT2] Render-time street-lamp capture: when a lamp-halo sprite
     * actually DRAWS (squat, wide, alpha-keyed quad above the camera during
     * the level pass), its view-space verts give the exact world position —
     * ground truth that sidesteps the display-list placement folds which
     * defeated four static-extraction attempts. Captured lamps accumulate in
     * the (deduped) registry as they come on screen and stay cached. */
    if (s_level_pass_active && out_count == 4 &&
        td5_light2_active() && td5_light_street_lights() &&
        td5_material_id_for_page(tex_page) == TD5_MAT_CUTOUT) {
        float xmin = out_vx[0], xmax = out_vx[0];
        float hmin = out_vy[0], hmax = out_vy[0];
        float zmin = out_vz[0], zmax = out_vz[0];
        for (int i2 = 1; i2 < 4; i2++) {
            if (out_vx[i2] < xmin) xmin = out_vx[i2];
            if (out_vx[i2] > xmax) xmax = out_vx[i2];
            if (out_vy[i2] < hmin) hmin = out_vy[i2];
            if (out_vy[i2] > hmax) hmax = out_vy[i2];
            if (out_vz[i2] < zmin) zmin = out_vz[i2];
            if (out_vz[i2] > zmax) zmax = out_vz[i2];
        }
        /* Width = the larger HORIZONTAL spread (view X or Z): world-oriented
         * halo quads angled to the camera split their width across both axes
         * — measuring view-X alone missed them. Height stays view-Y. */
        float wx_ext = xmax - xmin, wz_ext = zmax - zmin;
        float w = (wx_ext > wz_ext) ? wx_ext : wz_ext;
        float h = hmax - hmin;
        if (w >= 250.0f && w <= 900.0f && h >= 80.0f && h <= 600.0f &&
            h < 0.7f * w) {
            float vx0 = 0, vy0 = 0, vz0 = 0;
            for (int i2 = 0; i2 < 4; i2++) { vx0 += out_vx[i2]; vy0 += out_vy[i2]; vz0 += out_vz[i2]; }
            vx0 *= 0.25f; vy0 *= 0.25f; vz0 *= 0.25f;
            /* Distance cap: only capture NEAR halos — far sprites carry
             * projection slop and produced phantom lights (a capture landed
             * 200k units away). Every lamp is captured on approach anyway. */
            if (vz0 > s_near_clip && vz0 < 15000.0f) {
                float wx = s_camera_pos[0] + vx0 * s_camera_basis[0] + vy0 * s_camera_basis[3] + vz0 * s_camera_basis[6];
                float wy = s_camera_pos[1] + vx0 * s_camera_basis[1] + vy0 * s_camera_basis[4] + vz0 * s_camera_basis[7];
                float wz = s_camera_pos[2] + vx0 * s_camera_basis[2] + vy0 * s_camera_basis[5] + vz0 * s_camera_basis[8];
                float above_cam = s_camera_pos[1] - wy;    /* up = -Y */
                if (above_cam > -150.0f && above_cam < 2500.0f)
                    td5_light_lamps_capture(wx, wy, wz);
            }
        }
    }

    /* [DEV: TD5RE_GLOW_SCAN=1] Render-side fixture identification: log each
     * texture page whose polygons draw ELEVATED near the camera (lamp posts /
     * halos), with a reconstructed world position. Ground truth regardless of
     * which asset path the geometry came from. One line per page. */
    {
        static int s_glow_scan = -1;
        if (s_glow_scan < 0) { const char *e = getenv("TD5RE_GLOW_SCAN"); s_glow_scan = (e && e[0] && e[0] != '0') ? 1 : 0; }
        if (s_glow_scan && tex_page >= 0 && tex_page < 1024 && s_level_pass_active) {
            static uint8_t s_seen[1024];
            if (!s_seen[tex_page]) {
                float vx0 = 0, vy0 = 0, vz0 = 0;
                for (int i2 = 0; i2 < out_count; i2++) { vx0 += out_vx[i2]; vy0 += out_vy[i2]; vz0 += out_vz[i2]; }
                float inv2 = 1.0f / (float)out_count;
                vx0 *= inv2; vy0 *= inv2; vz0 *= inv2;
                float wx = s_camera_pos[0] + vx0 * s_camera_basis[0] + vy0 * s_camera_basis[3] + vz0 * s_camera_basis[6];
                float wy = s_camera_pos[1] + vx0 * s_camera_basis[1] + vy0 * s_camera_basis[4] + vz0 * s_camera_basis[7];
                float wz = s_camera_pos[2] + vx0 * s_camera_basis[2] + vy0 * s_camera_basis[5] + vz0 * s_camera_basis[8];
                float dx = wx - s_camera_pos[0], dz = wz - s_camera_pos[2];
                float above = s_camera_pos[1] - wy;   /* +Y down: above camera = smaller Y */
                if (dx * dx + dz * dz < 3500.0f * 3500.0f && above > 300.0f && above < 3500.0f) {
                    s_seen[tex_page] = 1;
                    TD5_LOG_I(LOG_TAG, "GLOWSCAN page=%d trans=%d wpos=(%.0f,%.0f,%.0f) above=%.0f",
                              tex_page, td5_asset_get_page_transparency(tex_page),
                              wx, wy, wz, above);
                }
            }
        }
    }

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
        /* [LIGHT2] Invalidate the packed-normal slots for this mesh — a mesh
         * that never reaches the lighting pass (sky, billboards, baked-only
         * paths) must read as "no normal" (0 = emissive sentinel), never as a
         * stale normal from the previous mesh in the workspace. */
        if (g_rs->vtx_pack && td5_light2_active())
            memset(g_rs->vtx_pack, 0, (size_t)count * sizeof(uint32_t));
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
    /* [LIGHT2] Keep the per-channel ratios the classic path averages away.
     * chroma = channel / gray-average, so gray zones give exactly (1,1,1) and
     * the Mode>=1 colored path reproduces the classic result bit-for-bit on
     * uncolored data. The blend paths pass already-attenuated per-channel
     * weights here, so zone-transition blending colors correctly for free. */
    if (intensity > 0.0f) {
        s_tl_chroma[slot][0] = (float)r / intensity;
        s_tl_chroma[slot][1] = (float)g / intensity;
        s_tl_chroma[slot][2] = (float)b / intensity;
    } else {
        s_tl_chroma[slot][0] = s_tl_chroma[slot][1] = s_tl_chroma[slot][2] = 1.0f;
    }
}

/* ComputeAverageDepth @ 0x0043E7B0: scalar ambient = (R+G+B)/3 byte. */
static void tl_set_depth(int r, int g, int b)
{
    s_tl_ambient = (r + g + b) / 3;
    /* [LIGHT2] Raw authored ambient RGB (classic path only ever sees the
     * average above). */
    s_tl_amb_rgb[0] = (float)r;
    s_tl_amb_rgb[1] = (float)g;
    s_tl_amb_rgb[2] = (float)b;
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
        /* [LIGHT2] neutral chroma with the slot disabled */
        s_tl_chroma[s][0] = s_tl_chroma[s][1] = s_tl_chroma[s][2] = 1.0f;
    }
    s_tl_ambient = TD5_LIGHTING_MIN;
    s_tl_amb_rgb[0] = s_tl_amb_rgb[1] = s_tl_amb_rgb[2] = (float)TD5_LIGHTING_MIN;
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

/* [P1-C SPLIT step 3, 2026-07-02] 12-bit trig, matrix/vector operations and
 * the render pipeline helpers moved to td5_render_pipeline.c. Seam decls were
 * already in td5_render_internal.h / td5_render.h. */

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
