/* ========================================================================
 * td5_render_effects.c -- Per-actor render effects & world billboards
 *
 * Split out of td5_render.c (P1-C step 1, 2026-07-02). Contents, in original
 * core order:
 *   - Terrain-conforming raycast vehicle shadow + legacy shadow quad
 *   - TD6 prop meshes (world props on migrated TD6 tracks)
 *   - Brake lights, headlight flood + headlights
 *   - Tracked-actor marker billboard (cop-chase visual)
 *   - [ARCADE] item-box pads/billboards + oil-slick disc
 *   - Sky load/draw/rotation, billboard animation advance
 *   - Unified procedural wheel system (racers/traffic/wrecks)
 * Cross-TU seam: td5_render_internal.h (PRIVATE).
 * ======================================================================== */

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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <string.h>
#include "td5_render_internal.h"  /* PRIVATE core<->effects seam */

/* ======== [split] per-actor effects: shadow .. wheels (moved verbatim from td5_render.c) ======== */
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
/* Toward-camera depth-compare pull (view-z). LIVE again [2026-06-11]: the
 * shadow preset depth test is re-enabled (LEQUAL, see TD5_PRESET_SHADOW in
 * td5_platform_win32.c) so walls/kerbs/crests occlude ground shadows. The
 * conforming raycast mesh ties the road depth (same barycentric ground solve,
 * same depth formula), and this pull breaks the coplanar tie in the shadow's
 * favour. The legacy flat quad (TD5RE_SHADOW_RAYCAST=0 debug A/B) may shimmer
 * under the test — its depth diverges from the curved road across the quad. */
#define SHADOW_PULL_VIEWZ       (2.0f)
#define SHADOW_DEPTH_Z_BIAS     (SHADOW_PULL_VIEWZ * DEPTH_NORMALIZE_INV)

/* ============================================================================
 * Terrain-conforming raycast vehicle shadow (2026-06-10 overhaul)
 *
 * Replaces the single flat 4-corner SHADOW-blob quad (render_vehicle_shadow_
 * quad_legacy below) with a tessellated mesh that is RAYCAST DOWN onto the
 * actual track surface at every grid node, so the shadow hugs slopes / crests /
 * dips instead of clipping through or floating over them (the long-standing
 * "tail visible depending on angle" class documented on the legacy path). It is
 * also TEXTURE-FREE: softness and the rounded-car-blob shape come entirely from
 * per-vertex alpha, so the SHADOW.png atlas sprite is no longer sampled (one
 * more retired FX PNG).
 *
 * "Ray cast from above" == the proven physics ground query: seed a transient
 * TD5_TrackProbeState at the actor's current span, single-step-walk it to each
 * node's XZ (td5_track_update_probe_position), then read the barycentric ground
 * height there (td5_track_compute_contact_height_with_normal). This is exactly
 * the per-body-corner contact refresh pattern in td5_physics.c (~6668).
 *
 * Cost control: the grid is rebuilt at most ONCE per sim tick per actor. The
 * walks are integer span math, but split-screen would otherwise re-pay them per
 * pane per frame. Within a tick every viewport reuses the cached world-space
 * grid and only re-applies the cheap per-frame sub-tick velocity shift, so the
 * shadow stays view-consistent AND frame-rate-independent.
 *
 * A/B: env TD5RE_SHADOW_RAYCAST=0 falls back to the legacy textured quad.
 * ==========================================================================*/

#define SHADOW_GRID_COLS    7   /* nodes across the car's width (lateral)      */
#define SHADOW_GRID_ROWS    9   /* nodes along the car's length (longitudinal) */
#define SHADOW_GRID_NODES   (SHADOW_GRID_COLS * SHADOW_GRID_ROWS)
#define SHADOW_GRID_CELLS   ((SHADOW_GRID_COLS - 1) * (SHADOW_GRID_ROWS - 1))

/* 1x1 white texture page (shared id with the HUD pause dimmer) so the standard
 * (texture * vertex) pixel path returns pure vertex colour — the shadow carries
 * black RGB + soft alpha, no atlas page. */
#define SHADOW_WHITE_TEX_PAGE   899

/* Centre darkness of the drop shadow (alpha at the footprint centre). With the
 * SHADOW preset's SRCALPHA/INVSRCALPHA blend + black RGB, the ground beneath the
 * centre is multiplied by (1 - alpha). */
#define SHADOW_CENTRE_ALPHA     (0.86f)

/* Separable rounded-rectangle falloff (per car axis, NOT radial): on each of
 * the lateral / longitudinal axes the alpha is full out to *_PLATEAU of the
 * half-extent, then smoothsteps to zero by *_EDGE. The product of the two axis
 * factors fills the car's oriented footprint (longer than wide) with rounded
 * corners and a clearly defined feathered rim — reads as the car's shape rather
 * than the inscribed oval the old radial falloff produced. */
#define SHADOW_FALLOFF_PLATEAU  (0.62f)
#define SHADOW_FALLOFF_EDGE     (1.00f)

typedef struct {
    float    base[SHADOW_GRID_NODES][3]; /* world-space node pos at sim-tick time  */
    uint8_t  alpha[SHADOW_GRID_NODES];   /* baked soft footprint alpha (0..255)     */
    uint32_t built_tick;                 /* simulation_tick_counter at build        */
    int      valid;
} ShadowGrid;

static ShadowGrid s_shadow_grid[TD5_MAX_TOTAL_ACTORS];
static int        s_shadow_white_uploaded = 0;

/* A/B toggle (cached once): TD5RE_SHADOW_RAYCAST=0 -> legacy textured quad. */
static int shadow_raycast_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        cached = td5_env_flag_on("TD5RE_SHADOW_RAYCAST");
    }
    return cached;
}

/* [task#4] Shadow anti-flicker (cached once): TD5RE_SHADOW_ANTIFLICKER=0 -> off.
 * Two render-side stabilisers (the raycast PROBE/cache itself lives in
 * td5_track.c and is untouched):
 *   (1) per-node ground-Y LOW-PASS across the 30Hz grid rebuilds, so the
 *       conforming mesh stops STEPPING (the "pop") every sim tick as the
 *       downward raycast lands on a new span / barycentric cell; XZ still tracks
 *       the car each tick + sub-tick so there is no positional lag.
 *   (2) a slightly stronger toward-camera depth pull (below) so the coarse 7x9
 *       shadow surface reliably wins the depth tie against the finely-tessellated
 *       road it is cast on — between grid nodes the linear shadow dips under a
 *       road that bulges up, and the old 2-view-z pull let those pixels z-fight
 *       (angle-dependent shimmer). */
static int shadow_antiflicker_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("TD5RE_SHADOW_ANTIFLICKER");
        cached = (e && e[0] == '0' && e[1] == '\0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "shadow anti-flicker %s", cached ? "ON" : "OFF");
    }
    return cached;
}
/* Smoothing weight for the new raycast Y each tick (0..1): higher = snappier,
 * lower = smoother but laggier on real slopes. 0.5 halves the step each tick
 * (~settles in a few ticks) which removes the visible pop without perceptible
 * terrain lag. */
/* [#4 2026-06-19] Temporal low-pass factor for the per-node shadow ground Y (ease
 * toward the fresh raycast each tick). Lowered from 0.50 -> 0.35 to further damp
 * the residual uphill span-boundary flicker; downhill is already stable so a
 * stronger low-pass is a near-no-op there. Env-tunable TD5RE_SHADOW_Y_SMOOTH
 * (percent 1..100; lower = smoother/slightly laggier, higher = snappier/more flicker). */
static float shadow_y_smooth(void)
{
    static float cached = -1.0f;
    if (cached < 0.0f) {
        const char *e = getenv("TD5RE_SHADOW_Y_SMOOTH");
        long pct = (e && e[0]) ? strtol(e, NULL, 10) : 0;
        if (pct <= 0)   pct = 35;          /* default 0.35 */
        if (pct < 1)    pct = 1;
        if (pct > 100)  pct = 100;
        cached = (float)pct / 100.0f;
    }
    return cached;
}

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
static void render_vehicle_shadow_quad_legacy(const TD5_Actor *actor)
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
        /* white — alpha comes from texture, scaled by the actor fade
         * ([dynamic-traffic]; 255 = identity, MODULATEALPHA preset). */
        verts[i].diffuse  = ((uint32_t)s_actor_draw_alpha << 24) | 0x00FFFFFFu;
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

/* smoothstep(a,b,x) clamped to [0,1]. */
static float shadow_smoothstep(float a, float b, float x)
{
    if (x <= a) return 0.0f;
    if (x >= b) return 1.0f;
    float t = (x - a) / (b - a);
    return t * t * (3.0f - 2.0f * t);
}

/* Build the terrain-conforming shadow grid for `actor` at the current sim tick:
 * bilinear footprint XZ from the four outward-scaled wheel probes, a downward
 * raycast at every node for the true ground Y, and the baked soft footprint
 * alpha. Stored in world space at sub-tick fraction 0 (the per-frame sub-tick
 * shift is applied at projection time). */
/* [#7 WHEEL-PLANE SHADOW 2026-06-20] Default ON. Derive the shadow's ground Y
 * from the plane through the FOUR WHEEL-CONTACT points (probe_FL/FR/RL/RR, which
 * physics integrates smoothly each tick) instead of re-probing the track span at
 * every grid node. The per-node — and even the "stable" per-chassis — span probe
 * RE-RESOLVES which span contains the point every frame, and on a slope that
 * resolution flips between adjacent spans, jumping the Y → the persistent uphill
 * FLICKER. It also returns a bad/zero height on some spans, which clamped the
 * shadow under the terrain → an OPPONENT's shadow vanished at some heights. The
 * wheel-contact plane has neither failure mode: it is continuous (no span
 * lookup), always valid (the wheels are on the ground by construction), and
 * conforms to the car's real pitch/roll. TD5RE_SHADOW_WHEELPLANE=0 reverts to
 * the old track-span probe. */
static int shadow_wheelplane_enabled(void)
{
    static int s = -1;
    if (s < 0) { s = td5_env_flag_on("TD5RE_SHADOW_WHEELPLANE"); }
    return s;
}

/* [#7b WORLD-LIFT 2026-06-20] The vehicle shadow is a ground decal coplanar with
 * the road, and the LEQUAL depth test + small toward-camera bias could not win
 * the tie: the shadow sat a hair BELOW the road render surface, so the road
 * occluded the whole shadow and it only peeked through at the SPAN-BOUNDARY seam
 * (the road mesh's depth discontinuity) — the "dark line under the car aligned
 * with the span divider", and the soft blob was otherwise invisible. Raising the
 * shadow a small amount in WORLD Y puts it just above the road so it wins the
 * tie everywhere, while staying far below walls / kerbs / car bodies (which are
 * much higher), so those still occlude it correctly. The lift is a fraction of
 * the car's half-length so it auto-scales with the world unit. TD5RE_SHADOW_LIFT
 * (percent of half-length, default 6 — required for visibility, see #7c; 0 = no lift). */
static float shadow_lift_frac(void)
{
    static float f = -1.0f;
    if (f < 0.0f) {
        /* [#7c 2026-06-21] Kept at 6. The lift is REQUIRED for visibility: with the
         * port's separate shadow projection the toward-camera depth bias alone does
         * NOT reliably win the coplanar tie, so the road occludes the whole shadow
         * (verified: dropping this to 2 made the countdown shadows VANISH). The
         * opponent "desync on slopes" was NOT this float (it persisted at 2%); its
         * real cause was the #4 low-pass LAG on the smooth wheel-plane Y, now gated
         * to the raycast path only (see smooth_y below). 0 = no lift (debug). */
        int pct = td5_env_int("TD5RE_SHADOW_LIFT", 6, 0, 100);
        f = (float)pct / 100.0f;
    }
    return f;
}

/* [#7c UPHILL-CEILING 2026-06-21] Slope-proportional EXTRA world-Y lift gain.
 * DEFAULT OFF (0). Rationale: a slope-proportional WORLD-Y lift floats the shadow
 * up by an amount that grows with the grade, and from a distance that reads as the
 * OPPONENT shadow detaching from the car on slopes ("desync up & down" — user
 * report 2026-06-21). The real band fix is the relaxed CEILING below (the shadow
 * now FOLLOWS the rising road instead of being flattened under it), which makes
 * shadow & road ~coplanar so the existing toward-camera DEPTH bias wins the tie
 * WITHOUT any extra float. The slope lift is kept as an opt-in A/B knob only.
 * Env TD5RE_SHADOW_LIFT_SLOPE (percent gain, default 0; >0 re-enables the float). */
static float shadow_lift_slope_gain(void)
{
    static float f = -1.0f;
    if (f < 0.0f) {
        int pct = td5_env_int("TD5RE_SHADOW_LIFT_SLOPE", 0, 0, 400);
        f = (float)pct / 100.0f;
        TD5_LOG_I(LOG_TAG, "shadow slope-lift gain = %.2f (0 = off, no float)", f);
    }
    return f;
}

/* Least-squares fit of the plane Y = a*X + b*Z + c through the four wheel
 * contacts (XZ in render units, Y the contact height). Returns 1 on success; on
 * a near-degenerate system (wheels collinear — never for a real car) returns 0
 * and the caller uses a flat plane at the mean height. */
static int shadow_fit_wheel_plane(const float xz[4][2], const float y[4],
                                  float *out_a, float *out_b, float *out_c)
{
    double Sx=0,Sz=0,Sy=0,Sxx=0,Szz=0,Sxz=0,Sxy=0,Szy=0;
    for (int i = 0; i < 4; i++) {
        double X = xz[i][0], Z = xz[i][1], Y = y[i];
        Sx += X; Sz += Z; Sy += Y;
        Sxx += X*X; Szz += Z*Z; Sxz += X*Z;
        Sxy += X*Y; Szy += Z*Y;
    }
    /* Solve the 3x3 normal equations via Cramer's rule.
     *   [Sxx Sxz Sx][a]   [Sxy]
     *   [Sxz Szz Sz][b] = [Szy]
     *   [Sx  Sz  4 ][c]   [Sy ]   */
    double det =
        Sxx*(Szz*4.0 - Sz*Sz) - Sxz*(Sxz*4.0 - Sz*Sx) + Sx*(Sxz*Sz - Szz*Sx);
    if (det > -1e-6 && det < 1e-6) return 0;
    double idet = 1.0 / det;
    double a = (Sxy*(Szz*4.0 - Sz*Sz) - Sxz*(Szy*4.0 - Sz*Sy) + Sx*(Szy*Sz - Szz*Sy)) * idet;
    double b = (Sxx*(Szy*4.0 - Sz*Sy) - Sxy*(Sxz*4.0 - Sz*Sx) + Sx*(Sxz*Sy - Szy*Sx)) * idet;
    double c = (Sxx*(Szz*Sy - Sz*Szy) - Sxz*(Sxz*Sy - Sx*Szy) + Sxy*(Sxz*Sz - Szz*Sx)) * idet;
    *out_a = (float)a; *out_b = (float)b; *out_c = (float)c;
    return 1;
}

static void shadow_build_grid(const TD5_Actor *actor, ShadowGrid *g)
{
    const float inv256 = 1.0f / 256.0f;

    /* Four footprint corners in render units (world/256), order FL,FR,RL,RR.
     * The bilinear basis maps tF (front->back) and tR (left->right). */
    const TD5_Vec3_Fixed *pr[4] = {
        &actor->probe_FL, &actor->probe_FR, &actor->probe_RL, &actor->probe_RR
    };
    float corner[4][2]; /* XZ only — Y comes from the per-node raycast */
    float cornerY[4];   /* the four wheel-contact Ys = the ground/wheel plane    */
    float cx = 0.0f, cz = 0.0f;
    for (int i = 0; i < 4; i++) {
        corner[i][0] = (float)pr[i]->x * inv256;
        corner[i][1] = (float)pr[i]->z * inv256;
        cornerY[i]   = (float)pr[i]->y * inv256;
        cx += corner[i][0];
        cz += corner[i][1];
    }
    cx *= 0.25f; cz *= 0.25f;

    /* [#7 2026-06-20] Fit the wheel-contact ground plane from the four UNSCALED
     * wheel XZ + contact Y, BEFORE the footprint is scaled out. Evaluated per
     * node below (when enabled) in place of the flicker-prone span probe. */
    int   wp_ok = 0;
    float wp_a = 0.0f, wp_b = 0.0f, wp_c = 0.0f;
    if (shadow_wheelplane_enabled()) {
        wp_ok = shadow_fit_wheel_plane(corner, cornerY, &wp_a, &wp_b, &wp_c);
        if (!wp_ok) {   /* degenerate (collinear wheels) -> flat plane at mean Y */
            wp_a = wp_b = 0.0f;
            wp_c = 0.25f * (cornerY[0] + cornerY[1] + cornerY[2] + cornerY[3]);
            wp_ok = 1;
        }
    }

    /* Scale the footprint outward from the centroid (body overhang past the
     * wheels) — same footprint as the legacy SHADOW_CORNER_SCALE quad. */
    for (int i = 0; i < 4; i++) {
        corner[i][0] = cx + (corner[i][0] - cx) * SHADOW_CORNER_SCALE;
        corner[i][1] = cz + (corner[i][1] - cz) * SHADOW_CORNER_SCALE;
    }

    /* Per-node ground Y is clamped to a band [floorY, ceilY].
     *
     * [#7c UPHILL-CEILING 2026-06-21 — RE-confirmed] The CEILING is the fix for
     * the persistent uphill no-shadow band. RE of the ORIGINAL (RenderRaceActor-
     * ForView @0x0040C120) shows it applies NO Y ceiling at all: the four shadow
     * corner Ys are the raw wheel Ys scaled out by 1.25 in every axis, so on a
     * climb the leading corners legitimately sit ABOVE the rear wheels (the road
     * keeps rising ahead). The port's old tight ceiling (== maxWY, the highest
     * wheel) FLATTENED the footprint's front nodes — which are scaled out past the
     * front wheels, up-slope — down to wheel height, sinking them BELOW the rising
     * per-span road poly so the road occluded them (LEQUAL) -> the no-shadow band
     * at the span seam ahead. The flat 6% lift could not clear a real grade.
     *
     * So for the smooth, bounded WHEEL-PLANE path use a GENEROUS ceiling
     * (maxWY + half_len, symmetric with floorY): the front edge can now follow the
     * road up the hill, while a pathological wheel-contact Y is still bounded. The
     * RAYCAST FALLBACK keeps the tight maxWY ceiling — its single-step ground walk
     * CAN land on a mis-extrapolated span and return a bogus-high Y (the original
     * reason the ceiling existed). The shadow still cannot land on the car: every
     * car body is drawn AFTER all shadows in the pre-pass and overpaints them.
     * FLOOR stays generous (one footprint half-length below the lowest wheel). */
    float minWY = cornerY[0], maxWY = cornerY[0];
    for (int i = 1; i < 4; i++) {
        if (cornerY[i] < minWY) minWY = cornerY[i];
        if (cornerY[i] > maxWY) maxWY = cornerY[i];
    }
    float fmx = 0.5f * (corner[0][0] + corner[1][0]); /* front-edge midpoint */
    float fmz = 0.5f * (corner[0][1] + corner[1][1]);
    float bmx = 0.5f * (corner[2][0] + corner[3][0]); /* back-edge midpoint  */
    float bmz = 0.5f * (corner[2][1] + corner[3][1]);
    float half_len = 0.5f * sqrtf((fmx - bmx) * (fmx - bmx) + (fmz - bmz) * (fmz - bmz));
    float floorY = minWY - half_len;
    /* [#7c] Relaxed ceiling on the wheel-plane path (lets the uphill front edge
     * follow the road); tight maxWY on the raycast fallback (bogus-span guard). */
    float ceilY  = wp_ok ? (maxWY + half_len) : maxWY;

    int max_sp    = td5_track_get_span_count();
    int seed_span = (int)actor->track_span_raw;
    if (seed_span < 0) seed_span = 0;
    if (max_sp > 0 && seed_span >= max_sp) seed_span = max_sp - 1;
    int seed_lane = (int)actor->track_sub_lane_index;

    /* [#7b] World-Y lift that floats the shadow just above the road so it wins the
     * coplanar depth tie (otherwise the road occludes it -> the span-seam line).
     * A fraction of the car half-length, so it scales with the world unit.
     * [#7c] Plus a slope-proportional term: the coarse linear mesh undershoots the
     * road that keeps curving up between nodes on a grade, so scale extra clearance
     * with the wheel-plane gradient |grad| (rise/run). Flat ground (grad ~ 0) keeps
     * the planted base lift and never floats; steeper grades get more margin. */
    float wp_slope    = wp_ok ? sqrtf(wp_a * wp_a + wp_b * wp_b) : 0.0f;
    float shadow_lift = half_len * (shadow_lift_frac() + shadow_lift_slope_gain() * wp_slope);

    /* [#7c] One-time diagnostic: confirm the relaxed ceiling + slope-lift are live
     * (logs on the first STEEP grade so the values are meaningful, not on flat
     * spawn). Per-build state event, not a per-frame spam point. */
    {
        static int s_uphill_log = 0;
        if (!s_uphill_log && wp_ok && wp_slope > 0.05f) {
            s_uphill_log = 1;
            TD5_LOG_I(LOG_TAG,
                      "shadow uphill: slot=%d slope=%.3f maxWY=%.1f ceilY=%.1f "
                      "half_len=%.1f lift=%.2f (band-fix live)",
                      (int)actor->slot_index, wp_slope, maxWY, ceilY,
                      half_len, shadow_lift);
        }
    }

    /* [task#4] Low-pass the per-node ground Y across the 30Hz rebuild to kill the
     * per-tick "pop" of the RAYCAST probe (its per-node span re-resolution flips the
     * Y between adjacent spans on a slope).
     * [#7c 2026-06-21] GATED to the raycast fallback (!wp_ok). The #7 wheel-plane Y is
     * already continuous tick-to-tick (the four wheel contacts move smoothly over the
     * continuous ground — no span lookup, no pop), so low-passing it adds NO benefit
     * and only LAGS the shadow Y behind the car as it climbs/descends. On a steady
     * grade a first-order low-pass trails a ramp by a constant offset; viewed side-on
     * THAT lag is the OPPONENT "shadow desync on slopes" the player (viewed down-axis)
     * hides. So the wheel-plane path tracks the car instantly. Smoothing still applies
     * to the raycast fallback, and only when the previous grid is the IMMEDIATELY
     * PRECEDING tick (consecutive); after a gap the old Ys are stale, so snap. */
    uint32_t cur_tick = (uint32_t)g_td5.simulation_tick_counter;
    int smooth_y = (shadow_antiflicker_enabled() && g->valid && !wp_ok &&
                    g->built_tick + 1u == cur_tick);

    for (int r = 0; r < SHADOW_GRID_ROWS; r++) {
        float tF = (SHADOW_GRID_ROWS > 1) ? (float)r / (float)(SHADOW_GRID_ROWS - 1) : 0.0f;
        for (int c = 0; c < SHADOW_GRID_COLS; c++) {
            float tR = (SHADOW_GRID_COLS > 1) ? (float)c / (float)(SHADOW_GRID_COLS - 1) : 0.0f;
            int   n  = r * SHADOW_GRID_COLS + c;

            /* Bilinear XZ between the four corners (front edge FL->FR, back RL->RR). */
            float fx = (1.0f - tR) * corner[0][0] + tR * corner[1][0]; /* front */
            float fz = (1.0f - tR) * corner[0][1] + tR * corner[1][1];
            float bx = (1.0f - tR) * corner[2][0] + tR * corner[3][0]; /* back  */
            float bz = (1.0f - tR) * corner[2][1] + tR * corner[3][1];
            float nx = (1.0f - tF) * fx + tF * bx;
            float nz = (1.0f - tF) * fz + tF * bz;

            /* Node ground Y. [#7] Default: evaluate the smooth wheel-contact
             * plane (no span lookup -> no uphill oscillation/flicker, always a
             * valid height -> opponent shadows never drop out). Fallback (knob
             * off): the per-node raycast probe that re-resolves the span at this
             * node — captures cross-span crests a plane misses, but oscillates on
             * slopes. */
            float ny;
            if (wp_ok) {
                ny = wp_a * nx + wp_b * nz + wp_c;
            } else {
                int32_t nx_fp = (int32_t)lroundf(nx * 256.0f);
                int32_t nz_fp = (int32_t)lroundf(nz * 256.0f);
                int32_t gy    = 0;
                if (max_sp > 0) {
                    gy = td5_track_shadow_probe_height((int)actor->slot_index, n,
                                                       seed_span, seed_lane,
                                                       nx_fp, nz_fp, NULL);
                }
                ny = (float)gy * inv256;
            }
            if (ny > ceilY) ny = ceilY;   /* [#7c] relaxed band: uphill front edge follows the rising road (body drawn after overpaints any climb) */
            if (ny < floorY) ny = floorY; /* generous floor so real dips still show */

            /* [#7b] Float the whole shadow just above the road so it wins the
             * coplanar depth tie (else the road occludes it -> the span-seam
             * line). Applied AFTER the wheel-plane clamp (so it sits a hair above
             * the wheel plane) but stays far below the car body / walls. */
            ny += shadow_lift;

            /* [task#4] ease toward the fresh raycast Y instead of snapping. */
            if (smooth_y) {
                float prevY = g->base[n][1];
                ny = prevY + (ny - prevY) * shadow_y_smooth();
            }

            g->base[n][0] = nx;
            g->base[n][1] = ny;
            g->base[n][2] = nz;

            /* Soft footprint alpha: SEPARABLE rounded-rectangle falloff. Each
             * car axis (lateral |u|, longitudinal |v|) stays full out to the
             * plateau then feathers to the footprint edge; the product fills the
             * oriented rectangle with rounded corners and a defined rim, so the
             * shadow reads as the car's shape rather than an oval. */
            float au   = fabsf((tR - 0.5f) * 2.0f);  /* 0 centre .. 1 lateral edge      */
            float av   = fabsf((tF - 0.5f) * 2.0f);  /* 0 centre .. 1 longitudinal edge */
            float fu   = 1.0f - shadow_smoothstep(SHADOW_FALLOFF_PLATEAU, SHADOW_FALLOFF_EDGE, au);
            float fv   = 1.0f - shadow_smoothstep(SHADOW_FALLOFF_PLATEAU, SHADOW_FALLOFF_EDGE, av);
            float fall = fu * fv;
            int ai = (int)(fall * SHADOW_CENTRE_ALPHA * 255.0f + 0.5f);
            if (ai < 0)   ai = 0;
            if (ai > 255) ai = 255;
            g->alpha[n] = (uint8_t)ai;
        }
    }

    g->built_tick = (uint32_t)g_td5.simulation_tick_counter;
    g->valid      = 1;
}

/* Texture-free, terrain-conforming vehicle shadow: project the cached raycast
 * grid (rebuilt once per sim tick) with the per-frame sub-tick shift and draw it
 * as a soft vertex-alpha mesh. */
static void render_vehicle_shadow_conforming(const TD5_Actor *actor)
{
    if (!actor) return;
    int slot = (int)actor->slot_index;
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return;

    /* Lazily upload the 1x1 white texture (an identical re-upload to the shared
     * HUD-dimmer page is harmless). Texture-free shadow == white * vertex. */
    if (!s_shadow_white_uploaded) {
        static const uint32_t k_white = 0xFFFFFFFFu;
        td5_plat_render_upload_texture(SHADOW_WHITE_TEX_PAGE, &k_white, 1, 1, 2);
        s_shadow_white_uploaded = 1;
    }

    ShadowGrid *g    = &s_shadow_grid[slot];
    uint32_t    tick = (uint32_t)g_td5.simulation_tick_counter;
    if (!g->valid || g->built_tick != tick)
        shadow_build_grid(actor, g);

    /* Per-frame sub-tick shift so the shadow tracks the car smoothly between sim
     * ticks (mirrors the legacy corner interpolation and the car mesh). */
    extern float g_subTickFraction;
    const float inv256 = 1.0f / 256.0f;
    const float frac   = g_subTickFraction;
    const float dx = (float)actor->linear_velocity_x * frac * inv256;
    const float dy = (float)actor->linear_velocity_y * frac * inv256;
    const float dz = (float)actor->linear_velocity_z * frac * inv256;

    /* [task#4] Depth pull toward camera. The coarse 7x9 shadow surface is a
     * linear approximation of the finely-tessellated road, so between nodes the
     * road can bulge ABOVE the shadow plane and z-fight the old 2-view-z pull
     * (angle-dependent shimmer). Anti-flicker raises the pull to ~6 view-z, still
     * an order of magnitude below the car-body depth gap (tens of view-z) so it
     * can never reach the body. =0 keeps the historical SHADOW_DEPTH_Z_BIAS. */
    const float shadow_depth_bias = shadow_antiflicker_enabled()
        ? (6.0f * DEPTH_NORMALIZE_INV)
        : SHADOW_DEPTH_Z_BIAS;

    TD5_D3DVertex verts[SHADOW_GRID_NODES];
    for (int n = 0; n < SHADOW_GRID_NODES; n++) {
        float wx = g->base[n][0] + dx;
        float wy = g->base[n][1] + dy + SHADOW_VERTICAL_OFFSET;
        float wz = g->base[n][2] + dz;

        float ddx = wx - s_camera_pos[0];
        float ddy = wy - s_camera_pos[1];
        float ddz = wz - s_camera_pos[2];

        /* camera_basis is row-major { right, up, forward } */
        float vx = ddx * s_camera_basis[0] + ddy * s_camera_basis[1] + ddz * s_camera_basis[2];
        float vy = ddx * s_camera_basis[3] + ddy * s_camera_basis[4] + ddz * s_camera_basis[5];
        float vz = ddx * s_camera_basis[6] + ddy * s_camera_basis[7] + ddz * s_camera_basis[8];

        /* [#7 2026-06-20] Near-plane handling. A node BEHIND the camera (vz <= 0)
         * means the car straddles the camera -> drop the whole shadow. But the
         * old test dropped the ENTIRE shadow when ANY node merely entered the near
         * GAP (0 < vz <= near) — so an opponent close ahead, or one cresting a
         * rise right in front, lost its shadow for several frames ("invisible at
         * some heights"). Clamp near-gap nodes to the near plane instead, keeping
         * the visible part of the patch drawn. */
        if (vz <= 0.0f) return;
        if (vz < s_near_clip) vz = s_near_clip;

        float inv_z = 1.0f / vz;
        verts[n].screen_x = -vx * s_focal_length * inv_z + s_center_x;
        verts[n].screen_y = -vy * s_focal_length * inv_z + s_center_y;
        verts[n].depth_z  = (vz - NEAR_DEPTH_OFFSET) * DEPTH_NORMALIZE_INV - shadow_depth_bias;
        verts[n].rhw      = inv_z;
        /* black RGB + soft alpha (BGRA), scaled by the actor fade
         * ([dynamic-traffic]; identity when no fade is active). */
        {
            uint32_t sa = (uint32_t)g->alpha[n];
            if (s_actor_draw_alpha < 255)
                sa = (sa * (uint32_t)s_actor_draw_alpha) >> 8;
            verts[n].diffuse = sa << 24;
        }
        verts[n].specular = 0;
        verts[n].tex_u    = 0.0f;
        verts[n].tex_v    = 0.0f;
    }

    /* Triangulate the grid: two tris per cell, winding matched to the legacy
     * quad (a,b,e then a,e,d == FL,FR,RR then FL,RR,RL). */
    uint16_t idx[SHADOW_GRID_CELLS * 6];
    int ii = 0;
    for (int r = 0; r < SHADOW_GRID_ROWS - 1; r++) {
        for (int c = 0; c < SHADOW_GRID_COLS - 1; c++) {
            uint16_t a = (uint16_t)(r * SHADOW_GRID_COLS + c);
            uint16_t b = (uint16_t)(a + 1);
            uint16_t d = (uint16_t)(a + SHADOW_GRID_COLS);
            uint16_t e = (uint16_t)(d + 1);
            idx[ii++] = a; idx[ii++] = b; idx[ii++] = e;
            idx[ii++] = a; idx[ii++] = e; idx[ii++] = d;
        }
    }

    static int s_logged = 0;
    if (!s_logged) {
        s_logged = 1;
        TD5_LOG_I(LOG_TAG,
                  "shadow: conforming raycast mesh %dx%d (%d nodes, %d tris) "
                  "texture-free, first tick=%u",
                  SHADOW_GRID_COLS, SHADOW_GRID_ROWS, SHADOW_GRID_NODES,
                  SHADOW_GRID_CELLS * 2, tick);
    }

    flush_immediate_internal();
    /* TD5_PRESET_SHADOW: z_write=0, SRCALPHA/INVSRCALPHA, point filter. */
    td5_plat_render_set_preset(TD5_PRESET_SHADOW);
    td5_plat_render_bind_texture(SHADOW_WHITE_TEX_PAGE);
    td5_plat_render_draw_tris(verts, SHADOW_GRID_NODES, idx, ii);
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

/* [task#14] VISIBLE BREAKABLE FURNITURE — TD6's smashable street props. RE
 * (2026-06-16): the VISIBLE props are level.mov (24-byte records) drawn as the
 * COL_NN.prr MESHES (NOT the invisible level.tcl collision footprints the port
 * used to render as boxes). Each MOV record picks one of 8 furniture meshes
 * (byte4&0xF) and places it at a world pos + Y-orientation. We de-indexed the
 * COL meshes into PROPMESH.BIN (tri-list pos+baked-grey) and draw each un-broken
 * prop's mesh, rotated by its angle, planted on the nearest-span ground, shaded
 * by its baked grey × a per-model furniture tint. Smashing sets broken in
 * td5_physics -> the mesh vanishes + debris. Env TD5RE_TD6_PROP_MESH (=0 off). */
static int td6_prop_mesh_enabled(void)
{
    static int s = -1;
    if (s < 0) { s = td5_env_flag_on("TD5RE_TD6_PROP_MESH"); }
    return s;
}

/* The 8 de-indexed COL furniture meshes (PROPMESH.BIN). pos = vcount*3 floats
 * (tri-list, local), col = vcount baked-grey ARGB, min_y for ground planting. */
#define TD6_PROP_MESH_MAX 12
typedef struct { float *pos; float *uv; uint32_t *col; int vcount; float min_y; } TD6PropMesh;
static TD6PropMesh s_td6_pmesh[TD6_PROP_MESH_MAX];
static int s_td6_pmesh_count = 0;

/* Furniture textures (real TD6 static.zip art), lazily uploaded once into 6
 * pages (5 source + 1 white). model (COL_NN) -> texture via the London subset.
 * [2026-06-16 BUGFIX] Base was 984 = the runtime FONT glyph atlas (td5_font.c
 * ATLAS_PAGE 984); props clobbered it -> "HUD stops rendering font". Moved to 990,
 * but that overlapped the ALLOY-RIM pool (WHEEL_RIM_TEX_BASE 994..1001) and the
 * envmap (990-993) -> cars showed the crate/redtape prop texture on their wheels.
 * 994-1001 rims, 984 font, 990-993 envmap, 1020 sky, 1021 fallback, cars 800-843,
 * track <=983 — so 1002-1007 is the free 6-page window for props. */
#define TD6_PROP_TEX_BASE 1002
#define TD6_PROP_TEX_N 5
static const char *k_td6_prop_srcs[TD6_PROP_TEX_N] = {
    "re/assets/props/td6_bench.png",     /* 0 BENCH    */
    "re/assets/props/td6_redtape.png",   /* 1 REDTAPE  */
    "re/assets/props/td6_1bollard.png",  /* 2 1BOLLARD */
    "re/assets/props/td6_k1box.png",     /* 3 K1BOX    */
    "re/assets/props/td6_1crate.png",    /* 4 1CRATE   */
};
/* model (COL_NN) -> texture index, indexed by MOV model (r[4]&0xF). AUTHORITATIVE
 * (TD6.exe FUN_0044c535 + London subset row @0x0049be58):
 *   model 0 = ff (untextured)   model 4 = REDTAPE (1)
 *   model 1 = ff (untextured)   model 5 = 1BOLLARD (2)
 *   model 2 = BENCH (0)         model 6 = K1BOX (3)
 *   model 3 = REDTAPE (1)       model 7 = 1CRATE (4)
 * [#20 2026-06-17] FIX: the table was transcribed shifted ({-1,0,1,1,1,2,3,4}),
 * which mapped the BENCH mesh (model 2 = COL_02) to texidx 1 = REDTAPE -> benches
 * rendered with the red/white tape texture. Corrected to match the row above. */
static const int k_td6_prop_texidx[8] = { -1, -1, 0, 1, 1, 2, 3, 4 };
#define TD6_PROP_WHITE_PAGE (TD6_PROP_TEX_BASE + TD6_PROP_TEX_N)  /* dedicated 1x1 white for untextured props */
static int s_td6_prop_pool_loaded = 0;
static void td6_prop_load_pool(void)
{
    int i;
    if (s_td6_prop_pool_loaded) return;
    s_td6_prop_pool_loaded = 1;
    for (i = 0; i < TD6_PROP_TEX_N; i++)
        if (!td5_asset_load_png_texture(TD6_PROP_TEX_BASE + i, k_td6_prop_srcs[i], TD5_COLORKEY_NONE))
            TD5_LOG_W(LOG_TAG, "td6 prop tex: failed %s", k_td6_prop_srcs[i]);
    { uint32_t white = 0xFFFFFFFFu;  /* own white page (shared 899 can carry HUD/font pixels) */
      td5_plat_render_upload_texture(TD6_PROP_WHITE_PAGE, &white, 1, 1, 2); }
}

void td5_render_load_td6_prop_meshes(const void *data, size_t size)
{
    int i;
    for (i = 0; i < s_td6_pmesh_count; i++) {
        free(s_td6_pmesh[i].pos); free(s_td6_pmesh[i].uv); free(s_td6_pmesh[i].col);
        s_td6_pmesh[i].pos = NULL; s_td6_pmesh[i].uv = NULL; s_td6_pmesh[i].col = NULL;
        s_td6_pmesh[i].vcount = 0;
    }
    s_td6_pmesh_count = 0;
    if (!data || size < 8) return;
    {
        const uint8_t *p = (const uint8_t *)data;
        uint32_t mc; int vcounts[TD6_PROP_MESH_MAX]; const uint8_t *vd; size_t off;
        if (p[0] != 'P' || p[1] != 'M' || p[2] != 'S' || p[3] != '2') return;  /* PMS2 = pos+uv+col */
        memcpy(&mc, p + 4, 4);
        if (mc > TD6_PROP_MESH_MAX) mc = TD6_PROP_MESH_MAX;
        if (size < 8 + (size_t)mc * 8) return;
        for (i = 0; i < (int)mc; i++) memcpy(&vcounts[i], p + 8 + i * 8, 4);
        vd = p + 8 + mc * 8; off = 0;
        for (i = 0; i < (int)mc; i++) {
            int vc = vcounts[i], v; float miny = 1e30f;
            if (vc < 0) vc = 0;
            if ((size_t)(vd - p) + off + (size_t)vc * 24 > size) break;  /* 24B/vert */
            s_td6_pmesh[i].pos = (float *)malloc((size_t)vc * 3 * sizeof(float));
            s_td6_pmesh[i].uv  = (float *)malloc((size_t)vc * 2 * sizeof(float));
            s_td6_pmesh[i].col = (uint32_t *)malloc((size_t)vc * sizeof(uint32_t));
            s_td6_pmesh[i].vcount =
                (s_td6_pmesh[i].pos && s_td6_pmesh[i].uv && s_td6_pmesh[i].col) ? vc : 0;
            for (v = 0; v < s_td6_pmesh[i].vcount; v++) {
                float buf[5]; uint32_t c;            /* x,y,z,u,v + color */
                memcpy(buf, vd + off + (size_t)v * 24, 20);
                memcpy(&c, vd + off + (size_t)v * 24 + 20, 4);
                s_td6_pmesh[i].pos[v*3+0] = buf[0];
                s_td6_pmesh[i].pos[v*3+1] = buf[1];
                s_td6_pmesh[i].pos[v*3+2] = buf[2];
                s_td6_pmesh[i].uv[v*2+0]  = buf[3];
                s_td6_pmesh[i].uv[v*2+1]  = buf[4];
                s_td6_pmesh[i].col[v] = c;
                if (buf[1] < miny) miny = buf[1];
            }
            s_td6_pmesh[i].min_y = (s_td6_pmesh[i].vcount > 0) ? miny : 0.0f;
            off += (size_t)vc * 24;
            s_td6_pmesh_count = i + 1;
        }
    }
    TD5_LOG_I(LOG_TAG, "TD6 prop meshes loaded: %d", s_td6_pmesh_count);
}

void render_td6_props(const TD5_Actor *ref)
{
    if (!td6_prop_mesh_enabled() || g_active_td6_level <= 0 || !ref) return;
    int n = td5_track_td6_prop_count();
    if (n <= 0 || s_td6_pmesh_count <= 0) return;

    const float inv256 = 1.0f / 256.0f;
    const float gx = (float)ref->world_pos.x * inv256;
    const float gy = (float)ref->world_pos.y * inv256;
    const float gz = (float)ref->world_pos.z * inv256;
    const float MAXD = 50000.0f;
    int drew = 0, last_page = -1;

    for (int i = 0; i < n; i++) {
        int32_t px, py, pz; int model, angle, page;
        const TD6PropMesh *m;
        float cx, cz, ax, az, base_y, lift, a, ca, sa;
        int v, vc, ni, t, any;
        TD5_D3DVertex vb[128];
        uint16_t ib[384];
        uint8_t ok[128];

        if (td5_track_td6_prop_is_broken(i)) continue;
        if (!td5_track_td6_prop_get_mov(i, &px, &py, &pz, &model, &angle)) continue;
        if (model < 0 || model >= s_td6_pmesh_count) continue;
        m = &s_td6_pmesh[model];
        if (m->vcount < 3) continue;
        cx = (float)px * inv256; cz = (float)pz * inv256;
        ax = cx - gx; az = cz - gz;
        if (ax * ax + az * az > MAXD * MAXD) continue;

        /* [#20] Plant FLAT on the track surface under the prop (yaw only). Matches the
         * original: props are glued to the ground horizontally; on a slope the high side
         * rests on the ground and the rest clips into the terrain (NOT tilted to the
         * slope normal). td5_track_td6_prop_ground_y gives the planting height. */
        base_y = td5_track_td6_prop_ground_y(i, gy);
        lift   = base_y - m->min_y;                    /* mesh bottom -> ground */
        (void)py;                                      /* MOV Y handled inside ground_y */
        a  = (float)angle * (2.0f * (float)M_PI / 4096.0f);
        ca = cosf(a); sa = sinf(a);

        vc = m->vcount; if (vc > 128) vc = 128;
        any = 0;
        for (v = 0; v < vc; v++) {
            float lx = m->pos[v*3+0], ly = m->pos[v*3+1], lz = m->pos[v*3+2];
            float rx = lx*ca + lz*sa, rz = -lx*sa + lz*ca;
            float wx = cx + rx, wy = lift + ly, wz = cz + rz;
            float ddx = wx-s_camera_pos[0], ddy = wy-s_camera_pos[1], ddz = wz-s_camera_pos[2];
            float vx = ddx*s_camera_basis[0]+ddy*s_camera_basis[1]+ddz*s_camera_basis[2];
            float vyv= ddx*s_camera_basis[3]+ddy*s_camera_basis[4]+ddz*s_camera_basis[5];
            float vz = ddx*s_camera_basis[6]+ddy*s_camera_basis[7]+ddz*s_camera_basis[8];
            float invz; int grey, sh;
            if (vz <= s_near_clip) {
                ok[v] = 0;
                vb[v].screen_x=0; vb[v].screen_y=0; vb[v].depth_z=0; vb[v].rhw=0;
                vb[v].diffuse=0; vb[v].specular=0; vb[v].tex_u=0; vb[v].tex_v=0;
                continue;
            }
            invz = 1.0f/vz;
            /* baked grey (R=G=B) modulates the furniture texture for 3D shading;
             * gl==0 verts default to near-full brightness. */
            grey = (int)((m->col[v] >> 16) & 0xFF);
            sh = (grey > 0) ? (110 + grey * 145 / 255) : 235;
            if (sh > 255) sh = 255;
            vb[v].screen_x = -vx *s_focal_length*invz + s_center_x;
            vb[v].screen_y = -vyv*s_focal_length*invz + s_center_y;
            vb[v].depth_z  = (vz - NEAR_DEPTH_OFFSET) * DEPTH_NORMALIZE_INV;
            vb[v].rhw      = invz;
            vb[v].diffuse  = 0xFF000000u | ((uint32_t)sh<<16) | ((uint32_t)sh<<8) | (uint32_t)sh;
            vb[v].specular = 0;
            vb[v].tex_u = m->uv[v*2+0]; vb[v].tex_v = m->uv[v*2+1];
            ok[v] = 1; any = 1;
        }
        if (!any) continue;

        ni = 0;
        for (t = 0; t + 2 < vc; t += 3) {
            if (!ok[t] || !ok[t+1] || !ok[t+2]) continue;
            if (ni + 6 > (int)(sizeof ib / sizeof ib[0])) break;
            /* both windings so the mesh is solid regardless of cull (z-buffer picks nearest) */
            ib[ni++]=(uint16_t)t;     ib[ni++]=(uint16_t)(t+1); ib[ni++]=(uint16_t)(t+2);
            ib[ni++]=(uint16_t)t;     ib[ni++]=(uint16_t)(t+2); ib[ni++]=(uint16_t)(t+1);
        }
        if (ni == 0) continue;

        if (!drew) {
            flush_immediate_internal();
            td6_prop_load_pool();
            td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
            drew = 1;
        }
        {
            int ti = k_td6_prop_texidx[model & 7];
            page = (ti < 0) ? TD6_PROP_WHITE_PAGE : (TD6_PROP_TEX_BASE + ti);
        }
        if (page != last_page) { td5_plat_render_bind_texture(page); last_page = page; }
        td5_plat_render_draw_tris(vb, vc, ib, ni);
    }
}

/* Dispatcher (keeps the name the per-view shadow pre-pass calls): the new
 * conforming raycast mesh by default, the legacy textured quad when the A/B
 * env knob TD5RE_SHADOW_RAYCAST=0 is set. */
void render_vehicle_shadow_quad(const TD5_Actor *actor)
{
    if (shadow_raycast_enabled())
        render_vehicle_shadow_conforming(actor);
    else
        render_vehicle_shadow_quad_legacy(actor);
}

/* --- Vehicle Brake Lights (0x4011C0) --- */

/* BRAKED sprite atlas cache */
static int   s_braked_page = -1;
static float s_braked_u0, s_braked_v0, s_braked_u1, s_braked_v1;
static int   s_braked_lookup_done = 0;
static uint8_t s_brake_brightness[TD5_ACTOR_MAX_TOTAL_SLOTS]; /* per-slot brightness ramp */

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
void render_vehicle_brake_lights(const TD5_Actor *actor, int slot)
{
    if (!actor) return;
    if (!s_braked_lookup_done) brake_light_lookup_atlas();
    if (s_braked_page < 0) return;
    if (slot < 0 || slot >= TD5_ACTOR_MAX_TOTAL_SLOTS) return;

    /* Read brake_flag at actor+0x36D; also light on the HANDBRAKE (+0x36E). */
    const uint8_t *ap = (const uint8_t *)actor;
    /* [FIX 2026-06-02] Brake lights illuminate for the handbrake too, not just the
     * foot brake. The throttle+handbrake power-slide deviation clears brake_flag so
     * the drive path runs for the donut, which left the lights dark; the original
     * handbrake sets brake_flag=1 (lights on), so include handbrake_flag here. */
    int braking = (*(ap + 0x36D) != 0 || *(ap + 0x36E) != 0);

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

    /* [2026-06-08 procedural FX] Texture-free red brake lamp via ps_fx_glow
     * (p0=0 -> SRC_ALPHA lamp). Bound once for both lights; the per-light draws
     * below inherit the override. No BRAKED atlas sprite. */
    int glow_fx = 0;
    uint32_t lamp_col = 0xFFFFFFFFu;          /* legacy: white * red BRAKED texture */
    float bu0 = s_braked_u0, bv0 = s_braked_v0, bu1 = s_braked_u1, bv1 = s_braked_v1;
    if (td5_vfx_proc_enabled() && td5_plat_fx_begin(TD5_FX_GLOW, 0.0f, 0.0f)) {
        glow_fx = 1;
        uint32_t ai = (uint32_t)bright * 2u; if (ai > 255u) ai = 255u;  /* 0..0x80 -> 0..255 */
        lamp_col = (ai << 24) | 0x00FF1808u;  /* bright red lamp, alpha = brightness ramp */
        bu0 = 0.0f; bv0 = 0.0f; bu1 = 1.0f; bv1 = 1.0f;   /* canonical UVs for the shader */
    }

    for (int light = 0; light < 2; light++) {
        /* Taillight hardpoint, int16[3] model space. [S23] For ported TD6 cars
         * the binary carparam.dat carries WRONG values at +0x60/+0x68 (it is not
         * TD6's CAR_LIGHTS field), so the asset loader installs the authored TD6
         * :CAR_LIGHTS0/1: positions per slot via td5_render_set_vehicle_taillights.
         * Use those when present; otherwise read the cardef hardpoint (TD5 cars +
         * donor-param TD6 cars aud/pro/xjr with no .scr). */
        int16_t hp[3];
        if (g_vehicle_taillight_valid[slot]) {
            hp[0] = g_vehicle_taillight[slot][light][0];
            hp[1] = g_vehicle_taillight[slot][light][1];
            hp[2] = g_vehicle_taillight[slot][light][2];
        } else {
            memcpy(hp, (uint8_t *)car_def + 0x60 + light * 8, 6);
        }

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
        v[0].diffuse  = lamp_col; v[0].specular = 0;
        v[0].tex_u    = bu0; v[0].tex_v = bv0;
        /* BL */
        v[1].screen_x = cx - h;  v[1].screen_y = cy + h;
        v[1].depth_z  = depth;   v[1].rhw      = inv_z;
        v[1].diffuse  = lamp_col; v[1].specular = 0;
        v[1].tex_u    = bu0; v[1].tex_v = bv1;
        /* TR */
        v[2].screen_x = cx + h;  v[2].screen_y = cy - h;
        v[2].depth_z  = depth;   v[2].rhw      = inv_z;
        v[2].diffuse  = lamp_col; v[2].specular = 0;
        v[2].tex_u    = bu1; v[2].tex_v = bv0;
        /* BR */
        v[3].screen_x = cx + h;  v[3].screen_y = cy + h;
        v[3].depth_z  = depth;   v[3].rhw      = inv_z;
        v[3].diffuse  = lamp_col; v[3].specular = 0;
        v[3].tex_u    = bu1; v[3].tex_v = bv1;

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

    if (glow_fx) td5_plat_fx_end();

    /* Restore opaque so it doesn't leak into next mesh */
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

/* --- Vehicle Headlight FLOOD (port extension — ground light pool) ---
 * Draws a soft ADDITIVE light quad on the ROAD, extending forward from the car's
 * front axle, bright at the car and fading forward — the visible "flood zone"
 * that per-vertex lighting alone can't paint on the coarse track geometry. Built
 * from the four wheel probes (world-space ground contacts): front-axle midpoint
 * as the near edge, forward = front->rear axle direction, half-width from the
 * front-track vector. Rendered like the vehicle shadow (world corners projected
 * through the camera basis, z-tested, tiny toward-camera bias vs the road).
 * Env: TD5RE_HEADLIGHT_FLOOD (0/1), _FLOOD_LEN (forward reach), _FLOOD_BRIGHT. */
static void render_vehicle_headlight_flood(const TD5_Actor *actor)
{
    static int   rd = 0;
    static int   s_flood_on      = 0;   /* superseded by the deferred light pass; TD5RE_HEADLIGHT_FLOOD=1 re-enables the legacy mesh flood */
    static float s_flood_len      = 14000.0f; /* forward reach on the road (world units; track scale ~100k) */
    static float s_flood_ahead    = 300.0f;   /* start this far ahead of the front axle */
    static float s_flood_bright   = 225.0f;   /* additive brightness at the lamps (0..255) */
    static float s_flood_nearw    = 750.0f;   /* fan half-width near the car */
    static float s_flood_farw     = 4200.0f;  /* fan half-width far ahead */
    static float s_flood_lift     = 22.0f;    /* raise the mesh off the road; world +Y is DOWN */
    static float s_flood_falloff  = 1.05f;    /* forward fade exponent (higher = shorter throw) */
    static float s_flood_zbias    = 45.0f;    /* toward-camera depth bias (view-z): big enough to beat the coplanar road z-fight (no flicker), far below the car gap so the car still occludes it */
    if (!rd) {
        rd = 1;
        const char *e;
        if ((e = getenv("TD5RE_HEADLIGHT_FLOOD"))         && e[0]) s_flood_on      = (e[0] != '0');
        if ((e = getenv("TD5RE_HEADLIGHT_FLOOD_LEN"))     && e[0]) s_flood_len     = (float)atof(e);
        if ((e = getenv("TD5RE_HEADLIGHT_FLOOD_AHEAD"))   && e[0]) s_flood_ahead   = (float)atof(e);
        if ((e = getenv("TD5RE_HEADLIGHT_FLOOD_BRIGHT"))  && e[0]) s_flood_bright  = (float)atof(e);
        if ((e = getenv("TD5RE_HEADLIGHT_FLOOD_NEARW"))   && e[0]) s_flood_nearw   = (float)atof(e);
        if ((e = getenv("TD5RE_HEADLIGHT_FLOOD_FARW"))    && e[0]) s_flood_farw    = (float)atof(e);
        if ((e = getenv("TD5RE_HEADLIGHT_FLOOD_LIFT"))    && e[0]) s_flood_lift    = (float)atof(e);
        if ((e = getenv("TD5RE_HEADLIGHT_FLOOD_FALLOFF")) && e[0]) s_flood_falloff = (float)atof(e);
        if ((e = getenv("TD5RE_HEADLIGHT_FLOOD_ZBIAS"))   && e[0]) s_flood_zbias   = (float)atof(e);
    }
    if (!s_flood_on || !actor) return;

    const float inv256 = 1.0f / 256.0f;
    extern float g_subTickFraction;
    float frac = g_subTickFraction;
    float idx = (float)actor->linear_velocity_x * frac * inv256;
    float idy = (float)actor->linear_velocity_y * frac * inv256;
    float idz = (float)actor->linear_velocity_z * frac * inv256;

    float FLx = (float)actor->probe_FL.x*inv256+idx, FLy = (float)actor->probe_FL.y*inv256+idy, FLz = (float)actor->probe_FL.z*inv256+idz;
    float FRx = (float)actor->probe_FR.x*inv256+idx, FRy = (float)actor->probe_FR.y*inv256+idy, FRz = (float)actor->probe_FR.z*inv256+idz;
    float RLx = (float)actor->probe_RL.x*inv256+idx, RLy = (float)actor->probe_RL.y*inv256+idy, RLz = (float)actor->probe_RL.z*inv256+idz;
    float RRx = (float)actor->probe_RR.x*inv256+idx, RRy = (float)actor->probe_RR.y*inv256+idy, RRz = (float)actor->probe_RR.z*inv256+idz;

    float fmx = (FLx+FRx)*0.5f, fmz = (FLz+FRz)*0.5f;   /* front-axle mid (ground) */
    float rmx = (RLx+RRx)*0.5f, rmz = (RLz+RRz)*0.5f;   /* rear-axle mid  */
    float fdx = fmx-rmx, fdz = fmz-rmz;                 /* ground forward (XZ) */
    float flen = sqrtf(fdx*fdx + fdz*fdz);
    if (flen < 1e-3f) return;
    fdx /= flen; fdz /= flen;
    float rgx = -fdz, rgz = fdx;                        /* ground right (lateral) unit */

    /* Fit the road plane y = pa*x + pb*z + pc from the four wheel contacts, so
     * every grid node's Y hugs the road surface (conforming). This is why the
     * shadow is visible from this low camera where a flat quad goes edge-on. */
    float xz[4][2] = { {FLx,FLz}, {FRx,FRz}, {RLx,RLz}, {RRx,RRz} };
    float wy[4]    = { FLy, FRy, RLy, RRy };
    float pa, pb, pc;
    if (!shadow_fit_wheel_plane(xz, wy, &pa, &pb, &pc)) {
        pa = pb = 0.0f;
        pc = 0.25f*(FLy+FRy+RLy+RRy);
    }

    /* Headlight lateral separation (each lamp inboard of its front wheel) + the
     * per-beam gaussian half-width near the car. */
    float track    = sqrtf((FRx-FLx)*(FRx-FLx) + (FRz-FLz)*(FRz-FLz));
    float lamp_sep = track * 0.42f;
    float bw_near  = lamp_sep * 1.25f + 550.0f;   /* broad lobes -> cones merge into a full flood */

    /* Conforming fan grid on the road ahead. Per-node additive brightness = TWO
     * headlight cones: a soft gaussian lobe around each lamp's forward beam axis
     * (+/- lamp_sep) that widens with distance, times a forward fade. Drawn as an
     * additive road-conforming mesh (same tech as the shadow -> visible on any
     * slope). Env: _LEN reach, _AHEAD start, _NEARW/_FARW fan width, _BRIGHT,
     * _FALLOFF forward-fade exponent, _LIFT. */
    #define HLF_COLS 11
    #define HLF_ROWS 14
    TD5_D3DVertex verts[HLF_COLS*HLF_ROWS];
    for (int r = 0; r < HLF_ROWS; r++) {
        float tF = (float)r / (float)(HLF_ROWS - 1);              /* 0 near .. 1 far */
        float d  = s_flood_ahead + (s_flood_len - s_flood_ahead) * tF;
        float hw = s_flood_nearw + (s_flood_farw - s_flood_nearw) * tF;
        float bw = bw_near + (s_flood_farw*0.85f - bw_near) * tF; /* beam widens with distance */
        float ff = powf(1.0f - tF, s_flood_falloff);             /* forward fade */
        float ccx = fmx + fdx*d, ccz = fmz + fdz*d;
        for (int c = 0; c < HLF_COLS; c++) {
            float tR  = (HLF_COLS>1) ? ((float)c/(float)(HLF_COLS-1))*2.0f - 1.0f : 0.0f;
            float lat = tR * hw;                                 /* lateral offset from centreline */
            float nx  = ccx + rgx*lat;
            float nz  = ccz + rgz*lat;
            float ny  = pa*nx + pb*nz + pc - s_flood_lift;
            /* two-cone brightness: gaussian lobes centred on +/- lamp_sep */
            float dl  = (lat + lamp_sep) / bw;
            float dr  = (lat - lamp_sep) / bw;
            float lobe = expf(-dl*dl) + expf(-dr*dr);
            if (lobe > 1.0f) lobe = 1.0f;
            int bri = (int)(s_flood_bright * ff * lobe);
            if (bri < 0)   bri = 0;
            if (bri > 255) bri = 255;
            uint32_t col = 0xFF000000u | ((uint32_t)bri<<16) | ((uint32_t)bri<<8) | (uint32_t)bri;

            int n = r*HLF_COLS + c;
            float dx = nx - s_camera_pos[0], dy = ny - s_camera_pos[1], dz = nz - s_camera_pos[2];
            float vx = dx*s_camera_basis[0] + dy*s_camera_basis[1] + dz*s_camera_basis[2];
            float vy = dx*s_camera_basis[3] + dy*s_camera_basis[4] + dz*s_camera_basis[5];
            float vz = dx*s_camera_basis[6] + dy*s_camera_basis[7] + dz*s_camera_basis[8];
            if (vz < s_near_clip) vz = s_near_clip;   /* clamp near-gap nodes, keep the patch */
            float inv_z = 1.0f/vz;
            verts[n].screen_x = -vx*s_focal_length*inv_z + s_center_x;
            verts[n].screen_y = -vy*s_focal_length*inv_z + s_center_y;
            verts[n].depth_z  = (vz-NEAR_DEPTH_OFFSET)*DEPTH_NORMALIZE_INV - s_flood_zbias*DEPTH_NORMALIZE_INV;
            verts[n].rhw = inv_z; verts[n].diffuse = col; verts[n].specular = 0;
            verts[n].tex_u = 0.0f; verts[n].tex_v = 0.0f;
        }
    }

    uint16_t idxb[(HLF_COLS-1)*(HLF_ROWS-1)*6];
    int ii = 0;
    for (int r = 0; r < HLF_ROWS-1; r++) {
        for (int c = 0; c < HLF_COLS-1; c++) {
            uint16_t a  = (uint16_t)(r*HLF_COLS + c);
            uint16_t b2 = (uint16_t)(a + 1);
            uint16_t d2 = (uint16_t)(a + HLF_COLS);
            uint16_t e2 = (uint16_t)(d2 + 1);
            idxb[ii++]=a; idxb[ii++]=b2; idxb[ii++]=e2;
            idxb[ii++]=a; idxb[ii++]=e2; idxb[ii++]=d2;
        }
    }

    flush_immediate_internal();
    /* z-tested additive (ONE/ONE, z_write off): the CAR (hundreds of view-z
     * closer) occludes the flood, but the strong toward-camera s_flood_zbias
     * beats the coplanar-road z-fight so it doesn't flicker. Far nodes fade to ~0
     * brightness, so any residual z-fight far out adds nothing visible. */
    td5_plat_render_set_preset(TD5_PRESET_ADDITIVE);
    td5_plat_render_bind_texture(899);   /* 1x1 white -> additive brightness = per-node value */
    td5_plat_render_draw_tris(verts, HLF_COLS*HLF_ROWS, idxb, ii);
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
    #undef HLF_COLS
    #undef HLF_ROWS
}

/* --- Vehicle Headlights (port extension — visible front lamps + road flood) ---
 * The original TD5 has no headlights (only the rear taillight quads above). This
 * draws two ADDITIVE white glow sprites at each car's FRONT lamp positions — the
 * rear taillight hardpoints (car_def+0x60/+0x68) mirrored in Z — so headlights
 * read as visibly "on", plus the ground flood pool above. The per-vertex point
 * lights in td5_light.c still tint nearby geometry. Called from the actor pass
 * with the render transform already loaded, like render_vehicle_brake_lights.
 * Gated by the lighting/headlight enables. */
void render_vehicle_headlights(const TD5_Actor *actor, int slot)
{
    if (!actor) return;
    if (slot < 0 || slot >= TD5_ACTOR_MAX_TOTAL_SLOTS) return;
    if (!td5_light_enabled() || !td5_light_headlights_enabled()) return;

    /* Legacy mesh flood (default off; deferred light pass replaces it). */
    render_vehicle_headlight_flood(actor);

    /* Visible front headlamp GLOW sprite — the little circle on the car. Removed
     * by default (user request 2026-07-01): the deferred road flood is the whole
     * effect now. TD5RE_HEADLAMP_SPRITE=1 restores the lamp glow. */
    static int s_headlamp_sprite = -1;
    if (s_headlamp_sprite < 0) {
        s_headlamp_sprite = td5_env_flag_off("TD5RE_HEADLAMP_SPRITE");
    }
    if (!s_headlamp_sprite) return;

    const uint8_t *ap = (const uint8_t *)actor;
    void *car_def = NULL;
    memcpy(&car_def, ap + 0x1B8, sizeof(void *));
    if (!car_def) return;

    /* Rear lamp hardpoints (int16[3] model space); front = mirror of Z. */
    int16_t hp0[3], hp1[3];
    memcpy(hp0, (const uint8_t *)car_def + 0x60, 6);
    memcpy(hp1, (const uint8_t *)car_def + 0x68, 6);
    const int16_t *hp[2] = { hp0, hp1 };

    const float *m = s_render_transform.m;
    const float half_size = 60.0f;   /* lamp billboard half-extent (model units) */

    /* Texture-free additive white lamp via the glow FX (canonical UVs); falls
     * back to the 1x1 white page as an additive square when the proc FX is off. */
    int glow_fx = (td5_vfx_proc_enabled() && td5_plat_fx_begin(TD5_FX_GLOW, 0.0f, 1.0f));
    uint32_t lamp_col = 0xFFFFFFFFu;   /* opaque white (additive) */

    for (int lamp = 0; lamp < 2; lamp++) {
        float px =  (float)hp[lamp][0];
        float py =  (float)hp[lamp][1];
        float pz = -(float)hp[lamp][2];   /* front = mirror of the rear lamp in Z */

        float vx = px*m[0] + py*m[1] + pz*m[2] + m[9];
        float vy = px*m[3] + py*m[4] + pz*m[5] + m[10];
        float vz = px*m[6] + py*m[7] + pz*m[8] + m[11];
        if (vz <= s_near_clip) continue;

        float inv_z = 1.0f / vz;
        float cx = -vx * s_focal_length * inv_z + s_center_x;
        float cy = -vy * s_focal_length * inv_z + s_center_y;
        float depth = (vz - NEAR_DEPTH_OFFSET) * DEPTH_NORMALIZE_INV - BRAKE_DEPTH_BIAS;
        float h = half_size * s_focal_length * inv_z;

        TD5_D3DVertex v[4];
        v[0].screen_x=cx-h; v[0].screen_y=cy-h; v[0].depth_z=depth; v[0].rhw=inv_z; v[0].diffuse=lamp_col; v[0].specular=0; v[0].tex_u=0.0f; v[0].tex_v=0.0f;
        v[1].screen_x=cx-h; v[1].screen_y=cy+h; v[1].depth_z=depth; v[1].rhw=inv_z; v[1].diffuse=lamp_col; v[1].specular=0; v[1].tex_u=0.0f; v[1].tex_v=1.0f;
        v[2].screen_x=cx+h; v[2].screen_y=cy-h; v[2].depth_z=depth; v[2].rhw=inv_z; v[2].diffuse=lamp_col; v[2].specular=0; v[2].tex_u=1.0f; v[2].tex_v=0.0f;
        v[3].screen_x=cx+h; v[3].screen_y=cy+h; v[3].depth_z=depth; v[3].rhw=inv_z; v[3].diffuse=lamp_col; v[3].specular=0; v[3].tex_u=1.0f; v[3].tex_v=1.0f;
        uint16_t idx[6] = { 0, 1, 2, 1, 3, 2 };
        flush_immediate_internal();
        td5_plat_render_set_preset(TD5_PRESET_ADDITIVE_GLOW);
        if (!glow_fx) td5_plat_render_bind_texture(899);   /* 1x1 white fallback */
        td5_plat_render_draw_tris(v, 4, idx, 6);
    }

    if (glow_fx) td5_plat_fx_end();
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
    /* [2026-06-08 procedural FX] When proc FX is on, draw the strobe as a
     * texture-free additive radial glow (ps_fx_glow, p0=1). `color` then carries
     * the layer tint (red/blue) with the pulse in alpha; UVs become canonical. */
    int glow_fx = td5_vfx_proc_enabled();
    if (!glow_fx && tex_page < 0) return;
    if (shared_world_z <= s_near_clip) return;
    if (glow_fx) { u0 = 0.0f; v0 = 0.0f; u1 = 1.0f; v1 = 1.0f; }

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
    int gx = glow_fx && td5_plat_fx_begin(TD5_FX_GLOW, 0.0f, 1.0f); /* p0=1 -> additive glow */
    if (!gx) td5_plat_render_bind_texture(tex_page);
    td5_plat_render_draw_tris(v, 4, idx, 6);
    if (gx) td5_plat_fx_end();
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
void render_tracked_actor_marker(const TD5_Actor *actor,
                                        const TD5_Mat3x3 *body_rot,
                                        const TD5_Vec3f *body_pos,
                                        int32_t intensity)
{
    if (!actor) return;
    if (!td5_vfx_tracked_marker_initialized()) return;

    /* Intensity 0..0x1000 — 0 means nothing to draw. Passed by the caller
     * (the wanted-target tracker for Cop Chase mode, or a steady value for a
     * chasing police cop in the rewritten traffic chase). */
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
    int wrapped    = td5_angle12_signed(base_yaw);
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
        /* [2026-06-08 procedural FX] Per-layer glow tint for the texture-free
         * path (layer 0 = red strobe, 1 = blue strobe, 2 = base red/blue by
         * marker). Pulse brightness `a` goes in the alpha; legacy path keeps the
         * gray pulse_color (texture supplies the colour). */
        const int   proc_marker = td5_vfx_proc_enabled();
        const uint32_t tint_red  = 0x00FF2018u;
        const uint32_t tint_blue = 0x002048FFu;
        uint32_t col0 = proc_marker ? ((a << 24) | tint_red)  : pulse_color;
        uint32_t col1 = proc_marker ? ((a << 24) | tint_blue) : pulse_color;
        uint32_t col2 = proc_marker ? ((a << 24) | (marker == 0 ? tint_red : tint_blue))
                                     : pulse_color;
        {
            int   page = td5_vfx_tracked_marker_get_page(marker, 0);
            float u0, v0, u1, v1;
            td5_vfx_tracked_marker_get_uv(marker, 0, &u0, &v0, &u1, &v1);
            tracked_marker_emit_quad_world(l0_corners, fVar7,
                                            u0, v0, u1, v1, col0, page);
        }
        {
            int   page = td5_vfx_tracked_marker_get_page(marker, 1);
            float u0, v0, u1, v1;
            td5_vfx_tracked_marker_get_uv(marker, 1, &u0, &v0, &u1, &v1);
            tracked_marker_emit_quad_world(l1_corners, fVar7,
                                            u0, v0, u1, v1, col1, page);
        }
        {
            int   page = td5_vfx_tracked_marker_get_page(marker, 2);
            float u0, v0, u1, v1;
            td5_vfx_tracked_marker_get_uv(marker, 2, &u0, &v0, &u1, &v1);
            tracked_marker_emit_quad_world(l2_corners, l2_z,
                                            u0, v0, u1, v1, col2, page);
        }
    }

    /* Restore opaque preset so it doesn't leak into next pass (mirrors
     * brake_lights tail). */
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

/* ========================================================================
 * [ARCADE 2026-06-26 / item-box rework 2026-06-26] Mario-Kart-style floating
 * collectible power-up BOXES + dropped hazards. Drawn once per viewport from
 * the per-view world-FX pass in td5_game.c (right after the particles), so the
 * camera basis/pos for THIS view is already loaded.
 *
 * Each active pad renders as:
 *   - an additive radial GLOW HALO behind it (proc-FX path; per-kind tint),
 *   - a rotating, pulsating WIREFRAME CUBE (12 thick depth-correct edges via the
 *     debug-line path — opaque, depth-tested vs the scene, depth-write off), and
 *   - a distinctive per-kind ICON floating inside, camera-facing.
 * Animation (spin / pulse / bob) is wall-clock (td5_plat_time_ms) — purely
 * cosmetic, never feeds the deterministic sim, so it's netplay-safe.
 *
 * No-op outside ARCADE mode. Reads replicated arcade state only.
 * ======================================================================== */

/* ARGB (0xAARRGGBB) tint per power-up kind (matches the HUD effect chip). */
static uint32_t arcade_pad_color(int kind)
{
    switch (kind) {
    case TD5_PU_NITRO:  return 0xFF20E0FFu;  /* cyan    — speed   */
    case TD5_PU_GHOST:  return 0xFFE6E6FFu;  /* white   — ghost   */
    case TD5_PU_WRECK:  return 0xFFFF3020u;  /* red     — wreck   */
    case TD5_PU_HAZARD: return 0xFFFFB000u;  /* amber   — hazard  */
    /* [ARCADE EXPANSION 2026-06-28] new kinds */
    case TD5_PU_SHIELD: return 0xFF40C0FFu;  /* sky blue — shield */
    case TD5_PU_FREEZE: return 0xFF80FFF0u;  /* ice cyan — EMP    */
    case TD5_PU_MAGNET: return 0xFFFF40C0u;  /* magenta  — magnet */
    case TD5_PU_ROCKET: return 0xFFFF8020u;  /* orange   — rocket */
    case TD5_PU_REPAIR: return 0xFF40FF60u;  /* green    — repair */
    default:            return 0xFFFFFFFFu;
    }
}

/* Scale an ARGB's RGB channels by f in [0,1] (keeps alpha) — drives the glow
 * "pulse" without changing the hue. */
static uint32_t arcade_scale_rgb(uint32_t c, float f)
{
    if (f < 0.0f) f = 0.0f; else if (f > 1.0f) f = 1.0f;
    uint32_t a = (c >> 24) & 0xFFu;
    uint32_t r = (uint32_t)(((c >> 16) & 0xFFu) * f);
    uint32_t g = (uint32_t)(((c >>  8) & 0xFFu) * f);
    uint32_t b = (uint32_t)(( c        & 0xFFu) * f);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static void arcade_emit_glow_at(float wx, float wy, float wz,
                                float half, uint32_t color)
{
    static const TD5_Mat3x3 ident = { { 1.0f,0.0f,0.0f, 0.0f,1.0f,0.0f, 0.0f,0.0f,1.0f } };
    TD5_Vec3f p; p.x = wx; p.y = wy; p.z = wz;
    /* Identity model rotation + world translation -> m[9..11] is the world
     * point in view space (camera basis/pos already set for this view). */
    td5_render_load_rotation(&ident);
    td5_render_load_translation(&p);
    float vx = s_render_transform.m[9];
    float vy = s_render_transform.m[10];
    float vz = s_render_transform.m[11];
    if (vz <= s_near_clip) return;
    float corners[4][2] = {
        { vx - half, vy + half }, { vx - half, vy - half },
        { vx + half, vy + half }, { vx + half, vy - half }
    };
    tracked_marker_emit_quad_world(corners, vz, 0.0f, 0.0f, 1.0f, 1.0f, color, -1);
}

/* One camera-facing icon stroke: a line from icon-local (ax,ay) to (bx,by)
 * (units of `scale`, +y up) laid in the billboard plane at world centre
 * (cx,cy,cz). Uses the camera right/up world axes (camera_basis rows 0 and 1)
 * so the symbol always faces the viewer. */
static void arcade_icon_stroke(float cx, float cy, float cz, float scale,
                               float ax, float ay, float bx, float by,
                               uint32_t col)
{
    float rx = s_camera_basis[0], ry = s_camera_basis[1], rz = s_camera_basis[2];
    float ux = s_camera_basis[3], uy = s_camera_basis[4], uz = s_camera_basis[5];
    float wax = cx + (ax * rx + ay * ux) * scale;
    float way = cy + (ax * ry + ay * uy) * scale;
    float waz = cz + (ax * rz + ay * uz) * scale;
    float wbx = cx + (bx * rx + by * ux) * scale;
    float wby = cy + (bx * ry + by * uy) * scale;
    float wbz = cz + (bx * rz + by * uz) * scale;
    td5_render_debug_line_world(wax, way, waz, wbx, wby, wbz, col);
}

/* Distinctive per-kind emblem, camera-facing, centred at (cx,cy,cz), radius
 * `scale` (render units). Line-art so it shows through the wireframe cube. */
static void arcade_draw_icon(int kind, float cx, float cy, float cz,
                             float scale, uint32_t col)
{
    switch (kind) {
    case TD5_PU_NITRO:                              /* up-arrow = speed boost */
        arcade_icon_stroke(cx, cy, cz, scale,  0.0f, -0.9f,  0.0f,  0.95f, col);
        arcade_icon_stroke(cx, cy, cz, scale, -0.55f, 0.35f, 0.0f,  0.95f, col);
        arcade_icon_stroke(cx, cy, cz, scale,  0.55f, 0.35f, 0.0f,  0.95f, col);
        arcade_icon_stroke(cx, cy, cz, scale, -0.55f,-0.2f,  0.0f,  0.4f,  col);
        arcade_icon_stroke(cx, cy, cz, scale,  0.55f,-0.2f,  0.0f,  0.4f,  col);
        break;
    case TD5_PU_GHOST: {                            /* hollow ring = phase-through */
        const float R = 0.85f;
        for (int k = 0; k < 8; k++) {
            float a0 = (float)k * 0.78539816f;       /* 45 deg steps */
            float a1 = (float)(k + 1) * 0.78539816f;
            arcade_icon_stroke(cx, cy, cz, scale,
                               cosf(a0) * R, sinf(a0) * R,
                               cosf(a1) * R, sinf(a1) * R, col);
        }
        break;
    }
    case TD5_PU_WRECK:                              /* X = smash / wrecking ball */
        arcade_icon_stroke(cx, cy, cz, scale, -0.8f, -0.8f,  0.8f,  0.8f, col);
        arcade_icon_stroke(cx, cy, cz, scale, -0.8f,  0.8f,  0.8f, -0.8f, col);
        break;
    case TD5_PU_HAZARD:                             /* exclamation mark = trap */
        arcade_icon_stroke(cx, cy, cz, scale,  0.0f,  0.9f,  0.0f, -0.1f, col);   /* bar */
        arcade_icon_stroke(cx, cy, cz, scale, -0.14f,-0.55f, 0.0f, -0.42f, col);  /* dot */
        arcade_icon_stroke(cx, cy, cz, scale,  0.0f, -0.42f, 0.14f,-0.55f, col);
        arcade_icon_stroke(cx, cy, cz, scale,  0.14f,-0.55f, 0.0f, -0.68f, col);
        arcade_icon_stroke(cx, cy, cz, scale,  0.0f, -0.68f,-0.14f,-0.55f, col);
        break;
    /* [ARCADE EXPANSION 2026-06-28] new-kind emblems (simple line-art) */
    case TD5_PU_SHIELD:                            /* shield = chevron crest */
        arcade_icon_stroke(cx, cy, cz, scale, -0.7f, 0.7f,  0.7f, 0.7f, col);
        arcade_icon_stroke(cx, cy, cz, scale, -0.7f, 0.7f, -0.7f,-0.1f, col);
        arcade_icon_stroke(cx, cy, cz, scale,  0.7f, 0.7f,  0.7f,-0.1f, col);
        arcade_icon_stroke(cx, cy, cz, scale, -0.7f,-0.1f,  0.0f,-0.85f, col);
        arcade_icon_stroke(cx, cy, cz, scale,  0.7f,-0.1f,  0.0f,-0.85f, col);
        break;
    case TD5_PU_FREEZE:                            /* snowflake / asterisk = EMP */
        arcade_icon_stroke(cx, cy, cz, scale,  0.0f, 0.9f,  0.0f,-0.9f, col);
        arcade_icon_stroke(cx, cy, cz, scale, -0.78f,0.45f, 0.78f,-0.45f, col);
        arcade_icon_stroke(cx, cy, cz, scale, -0.78f,-0.45f,0.78f, 0.45f, col);
        break;
    case TD5_PU_MAGNET:                            /* horseshoe magnet */
        arcade_icon_stroke(cx, cy, cz, scale, -0.5f, 0.85f,-0.5f,-0.2f, col);
        arcade_icon_stroke(cx, cy, cz, scale,  0.5f, 0.85f, 0.5f,-0.2f, col);
        arcade_icon_stroke(cx, cy, cz, scale, -0.5f,-0.2f, -0.2f,-0.6f, col);
        arcade_icon_stroke(cx, cy, cz, scale,  0.5f,-0.2f,  0.2f,-0.6f, col);
        arcade_icon_stroke(cx, cy, cz, scale, -0.2f,-0.6f,  0.2f,-0.6f, col);
        break;
    case TD5_PU_ROCKET:                            /* up-arrow w/ tail = rocket */
        arcade_icon_stroke(cx, cy, cz, scale,  0.0f,-0.9f,  0.0f, 0.95f, col);
        arcade_icon_stroke(cx, cy, cz, scale, -0.5f, 0.4f,  0.0f, 0.95f, col);
        arcade_icon_stroke(cx, cy, cz, scale,  0.5f, 0.4f,  0.0f, 0.95f, col);
        arcade_icon_stroke(cx, cy, cz, scale, -0.3f,-0.9f, -0.3f,-0.5f, col);
        arcade_icon_stroke(cx, cy, cz, scale,  0.3f,-0.9f,  0.3f,-0.5f, col);
        break;
    case TD5_PU_REPAIR:                            /* plus / cross = repair */
        arcade_icon_stroke(cx, cy, cz, scale,  0.0f, 0.8f,  0.0f,-0.8f, col);
        arcade_icon_stroke(cx, cy, cz, scale, -0.8f, 0.0f,  0.8f, 0.0f, col);
        break;
    default: break;
    }
}

/* Rotating wireframe cube: 8 corners rotated about the WORLD-vertical axis by
 * `yaw` then tilted about the world-X axis by `tilt` (so the tumble reads as 3D),
 * 12 edges emitted as thick depth-correct lines. h = half-size (render units). */
static void arcade_draw_cube(float cx, float cy, float cz, float h,
                             float yaw, float tilt, uint32_t col)
{
    static const signed char cc[8][3] = {
        {-1,-1,-1},{ 1,-1,-1},{ 1,-1, 1},{-1,-1, 1},   /* bottom */
        {-1, 1,-1},{ 1, 1,-1},{ 1, 1, 1},{-1, 1, 1},   /* top    */
    };
    static const unsigned char ed[12][2] = {
        {0,1},{1,2},{2,3},{3,0},   /* bottom loop */
        {4,5},{5,6},{6,7},{7,4},   /* top loop    */
        {0,4},{1,5},{2,6},{3,7},   /* verticals   */
    };
    float ca = cosf(yaw), sa = sinf(yaw);
    float cb = cosf(tilt), sb = sinf(tilt);
    float wc[8][3];
    for (int i = 0; i < 8; i++) {
        float lx = (float)cc[i][0] * h, ly = (float)cc[i][1] * h, lz = (float)cc[i][2] * h;
        float rx = lx * ca + lz * sa, rz = -lx * sa + lz * ca, ry = ly;  /* yaw (Y) */
        float ry2 = ry * cb - rz * sb, rz2 = ry * sb + rz * cb;          /* tilt (X) */
        wc[i][0] = cx + rx; wc[i][1] = cy + ry2; wc[i][2] = cz + rz2;
    }
    for (int e = 0; e < 12; e++) {
        int a = ed[e][0], b = ed[e][1];
        td5_render_debug_line_world(wc[a][0], wc[a][1], wc[a][2],
                                    wc[b][0], wc[b][1], wc[b][2], col);
    }
}

/* Flat ring laid in the world XZ plane (an oil-slick footprint) at (cx,cy,cz). */
static void arcade_draw_flat_ring(float cx, float cy, float cz, float r, uint32_t col)
{
    for (int k = 0; k < 8; k++) {
        float a0 = (float)k * 0.78539816f, a1 = (float)(k + 1) * 0.78539816f;
        td5_render_debug_line_world(cx + cosf(a0) * r, cy, cz + sinf(a0) * r,
                                    cx + cosf(a1) * r, cy, cz + sinf(a1) * r, col);
    }
}

/* world->screen projector for the flat oil disc: debug_line_project, extern
 * in td5_render.c (declared in td5_render_internal.h). */

/* Filled OPAQUE disc lying flat in the world XZ plane — the dark oil puddle.
 * Triangle fan (centre + `seg` rim points), both windings emitted so it shows
 * regardless of cull mode. Drawn via the textureless flat path (depth-tested vs
 * the scene, depth-write off) — a dark disc on the asphalt reads as an oil slick. */
static void arcade_draw_oil_disc(float cx, float cy, float cz, float r,
                                 uint32_t color, int seg)
{
    if (seg < 6)  seg = 6;
    if (seg > 24) seg = 24;
    TD5_D3DVertex vb[1 + 24];
    uint16_t      ib[24 * 6];
    if (!debug_line_project(cx, cy, cz, color, &vb[0])) return;   /* centre */
    for (int k = 0; k < seg; k++) {
        float ang = (float)k * (6.2831853f / (float)seg);
        if (!debug_line_project(cx + cosf(ang) * r, cy, cz + sinf(ang) * r,
                                color, &vb[1 + k]))
            return;                                               /* clipped — drop disc */
    }
    int ni = 0;
    for (int k = 0; k < seg; k++) {
        uint16_t a = (uint16_t)(1 + k);
        uint16_t b = (uint16_t)(1 + ((k + 1) % seg));
        ib[ni++] = 0; ib[ni++] = a; ib[ni++] = b;   /* front  */
        ib[ni++] = 0; ib[ni++] = b; ib[ni++] = a;   /* back   */
    }
    td5_plat_render_draw_tris_flat(vb, 1 + seg, ib, ni);
}

void td5_render_arcade_pads(void)
{
    if (!td5_arcade_mode_active()) return;
    int np = td5_arcade_pad_count();
    int nh = td5_arcade_hazard_count();
    if (np <= 0 && nh <= 0) return;

    /* Box half-size: auto-scaled at race init from the span length; env override. */
    float box_half = td5_arcade_box_half_world();
    if (box_half < 1.0f) box_half = 120.0f;            /* defensive fallback */
    {
        const char *e = getenv("TD5RE_ARCADE_PAD_VIS");  /* legacy override knob */
        if (e) { int v = atoi(e); if (v >= 8 && v <= 40000) box_half = (float)v; }
    }

    /* Cull boxes too far to matter so a 50-pad ring stays cheap. */
    float cull = box_half * 160.0f;
    float cull2 = cull * cull;

    float t = (float)td5_plat_time_ms() * 0.001f;       /* wall-clock seconds */
    const float tilt = 0.42f;                           /* ~24 deg fixed tumble tilt */

    for (int i = 0; i < np; i++) {
        float wx, wy, wz; int active = 0, kind = 0;
        if (!td5_arcade_pad_get(i, &wx, &wy, &wz, &active, &kind)) continue;
        if (!active) continue;

        float dcx = wx - s_camera_pos[0];
        float dcy = wy - s_camera_pos[1];
        float dcz = wz - s_camera_pos[2];
        if (dcx*dcx + dcy*dcy + dcz*dcz > cull2) continue;

        float ph    = (float)i * 0.7f;                  /* per-box phase offset */
        float pulse = 0.5f + 0.5f * sinf(t * 3.0f + ph);
        float spin  = t * 1.6f + ph;
        float bob   = sinf(t * 2.0f + ph) * box_half * 0.18f;
        float cy    = wy + bob;
        float scl   = box_half * (1.0f + 0.08f * sinf(t * 4.0f + ph));
        uint32_t kc = arcade_pad_color(kind);

        /* additive glow halo behind the box (pulses in alpha) */
        uint32_t glow = ((uint32_t)(110.0f + 110.0f * pulse) << 24) | (kc & 0x00FFFFFFu);
        arcade_emit_glow_at(wx, cy, wz, box_half * 1.9f, glow);

        /* rotating wireframe cube (brightness pulses) */
        arcade_draw_cube(wx, cy, wz, scl, spin, tilt,
                         arcade_scale_rgb(kc, 0.65f + 0.35f * pulse));

        /* distinctive per-kind icon, camera-facing, inside the cube */
        arcade_draw_icon(kind, wx, cy, wz, box_half * 0.55f,
                         arcade_scale_rgb(kc, 0.85f + 0.15f * pulse));
    }

    /* Dropped hazards: dark-oily glow + a flat amber ring on the road. */
    /* Dropped HAZARD: a dark OIL SLICK (filled near-black disc) on the road, 3
     * lanes wide (matches the spin-out radius), with bright amber hazard markings
     * (rings + an X) so it's a distinctive danger zone — not a yellow glow. The
     * next car within range spins out. */
    {
        float haz_r = td5_arcade_hazard_radius_world();
        if (haz_r < 1.0f) haz_r = box_half * 3.0f;             /* fallback */
        float hp    = 0.5f + 0.5f * sinf(t * 5.0f);            /* danger pulse (markings) */
        uint32_t ma = (uint32_t)(170.0f + 85.0f * hp); if (ma > 255u) ma = 255u;
        uint32_t mark = (ma << 24) | 0x00FFC000u;              /* bright amber markings */
        const uint32_t oil = 0xFF0B0A08u;                      /* near-black oil */
        for (int i = 0; i < nh; i++) {
            float wx, wy, wz; int owner = 0;
            if (!td5_arcade_hazard_get(i, &wx, &wy, &wz, &owner)) continue;
            float ry = wy + 6.0f;                              /* a hair above the asphalt */
            arcade_draw_oil_disc(wx, ry, wz, haz_r, oil, 18);  /* the dark oil puddle */
            arcade_draw_flat_ring(wx, ry + 1.0f, wz, haz_r,         mark);  /* outer ring */
            arcade_draw_flat_ring(wx, ry + 1.0f, wz, haz_r * 0.55f, mark);  /* inner ring */
            td5_render_debug_line_world(wx - haz_r*0.62f, ry+1.0f, wz - haz_r*0.62f,
                                        wx + haz_r*0.62f, ry+1.0f, wz + haz_r*0.62f, mark);
            td5_render_debug_line_world(wx - haz_r*0.62f, ry+1.0f, wz + haz_r*0.62f,
                                        wx + haz_r*0.62f, ry+1.0f, wz - haz_r*0.62f, mark);
        }
    }

    /* Flush the accumulated cube + icon + ring lines for this view. */
    td5_render_debug_lines_flush();

    /* NOTE: the per-car EFFECT visual (a glowing car SILHOUETTE for NITRO/WRECK/
     * HAZARD, and a translucent "ghost" body for GHOST) is drawn in the ACTOR
     * pass (td5_render_actors_for_view), not here — it tints/fades the car's own
     * mesh so it follows the silhouette, instead of a flat glow billboard over
     * the car. See s_actor_effect_tint / the arcade block in that loop. */

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

/* ============================================================================
 *  UNIFIED PROCEDURAL WHEEL SYSTEM  (overhaul 2026-06-12)
 *
 *  Replaces the per-class wheel handling with ONE renderer used by racers,
 *  police, TD6 cars AND traffic:
 *    - higher-poly tire cylinder (WHEEL_SEG_HI facets vs the old octagon)
 *    - styled rim: procedural spoke designs + per-style colour
 *    - tire AND rim share ONE steering-yaw + projection helper, which
 *      structurally guarantees the rim stays synced with the tire (the old
 *      code rotated the hub disc with the OPPOSITE sign convention to the
 *      tire ring, so the rim swung the wrong way under steering).
 *    - per-race RANDOM style per slot (td5_render_set_wheel_style), assigned
 *      from the deterministic per-race RNG so netplay stays in sync.
 *
 *  A/B: TD5RE_WHEEL_OVERHAUL=0 falls back to render_vehicle_wheel_billboards.
 *       TD5RE_WHEEL_TRAFFIC=0 keeps traffic on its baked-in mesh wheels.
 * ========================================================================== */
#define WHEEL_SEG_HI     24      /* tire tread facets (was WHEEL_SEGMENTS = 8) */

/* Rim texture pool: real per-car alloy-wheel art (carhubN.png — a 64x64 sheet
 * of four 32x32 motion-blur sub-frames). One is randomly assigned per slot so
 * every car/traffic vehicle gets a proper-looking, varied rim. Loaded once into
 * dedicated pages 994.. (clear of cars 800-843, frontend <=960, fonts 970-971,
 * envmap 990-993, sky 1020). Chosen for visual variety: snowflake, 5-spoke
 * star, chrome multi-spoke, complex, classic 5-spoke, dark thin, steel,
 * 5-spoke smooth. */
#define WHEEL_RIM_TEX_BASE 994
static const char *k_wheel_rim_srcs[] = {
    "re/assets/cars/sky/carhub0.png",
    "re/assets/cars/bmw/carhub0.png",
    "re/assets/cars/jag/carhub0.png",
    "re/assets/cars/vip/carhub0.png",
    "re/assets/cars/gto/carhub0.png",
    "re/assets/cars/cob/carhub0.png",
    "re/assets/cars/day/carhub0.png",
    "re/assets/cars/mus/carhub0.png",
};
#define WHEEL_STYLE_COUNT ((int)(sizeof(k_wheel_rim_srcs) / sizeof(k_wheel_rim_srcs[0])))

static int8_t s_wheel_style[TD5_MAX_TOTAL_ACTORS];   /* per-slot, -1 = unset */
static int    s_wheel_style_init = 0;
static int    s_wheel_pool_loaded = 0;   /* rim-texture pool uploaded */

/* Lazily upload the rim-texture pool (once). Carhub PNGs have alpha=0 outside
 * the wheel disc, so an alpha-tested textured quad renders a clean round rim. */
static void wheel_load_rim_pool(void) {
    if (s_wheel_pool_loaded) return;
    s_wheel_pool_loaded = 1;
    for (int i = 0; i < WHEEL_STYLE_COUNT; i++) {
        if (!td5_asset_load_png_texture(WHEEL_RIM_TEX_BASE + i, k_wheel_rim_srcs[i],
                                        TD5_COLORKEY_NONE))
            TD5_LOG_W(LOG_TAG, "wheel rim pool: failed to load %s", k_wheel_rim_srcs[i]);
    }
    TD5_LOG_I(LOG_TAG, "wheel rim pool: %d alloy textures loaded at page %d..",
              WHEEL_STYLE_COUNT, WHEEL_RIM_TEX_BASE);
}

int wheel_overhaul_enabled(void) {
    static int cached = -1;
    if (cached < 0) { cached = td5_env_flag_on("TD5RE_WHEEL_OVERHAUL"); }
    return cached;
}
int wheel_traffic_enabled(void) {
    static int cached = -1;
    if (cached < 0) { cached = td5_env_flag_on("TD5RE_WHEEL_TRAFFIC"); }
    return cached;
}
/* [BUG 3a — traffic wheels outside the chassis on the WIDTH axis]
 * Traffic slots never load their own carparam.dat; InitRace seeds each traffic
 * slot's cardef by COPYING SLOT 0's (the PLAYER car's) cardef
 * (td5_physics_seed_traffic_cardef_from_player), so traffic inherits the player
 * car's wheel hardpoints (wheel_display_angles[w][0], the lateral arm @cardef
 * +0x40..) AND the player car's axle half-width (cardef +0x84). When the player
 * is in a wide car, every traffic car gets that wide axle track and its tyres
 * splay proud of the (often narrower) traffic body mesh — worst on Sydney where
 * the model set's bodies are narrow. Fix is render-only (the inherited arm feeds
 * ONLY the visual for traffic): clamp the traffic wheel lateral position + tyre
 * half-width to the traffic model's OWN mesh half-width, so the wheels always sit
 * inside the bodywork regardless of the borrowed cardef. Racers untouched.
 * TD5RE_WHEEL_TRACK_FIX=0 reverts to the inherited (player-car) track. */
static int wheel_track_fix_enabled(void) {
    /* "0" (exactly) disables; unset = on; any other value (incl. a tuning
     * fraction like "0.5") enables, with the cap block parsing the number. */
    static int cached = -1;
    if (cached < 0) { const char *e = getenv("TD5RE_WHEEL_TRACK_FIX");
                      cached = (e && e[0] == '0' && e[1] == '\0') ? 0 : 1; }
    return cached;
}
/* [BUG 3b — traffic wheels don't spin]
 * The faithful traffic dynamics (td5_physics_update_traffic, orig
 * IntegrateVehicleFrictionForces @0x4438F0) update longitudinal_speed but NEVER
 * advance accumulated_tire_slip_x/z — the original game drew no spinning wheels
 * for traffic, so it had no reason to. The unified renderer derives wheel spin
 * from those slip odometers, so for traffic they stay 0 and the wheels are
 * frozen. Synthesize a deterministic spin odometer here from the traffic
 * actor's own longitudinal_speed (advanced once per SIM TICK, viewport-count
 * independent, no rand()). TD5RE_TRAFFIC_WHEEL_SPIN=0 reverts (frozen wheels). */
static int traffic_wheel_spin_enabled(void) {
    static int cached = -1;
    if (cached < 0) { cached = td5_env_flag_on("TD5RE_TRAFFIC_WHEEL_SPIN"); }
    return cached;
}
/* [WHEELS-TOO-LOW on NON-FOCUSED cars — 2026-06-17]
 * The unified wheel renderer draws every car's wheels through the SAME
 * s_render_transform that drew its body (m[9..11] = the actor's view-space
 * translation set by td5_render_load_translation in td5_render_actors_for_view).
 *
 * For RACER slots (slot < g_traffic_slot_base) the body translation already
 * carries the render-Y "ground plant" lift (td5_render.c:~3545,
 * `interp_y -= height_base_offset<<8` => +36/+18 world-Y DOWN), and because the
 * wheels share that transform they inherit the same lift — so the focused player
 * AND the AI racers (which run the identical render + physics path) draw their
 * wheels at the correct gap. That path is left byte-identical here.
 *
 * TRAFFIC slots (slot >= g_traffic_slot_base) get NO body render-lift (the gate
 * at td5_render.c:~3545 excludes them — the traffic body is planted in physics
 * via world_pos.y = ground_y - height_offset instead). Their wheels are SYNTHED
 * from the mesh bounds (wy0 = bounding_center_y - rb*0.22 + s_tlift) and never
 * referenced to the racer ground convention, so they sit BELOW the body line —
 * the "non-focused wheels dropped/sunken" report. Raise the synthesized wheel
 * centre by the same body-lift magnitude the racers use so traffic wheels meet
 * the body/ground line that the racer (focused + AI) wheels already use.
 * A/B: TD5RE_WHEEL_LIFT_FIX=0 restores the old (un-aligned) synth wheel Y. */
static int wheel_lift_fix_enabled(void) {
    static int cached = -1;
    if (cached < 0) { cached = td5_env_flag_on("TD5RE_WHEEL_LIFT_FIX"); }
    return cached;
}
/* The racer body render-lift magnitude (body-units), mirroring the
 * height_base_offset used by the body draw at td5_render.c:~3549:
 *   normal gameplay -> 36, replay playback -> 18.
 * Returned as a positive "raise the wheel toward the body" amount; in the synth
 * wheel's body-local frame +Y is UP (see the s_tlift comment), so adding this
 * lifts the traffic wheel to the racer ground convention. */
static float wheel_body_lift_magnitude(void) {
    extern int td5_input_is_playback_active(void);
    return td5_input_is_playback_active() ? 18.0f : 36.0f;
}
/* [BUG 6 — inner (inboard) wheel face is texture-less]
 * The unified renderer caps only the OUTBOARD wheel face (the carhub alloy
 * disc); the inboard end of the tyre tube is left open, so from any angle that
 * exposes it the wheel reads as a blank/black hole. The original drew the
 * INWHEEL atlas texture (s_inwheel_*, loaded but otherwise UNUSED) on the inner
 * face. Draw a matching INWHEEL-textured disc on the inboard face.
 * TD5RE_WHEEL_INNER_TEX=0 reverts (open inner face). */
static int wheel_inner_tex_enabled(void) {
    static int cached = -1;
    if (cached < 0) { cached = td5_env_flag_on("TD5RE_WHEEL_INNER_TEX"); }
    return cached;
}

/* Per-slot synthesized wheel-spin phase (BUG 3b). Advanced once per sim tick by
 * the traffic actor's longitudinal_speed so it is independent of viewport count
 * and frame rate, and fully deterministic (netplay/replay safe). Stored in the
 * same 12-bit angle units as the slip odometer the racer path uses. */
static int32_t s_traffic_spin_phase[TD5_MAX_TOTAL_ACTORS];
static uint32_t s_traffic_spin_tick[TD5_MAX_TOTAL_ACTORS];
static int      s_traffic_spin_init = 0;

/* Set a slot's wheel style (called at race init with a per-race RNG roll). */
void td5_render_set_wheel_style(int slot, int style) {
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return;
    int n = WHEEL_STYLE_COUNT;
    s_wheel_style[slot] = (int8_t)(((style % n) + n) % n);
}

static int wheel_style_for_slot(int slot) {
    /* Dev override: TD5RE_WHEEL_FORCE_STYLE=N forces every slot to style N
     * (so each design can be inspected deterministically). */
    static int s_force = -2;
    if (s_force == -2) { const char *e = getenv("TD5RE_WHEEL_FORCE_STYLE");
                         s_force = (e && e[0]) ? atoi(e) : -1; }
    if (s_force >= 0) return s_force % WHEEL_STYLE_COUNT;

    if (!s_wheel_style_init) {
        for (int i = 0; i < TD5_MAX_TOTAL_ACTORS; i++) s_wheel_style[i] = -1;
        s_wheel_style_init = 1;
    }
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return 0;
    int s = s_wheel_style[slot];
    if (s < 0) s = (slot * 7 + 3) % WHEEL_STYLE_COUNT;  /* stable fallback before assignment */
    return s;
}

/* Project a wheel-LOCAL point (lx along axle, ly vertical, lz longitudinal)
 * through the SINGLE shared steering-yaw convention (CW-from-+Z, same as the
 * faithful tire ring) and the vehicle render matrix. Tire and rim both call
 * this, so they can never desync under steering. Returns 0 if behind near. */
static int wheel_project(const float *m, float wx, float wy, float wz,
                         float lx, float ly, float lz, float cs, float sn,
                         float u, float v, uint32_t col, TD5_D3DVertex *out)
{
    /* Steering yaw [cos -sin; sin cos] — the convention the ORIGINAL hub/rim
     * used (0x00446F00), which is the visually-correct turn direction. (The
     * old tire ring used the transpose, which read as inverted steering; tire
     * and rim now share THIS one helper so they stay synced AND turn the right
     * way.) [user feedback 2026-06-13: prior unify-to-tire was inverted] */
    float rx = lx * cs - lz * sn;
    float rz = lx * sn + lz * cs;
    float px = wx + rx, py = wy + ly, pz = wz + rz;
    float vx = px*m[0] + py*m[1] + pz*m[2] + m[9];
    float vy = px*m[3] + py*m[4] + pz*m[5] + m[10];
    float vz = px*m[6] + py*m[7] + pz*m[8] + m[11];
    if (vz <= s_near_clip) return 0;
    float iz = 1.0f / vz;
    out->screen_x = -vx * s_focal_length * iz + s_center_x;
    out->screen_y = -vy * s_focal_length * iz + s_center_y;
    out->depth_z  = (vz - NEAR_DEPTH_OFFSET) * DEPTH_NORMALIZE_INV;
    out->rhw      = iz;
    out->diffuse  = col;
    out->specular = 0;
    out->tex_u    = u;
    out->tex_v    = v;
    return 1;
}

void render_vehicle_wheels_unified(TD5_Actor *actor, int slot)
{
    const float *m = s_render_transform.m;

    if (!s_wheel_lookup_done)
        wheel_lookup_static_hed();
    wheel_load_rim_pool();

    /* Wheel dimensions from cardef (racers/police/TD6 cars); traffic has no
     * per-actor cardef -> sensible defaults so traffic still gets wheels. */
    float rim_radius = WHEEL_RADIUS_DEFAULT;
    float axle_halfw = WHEEL_HALFW_DEFAULT;
    if (actor->car_definition_ptr) {
        int16_t r  = *(int16_t *)((uint8_t *)actor->car_definition_ptr + 0x82);
        int16_t hw = *(int16_t *)((uint8_t *)actor->car_definition_ptr + 0x84);
        if (r  > 0) rim_radius = (float)r * WHEEL_RADIUS_SCALE;
        if (hw > 0) axle_halfw = (float)hw;
    }

    /* [BUG 3a] Traffic-only outer-stance cap (the "wheels outside the chassis on
     * the width axis" report). Traffic inherits the PLAYER car's wheel hardpoints
     * + axle half-width (see wheel_track_fix_enabled() note), which can be wider
     * than this traffic model's body. Cap the OUTERMOST tyre face (wheel-centre
     * |X| + axle_halfw) to the traffic model's own mesh half-width so the tyres
     * always tuck under the bodywork. The cap fraction of the bounding radius is
     * the same 0.30 the synth path uses for the wheel CENTRE, plus headroom for
     * the half-width; clamp only narrows, never widens. Racers are never capped
     * (they use their own authentic hardpoints). */
    float traffic_outer_cap = 0.0f;  /* 0 = no cap */
    if (slot >= g_traffic_slot_base && wheel_track_fix_enabled()) {
        TD5_MeshHeader *mh = (slot >= 0 && slot < TD5_ACTOR_MAX_TOTAL_SLOTS)
                             ? s_vehicle_meshes[slot] : NULL;
        float rb = (mh && mh->bounding_radius > 1.0f) ? mh->bounding_radius : 360.0f;
        /* Body half-WIDTH is ~0.30..0.36*rb of the bounding SPHERE radius; cap the
         * tyre's outer face just inside that. TD5RE_WHEEL_TRACK_FIX may be set to
         * a numeric fraction to tune the cap (default 0.34). */
        static float s_cap_frac = -1.0f;
        if (s_cap_frac < 0.0f) {
            const char *e = getenv("TD5RE_WHEEL_TRACK_FIX");
            float v = (e && e[0]) ? (float)atof(e) : 0.0f;
            s_cap_frac = (v > 0.0f && v <= 1.0f) ? v : 0.34f;
            TD5_LOG_I(LOG_TAG, "traffic wheel outer-stance cap = %.3f * bounding_radius",
                      s_cap_frac);
        }
        traffic_outer_cap = rb * s_cap_frac;
    }

    /* Wheel spin odometer (same fields/scale as the original hub @0x446F00):
     * the slip accumulators grow with distance travelled, so cos/sin of them
     * gives a continuously-rotating angle. Used to ROTATE the textured rim so
     * the wheel visibly spins (front wheels spin on slip_z, rear on slip_x). */
    int32_t slip_front_12 = (int32_t)actor->accumulated_tire_slip_z * -4;
    int32_t slip_rear_12  = (int32_t)actor->accumulated_tire_slip_x * -4;
    float front_spin = (float)slip_front_12 * ((float)M_PI / 2048.0f);
    float rear_spin  = (float)slip_rear_12  * ((float)M_PI / 2048.0f);

    /* [BUG 3b] Traffic slots never advance the slip odometer, so the formula
     * above yields 0 and their wheels are frozen. Replace it with a per-slot
     * phase advanced ONCE PER SIM TICK by the traffic actor's longitudinal_speed
     * (matching the >>8 scale the racer slip accumulator uses, then the same *-4
     * → 12-bit-angle scaling), so the wheels roll at a speed that tracks the car
     * and stays identical across viewports / frame rates. Both axles use the same
     * phase (traffic has no per-axle slip split). */
    if (slot >= g_traffic_slot_base && slot < TD5_MAX_TOTAL_ACTORS &&
        traffic_wheel_spin_enabled()) {
        if (!s_traffic_spin_init) {
            for (int i = 0; i < TD5_MAX_TOTAL_ACTORS; i++) {
                s_traffic_spin_phase[i] = 0;
                s_traffic_spin_tick[i]  = 0xFFFFFFFFu;
            }
            s_traffic_spin_init = 1;
        }
        uint32_t cur_tick = (uint32_t)g_td5.simulation_tick_counter;
        if (s_traffic_spin_tick[slot] != cur_tick) {
            s_traffic_spin_tick[slot] = cur_tick;
            /* same per-tick increment the racer odometer accumulates:
             * accumulated_tire_slip_z += longitudinal_speed >> 8 */
            s_traffic_spin_phase[slot] += (actor->longitudinal_speed >> 8);
        }
        int32_t spin_12 = s_traffic_spin_phase[slot] * -4;
        front_spin = (float)spin_12 * ((float)M_PI / 2048.0f);
        rear_spin  = front_spin;
    }

    /* Rim texture page for this slot's randomly-assigned alloy design. */
    int rim_page = WHEEL_RIM_TEX_BASE + (wheel_style_for_slot(slot) % WHEEL_STYLE_COUNT);
    /* The alloy art's circle should fill most of the wheel, leaving a thin
     * black tire ring around it. */
    float tex_r = rim_radius * 0.84f;
    /* Motion-blur sub-frame by speed (same as the original hub-cap @0x446F00):
     * frame = min(|long_speed|>>14, 3); col = frame&1, row = frame>>1. The 64x64
     * sheet holds four 32x32 sub-tiles; sample with a 1.5px margin so LINEAR
     * sampling can't bleed across sub-frame boundaries. */
    int32_t spd_raw = actor->longitudinal_speed;
    uint32_t spd_abs = (uint32_t)(spd_raw < 0 ? -spd_raw : spd_raw);
    int frame = (int)(spd_abs >> 14); if (frame > 3) frame = 3;
    int fcol = frame & 1, frow = frame >> 1;
    float ru0 = ((float)(fcol * 32) + 1.5f) / 64.0f, ru1 = ((float)(fcol * 32 + 32) - 1.5f) / 64.0f;
    float rv0 = ((float)(frow * 32) + 1.5f) / 64.0f, rv1 = ((float)(frow * 32 + 32) - 1.5f) / 64.0f;

    /* Traffic vehicles carry their wheels baked into the body mesh and never
     * populate wheel_display_angles. Synthesize a 4-wheel layout from the mesh
     * bounding radius so the unified renderer can draw them too. (The baked
     * mesh wheels still draw underneath — a converter pass to strip them is the
     * follow-up; until then TD5RE_WHEEL_TRAFFIC=0 disables this.) Order: FL, FR,
     * RL, RR with (w&1)=right. */
    int16_t synth[4][3];
    int use_synth = 0;
    if (actor->wheel_display_angles[0][0] == 0 && actor->wheel_display_angles[0][1] == 0 &&
        actor->wheel_display_angles[0][2] == 0 && actor->wheel_display_angles[1][0] == 0) {
        TD5_MeshHeader *mh = (slot >= 0 && slot < TD5_ACTOR_MAX_TOTAL_SLOTS)
                             ? s_vehicle_meshes[slot] : NULL;
        float rb = (mh && mh->bounding_radius > 1.0f) ? mh->bounding_radius : 360.0f;
        /* [task#5] Lateral half-track as a fraction of the mesh bounding radius.
         * rb is the bounding SPHERE radius, dominated by the car's diagonal
         * (length), so the body half-WIDTH is only ~0.34..0.36*rb at the widest
         * point. The old 0.38*rb placed the wheel CENTRE at/just outside that
         * edge, so the tyres splayed proud of the bodywork ("traffic wheels
         * outside the chassis"). 0.30*rb tucks the wheel centre comfortably
         * INBOARD of the body edge so the tyre sits under the wing on the common
         * roster while still reading as a normal stance. TD5RE_SYNTH_WHEEL_TRACK
         * overrides it (=0 restores the old 0.38 splay). */
        static float s_synth_track = -1.0f;
        if (s_synth_track < 0.0f) {
            const char *e = getenv("TD5RE_SYNTH_WHEEL_TRACK");
            if (e && e[0] == '0' && e[1] == '\0') s_synth_track = 0.38f; /* "0" = old */
            else s_synth_track = (e && e[0]) ? (float)atof(e) : 0.30f;
            if (s_synth_track <= 0.0f) s_synth_track = 0.30f;             /* guard */
            TD5_LOG_I(LOG_TAG, "synth traffic wheel half-track = %.3f * bounding_radius",
                      s_synth_track);
        }
        float track = rb * s_synth_track; /* lateral half-track */
        float front =  rb * 0.55f;       /* front axle Z */
        float rear  = -rb * 0.60f;       /* rear axle Z  */
        /* Wheel-centre Y. Was -rb*0.34 which sank traffic wheels under the road
         * (user: "barely visible — lift TD5 traffic + police off the ground").
         * Raised toward the body centre + a tunable nudge so the wheels sit at
         * the ground line. TD5RE_WHEEL_TRAFFIC_LIFT=N (body-units, default 18)
         * shifts them further up if a model still rides low. */
        static float s_tlift = -1.0f;
        if (s_tlift < 0.0f) { const char *e = getenv("TD5RE_WHEEL_TRAFFIC_LIFT");
                              s_tlift = (e && e[0]) ? (float)atof(e) : 18.0f; }
        /* [WHEELS-TOO-LOW fix] Align the synth (traffic) wheel vertical reference
         * with the body's render lift. The traffic body gets NO render-Y lift
         * (only racers do, at td5_render.c:~3545), so without this the synth
         * wheels sit below the body/ground line that the racer (focused + AI)
         * wheels already use. Raise them by the same body-lift magnitude the
         * racers receive. A/B: TD5RE_WHEEL_LIFT_FIX=0 reverts. */
        float lift_align = wheel_lift_fix_enabled() ? wheel_body_lift_magnitude() : 0.0f;
        float wy0   = (mh ? mh->bounding_center_y : 0.0f) - rb * 0.22f + s_tlift + lift_align;  /* wheel centre Y */
        int16_t zf = (int16_t)front, zr = (int16_t)rear, tx = (int16_t)track, wy16 = (int16_t)wy0;
        synth[0][0] = -tx; synth[0][1] = wy16; synth[0][2] = zf;   /* FL */
        synth[1][0] =  tx; synth[1][1] = wy16; synth[1][2] = zf;   /* FR */
        synth[2][0] = -tx; synth[2][1] = wy16; synth[2][2] = zr;   /* RL */
        synth[3][0] =  tx; synth[3][1] = wy16; synth[3][2] = zr;   /* RR */
        use_synth = 1;
    }

    for (int w = 0; w < 4; w++) {
        float wx = use_synth ? (float)synth[w][0] : (float)actor->wheel_display_angles[w][0];
        float wy = use_synth ? (float)synth[w][1] : (float)actor->wheel_display_angles[w][1];
        float wz = use_synth ? (float)synth[w][2] : (float)actor->wheel_display_angles[w][2];
        if (wx == 0.0f && wy == 0.0f && wz == 0.0f)
            continue;

        float wheel_halfw = axle_halfw;
        /* [BUG 3a] Pull a traffic wheel inboard until its outer tyre face sits at
         * the model's half-width cap. Shrink the hub lateral offset first (keeps
         * the tyre at full width); only if the half-width alone still overshoots
         * do we also narrow the tyre. Never widens (cap==0 → disabled). Skipped
         * for the synth layout, whose stance is already tuned (rb*0.30). */
        if (traffic_outer_cap > 0.0f && !use_synth) {
            float side = (wx < 0.0f) ? -1.0f : 1.0f;
            float hub_mag = wx * side;                  /* |wx| */
            if (hub_mag + wheel_halfw > traffic_outer_cap) {
                float new_hub = traffic_outer_cap - wheel_halfw;
                if (new_hub < 0.0f) {                   /* tyre alone too wide */
                    new_hub = 0.0f;
                    wheel_halfw = traffic_outer_cap;
                }
                wx = side * new_hub;
            }
        }

        float inner_off = (w & 1) ? -wheel_halfw :  wheel_halfw;
        float outer_off = (w & 1) ?  wheel_halfw : -wheel_halfw;

        /* Front-wheel steering yaw (CW-from-+Z) — applied via wheel_project to
         * BOTH tire and rim, so they stay locked together. */
        float cs = 1.0f, sn = 0.0f;
        if (w < 2) {
            float steer_rad = (float)(actor->steering_command >> 8) * ((float)M_PI / 2048.0f);
            cs = cosf(steer_rad);
            sn = sinf(steer_rad);
        }

        /* ---- Tire tread cylinder (higher-poly, black) ---- */
        TD5_D3DVertex tv[2 * (WHEEL_SEG_HI + 1)];
        int ok = 1;
        for (int i = 0; i <= WHEEL_SEG_HI && ok; i++) {
            float a  = (float)i * (2.0f * (float)M_PI / (float)WHEEL_SEG_HI);
            float ly = cosf(a) * rim_radius;
            float lz = sinf(a) * rim_radius;
            if (!wheel_project(m, wx, wy, wz, inner_off, ly, lz, cs, sn,
                               s_tire_u, s_tire_v, 0xFFFFFFFFu, &tv[i])) { ok = 0; break; }
            if (!wheel_project(m, wx, wy, wz, outer_off, ly, lz, cs, sn,
                               s_tire_u, s_tire_v, 0xFFFFFFFFu, &tv[WHEEL_SEG_HI + 1 + i])) { ok = 0; break; }
        }
        if (!ok) continue;   /* wheel partly behind near plane — skip (legacy behaviour) */

        uint16_t tidx[WHEEL_SEG_HI * 12];
        int ti = 0;
        for (int i = 0; i < WHEEL_SEG_HI; i++) {
            uint16_t i0 = (uint16_t)i, i1 = (uint16_t)(i + 1);
            uint16_t o0 = (uint16_t)(WHEEL_SEG_HI + 1 + i), o1 = (uint16_t)(WHEEL_SEG_HI + 1 + i + 1);
            tidx[ti++] = i0; tidx[ti++] = o0; tidx[ti++] = o1;
            tidx[ti++] = i0; tidx[ti++] = o1; tidx[ti++] = i1;
            tidx[ti++] = i0; tidx[ti++] = o1; tidx[ti++] = o0;   /* back faces */
            tidx[ti++] = i0; tidx[ti++] = i1; tidx[ti++] = o1;
        }
        flush_immediate_internal();
        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        td5_plat_render_bind_texture(s_wheel_tex_page);
        td5_plat_render_draw_tris(tv, 2 * (WHEEL_SEG_HI + 1), tidx, ti);

        /* ---- Textured rim face: real alloy-wheel art (carhub pool) ----
         * A single alpha-tested textured quad in the wheel plane at the outer
         * face. The carhub PNG is transparent outside the wheel disc, so
         * OPAQUE_LINEAR's alpha test discards the quad corners → clean round
         * rim. The quad is ROTATED about the axle by the spin odometer so the
         * wheel visibly spins, and steering yaw is applied via wheel_project
         * (same helper as the tire) so the rim turns with the wheel. */
        {
            float spin = (w < 2) ? front_spin : rear_spin;
            float sc = cosf(spin), ss = sinf(spin);
            /* corner (ly,lz) pre-spin + matching sub-tile UV */
            static const float cly[4] = {  1.0f,  1.0f, -1.0f, -1.0f };
            static const float clz[4] = { -1.0f,  1.0f,  1.0f, -1.0f };
            const float cu[4] = { ru0, ru1, ru1, ru0 };
            const float cv[4] = { rv0, rv0, rv1, rv1 };
            TD5_D3DVertex q[4];
            int qok = 1;
            for (int c = 0; c < 4 && qok; c++) {
                float pl = cly[c] * tex_r, pz = clz[c] * tex_r;
                float ly = pl * sc - pz * ss;   /* rotate by spin about the axle */
                float lz = pl * ss + pz * sc;
                qok &= wheel_project(m, wx, wy, wz, outer_off, ly, lz, cs, sn,
                                     cu[c], cv[c], 0xFFFFFFFFu, &q[c]);
            }
            if (qok) {
                static const uint16_t qi[12] = { 0,1,2, 0,2,3,  0,2,1, 0,3,2 }; /* double-sided */
                flush_immediate_internal();
                td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);  /* alpha test discards transparent corners */
                td5_plat_render_bind_texture(rim_page);
                td5_plat_render_draw_tris(q, 4, qi, 12);
            }
        }

        /* ---- Inboard wheel face: INWHEEL texture (BUG 6) ----
         * The tyre tube was open on the inboard end, so the inside of the wheel
         * read as a texture-less hole. Cap it with the INWHEEL atlas texture
         * (the original game's inner-wheel art, s_inwheel_*), as a disc in the
         * wheel plane at inner_off. Same spin + steering as the outer rim so it
         * stays locked to the wheel. Sampled from tpage5 (s_wheel_tex_page). */
        if (wheel_inner_tex_enabled()) {
            float spin = (w < 2) ? front_spin : rear_spin;
            float sc = cosf(spin), ss = sinf(spin);
            static const float cly[4] = {  1.0f,  1.0f, -1.0f, -1.0f };
            static const float clz[4] = { -1.0f,  1.0f,  1.0f, -1.0f };
            const float cu[4] = { s_inwheel_u0, s_inwheel_u1, s_inwheel_u1, s_inwheel_u0 };
            const float cv[4] = { s_inwheel_v0, s_inwheel_v0, s_inwheel_v1, s_inwheel_v1 };
            /* Fill the wheel disc (rim_radius), not the smaller alloy inset. */
            float in_r = rim_radius;
            TD5_D3DVertex q[4];
            int qok = 1;
            for (int c = 0; c < 4 && qok; c++) {
                float pl = cly[c] * in_r, pz = clz[c] * in_r;
                float ly = pl * sc - pz * ss;
                float lz = pl * ss + pz * sc;
                qok &= wheel_project(m, wx, wy, wz, inner_off, ly, lz, cs, sn,
                                     cu[c], cv[c], 0xFFFFFFFFu, &q[c]);
            }
            if (qok) {
                static const uint16_t qi[12] = { 0,1,2, 0,2,3,  0,2,1, 0,3,2 }; /* double-sided */
                flush_immediate_internal();
                td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
                td5_plat_render_bind_texture(s_wheel_tex_page);
                td5_plat_render_draw_tris(q, 4, qi, 12);
            }
        }
    }
}

void render_vehicle_wheel_billboards(TD5_Actor *actor, int slot)
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

/* TD6 sky-dome pitch (radians) that lowers the panorama horizon to eye level.
 * Applied only when g_active_td6_level > 0. Overridable at runtime via
 * TD5RE_SKY_PITCH for bring-up; 0.0 = no change. */
#ifndef TD6_SKY_PITCH_DEFAULT
#define TD6_SKY_PITCH_DEFAULT 0.12f
#endif

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

        /* TD6 sky horizon adjustment. The TD6 FORWSKY panoramas place their
         * horizon higher than the TD5 sky dome was tuned for, so the sky reads
         * "too high". Pitch the dome about the view right-axis (compose a pitch
         * P with the camera basis: rows 1,2 = up/forward mixed) to slide the
         * horizon down. Gated on TD6 so faithful tracks are byte-unchanged.
         * Angle from TD5RE_SKY_PITCH (radians) during bring-up; falls back to
         * the baked default below. */
        if (g_active_td6_level > 0) {
            const char *sp = getenv("TD5RE_SKY_PITCH");
            float ang = sp ? (float)atof(sp)
                           : td5_asset_td6_sky_pitch_for_level(g_active_td6_level);
            if (ang != 0.0f) {
                float c = cosf(ang), s = sinf(ang);
                float u0 = sky_rot.m[3], u1 = sky_rot.m[4], u2 = sky_rot.m[5];
                float f0 = sky_rot.m[6], f1 = sky_rot.m[7], f2 = sky_rot.m[8];
                sky_rot.m[3] = c * u0 - s * f0;
                sky_rot.m[4] = c * u1 - s * f1;
                sky_rot.m[5] = c * u2 - s * f2;
                sky_rot.m[6] = s * u0 + c * f0;
                sky_rot.m[7] = s * u1 + c * f1;
                sky_rot.m[8] = s * u2 + c * f2;
            }
        }

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
