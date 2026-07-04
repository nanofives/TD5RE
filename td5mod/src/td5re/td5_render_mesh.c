/* ========================================================================
 * td5_render_mesh.c -- Scene rendering: meshes, actors, spans, texture cache
 *
 * Split out of td5_render.c (P1-C step 2, 2026-07-02). Contents, in original
 * core order:
 *   - Dynamic point lights (port extension) + vertex lighting
 *   - Sphere/mesh frustum visibility tests
 *   - Track span display list walk (+ TD6 reverse banners, drag stadium)
 *   - Prepared mesh dispatch + per-slot vehicle/cop mesh registry
 *   - Actor rendering for a view (racers/traffic/props dispatch)
 *   - Projection configuration (viewport/focal)
 *   - Translucent / projected-bucket / immediate flush pipelines
 *   - Texture cache (reset/ages/bind + page blend presets)
 *   - Environs texture load
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
#include "td5_light2.h"   /* [LIGHT2] lighting rework mode knob */
#include "td5_material.h" /* [LIGHT2 P3] per-material reflectivity for SSR */
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

/* ======== [split] scene/mesh region (moved verbatim from td5_render.c) ======== */
/* ====================== DYNAMIC POINT LIGHTS (port extension) =============
 * The original engine lit vertices with only 3 DIRECTIONAL contributions + a
 * scalar ambient (ComputeMeshVertexLighting @ 0x0043DDF0). The helpers below
 * add an attenuated POSITIONAL N.L term per registered dynamic light (see
 * td5_light.c) into the same per-vertex luminance, before the original's
 * [0x40,0xFF] clamp — the minimum addition that lets a moving light (a
 * headlight) illuminate an area. When no lights are registered, or none reach
 * the mesh, every loop below is fully skipped and lighting is byte-identical to
 * the original. */

/* A dynamic light transformed into the MODEL space of the mesh being lit. */
typedef struct LightModelPt {
    float x, y, z;     /* light position in mesh model space (== vertex space) */
    float inv_range;   /* 1/range, for the linear distance falloff */
    float intensity;   /* peak luminance bump at the surface */
} LightModelPt;

/* Dark-mode config. When on, the ambient + directional luminance is scaled down
 * and the clamp floor is lowered so "very dark areas" read as dark and the
 * additive headlight pool stands out. Headlight bumps are NOT dimmed. Default
 * OFF (set from [Lighting] DarkMode / --DarkMode), so daylight tracks render
 * exactly as before. The scale/floor are env-tunable. */
static int   s_light_dark_mode  = 0;
static int   s_dark_knobs_read  = 0;
static float s_dark_scale       = 0.50f;   /* ambient/directional dim under dark mode */
static int   s_dark_floor       = 0x2A;     /* clamp floor: distant geometry stays dim, not black */
/* [car look] Saturation of the authored zone COLOUR applied to VEHICLE bodies in
 * the Mode>=1 colored path: 1.0 = full authored colour, 0.0 = neutral grey (the
 * original averaged zone light to grey, so 0 == the faithful car look). Warm-lit
 * tracks (e.g. Bern, amber ambient) otherwise cast a red tint on cars; this pulls
 * the car lighting back toward its own luminance while the TRACK keeps full
 * colour. Default 0 = neutral cars (matches the original + the approved look);
 * raise toward 1 to let the authored zone colour tint cars. Env: TD5RE_CAR_ZONE_SAT. */
static float s_car_zone_sat     = 0.0f;

/* [AUTO LIGHTS] Environment-brightness probe, sampled each frame in the actor
 * lighting pass for the player (slot 0). Two components:
 *   s_env_ambient — the zone ambient scalar (sits at the 0x40 floor even on
 *                   sunny tracks; a poor discriminator ON ITS OWN, which is why
 *                   the old ambient-only term was dead code / threshold 0).
 *   s_env_direct  — the zone directional ("sun") budget = sum of |vec_world|
 *                   over the enabled light slots. THIS is what separates a
 *                   tunnel (no sun) from an open sunlit road (strong sun) at
 *                   the same ambient floor.
 * scene_luma = s_env_ambient + AUTO_DIRECT_SCALE*s_env_direct is the per-zone
 * brightness the auto verdict thresholds against (see td5_render_env_is_dark). */
static float s_env_ambient       = 255.0f;
static float s_env_direct        = 0.0f;
static int   s_auto_knobs_read   = 0;
/* Per-zone scene-luma threshold: below this -> "dark zone" (tunnels, very dim
 * stretches). Non-zero by default now that the directional term makes scene_luma
 * a real discriminator. Calibrated on Bern: tunnel scene~48, open road scene~106
 * -> 80 cleanly splits them (tunnels ON, sunlit road OFF). Tunables (env):
 * TD5RE_LIGHT_AUTO_ZONE (alias legacy TD5RE_LIGHT_AUTO_AMBIENT), _AUTO_SKY, _AUTO_HYST. */
static int   s_auto_zone_thr     = 80;
/* Per-track sky-luminance threshold: a track whose average sky brightness is
 * below this reads "dark everywhere" (night/dusk/overcast tracks), so headlights
 * stay on across the whole track regardless of the local zone. Calibrated on the
 * sky-texture averages: Moscow(dark) ~64, Bern(day) ~124 -> 110 flags Moscow and
 * dimmer/dusk tracks while leaving bright daylight (>=~124) off. */
static int   s_auto_sky_thr      = 110;
/* Hysteresis (scene-luma units) on the dynamic zone term so headlights don't
 * strobe at zone boundaries: once ON they need scene_luma >= thr+hyst to flip OFF.
 * Kept below (open-road - tunnel) so open road always clears (80+15=95 < 106). */
static int   s_auto_hyst         = 15;
static int   s_env_zone_dark     = 0;   /* latched zone verdict (hysteresis) */
#define AUTO_DIRECT_SCALE 0.25f         /* |vec_world| ~= 4*weight_avg, so /4 to ambient units */

static void light_read_dark_knobs(void)
{
    if (s_dark_knobs_read) return;
    s_dark_knobs_read = 1;
    const char *es = getenv("TD5RE_LIGHT_DARK_SCALE");
    if (es && es[0]) { float v = (float)atof(es); if (v > 0.0f && v <= 1.0f) s_dark_scale = v; }
    const char *ef = getenv("TD5RE_LIGHT_DARK_FLOOR");
    if (ef && ef[0]) { int v = atoi(ef); if (v >= 0 && v <= 0xFF) s_dark_floor = v; }
    const char *em = getenv("TD5RE_LIGHT_DARK_MODE");   /* env override of the INI toggle */
    if (em && em[0]) s_light_dark_mode = (em[0] != '0');
    const char *ecs = getenv("TD5RE_CAR_ZONE_SAT");     /* vehicle zone-colour saturation */
    if (ecs && ecs[0]) { float v = (float)atof(ecs); if (v >= 0.0f && v <= 1.0f) s_car_zone_sat = v; }
    TD5_LOG_I(LOG_TAG, "lighting dark-mode: %s (scale=%.2f floor=0x%02X)",
              s_light_dark_mode ? "ON" : "off", (double)s_dark_scale, s_dark_floor);
}

void td5_render_set_dark_mode(int on)
{
    s_light_dark_mode = on ? 1 : 0;
}

/* [DEFERRED LIGHTS] Run the screen-space light pass for the CURRENT viewport.
 * Copies the world-space dynamic-light registry into the GPU light array and
 * hands it to the backend with this pane's camera params; the backend samples
 * scene depth, reconstructs each pixel's world position, and adds the lights.
 * Call once per viewport AFTER the opaque world (track + actors), BEFORE the
 * translucent VFX / HUD. vp_x/vp_y = this pane's origin in render-target pixels
 * (0,0 for a single full-screen viewport). */
void td5_render_apply_light_pass(int vp_x, int vp_y)
{
    if (!td5_light_enabled()) return;

    int n = 0;
    const TD5_DynLight *L = td5_light_list(&n);
    if (!L || n <= 0) return;
    if (n > TD5_LIGHT_MAX) n = TD5_LIGHT_MAX;

    TD5_LightGPU gpu[TD5_LIGHT_MAX];
    for (int i = 0; i < n; i++) {
        gpu[i].x = L[i].x; gpu[i].y = L[i].y; gpu[i].z = L[i].z; gpu[i].range = L[i].range;
        gpu[i].r = L[i].r; gpu[i].g = L[i].g; gpu[i].b = L[i].b; gpu[i].intensity = L[i].intensity;
        gpu[i].dx = L[i].dx; gpu[i].dy = L[i].dy; gpu[i].dz = L[i].dz; gpu[i].cone = L[i].cone;
    }

    /* camera basis rows are {right, up, forward}; depth_z = (view_z-bias)/scale. */
    float basis9[9];
    for (int i = 0; i < 9; i++) basis9[i] = s_camera_basis[i];
    float cam[3] = { s_camera_pos[0], s_camera_pos[1], s_camera_pos[2] };
    float depth_scale = 1.0f / DEPTH_NORMALIZE_INV;   /* 195000 */

    /* [P2] per-light occlusion march config (Mode>=1 + knob; 0 = off). */
    int occl = (td5_light2_active() && td5_light2_light_occlusion()) ? 12 : 0;

    td5_plat_render_apply_lights(cam, basis9,
                                 s_focal_length, s_center_x, s_center_y,
                                 (float)vp_x, (float)vp_y,
                                 depth_scale, NEAR_DEPTH_OFFSET,
                                 gpu, n,
                                 occl, (float)s_viewport_width, (float)s_viewport_height);
}

/* [LIGHT2 P2] Screen-space ray-marched sun shadows for the CURRENT viewport.
 * The "sun" is the zone table's dominant directional light for this pane
 * (strongest enabled tl_contrib slot, world frame — the same authored data
 * that lights the cars). Shadow strength scales with directional dominance so
 * ambient-only zones (tunnels) cast nothing. Call AFTER the opaque world,
 * BEFORE td5_render_apply_light_pass (headlight pools must not be darkened).
 * March tuning env knobs (dev): TD5RE_SHADOW_STEPS / _DIST / _THICK. */
void td5_render_apply_shadow_pass(int vp_x, int vp_y)
{
    if (!td5_light2_active() || !td5_light2_sun_shadows()) return;

    /* Strongest enabled directional slot = the scene's sun. */
    float best_mag2 = 0.0f;
    float sun[3] = { 0.0f, 0.0f, 0.0f };
    for (int s = 0; s < 3; s++) {
        if (!s_tl_contrib[s].enabled) continue;
        const float *v = s_tl_contrib[s].vec_world;
        float m2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        if (m2 > best_mag2) { best_mag2 = m2; sun[0] = v[0]; sun[1] = v[1]; sun[2] = v[2]; }
    }
    if (best_mag2 <= 1.0f) return;               /* no directional light (tunnel) */
    float mag = sqrtf(best_mag2);
    sun[0] /= mag; sun[1] /= mag; sun[2] /= mag;
    /* [Y-CONVENTION] Zone dirs live in the original's Y-flipped lighting
     * convention (+Y = toward the sky); the shadow march happens in POSITION
     * space (world +Y is DOWN) — flip so rays leave the ground upward. */
    sun[1] = -sun[1];
    if (sun[1] >= -0.05f) return;                /* sun at/below horizon — skip */

    /* Directional dominance: how much of the zone's lighting is the sun vs
     * flat ambient. |vec_world| ~ 4*intensity (dir_shorts ~4096 / 1024). */
    float dir_lum = mag * 0.25f;
    float amb     = s_ambient_intensity > 0.0f ? s_ambient_intensity : 1.0f;
    float dom     = dir_lum / (dir_lum + amb);
    float strength = ((float)td5_light2_shadow_strength() / 100.0f) * dom;
    if (strength <= 0.01f) return;

    /* March tuning (env-overridable for look iteration). */
    static float s_dist = -1.0f, s_thick = -1.0f;
    static int   s_steps = -1;
    if (s_steps < 0) {
        const char *e;
        s_steps = ((e = getenv("TD5RE_SHADOW_STEPS")) && e[0]) ? atoi(e)         : 24;
        s_dist  = ((e = getenv("TD5RE_SHADOW_DIST"))  && e[0]) ? (float)atof(e)  : 2500.0f;
        s_thick = ((e = getenv("TD5RE_SHADOW_THICK")) && e[0]) ? (float)atof(e)  : 600.0f;
        if (s_steps < 4)  s_steps = 4;
        if (s_steps > 64) s_steps = 64;
        TD5_LOG_I(LOG_TAG, "light2: shadow pass steps=%d dist=%.0f thick=%.0f",
                  s_steps, (double)s_dist, (double)s_thick);
    }

    float basis9[9];
    for (int i = 0; i < 9; i++) basis9[i] = s_camera_basis[i];
    float cam[3] = { s_camera_pos[0], s_camera_pos[1], s_camera_pos[2] };
    float depth_scale = 1.0f / DEPTH_NORMALIZE_INV;   /* 195000 */

    td5_plat_render_apply_shadow(cam, basis9,
                                 s_focal_length, s_center_x, s_center_y,
                                 (float)vp_x, (float)vp_y,
                                 depth_scale, NEAR_DEPTH_OFFSET,
                                 sun, strength,
                                 s_steps, s_dist, s_thick, 8.0f,
                                 (float)s_viewport_width, (float)s_viewport_height);
}

/* [LIGHT2 P3] Screen-space reflections for the CURRENT viewport. Reflective
 * materials (car paint, glass — per-id reflectivity from td5_material) mirror
 * the already lit + shadowed scene; up-facing road pixels gain reflectivity
 * in non-clear weather ("wet roads"). Call AFTER td5_render_apply_light_pass.
 * March tuning env knobs (dev): TD5RE_SSR_STEPS / _DIST / _THICK / _INTENSITY. */
void td5_render_apply_ssr_pass(int vp_x, int vp_y)
{
    if (!td5_light2_active() || !td5_light2_reflections()) return;

    /* Per-material base reflectivity LUT (ids 0..7; beyond COUNT = 0). */
    float refl8[8];
    for (int i = 0; i < 8; i++)
        refl8[i] = (i < TD5_MAT_COUNT) ? td5_material_params(i)->reflectivity : 0.0f;
    refl8[TD5_MAT_NONE] = 0.0f;   /* sentinel never reflects */

    /* Wet roads: any non-clear weather makes up-facing DEFAULT-material
     * pixels reflective. */
    float wet = (td5_light2_wet_roads() && g_td5.weather != TD5_WEATHER_CLEAR)
              ? 0.35f : 0.0f;

    static float s_dist = -1.0f, s_thick = -1.0f, s_intensity = -1.0f;
    static int   s_steps = -1;
    if (s_steps < 0) {
        const char *e;
        s_steps     = ((e = getenv("TD5RE_SSR_STEPS"))     && e[0]) ? atoi(e)        : 24;
        s_dist      = ((e = getenv("TD5RE_SSR_DIST"))      && e[0]) ? (float)atof(e) : 4000.0f;
        s_thick     = ((e = getenv("TD5RE_SSR_THICK"))     && e[0]) ? (float)atof(e) : 500.0f;
        s_intensity = ((e = getenv("TD5RE_SSR_INTENSITY")) && e[0]) ? (float)atof(e) : 0.8f;
        if (s_steps < 4)  s_steps = 4;
        if (s_steps > 64) s_steps = 64;
        TD5_LOG_I(LOG_TAG, "light2: SSR pass steps=%d dist=%.0f thick=%.0f intensity=%.2f",
                  s_steps, (double)s_dist, (double)s_thick, (double)s_intensity);
    }

    float basis9[9];
    for (int i = 0; i < 9; i++) basis9[i] = s_camera_basis[i];
    float cam[3] = { s_camera_pos[0], s_camera_pos[1], s_camera_pos[2] };
    float depth_scale = 1.0f / DEPTH_NORMALIZE_INV;   /* 195000 */

    td5_plat_render_apply_ssr(cam, basis9,
                              s_focal_length, s_center_x, s_center_y,
                              (float)vp_x, (float)vp_y,
                              depth_scale, NEAR_DEPTH_OFFSET,
                              refl8, wet, s_intensity,
                              s_steps, s_dist, s_thick,
                              (float)s_viewport_width, (float)s_viewport_height);
}

/* [LIGHT2 P0] Per-frame gate for the G-buffer feed. Call once per rendered
 * race frame BEFORE the world pass (alongside td5_light_begin_frame): enables
 * + clears the wrapper's normal+material target when Mode>=1, so z-writing
 * opaque draws (which carry packed world normals + material ids in COLOR1)
 * populate it and the deferred light pass gets proper N.L. Mode 0 keeps it
 * off — classic behavior. */
void td5_render_lighting2_frame_begin(void)
{
    static int s_logged = 0;
    int on = td5_light2_active() ? 1 : 0;
    td5_plat_render_set_gbuffer(on);
    if (!s_logged) {
        s_logged = 1;
        TD5_LOG_I(LOG_TAG, "light2: frame gate first run, mode=%d gbuffer=%d",
                  td5_light2_mode(), on);
    }
}

/* [AUTO LIGHTS] Verdict: is the current environment poorly lit (so headlights
 * should auto-enable)? Three independent triggers, OR'd together:
 *   1. weather_dark — RAIN only. Not "any non-clear": TD5's SNOW render path is
 *      gated off (a cut feature), so snow tracks (e.g. Bern) LOOK like bright
 *      sunny days -- forcing their headlights on all race, over the sunlit
 *      stretches too, is wrong. Snow therefore falls through to the sky/zone
 *      terms; only visible rain forces dark on its own.
 *   2. zone_dark    — the player's per-zone scene luminance (ambient + scaled
 *                     directional/"sun" budget) is below s_auto_zone_thr. This
 *                     is the DYNAMIC, within-track trigger: a tunnel drops the
 *                     sun budget to ~0 so scene_luma falls below thr, an open
 *                     sunlit stretch climbs back above it (Bern tunnels vs sun).
 *                     Hysteresis (s_auto_hyst) stops it strobing at zone edges.
 *   3. sky_dark     — the track's average sky luminance is below s_auto_sky_thr.
 *                     This is the STATIC, whole-track trigger for night/dusk
 *                     tracks (e.g. Moscow) whose zone lighting reads medium but
 *                     whose sky is dark; headlights then stay on all race.
 * The manual [Lighting] DarkMode also forces it (folded in by the caller). Env
 * knobs: TD5RE_LIGHT_AUTO_ZONE (alias TD5RE_LIGHT_AUTO_AMBIENT), _AUTO_SKY,
 * _AUTO_HYST, TD5RE_LIGHT_AUTO_LOG. */
int td5_render_env_is_dark(void)
{
    if (!s_auto_knobs_read) {
        s_auto_knobs_read = 1;
        const char *e = getenv("TD5RE_LIGHT_AUTO_AMBIENT");   /* legacy alias */
        if (e && e[0]) { int v = atoi(e); if (v >= 0 && v <= 512) s_auto_zone_thr = v; }
        const char *ez = getenv("TD5RE_LIGHT_AUTO_ZONE");
        if (ez && ez[0]) { int v = atoi(ez); if (v >= 0 && v <= 512) s_auto_zone_thr = v; }
        const char *es = getenv("TD5RE_LIGHT_AUTO_SKY");
        if (es && es[0]) { int v = atoi(es); if (v >= 0 && v <= 255) s_auto_sky_thr = v; }
        const char *eh = getenv("TD5RE_LIGHT_AUTO_HYST");
        if (eh && eh[0]) { int v = atoi(eh); if (v >= 0 && v <= 128) s_auto_hyst = v; }
    }

    /* RAIN only (see header): snow is invisible in TD5 so snow tracks read as
     * bright daylight and must not force headlights on by weather alone. */
    int weather_dark = (g_td5.weather == TD5_WEATHER_RAIN);

    /* Per-zone scene luminance = ambient + scaled directional (sun) budget. */
    float scene_luma = s_env_ambient + AUTO_DIRECT_SCALE * s_env_direct;

    /* Hysteresis on the dynamic zone term: once ON, require scene_luma to climb
     * thr+hyst before flipping OFF; once OFF, require it to drop below thr. */
    if (s_env_zone_dark) {
        if (scene_luma >= (float)(s_auto_zone_thr + s_auto_hyst)) s_env_zone_dark = 0;
    } else {
        if (scene_luma <  (float)s_auto_zone_thr)                 s_env_zone_dark = 1;
    }

    /* Per-track sky baseline (-1 = no sky loaded / unknown -> term disabled). */
    float sky_luma = td5_render_sky_luma();
    int sky_dark = (sky_luma >= 0.0f) && (sky_luma < (float)s_auto_sky_thr);

    int dark = weather_dark || s_env_zone_dark || sky_dark;

    static int s_log = 0;
    if (getenv("TD5RE_LIGHT_AUTO_LOG") && (s_log++ % 120) == 0) {
        TD5_LOG_I(LOG_TAG, "auto-lights: amb=%.0f direct=%.0f scene=%.0f (zone_thr=%d hyst=%d) "
                  "sky=%.0f (sky_thr=%d) weather=%d zone_dark=%d sky_dark=%d -> dark=%d",
                  (double)s_env_ambient, (double)s_env_direct, (double)scene_luma,
                  s_auto_zone_thr, s_auto_hyst, (double)sky_luma, s_auto_sky_thr,
                  weather_dark, s_env_zone_dark, sky_dark, dark);
    }
    return dark;
}

/* Install the model->world basis for the next compute_vertex_lighting call.
 * origin = mesh world origin (world units); rot9 = body->world rotation (9
 * floats row-major) or NULL for identity (track geometry already in world-offset
 * space). Per-pane (g_rs) so concurrent threaded panes don't race. */
void td5_render_set_light_basis(const float origin[3], const float *rot9)
{
    s_light_basis_origin[0] = origin[0];
    s_light_basis_origin[1] = origin[1];
    s_light_basis_origin[2] = origin[2];
    if (rot9) {
        for (int i = 0; i < 9; i++) s_light_basis_rot[i] = rot9[i];
        s_light_basis_has_rot = 1;
    } else {
        s_light_basis_has_rot = 0;
    }
}

/* Transform every registered dynamic light into `mesh` model space, keeping
 * only those whose range sphere intersects the mesh bounding sphere. Returns
 * the kept count (0 = no dynamic lighting work for this mesh). */
static int light_build_model_list(const TD5_MeshHeader *mesh,
                                   LightModelPt *out, int max_out)
{
    /* [DEFERRED LIGHTS] The per-vertex point-light bump is superseded by the
     * screen-space deferred light pass (td5_render_apply_light_pass) and is
     * disabled by default. TD5RE_LIGHT_PERVERTEX=1 re-enables the legacy path
     * (note: registry intensity is now 0..1, so it would need rescaling). */
    static int s_pv = -1;
    if (s_pv < 0) { s_pv = td5_env_flag_off("TD5RE_LIGHT_PERVERTEX"); }
    if (!s_pv) return 0;

    int n = 0;
    const TD5_DynLight *lights = td5_light_list(&n);
    if (!lights || n <= 0) return 0;

    float ox = s_light_basis_origin[0];
    float oy = s_light_basis_origin[1];
    float oz = s_light_basis_origin[2];
    const float *R = s_light_basis_has_rot ? &s_light_basis_rot[0] : NULL;

    float bcx = mesh->bounding_center_x;
    float bcy = mesh->bounding_center_y;
    float bcz = mesh->bounding_center_z;
    float br  = mesh->bounding_radius;

    int kept = 0;
    for (int i = 0; i < n && kept < max_out; i++) {
        const TD5_DynLight *L = &lights[i];
        float dx = L->x - ox, dy = L->y - oy, dz = L->z - oz;
        float mx, my, mz;
        if (R) {
            /* world->model = R^T * dpos (R is body->world, row-major): the same
             * column-sum tl_commit_to_render_globals uses for light dirs. */
            mx = dx * R[0] + dy * R[3] + dz * R[6];
            my = dx * R[1] + dy * R[4] + dz * R[7];
            mz = dx * R[2] + dy * R[5] + dz * R[8];
        } else {
            mx = dx; my = dy; mz = dz;
        }
        /* Light-sphere vs mesh bounding-sphere reject (model space). */
        float sdx = mx - bcx, sdy = my - bcy, sdz = mz - bcz;
        float reach = L->range + br;
        if (sdx * sdx + sdy * sdy + sdz * sdz > reach * reach) continue;
        out[kept].x = mx; out[kept].y = my; out[kept].z = mz;
        out[kept].inv_range = (L->range > 0.0f) ? (1.0f / L->range) : 0.0f;
        out[kept].intensity = L->intensity;
        kept++;
    }
    return kept;
}

/* Per-vertex additive luminance from the model-space light list. nx/ny/nz is the
 * (unit) vertex normal; px/py/pz the model-space vertex position. */
static float light_vertex_bump(const LightModelPt *lm, int nlights,
                               float px, float py, float pz,
                               float nx, float ny, float nz)
{
    float bump = 0.0f;
    for (int k = 0; k < nlights; k++) {
        float dx = lm[k].x - px, dy = lm[k].y - py, dz = lm[k].z - pz;
        float d2 = dx * dx + dy * dy + dz * dz;
        float d  = sqrtf(d2);
        float t  = 1.0f - d * lm[k].inv_range;   /* linear falloff [0..1] */
        if (t <= 0.0f) continue;
        float ndotl;
        if (d > 1e-3f) {
            float inv = 1.0f / d;
            ndotl = (dx * nx + dy * ny + dz * nz) * inv;
        } else {
            ndotl = 1.0f;
        }
        if (ndotl < 0.0f) ndotl = 0.0f;
        /* Soft wrap: a little fill so surfaces angled away from the lamp still
         * catch the pool (the road and side faces don't all face the light). */
        float lit = ndotl * 0.85f + 0.15f;
        bump += lm[k].intensity * t * lit;
    }
    return bump;
}

void td5_render_compute_vertex_lighting(TD5_MeshHeader *mesh, int slot)
{
    if (!mesh) return;

    int count = mesh->total_vertex_count;
    /* [parallel-build] lighting writes target the pane workspace copy (the
     * blob is read-only in the render path); normals stay blob (read-only).
     * [LIGHT2 P1] bit 0 of normals_offset tags a DERIVED flat-normal stream
     * (td5_track_derive_missing_normals): those meshes feed the G-buffer only
     * — their artist-baked vertex lighting is left exactly as loaded. */
    TD5_MeshVertex *verts   = rs_vtx_rebase((void *)(uintptr_t)mesh->vertices_offset);
    uintptr_t nraw          = (uintptr_t)mesh->normals_offset;
    int norms_derived       = (int)(nraw & 1u);
    TD5_VertexNormal *norms = (TD5_VertexNormal *)(nraw & ~(uintptr_t)1);
    /* Blob (read-only original) — read the un-modulated baked grey from here so
     * the per-frame zone modulation never compounds on the workspace copy. */
    TD5_MeshVertex *vb = (TD5_MeshVertex *)(uintptr_t)mesh->vertices_offset;
    if (!verts || !norms || count <= 0) return;

    /* Paint tint (TD6 cars). 0/white => the original luminance-index path below
     * (TD5 cars are byte-identical). A real color packs the lit luminance into a
     * full ARGB diffuse so the color-LUT step (alpha==0 path) is bypassed and the
     * grayscale body is multiplied by the chosen hue. */
    uint32_t tint = (slot >= 0 && slot < TD5_ACTOR_MAX_TOTAL_SLOTS) ? s_vehicle_tint[slot] : 0;
    int use_tint = (tint != 0 && tint != 0x00FFFFFFu);
    int tr = (tint >> 16) & 0xFF, tg = (tint >> 8) & 0xFF, tb = tint & 0xFF;

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

    /* [2026-06-12 task#13] TD6 BAKED per-vertex lighting. The TD6 converter now
     * emits the artist-baked vertex grey as a full ARGB diffuse (alpha=0xFF) for
     * track geometry; preserve it instead of overwriting with our synthetic
     * 3-light guess, so faces shade like the original TD6 'lighting engine'.
     * Track geometry only (slot < 0) — vehicles keep synthetic lighting + paint
     * tint. A/B via TD5RE_TD6_VLIGHT (default on); =0 falls back to synthetic. */
    static int s_td6_vlight = -1;
    if (s_td6_vlight < 0) {
        s_td6_vlight = td5_env_flag_on("TD5RE_TD6_VLIGHT");
    }
    int prelit = (s_td6_vlight && slot < 0);

    /* [CAR DAMAGE 2026-06-28] Per-vertex damage "scuff": darken the diffuse on
     * struck panels so dents read as scuffed/scorched (a cheap texture-damage
     * look in the software lighting pass). NULL / 0 when no damage or off. */
    const float *dmg_scuff = NULL; int dmg_sc = 0; float scuff_str = 0.0f;
    if (td5_damage_get_scuff(slot, mesh, &dmg_scuff, &dmg_sc))
        scuff_str = td5_damage_scuff_strength();

    /* [DYNAMIC LIGHTS] Build the model-space light list for this mesh once.
     * nlights==0 (and dark off) leaves every branch below byte-identical to the
     * original. The light basis was installed by td5_render_set_light_basis()
     * right before this call (actor pos+rotation, or track origin / identity). */
    light_read_dark_knobs();
    LightModelPt lm[TD5_LIGHT_MAX];
    int nlights = light_build_model_list(mesh, lm, TD5_LIGHT_MAX);
    int dark = s_light_dark_mode;
    int floor_lum = dark ? s_dark_floor : TD5_LIGHTING_MIN;

    /* [LIGHT2 P0] Mode>=1 extras: (a) pack each vertex's WORLD normal into the
     * parallel workspace array so clip_and_submit_polygon can feed the
     * G-buffer; (b) resurrect the authored per-channel zone color that the
     * classic path averaged away (tl_set_contrib/tl_set_depth captured it).
     * Mode 0 leaves pack=NULL and colored=0 — every branch below is then
     * byte-identical to the classic path. */
    int l2mode = td5_light2_active();
    uint32_t *pack = (l2mode && g_rs->vtx_pack && verts == g_rs->vtx_work)
                   ? g_rs->vtx_pack : NULL;
    const float *nR = s_light_basis_has_rot ? &s_light_basis_rot[0] : NULL;
    float ch[3][3], amb_rgb[3];
    int colored = 0;
    if (l2mode) {
        for (int s = 0; s < 3; s++) {
            float cr = s_tl_chroma[s][0], cg = s_tl_chroma[s][1], cb = s_tl_chroma[s][2];
            /* all-zero = never captured (pre-first-zone frame) -> neutral */
            if (cr == 0.0f && cg == 0.0f && cb == 0.0f) { cr = cg = cb = 1.0f; }
            ch[s][0] = cr; ch[s][1] = cg; ch[s][2] = cb;
            if (cr != cg || cg != cb) colored = 1;
        }
        float ar = s_tl_amb_rgb[0], ag = s_tl_amb_rgb[1], ab = s_tl_amb_rgb[2];
        if (ar == 0.0f && ag == 0.0f && ab == 0.0f)
            ar = ag = ab = s_ambient_intensity;
        amb_rgb[0] = ar; amb_rgb[1] = ag; amb_rgb[2] = ab;
        if (ar != ag || ag != ab) colored = 1;
    }

    for (int i = 0; i < count; i++) {
        /* damage darkening factor for this vertex (1.0 = undamaged) */
        float df = 1.0f;
        if (dmg_scuff && i < dmg_sc && dmg_scuff[i] > 0.0f) {
            df = 1.0f - dmg_scuff[i] * scuff_str;
            if (df < 0.0f) df = 0.0f;
        }
        float nx = norms[i].nx;
        float ny = norms[i].ny;
        float nz = norms[i].nz;

        /* [LIGHT2 P0] World-space normal -> packed 8:8:8 (biased) for the
         * G-buffer feed. Rotate model->world with the mesh's body basis (rows
         * of the row-major body->world matrix); identity for track geometry.
         * Low bound 1 keeps the pack nonzero (0 = "no normal" sentinel). Runs
         * for prelit (TD6 baked) vertices too — they still want N.L lights. */
        if (pack) {
            float wnx = nx, wny = ny, wnz = nz;
            if (nR) {
                wnx = nR[0] * nx + nR[1] * ny + nR[2] * nz;
                wny = nR[3] * nx + nR[4] * ny + nR[5] * nz;
                wnz = nR[6] * nx + nR[7] * ny + nR[8] * nz;
            }
            /* [LIGHT2 P1] Zero-length normal = "unknown" (derived streams
             * leave billboard/degenerate corners at zero): keep pack[i] = 0
             * so those pixels stay G-buffer-less instead of getting a bogus
             * mid-grey normal. */
            if (wnx != 0.0f || wny != 0.0f || wnz != 0.0f) {
                /* [Y-CONVENTION] The G-buffer stores normals in POSITION
                 * space (world +Y is DOWN, so "up" = -Y). Authored normals +
                 * the zone light dirs live in the original's Y-FLIPPED
                 * lighting convention (car roofs = +Y, sun dirs = +Y) — the
                 * CPU vertex lighting stays wholly in that convention, but
                 * the GPU passes (N.L vs reconstructed positions, shadow /
                 * SSR ray marching) do geometry in position space, so the Y
                 * must flip here or roads reject overhead light and sun rays
                 * march into the ground. */
                int bx = clampi((int)( wnx * 127.0f) + 128, 1, 255);
                int by = clampi((int)(-wny * 127.0f) + 128, 1, 255);
                int bz = clampi((int)( wnz * 127.0f) + 128, 1, 255);
                pack[i] = ((uint32_t)bx << 16) | ((uint32_t)by << 8) | (uint32_t)bz;
            }
        }

        /* [LIGHT2 P1] Derived flat normals exist ONLY to feed the G-buffer —
         * the mesh keeps its artist-baked vertex lighting exactly as loaded
         * (the original never runtime-lights track geometry, and these meshes
         * were never lit by the port before either). */
        if (norms_derived)
            continue;

        if (prelit && (vb[i].lighting & 0xFF000000u) != 0u) {
            uint32_t c = vb[i].lighting;          /* keep the #13 baked TD6 grey... */
            if (df < 0.999f) {                    /* ...but darken where damaged */
                int r = (int)(((c >> 16) & 0xFF) * df);
                int g = (int)(((c >> 8)  & 0xFF) * df);
                int b = (int)(( c        & 0xFF) * df);
                c = (c & 0xFF000000u) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
            /* [DYNAMIC LIGHTS] dark-mode dim + additive headlight bump on the
             * baked ARGB (zero-cost / byte-identical when both are off). */
            if (dark || nlights) {
                int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
                if (dark) { r = (int)(r * s_dark_scale); g = (int)(g * s_dark_scale); b = (int)(b * s_dark_scale); }
                if (nlights) {
                    int ib = (int)light_vertex_bump(lm, nlights,
                                 vb[i].pos_x, vb[i].pos_y, vb[i].pos_z, nx, ny, nz);
                    r += ib; g += ib; b += ib;
                }
                if (dark) {
                    if (r < floor_lum) r = floor_lum;
                    if (g < floor_lum) g = floor_lum;
                    if (b < floor_lum) b = floor_lum;
                }
                if (r > 255) r = 255;
                if (g > 255) g = 255;
                if (b > 255) b = 255;
                c = (c & 0xFF000000u) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
            verts[i].lighting = c;
            continue;
        }

        /* 3-light diffuse accumulation */
        float dot0 = nx * l0[0] + ny * l0[1] + nz * l0[2];
        float dot1 = nx * l1[0] + ny * l1[1] + nz * l1[2];
        float dot2 = nx * l2[0] + ny * l2[1] + nz * l2[2];
        float intensity = dot0 + dot1 + dot2;

        /* Add ambient, clamp to [0x40, 0xFF] */
        intensity += s_ambient_intensity;
        /* [DYNAMIC LIGHTS] dark-mode dims the ambient+directional base; the
         * additive headlight bump is applied ON TOP (undimmed) so the lit pool
         * stands out in dark areas. Both no-ops when off => byte-identical. */
        if (dark) intensity *= s_dark_scale;
        float bump = 0.0f;
        if (nlights)
            bump = light_vertex_bump(lm, nlights,
                       verts[i].pos_x, verts[i].pos_y, verts[i].pos_z, nx, ny, nz);
        intensity += bump;
        int lum = (int)intensity;
        lum = clampi(lum, floor_lum, TD5_LIGHTING_MAX);

        /* [LIGHT2 P0] Colored zone lighting: the same 3-dot + ambient model,
         * evaluated per channel with the authored zone RGB (chroma ratios +
         * raw ambient RGB). Gray zones give chroma (1,1,1) / equal ambient
         * channels, in which case colored==0 and this path never runs — the
         * classic result is preserved bit-for-bit on uncolored data. */
        if (colored) {
            float fr = dot0 * ch[0][0] + dot1 * ch[1][0] + dot2 * ch[2][0] + amb_rgb[0];
            float fg = dot0 * ch[0][1] + dot1 * ch[1][1] + dot2 * ch[2][1] + amb_rgb[1];
            float fb = dot0 * ch[0][2] + dot1 * ch[1][2] + dot2 * ch[2][2] + amb_rgb[2];
            /* [car look] Vehicles only (slot>=0): desaturate the authored zone
             * colour toward its own average so warm/cool zones don't cast a tint
             * on car bodies. avg == the original's (r+g+b)/3 grey, so sat=0 gives
             * the faithful neutral car; sat=1 keeps full colour. Track (slot<0)
             * is never touched, so environment colour is preserved. */
            if (slot >= 0 && s_car_zone_sat < 0.999f) {
                float y = (fr + fg + fb) * (1.0f / 3.0f);
                fr = y + (fr - y) * s_car_zone_sat;
                fg = y + (fg - y) * s_car_zone_sat;
                fb = y + (fb - y) * s_car_zone_sat;
            }
            if (dark) { fr *= s_dark_scale; fg *= s_dark_scale; fb *= s_dark_scale; }
            fr += bump; fg += bump; fb += bump;
            int ir = clampi((int)fr, floor_lum, TD5_LIGHTING_MAX);
            int ig = clampi((int)fg, floor_lum, TD5_LIGHTING_MAX);
            int ib = clampi((int)fb, floor_lum, TD5_LIGHTING_MAX);
            if (use_tint) {
                ir = ir * tr / 255;
                ig = ig * tg / 255;
                ib = ib * tb / 255;
            }
            if (df < 0.999f) {
                ir = (int)(ir * df);
                ig = (int)(ig * df);
                ib = (int)(ib * df);
            }
            /* alpha 0xFF bypasses the gray color LUT at flush */
            verts[i].lighting = 0xFF000000u | ((uint32_t)ir << 16) |
                                ((uint32_t)ig << 8) | (uint32_t)ib;
            continue;
        }

        if (use_tint) {
            /* Full ARGB = lit-luminance * tint; alpha 0xFF bypasses the color LUT. */
            int lr = (int)((lum * tr) / 255 * df);
            int lg = (int)((lum * tg) / 255 * df);
            int lb = (int)((lum * tb) / 255 * df);
            verts[i].lighting = 0xFF000000u | ((uint32_t)lr << 16) |
                                ((uint32_t)lg << 8) | (uint32_t)lb;
        } else {
            /* luminance index -> color LUT; darken the index where scuffed. */
            int dl = (df < 0.999f) ? (int)(lum * df) : lum;
            if (dl < 0x18) dl = 0x18;            /* keep above the LUT's dark floor */
            verts[i].lighting = (uint32_t)dl;
        }
    }
}

/* TD6 track migration: a fixed daylight basis for OverrideTrackZip'd tracks.
 * Such tracks have no faithful per-span lighting zones (s_environs_level is out
 * of range), so apply_track_lighting falls back to a flat 0x40 ambient and the
 * de-indexed geometry renders uniformly dark. This sets 3 directional lights
 * (top + two obliques) + a bright ambient so compute_vertex_lighting produces
 * lit, textured geometry. The converter orients face normals to the upper
 * hemisphere, so the top light keeps ground/roofs bright. Gated by the caller
 * on g_td5.ini.override_track_zip, so faithful tracks never call this. */
void td5_render_set_override_daylight(void)
{
    s_light_dirs[0] =   0.0f; s_light_dirs[1] = 120.0f; s_light_dirs[2] =   0.0f; /* top   */
    s_light_dirs[3] =  70.0f; s_light_dirs[4] =  45.0f; s_light_dirs[5] =  45.0f; /* fr-rt */
    s_light_dirs[6] = -55.0f; s_light_dirs[7] =  45.0f; s_light_dirs[8] = -55.0f; /* bk-lf */
    s_ambient_intensity = 104.0f;   /* 0x68 base so even unlit faces stay visible */
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

/* ---------------------------------------------------------------------------
 * [reverse banners] TD6 P2P tracks bake START/FINISH + numbered 1..N banner
 * gantries at fixed spans in the LEVEL geometry, shared by both race
 * directions. Driven in REVERSE the player meets them in the opposite order,
 * so the START gantry sits at the finish line and the "4" banner is the first
 * one passed. We swap the banner TEXTURE PAGES at render time so reverse reads
 * START at the (reverse) start, FINISH at the (reverse) end, and the numbered
 * banners count 1,2,3,4 in pass order.
 *
 * Pages are textures.dir indices == runtime cmd->texture_page_id (preserved by
 * convert_td6_tracks.py; the converter also emits tex_NNN.png at that index, so
 * BOTH the source and target pages are loaded level textures). Keyed by
 * g_active_td6_level. Pairs below are the START<->FINISH swap plus the numbered
 * k<->(N+1-k) reversal (slot-paired across gantry posts, clamped to the other
 * role's last slot; the middle checkpoint of an odd count maps to itself and is
 * omitted). Banner textures are referenced ONLY by their gantry meshes, so
 * remapping a page never touches road/building geometry. Derived from the
 * authoritative per-role groups in re/tools (convert/extract banner classifiers).
 * ------------------------------------------------------------------------- */
static const struct { short level, from, to; } k_td6_rev_banner[] = {
    /* Paris (lvl 8): start[164] finish[218] cp[163,188,192,205] */
    {8,164,218},{8,218,164},{8,163,205},{8,205,163},{8,188,192},{8,192,188},
    /* NewYork (lvl 9): start[316,317,327] finish[306] cp[312,271,274,303] */
    {9,316,306},{9,317,306},{9,327,306},{9,306,316},
    {9,312,303},{9,303,312},{9,271,274},{9,274,271},
    /* Rome (lvl 10): no start/finish gantry; cp[{342,343},353,{354,355},{357,358,365},366] */
    {10,342,366},{10,343,366},{10,366,342},
    {10,353,357},{10,357,353},{10,358,353},{10,365,353},
    /* HongKong (lvl 11): start[216] finish[235] cp[215,192,228,236] */
    {11,216,235},{11,235,216},{11,215,236},{11,236,215},{11,192,228},{11,228,192},
    /* London (lvl 12): start[59,60] finish[57,58] cp[102,157,158,188] */
    {12,59,57},{12,60,58},{12,57,59},{12,58,60},
    {12,102,188},{12,188,102},{12,157,158},{12,158,157},
};

/* Set only while drawing LEVEL geometry in reverse on a TD6 track, so the
 * shared per-cmd dispatcher (also used by vehicles/props) never remaps them. */
static int s_td6_banner_remap_active = 0;

static int td6_reverse_banner_page(int page)
{
    int lvl = g_active_td6_level;
    size_t i;
    for (i = 0; i < sizeof(k_td6_rev_banner) / sizeof(k_td6_rev_banner[0]); i++)
        if (k_td6_rev_banner[i].level == lvl && k_td6_rev_banner[i].from == page)
            return k_td6_rev_banner[i].to;
    return page;
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
    /* [task#7] Billboard-tree (header tag 1/2) diagnostics: do camera-facing
     * tree/sign meshes actually reach the billboard branch, or are they rejected
     * upstream? Counts let the user tell "render culls them" (a code bug) from
     * "they're not in the stream" (the stale-asset origin-fold bug — an asset
     * regen, NOT a render fix). */
    static int s_dbg_bb_seen = 0;     /* meshes with tag 1/2 passing ptr+header checks */
    static int s_dbg_bb_culled = 0;   /* ... then rejected by the frustum sphere test  */
    static int s_dbg_bb_drawn = 0;    /* ... reaching the camera-facing billboard branch */

    /* [DIAGNOSTIC] TD5RE_BILLBOARD_DEBUG (default off): render billboard tree
     * quads opaque + solid-coloured (ignore texture & alpha key) to disambiguate
     * a geometry/transform/cull bug from a texture/alpha bug by eye. */
    static int s_bb_debug_solid = -1;
    if (s_bb_debug_solid < 0) {
        const char *e = getenv("TD5RE_BILLBOARD_DEBUG");
        s_bb_debug_solid = (e && e[0] && e[0] != '0') ? 1 : 0;
        TD5_LOG_I(LOG_TAG,
                  "billboard debug-solid: %s (TD5RE_BILLBOARD_DEBUG; opaque magenta "
                  "tree quads via white page 899)", s_bb_debug_solid ? "ON" : "OFF");
    }

    if (!display_list_block) return;

    uint32_t *block = (uint32_t *)display_list_block;
    int count = (int)block[0];
    s_debug_dl_calls++;
    if (count <= 0 || count > 256) return; /* sanity */

    TD5_LOG_D(LOG_TAG,
              "span display list: block=%p mesh_range=[0,%d)",
              display_list_block, count);

    /* [reverse banners] enable START<->FINISH + numbered banner-page swap for
     * this level-geometry pass only (cleared after the loop so vehicles/props
     * sharing td5_render_prepared_mesh are never remapped). */
    s_td6_banner_remap_active = (g_active_td6_level > 0 && g_td5.reverse_direction) ? 1 : 0;

    /* [banners] mark the level-geometry pass so the one-sided banner cull in
     * clip_and_submit_polygon applies (and never touches cars/HUD). */
    if (s_banner_cull < 0) {
        const char *f = getenv("TD5RE_BANNER_CULL_FLIP");
        s_banner_cull = td5_env_flag_on("TD5RE_BANNER_CULL");
        /* Default keeps the front (text-readable) face; verified on London —
         * the other sign showed the mirrored back. FLIP swaps back if needed. */
        s_banner_cull_keep_pos = (f && f[0] && f[0] != '0') ? 0 : 1;
        /* [#9 2026-06-19] In REVERSE the banner panels' screen winding flips, so
         * the forward-tuned kept side discards the camera-facing face and the
         * banners vanish (Paris-backwards report). Auto-flip the kept side in
         * reverse; "0" disables (then use _FLIP manually). */
        s_banner_cull_revflip = td5_env_flag_on("TD5RE_TD6_BANNER_REVFLIP");
        /* [NATIVE BANNERS] Kept winding sign for geometry-detected native-track
         * banner pages (td5_track_scan_banner_pages). Defaults to the TD6 sign
         * (native TD5 and converted-TD6 meshes share the projection/winding
         * convention); TD5RE_NATIVE_BANNER_FLIP=1 flips it if native banners
         * come out showing their mirrored back / vanish. */
        s_native_banner_keep_pos = td5_env_flag_off("TD5RE_NATIVE_BANNER_FLIP")
                                   ? !s_banner_cull_keep_pos : s_banner_cull_keep_pos;
        TD5_LOG_I(LOG_TAG, "banner cull: %s (keep_sign=%s revflip=%d native_keep=%s; TD5RE_BANNER_CULL/_FLIP/_TD6_BANNER_REVFLIP/_NATIVE_BANNER_FLIP)",
                  s_banner_cull ? "ON" : "OFF", s_banner_cull_keep_pos ? "pos" : "neg",
                  s_banner_cull_revflip, s_native_banner_keep_pos ? "pos" : "neg");
        /* [START-banner align] road-centre re-alignment of TD6 banner gantries.
         * Default ON; TD5RE_BANNER_ALIGN=0 restores the raw authored position. */
        s_banner_align = td5_env_flag_on("TD5RE_BANNER_ALIGN");
        TD5_LOG_I(LOG_TAG, "banner align: %s (TD5RE_BANNER_ALIGN)",
                  s_banner_align ? "ON" : "OFF");
    }
    s_level_pass_active = 1;

    /* [DRAG WIDE ROAD 2026-06-28] During a drag race, widen the visible level
     * geometry (asphalt, borders, start/finish banner) laterally so the road
     * physically scales with the field. Road meshes are centred at model X=0, so
     * a pos_x scale about 0 widens about the centreline. Scale = field/4 keeps
     * the asphalt width equal to the N-lane navigation strip (cars stay one per
     * lane on real road). Knobs: TD5RE_DRAG_WIDEN_ROAD=0 disables; TD5RE_DRAG_
     * ROAD_SCALE=f forces an exact factor (else auto = field/4, clamped). */
    s_drag_road_scale = 1.0f;
    if (g_td5.drag_race_enabled) {
        const char *wr = getenv("TD5RE_DRAG_WIDEN_ROAD");
        if (!wr || wr[0] != '0') {
            const char *rs = getenv("TD5RE_DRAG_ROAD_SCALE");
            float scale;
            if (rs && rs[0]) {
                scale = (float)atof(rs);
            } else {
                int field = td5_game_drag_field_size();
                scale = (float)field / 4.0f;   /* original drag strip = 4 lanes */
            }
            if (scale < 1.0f) scale = 1.0f;
            if (scale > 4.0f) scale = 4.0f;
            s_drag_road_scale = scale;
        }
    }

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

        /* [task#7] Is this a camera-facing billboard mesh (header tag 1/2)? */
        int dbg_is_bb = (mesh->texture_page_id == 1 || mesh->texture_page_id == 2);
        if (dbg_is_bb) s_dbg_bb_seen++;

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

        /* [DRAG STADIUM EXTEND] In a drag race the baked stadium scenery is a
         * STRAIGHT (dl 0..34, bc_z down to ~-193265) capped by a CURVED oval end
         * (dl 35, bc_z ~-198054) then bare ground tiles (bc_z <-204000). Once the
         * drag is lengthened the straight runs past dl 34, so the curved cap + back
         * stands + tiles would sit mid-track. Drop everything past the straight
         * (bc_z < -195000) during the NORMAL walk (s_dl_z_offset==0); the tiled
         * stadium-block copies carry s_dl_z_offset!=0 so they bypass this and
         * re-enclose the extension cleanly. */
        if (g_td5.drag_race_enabled && s_dl_z_offset == 0.0f && cz < -195000.0f)
            continue;

        /* [DRAG FINISH GANTRY] Suppress the finish-line gantry at its ORIGINAL
         * position (~span 204) during the normal walk — it's the FINISH BANNER, which
         * shouldn't sit mid-strip. That leaves a hole (the gantry bundles the road
         * there), so td5_render_drag_stadium_extension re-fills that exact spot with a
         * plain stadium block (no banner); the banner itself is re-drawn only at the
         * relocated finish by td5_render_drag_finish_line (s_dl_z_offset!=0). */
        if (g_td5.drag_race_enabled && s_dl_z_offset == 0.0f && mesh == s_drag_gantry_mesh)
            continue;

        /* [#20 HK reverse] DELIBERATE DEVIATION (user-requested): remove the building
         * standing in the Hong Kong REVERSE racing line (models entry 509 sub 8/10/11,
         * matched by EXACT bounding centre). Each of those submeshes spans from road
         * level up to the roof, so instead of dropping them whole (which holed the
         * road) we set a clip height: clip_and_submit_polygon then drops their WALL/
         * ROOF faces and KEEPS their road-level faces. Neighbours (different centres)
         * and the road are untouched. HK (level 11) reverse only; forward keeps it. */
        s_hk_clip_y = 0.0f;
        if (g_active_td6_level == 11 && g_td5.reverse_direction && cy > 5000.0f) {
            static const float hk_bldg[3][2] = {
                { 411028.0f, 1731209.0f }, { 416336.0f, 1725277.0f }, { 413088.0f, 1725313.0f } };
            int bi;
            for (bi = 0; bi < 3; bi++) {
                float dbx = cx - hk_bldg[bi][0], dbz = cz - hk_bldg[bi][1];
                if (dbx * dbx + dbz * dbz < 350.0f * 350.0f) { s_hk_clip_y = 3500.0f; break; }
            }
        }

        /* [task#7] One-shot dump of the first few billboard meshes' bounding
         * centres: clustered near (0,0,0) == the stale origin-fold asset bug
         * (trees piled at world origin -> culled everywhere but the start);
         * spread out == the converter folded origins correctly. */
        if (dbg_is_bb) {
            static int s_bb_dump = 0;
            if (s_bb_dump < 8) {
                TD5_LOG_I(LOG_TAG,
                    "BB_TREE_MESH tag=%d bc=(%.1f,%.1f,%.1f) origin_raw=(%.1f,%.1f,%.1f) r=%.1f",
                    (int)mesh->texture_page_id, cx, cy, cz,
                    mesh->origin_x, mesh->origin_y, mesh->origin_z, r);
                s_bb_dump++;
            }
        }

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

        if (!td5_render_is_sphere_visible(cx, cy, cz + s_dl_z_offset, r)) {
            if (dbg_is_bb) s_dbg_bb_culled++;   /* [task#7] */
            continue;
        }

        /* Build world-to-view basis from mesh origin — origin is in integer-
           coordinate space, must scale by 1/256 to match camera (render-float
           space) before subtraction [CONFIRMED @ 0x42d954-0x42d97a] */
        TD5_Vec3f origin;
        origin.x = mesh->origin_x * (1.0f / 256.0f);
        origin.y = mesh->origin_y * (1.0f / 256.0f);
        origin.z = mesh->origin_z * (1.0f / 256.0f) + s_dl_z_offset;  /* [DRAG STADIUM EXTEND] */

        /* [LONDON START BANNER 2026-06-23] Re-centre a TD6 banner gantry's OVERHEAD
         * panels over the road. The gantry mesh bundles the overhead banner (Y~2500-
         * 4000) with a ground start-plaza (pages 12/13 at Y~0); only the overhead
         * panels are mis-placed, so we shift ONLY those (via s_banner_vshift_x in
         * the vertex transform, Y-gated) and leave the ground plaza where it sits —
         * shifting the whole mesh dragged the plaza off the road. The shift is one
         * common delta = (road-centre X at the gantry's span) - (gantry group-centre
         * X), so a split START gantry's two half-panels keep their separation.
         * FINISH/checkpoints already sit over their road (group==road -> ~0, skipped).
         * The +/-50000 window rejects a pathological span mis-match (the far-end road
         * can sit ~1e6 units away) so a banner can never be flung off the map. */
        s_banner_vshift_x = 0.0f;
        if (s_banner_align && g_active_td6_level > 0 && td6_mesh_uses_banner_page(mesh)) {
            float gx = td6_banner_group_center_x(block, count,
                                                 mesh->bounding_center_z,
                                                 mesh->bounding_center_x);
            float rx = 0.0f;
            if (td6_banner_roadcenter_x(gx, mesh->bounding_center_z, &rx)) {
                float bdx = rx - gx;
                if (bdx > -50000.0f && bdx < 50000.0f && (bdx > 1.0f || bdx < -1.0f)) {
                    s_banner_vshift_x = bdx;
                    if (s_banner_align_log < 16) {
                        TD5_LOG_I(LOG_TAG,
                            "banner align: lvl=%d gantry_z=%.0f group_x=%.0f road_x=%.0f dx=%.0f (overhead panels only)",
                            g_active_td6_level, mesh->bounding_center_z,
                            gx, rx, bdx);
                        s_banner_align_log++;
                    }
                }
            }
        }

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
            s_dbg_bb_drawn++;   /* [task#7] reached the camera-facing branch */
            /* per-pane billboard basis (baked from g_cameraSecondaryUnscaled in
             * td5_render_bake_camera) — reading the shared global here would make
             * all threaded panes orient billboards with the last pane's camera. */
            td5_render_push_transform();
            td5_render_load_rotation((const TD5_Mat3x3 *)s_camera_secondary);
            td5_render_transform_mesh_vertices(mesh);
            /* Skip runtime vertex-lighting recompute for billboard meshes.
             * RenderTrackSpanDisplayList @ 0x00431270 only calls
             * TransformAndQueueTranslucentMesh which transforms XYZ only.
             * Per-vertex intensity comes from the asset's baked values
             * (pre-dimmed for type-3 additive billboards by
             * td5_track_dim_additive_billboard_meshes at track load). */

            /* [DIAGNOSTIC] TD5RE_BILLBOARD_DEBUG=1 (default off): render the tree
             * quads OPAQUE + solid magenta — bind the 1x1 white page (page 899,
             * type-0 => OPAQUE_LINEAR, no alpha-key discard) via tex_page_override
             * and overwrite the just-transformed workspace verts' diffuse with an
             * opaque colour (high alpha byte skips the flush colour-LUT remap).
             * This isolates GEOMETRY from TEXTURE/ALPHA: if solid magenta quads
             * appear where trees should be, the geometry/transform/cull is sound
             * and any remaining invisibility is texture/alpha; if STILL nothing,
             * the quads are collapsed/culled (geometry). Pair with
             * TD5RE_BILLBOARD_TREE_FIX to confirm the camera_secondary root
             * cause: FIX=0+DEBUG=1 stays blank (collapsed), FIX=1+DEBUG=1 shows
             * solid quads (basis now valid). */
            int bb_dbg_restore_override = g_rs->tex_page_override;
            if (s_bb_debug_solid) {
                int vc = mesh->total_vertex_count;
                if (g_rs->vtx_work && vc > 0 && vc <= g_rs->vtx_work_cap) {
                    for (int vi = 0; vi < vc; vi++)
                        g_rs->vtx_work[vi].lighting = 0xFFFF00FFu; /* opaque magenta (ARGB) */
                }
                g_rs->tex_page_override = 899; /* SHARED_PAGE_WHITE: 1x1 opaque white */
            }

            td5_render_prepared_mesh(mesh);

            if (s_bb_debug_solid)
                g_rs->tex_page_override = bb_dbg_restore_override;

            s_debug_span_meshes_submitted++;
            td5_render_pop_transform();
        } else {
            td5_render_transform_mesh_vertices(mesh);
            /* TD6 override tracks have no faithful lighting zones -> set a fixed
             * daylight basis so de-indexed geometry isn't flat-dark. Faithful
             * tracks (override 0) keep the per-span zone lighting untouched. */
            if (g_active_td6_level > 0)
                td5_render_set_override_daylight();
            /* [DYNAMIC LIGHTS] track verts are world-space offset by `origin`
             * (model rotation identity), so the light basis is origin + no rot. */
            { float lbo[3] = { origin.x, origin.y, origin.z };
              td5_render_set_light_basis(lbo, NULL); }
            td5_render_compute_vertex_lighting(mesh, -1);   /* track mesh: no tint */
            td5_render_prepared_mesh(mesh);
            s_debug_span_meshes_submitted++;
        }
    }
    s_hk_clip_y = 0.0f;   /* [#20 HK reverse] don't leak the building clip into vehicles/other */
    s_td6_banner_remap_active = 0;  /* [reverse banners] level-geometry pass only */
    s_level_pass_active = 0;        /* [banners] one-sided cull off outside level geometry */
    s_banner_vshift_x = 0.0f;       /* [START-banner align] never leak into sky/car/other mesh transforms */
    s_drag_road_scale = 1.0f;       /* [DRAG WIDE ROAD] never leak the lateral widen into car/sky/HUD */

    if ((s_debug_dl_calls % 500) == 1) {
        TD5_LOG_I(LOG_TAG,
            "mesh filter: calls=%d accept=%d rej_ptr=%d rej_cnt=%d "
            "rej_off=%d rej_blob=%d",
            s_debug_dl_calls, s_debug_accept, s_debug_reject_ptr,
            s_debug_reject_counts, s_debug_reject_offsets, s_debug_reject_blob);
        /* [task#7] billboard-tree reachability. seen=0 -> NO tag-1/2 meshes in
         * the stream (asset/converter problem, not render). seen>0 & drawn~=seen
         * -> they DO reach the camera-facing branch (render path OK). culled
         * dominating seen -> frustum-culled (suspect stale origin-fold piling
         * them at world origin; see BB_TREE_MESH bc dumps above). */
        TD5_LOG_I(LOG_TAG,
            "billboard-tree(tag1/2): seen=%d culled=%d drawn=%d",
            s_dbg_bb_seen, s_dbg_bb_culled, s_dbg_bb_drawn);
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

            /* Dispatch through table with correct vertex pointer.
             * [parallel-build] Rebase blob-derived pointers into this pane's
             * vertex workspace (where transform/lighting wrote the runtime
             * fields); out-of-range pointers pass through unchanged. The
             * per-pane texture override serves the reflection overlay, which
             * previously patched the SHARED blob command list in place. */
            TD5_PrimitiveCmd patched = *cmd;
            patched.vertex_data_ptr = (uint32_t)(uintptr_t)rs_vtx_rebase(cmd_verts);
            if (g_rs->tex_page_override >= 0)
                patched.texture_page_id = (int16_t)g_rs->tex_page_override;
            else if (s_td6_banner_remap_active)
                patched.texture_page_id =
                    (int16_t)td6_reverse_banner_page(patched.texture_page_id);
            s_dispatch_table[opcode](&patched, rs_vtx_rebase(base_verts));

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
    s_vehicle_tint[slot] = 0;   /* default white; game re-sets it for TD6 player car */
    s_vehicle_is_td6[slot] = 0; /* asset loader re-sets it for a transcoded TD6 mesh */
    g_vehicle_taillight_valid[slot] = 0; /* [S23] loader re-sets TD6 authored CAR_LIGHTS */
}

TD5_MeshHeader *td5_render_get_vehicle_mesh(int slot)
{
    if (slot < 0 || slot >= TD5_ACTOR_MAX_TOTAL_SLOTS)
        return NULL;

    return s_vehicle_meshes[slot];
}

/* [POLICE rewrite 2026-06-19] The dedicated police mesh drawn over cop slots
 * (set once per race by td5_game from td5_asset_load_cop_mesh; NULL = none, cops
 * then draw their ordinary traffic mesh). */
static TD5_MeshHeader *s_cop_mesh = NULL;
void td5_render_set_cop_mesh(TD5_MeshHeader *mesh) { s_cop_mesh = mesh; }

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
/* s_in_sky_draw moved to RenderScratch (Phase B Stage 1). */

/* Custom-track STRIP-ribbon render fallback (defined below, near the debug-line
 * helpers it reuses). Draws the collision ribbon as a solid road when a level
 * has no MODELS.DAT mesh table. */
/* td5_render_fallback_strip_ribbon: defined in td5_render.c (seam header decl). */
/* [DRAG STADIUM EXTEND] Re-render the real finish gantry (MODELS.DAT dl 26 sub 0) at
 * the relocated drag finish span (defined later, near the finish helpers). */
/* td5_render_drag_finish_line: defined in td5_render.c (seam header decl). */

/* Read display-list entry N's first sub-mesh bounding-centre Z (render-float =
 * the 24.8 world coord / 256). Returns 1 on success. */
static int td5_render_dl_first_bcz(int idx, float *out_z)
{
    void *e = td5_track_get_display_list_entry(idx);
    uint32_t *b;
    TD5_MeshHeader *m;
    if (!e) return 0;
    b = (uint32_t *)e;
    if ((int)b[0] < 1) return 0;
    m = (TD5_MeshHeader *)(uintptr_t)b[1];
    if (!m || !td5_track_is_valid_mesh_ptr(m)) return 0;
    *out_z = m->bounding_center_z;
    return 1;
}

/* [DRAG STADIUM EXTEND 2026-06-29] The drag strip is a straight inside an oval
 * stadium whose baked scenery (road + grandstands, MODELS.DAT dl 0..35) ends at
 * world-Z ~-198000 (~span 240); past it only bare ground tiles exist. When the
 * drag is lengthened to use the road beyond that, the extension looks orphaned.
 * Here we TILE a clean mid-stadium block (dl 18 = road+stands together) down the
 * extension by repeatedly drawing it shifted in Z (s_dl_z_offset), so the longer
 * straight stays enclosed. The per-block frustum cull (which now also honours
 * s_dl_z_offset) draws only the 2-3 copies near the camera. Drag-only. */
void td5_render_drag_stadium_extension(void)
{
    void *tmpl, *cap;
    uint32_t *b;
    TD5_MeshHeader *m;
    float tz, step, stride, T, seam_z, last_T, cap_z = 0.0f;
    /* [DRAG DISTANCE 2026-06-30] End the stadium near the CHOSEN finish, not always
     * at the strip end — otherwise a SHORT/MEDIUM race had stadium running far past
     * the finish (to the EPIC extent). Tile only to the finish span (+ ~30 spans of
     * stadium run-off), clamped so it never overshoots the strip end. For SHORT/
     * MEDIUM the finish sits inside the original stadium, so this lands shallower
     * than the seam and the loop below does not run (the vanilla ~span-240 stadium
     * + its back-cap stands; the original cap is still replaced near span 243). */
    float EXTEND_END_Z = -286000.0f;           /* fallback = base strip end (~span 298) */
    {
        int cpc = td5_game_get_minimap_checkpoint_count();
        int lx, ly, lz;
        int last_span = g_td5.track_span_ring_length - 2;  /* true strip end (moves with the lengthened strip) */
        int lln = (last_span > 1) ? td5_track_span_lane_count_at(last_span) : 1;
        float strip_end_z = -286000.0f;
        if (lln < 1) lln = 1;
        if (last_span > 1 && td5_track_get_span_lane_world(last_span, lln / 2, &lx, &ly, &lz))
            strip_end_z = (float)lz / 256.0f;
        if (cpc > 0) {
            int fspan = td5_game_get_minimap_checkpoint_span(cpc - 1);
            int fx, fy, fz, lanes = td5_track_span_lane_count_at(fspan);
            if (lanes < 1) lanes = 1;
            if (fspan >= 1 && td5_track_get_span_lane_world(fspan, lanes / 2, &fx, &fy, &fz))
                EXTEND_END_Z = (float)fz / 256.0f - 47000.0f;   /* ~30 spans past finish */
        }
        /* Never tile past the strip's REAL end — but that end now MOVES with the
         * lengthened LONG/EPIC strip. The old hardcoded -286000 floor capped the
         * stands at the original span-298 end, leaving the inserted road bare. */
        if (EXTEND_END_Z < strip_end_z) EXTEND_END_Z = strip_end_z;
        { static int sl = 0; if ((sl++ % 120) == 0)
            TD5_LOG_I(LOG_TAG, "drag extend bounds: finish_cp=%d strip_last=%d strip_end_z=%.0f end_z=%.0f",
                      (cpc > 0) ? td5_game_get_minimap_checkpoint_span(cpc - 1) : -1,
                      last_span, strip_end_z, EXTEND_END_Z); }
    }
    int copies = 0;
    static int s_frame = 0;
    int do_log = ((s_frame++ % 120) == 0);

    if (!g_td5.drag_race_enabled) return;

    tmpl = td5_track_get_display_list_entry(18);  /* clean mid-stadium block */
    if (!tmpl) return;
    b = (uint32_t *)tmpl;
    if ((int)b[0] < 1) return;
    m = (TD5_MeshHeader *)(uintptr_t)b[1];
    if (!m || !td5_track_is_valid_mesh_ptr(m)) return;
    tz = m->bounding_center_z;

    /* Per-block Z length. Measure over a WIDE span of straight blocks so per-block
     * jitter averages out — (bc_z[10] - bc_z[30]) / 20 — which is more reliable than
     * a single adjacent pair. Fall back to dl18/19, then the measured ~6029. */
    step = 6029.0f;
    {
        float z10, z30, za, zb;
        if (td5_render_dl_first_bcz(10, &z10) && td5_render_dl_first_bcz(30, &z30)) {
            float d = (z10 - z30) / 20.0f;       /* bc_z decreases down-track */
            if (d > 1500.0f && d < 20000.0f) step = d;
        } else if (td5_render_dl_first_bcz(18, &za) && td5_render_dl_first_bcz(19, &zb)) {
            float d = za - zb; if (d < 0.0f) d = -d;
            if (d > 1500.0f && d < 20000.0f) step = d;
        }
    }

    /* Inter-tile stride: a hair SHORTER than the true block length so consecutive
     * tiles overlap ~8% and can never accumulate a hairline gap down the row. */
    stride = step * 0.92f;

    /* Seam: dl 34 is the LAST original straight block (curved cap dl 35 suppressed).
     * START THE FIRST TILE ON TOP of dl 34 — half a block UP-track of its centre
     * (seam_z + step*0.5) — so the first tiled block double-covers the seam region
     * instead of butting it. The tiles are identical grandstand geometry, so the
     * overlap is visually invisible while a gap is not; bias hard toward overlap. */
    seam_z = -193265.0f;
    td5_render_dl_first_bcz(34, &seam_z);

    last_T = seam_z;
    for (T = seam_z + step * 0.5f; T > EXTEND_END_Z; T -= stride) {
        s_dl_z_offset = T - tz;                 /* shift template to T */
        td5_render_span_display_list(tmpl);
        last_T = T;
        if (do_log && copies < 8)
            TD5_LOG_I(LOG_TAG, "drag tile[%d] z=%.0f (off=%.0f)", copies, T, T - tz);
        if (++copies > 240) break;              /* safety bound — high for the lengthened
                                                 * LONG/EPIC strip; off-screen copies are
                                                 * frustum-culled inside the dispatch */
    }

    /* [DRAG FINISH GANTRY] The gantry block (dl 26) is suppressed at span ~204 in the
     * normal walk (it crosses the road as an overhead banner — unwanted mid-strip),
     * leaving a NOTCH in the otherwise-continuous tall banner walls. The gantry
     * footprint is ~1.5 blocks wide and is NOT centred between dl 25 and dl 27 (the
     * dl 26->dl 27 gap is ~9k units, far more than dl 25->dl 26), so a single fill
     * block left a sliver. TILE the adjacent original block (dl 27) across the WHOLE
     * dl 25 .. dl 27 span with the same overlapping stride the main extension uses, so
     * the banner walls are continuous with no gap and match the surrounding stadium
     * EXACTLY. Gated on s_drag_gantry_mesh (gantry identified => being suppressed) so
     * there is no 1-frame double-draw before the suppress kicks in. */
    if (s_drag_gantry_mesh) {
        void *adj = td5_track_get_display_list_entry(27);
        float z25 = 0.0f, z27 = 0.0f;
        if (adj && td5_render_dl_first_bcz(25, &z25) && td5_render_dl_first_bcz(27, &z27)) {
            float f; int nf = 0;
            for (f = z25; f >= z27 && nf < 6; f -= stride, nf++) {
                s_dl_z_offset = f - z27;
                td5_render_span_display_list(adj);
            }
            { static int sfl = 0; if ((sfl++ % 120) == 0)
                TD5_LOG_I(LOG_TAG, "drag gantry FILL(dl27 x%d): z25=%.0f z27=%.0f stride=%.0f",
                          nf, z25, z27, stride); }
        }
    }

    /* Back end-cap (dl 35, the curved oval end) butting onto the LAST tile — one
     * block past it — so the enclosure closes with no gap regardless of step. */
    cap = td5_track_get_display_list_entry(35);
    if (cap) {
        uint32_t *cb = (uint32_t *)cap;
        if ((int)cb[0] >= 1) {
            TD5_MeshHeader *cm = (TD5_MeshHeader *)(uintptr_t)cb[1];
            if (cm && td5_track_is_valid_mesh_ptr(cm)) {
                cap_z = last_T - stride;
                s_dl_z_offset = cap_z - cm->bounding_center_z;
                td5_render_span_display_list(cap);
            }
        }
    }
    s_dl_z_offset = 0.0f;                        /* never leak into other passes */

    if (do_log)
        TD5_LOG_I(LOG_TAG,
            "drag stadium extend: tmpl_dl18 tz=%.0f step=%.0f stride=%.0f copies=%d seam=%.0f start=%.0f last=%.0f cap=%.0f",
            tz, step, stride, copies, seam_z, seam_z + step * 0.5f, last_T, cap_z);
}

/* [traffic-view-dist 2026-06-29] Multiplier on how far traffic + AI cars (and the
 * road geometry under them) stay drawn. The faithful actor render-cull window is
 * ~88 spans at the default view distance (0.65) / 128 at max — much SHORTER than
 * the terrain horizon (frustum far-cull), so cars visibly pop in/out far closer
 * than the visible road ("traffic disappears too often / pops out at short range",
 * reported in EVERY race, single-player included). This widens the actor cull AND
 * the track display-list forward/back walk by the SAME factor so traffic can never
 * float on unrendered road. Applied to faithful TD5 tracks only — the dense TD6
 * city tracks keep their perf-capped distance (td5_render.c view-distance cap).
 * Default 1.6x; TD5RE_TRAFFIC_VIEW_DIST overrides (clamped [1.0, 2.5]). The
 * matching despawn keep-distance (trf_dyn_front_keep_floor in td5_ai.c) reads the
 * SAME env var so cars always fade out beyond the render window, never on-screen. */
static float td5_render_traffic_view_mult(void)
{
    static float m = -1.0f;
    if (m < 0.0f) {
        m = td5_env_float("TD5RE_TRAFFIC_VIEW_DIST", 1.6f, 1.0f, 2.5f);
        TD5_LOG_I(LOG_TAG, "traffic_view_dist knob: TD5RE_TRAFFIC_VIEW_DIST x%.2f "
                  "(traffic/car render distance scaled %d%%)", m, (int)(m * 100.0f + 0.5f));
    }
    return m;
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
            int ring = td5_track_get_ring_length();
            /* Branch-road cull center. The original render walk windows the
             * track display list on the actor's WRAPPED/normalized span
             * (actor +0x82, always in [0, ring_length)) — the strip walker
             * keeps that field advancing along the main-ring progress axis
             * even while the car is physically on a branch road
             * [CONFIRMED orig RunRaceFrame @ 0x0042bbb2 `MOVSX EAX,word ptr
             * [EAX+0x82]`; reverse mirror @ 0x0042bbc2]. The raw span (+0x80)
             * goes >= ring_length on a branch, so it must NOT be used as the
             * window center directly.
             *
             * The port previously remapped the raw branch span to a "junction"
             * main-road span via td5_track_branch_to_junction(). On the
             * migrated TD6 tracks (e.g. London level012) that jump table chains
             * branch->branch and returns a value that is ITSELF >= ring_length
             * (raw 2137 -> 4285 with ring 2136). The out-of-range center then
             * blew out the TD6 proportional span->entry map: start_entry=1049
             * against only 534 display-list entries -> every main-road entry
             * clamped out (P2P) -> all city geometry vanished on branch entry.
             * Use the normalized span like the original; keep the raw branch
             * span only to drive the port-only branch-road STRIP fallback. */
            if (player_span >= ring) {
                player_branch_span = player_span;
                player_span = (int)player->track_span_normalized;
            }
            /* Hard guarantee: the cull-window center must be a valid in-ring
             * span so both the TD5 (span>>2) and TD6 (proportional) entry
             * windows can never index past the display list. The normalizer's
             * negative-exact-multiple edge can store exactly ring_length, so
             * clamp as a last resort regardless of how player_span was set. */
            if (ring > 0) {
                if (player_span < 0)           player_span = 0;
                else if (player_span >= ring)  player_span = ring - 1;
            }
            {
                static int s_branch_logged = -2;
                int on_branch = (player_branch_span >= 0) ? 1 : 0;
                if (view_index == 0 && on_branch != s_branch_logged) {
                    TD5_LOG_I(LOG_TAG,
                        "branch cull-center: on_branch=%d branch_span=%d center(norm)=%d ring=%d td6lvl=%d",
                        on_branch, player_branch_span, player_span, ring,
                        g_active_td6_level);
                    s_branch_logged = on_branch;
                }
            }
        }
    }
    float view_dist_frac = td5_save_get_view_distance();
    /* [2026-06-08 split-screen perf] AI spectator panes (the extra split-screen
     * tiles beyond the local human players, view_index >= num_human_players)
     * render at reduced draw distance. v_track — the windowed track display-list
     * walk below — is the #1 per-pane render cost in N-way split (per-pass probe
     * at 9 panes), and these panes are secondary 640x336 tiles, so halving their
     * span window/actor cull trims the cost with little visible loss. The
     * player's pane(s) (view_index < num_human_players) keep full distance, so
     * single/2-player split is byte-unchanged. Composes with the TD6 cap below. */
    if (g_td5.split_screen_mode > 0 && view_index >= g_td5.num_human_players)
        view_dist_frac *= 0.5f;
    /* [PERF FIX 2026-06-05; generalised 2026-06-19] Dense TD6 city tracks — the
     * 5 P2P cities (Paris..London, levels 8-12) and Egypt (level022) — pack ~12
     * meshes per span, so the port's 1.0 view distance walks ~780 candidate
     * span-meshes/frame through the core, spiking the world-render zone (r_world)
     * to ~35ms avg / 120ms peak (~40fps) — the TD6 render lag. Cap THEIR effective
     * view distance to the ORIGINAL game's faithful 0.65 default (@0x0042AA27);
     * profiled A/B cuts sustained r_world ~4.5x (54->12ms) for a minor distant
     * pop-in, while every other track keeps the port's full 1.0. far_cull is
     * fixed/slider-independent (2026-05-31 popin fix), so only the MODELS.DAT
     * entry-walk depth shrinks — distant buildings still resolve. A user VIEW
     * slider / [Display] ViewDistance set lower than the cap still wins. (Was
     * London+Egypt only; the other 4 cities are equally dense.) */
    if (((g_active_td6_level >= 8 && g_active_td6_level <= 12) || g_active_td6_level == 22) &&
        view_dist_frac > 0.65f)
        view_dist_frac = 0.65f;
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
    /* [traffic-view-dist 2026-06-29] Widen the traffic/car visibility window on
     * faithful TD5 tracks so cars stop popping out at short range. The track walk
     * below is extended by the same factor (kept coupled => no floating). TD6
     * city tracks keep their perf-capped window unchanged. */
    if (g_active_td6_level <= 0) {
        float vmult = td5_render_traffic_view_mult();
        actor_cull_window = (int)((float)actor_cull_window * vmult + 0.5f);
    }
    if (actor_cull_window < 1) actor_cull_window = 1;
    /* [DRAG RACE TRAFFIC 2026-06-30] The drag strip is a short, cheap, dead-straight
     * stadium where ONCOMING traffic must be visible approaching from well ahead —
     * the default ~64-span (32-span split) actor cull pops it in far too late (the
     * dynamic spawner seeds it 50-175 spans out, so it rendered as an invisible
     * collision box). Open the actor cull to cover the whole strip ahead. */
    if (g_td5.drag_race_enabled && actor_cull_window < 260)
        actor_cull_window = 260;

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
     * so without this the render camera stays frozen at its initial position.
     * [Phase B Stage 2b] Skip when the per-pane camera was pre-baked into this
     * g_rs by the serial camera pass (else all panes re-read the same current). */
    if (!s_camera_prebaked)
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
    if (!s_photobooth_active) td5_render_draw_sky();   /* photo booth: chroma bg only */
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

        /* [traffic-view-dist 2026-06-29] Faithful TD5 tracks extend the road walk
         * by the SAME factor as the actor cull above, so traffic stays drawn over
         * real road instead of floating on bare terrain. `cull_window` itself is
         * left unscaled — the TD6 proportional map below reads it, so TD6 city
         * tracks keep their perf-capped walk (and unchanged actor cull). */
        int walk_window = cull_window;
        if (g_active_td6_level <= 0)
            walk_window = (int)((float)cull_window * td5_render_traffic_view_mult() + 0.5f);

        /* start_entry = (center - cullWindow) >> 2  [CONFIRMED @ 0x42BBC9-0x42BBD8:
         * SUB EAX,EDX; CDQ; AND EDX,3; ADD; SAR EDI,2 — the standard signed
         * divide-by-4 idiom `(x + (x>>31 & 3)) >> 2`]. C's `>>` on signed
         * negatives is arithmetic on all targets we ship (gcc/i686, MSVC/x86). */
        int start_entry = (eff_player - walk_window) >> 2;

        /* Drag pulls the cull-start back 0x19 entries (= 100 spans) so the
         * start/staging geometry behind the line stays visible at spawn
         * [CONFIRMED @ 0x42BBDF; drag flag write @ 0x42AD79]. */
        if (g_td5.drag_race_enabled)
            start_entry -= 0x19;

        /* Loop count = effectiveSpans ENTRIES, NOT (window)>>2+1. The port's
         * `((back+fwd)>>2)+1` ≈ 33 entries rendered only ~half the original's
         * depth [CONFIRMED count = effectiveSpans @ 0x42BBE9 `MOV EBP,[0x4aae44]`,
         * loop 0x42BC11-0x42BC3C]. */
        int n_entries   = (g_active_td6_level <= 0)
                          ? (int)((float)eff_spans * td5_render_traffic_view_mult() + 0.5f)
                          : eff_spans;

        /* TD6 track migration: the windowed `entry = span>>2` walk assumes TD5's
         * ~4-spans-per-entry MODELS.DAT layout. TD6 levels have a DIFFERENT and
         * VARIABLE entry density (e.g. ~113 chunk-entries for ~517 spans), so the
         * span>>2 window indexes the wrong entries and drifts off the player.
         *
         * [S22 fix #3: TD6 high frame-ms] Previously we worked around the drift
         * by rendering EVERY display-list entry each frame (~113-129 entries,
         * ~214 meshes) and leaning entirely on the per-mesh frustum cull. That is
         * correct but expensive: with no spatial windowing, TD6 tracks paid a
         * much higher per-frame render cost than TD5 tracks (every entry's meshes
         * sphere-tested every frame). Instead, map the SAME spatial span window
         * the TD5 walk uses ([eff_player - cull_window, eff_player + cull_window]
         * = ±2*eff_spans spans around the player) onto the TD6 display-list
         * PROPORTIONALLY using the real entry count, so the window tracks the
         * player without assuming a fixed spans-per-entry. The per-mesh frustum
         * cull (td5_render_test_mesh_frustum) + dedup set still gate fine
         * visibility; this just bounds how many entries are walked + sphere-tested
         * per frame, dropping TD6 dispatch toward what TD5 tracks pay. Gated on
         * the override knob, so faithful tracks keep the exact span>>2 window. */
        int td6_entries = (g_active_td6_level > 0)
                          ? td5_track_get_models_display_list_count() : 0;
        if (g_active_td6_level > 0 && td6_entries > 0 && ring > 0) {
            int span_lo = eff_player - cull_window;   /* = eff_player - 2*eff_spans */
            int span_hi = eff_player + cull_window;   /* = eff_player + 2*eff_spans */
            /* Proportional span->entry map; floor low / ceil high so the window
             * never under-covers a partially-visible boundary entry. 64-bit
             * intermediates so long tracks can't overflow the multiply. C integer
             * division truncates toward zero for the (rare) negative span_lo near
             * the track start; the per-iteration clamp/wrap below absorbs the
             * <=1-entry difference vs a true floor. */
            long long re = (long long)td6_entries;
            int lo = (int)((span_lo * re) / ring);
            int hi = (int)(((span_hi * re) + (ring - 1)) / ring);   /* ceil */
            if (hi <= lo) hi = lo + 1;
            start_entry = lo;
            n_entries   = hi - lo;
            /* Never walk more than the whole table (worst case == old render-all,
             * never worse) — guards a pathological cull_window vs short track. */
            if (n_entries > td6_entries) n_entries = td6_entries;
        }
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

        /* [S22 fix #3] TD6 wrap (circuit) / clamp (P2P) must use the REAL TD6
         * display-list length, not the TD5 span>>2 estimate (which over-counts
         * for TD6's variable entry density and would wrap/clamp at the wrong
         * boundary). Faithful tracks keep (ring+3)>>2. */
        int ring_entries = (g_active_td6_level > 0 && td6_entries > 0)
                           ? td6_entries
                           : ((ring > 0) ? ((ring + 3) >> 2) : 0);
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
        /* [parallel-build FIX 2026-06-11 threaded-pane flicker] This dedup
         * array was a function-local `static` — invisible to the Stage-1
         * file-scope re-entrancy sweep — so concurrent pane builds shared it:
         * each pane's dup-scan saw OTHER panes' pointers and skipped meshes
         * at random (RENDERSTAT spanmesh ~300 -> ~110, panes flickering).
         * It is per-frame-per-pane state by design -> plain stack local
         * (16KB, fine for both the main thread and the job-pool workers). */
        #define TD5_RENDER_SUBMITTED_CAP 4096
        const void *s_submitted[TD5_RENDER_SUBMITTED_CAP];
        int submitted_count = 0;

        /* [DRAG LENGTHEN] MODELS.DAT entries past the insertion point are baked at
         * the original (now-stale) positions; the procedural ribbon paints there
         * instead. Suppress those entries so the two don't overlap/double. */
        int drag_ins_entry = (g_td5.drag_race_enabled && td5_track_drag_insert_span() >= 0)
                             ? (td5_track_drag_insert_span() >> 2) : -1;

        /* [DRAG FINISH GANTRY] Resolve dl26/sub0 BEFORE the walk so the suppression
         * below (mesh == s_drag_gantry_mesh) has the pointer from the first frame. */
        if (g_td5.drag_race_enabled) td5_render_drag_gantry();

        for (int i = 0; i < n_entries; i++) {
            int entry_idx = start_entry + i;

            if (drag_ins_entry >= 0 && entry_idx >= drag_ins_entry)
                continue;

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

            if (!s_photobooth_active) td5_render_span_display_list(display_list);
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
                if (!s_photobooth_active) td5_render_span_display_list(display_list);
                rendered_spans++;
            }
        }

        /* [CUSTOM TRACK] When the level ships no MODELS.DAT mesh table (e.g. a
         * track built by re/tools/td5_trackgen.py), paint the STRIP collision
         * ribbon as a solid road so the track is visible and drivable. Gated on
         * the absence of a mesh table so faithful TD5/TD6 tracks never pay it.
         *
         * Use a wide, SPEED-INDEPENDENT span window (not cull_window, which is
         * velocity-scaled and collapses to a handful of spans at low speed -> the
         * road vanishes a short way ahead). A circuit draws the whole ring (cheap
         * for the small tracks the converter makes); point-to-point uses a large
         * fixed reach. Behind-camera spans self-cull in the projector. */
        if (!s_photobooth_active && td5_track_get_models_display_list_count() == 0) {
            int ribbon_win = 256;
            if (is_circuit && ring > 0 && (ring / 2 + 1) < ribbon_win)
                ribbon_win = ring / 2 + 1;
            td5_render_fallback_strip_ribbon(eff_player, ribbon_win, ring,
                                             total_spans, is_circuit, 0, 0);
        }
        /* [DRAG LENGTHEN] The inserted/shifted spans (>= insert point) have NO
         * matching MODELS.DAT road (those chunks are baked at the ORIGINAL
         * positions, which no longer line up). Paint the procedural strip ribbon
         * from the insertion point on so the road physically continues to the
         * finish; the section before the insertion keeps its textured MODELS.DAT
         * road + scenery. The ribbon uses the (already-widened) strip vertices. */
        if (!s_photobooth_active && g_td5.drag_race_enabled) {
            int ins = td5_track_drag_insert_span();
            if (ins >= 0) {
                /* Start the ribbon PAST the original straight (~span 240, where
                 * level030's strip curves away). The inserted straight spans ~154..240
                 * reproduce the original road's world positions, so painting the ribbon
                 * there Z-FIGHTS the baked road (alternating light/dark bands). Let the
                 * original textured road show for 154..240; the ribbon covers only the
                 * genuinely-new straight beyond where the original strip curved off. */
                int ribbon_start = 240;
                if (ribbon_start < ins) ribbon_start = ins;
                td5_render_fallback_strip_ribbon(eff_player, 256, ring,
                                                 total_spans, is_circuit, ribbon_start,
                                                 td5_track_drag_tail_end());
            }
        }
        /* [DRAG STADIUM EXTEND] Tile a clean stadium block down the extended
         * straight so it stays enclosed (road+stands) past where the baked
         * scenery ends, then re-render the real finish gantry (dl 26 sub 0),
         * relocated to the real finish span. Drag-only; tiling/gantry no-op when short. */
        if (!s_photobooth_active) {
            td5_render_drag_stadium_extension();
            td5_render_drag_finish_line();
        }
        #undef TD5_RENDER_SUBMITTED_CAP
    }

    /* [2026-06-08 split-screen perf probe] Split the per-view world cost: above
     * is the sky dome + track display-list walk; the actor loop follows. Profiled
     * per pane (fires viewport_count times/frame) so a 9-pane sweep shows whether
     * the render budget is track geometry (→ split view-window reduction) or the
     * actors. Zero cost when [Logging] Profile is off. */
    td5_profile_mark("v_track");

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
                if (s_photobooth_active) continue;  /* booth: no shadows in the preview */
                TD5_Actor *sa = td5_game_get_actor(slot);
                if (!sa || !td5_render_get_vehicle_mesh(slot))
                    continue;
                /* [PER-VIEWPORT TRAFFIC] in split-screen time trial each viewport
                 * renders ONLY its own traffic partition (owner == view_index);
                 * -1 (shared / racer slot) renders in every view as before. */
                { int tov = td5_ai_traffic_slot_owner_vp(slot);
                  if (tov >= 0 && tov != view_index) continue; }
                /* [dynamic-traffic] despawned traffic casts no shadow; a fading
                 * car's shadow fades with it (alpha consumed inside the shadow
                 * draw helpers via s_actor_draw_alpha). */
                int shadow_fade = td5_ai_traffic_get_draw_alpha(slot);
                if (shadow_fade == 0)
                    continue;
                if (slot == camera_target_slot && camera_preset_active)
                    continue;   /* bumper/interior cam: own car (incl. shadow) suppressed */
                /* [#14 2026-06-19] Skip INACTIVE racer slots (state==3) in ALL
                 * modes, not just drag: an unused grid slot whose mesh is still
                 * loaded was casting a shadow (and rendering a body below) as a
                 * stationary "ghost" car when the race had fewer than the max
                 * opponents. Traffic slots (>= base) keep their own fade gate. */
                if (slot < g_traffic_slot_base &&
                    td5_game_get_slot_state(slot) == 3)
                    continue;   /* inactive racer slot (was drag-only) */
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
                td5_render_set_actor_draw_alpha(shadow_fade);
                render_vehicle_shadow_quad(sa);
                td5_render_set_actor_draw_alpha(255);
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
            if (s_photobooth_active && slot != 0) continue;  /* booth: player car only */
            TD5_Actor *actor = td5_game_get_actor(slot);
            /* [POLICE rewrite 2026-06-19] Draw the dedicated police mesh over any
             * cop slot (idle or chasing) so cops read as police cars in traffic,
             * not ordinary cars. VISUAL only — physics/wheels/HUD still use the
             * slot's real mesh. Falls back to the slot mesh if no cop mesh. */
            /* [MP COP CHASE 2026-06-23] The MP cop-chase cop drives a real POLICE
             * CAR (Police Cerbera), so render its OWN mesh — not the generic
             * traffic-encounter cop mesh. Only traffic cops keep s_cop_mesh. */
            int draw_cop_mesh = (s_cop_mesh && td5_ai_actor_is_cop(slot));
            /* [multi-cop 2026-06-24] is_cop is mask-aware: suppress the generic
             * traffic-cop mesh over ANY human cop (each drives a real police car). */
            if (draw_cop_mesh && g_td5.mp_mode_config.mode == TD5_MP_MODE_COP_CHASE &&
                !g_td5.network_active && td5_game_cop_chase_is_cop(slot))
                draw_cop_mesh = 0;
            TD5_MeshHeader *mesh = draw_cop_mesh
                                   ? s_cop_mesh : td5_render_get_vehicle_mesh(slot);
            TD5_Mat3x3 view_rot;
            TD5_Vec3f render_pos;
            float depth;

            if (!actor || !mesh)
                continue;

            /* [PER-VIEWPORT TRAFFIC] each viewport draws only its own traffic
             * partition (owner == view_index); -1 = shared/racer, drawn in all. */
            { int tov = td5_ai_traffic_slot_owner_vp(slot);
              if (tov >= 0 && tov != view_index) continue; }

            /* [dynamic-traffic] despawned traffic is invisible; a spawning /
             * despawning car fades (alpha applied around the mesh dispatch
             * below — 255 for every racer and for dynamic-off, so the classic
             * path is untouched). */
            int actor_fade = td5_ai_traffic_get_draw_alpha(slot);
            if (actor_fade == 0)
                continue;
            /* [TRAFFIC BATTLE 2026-06-28] A wrecked traffic car renders
             * translucent (≈35%, like ghost mode) so it reads as destroyed +
             * intangible — the collision solver also lets the player drive clean
             * through it. Battle only; clamps DOWN so an in-progress fade-out is
             * never brightened. */
            if (slot >= g_traffic_slot_base && td5_game_battle_mode_active() &&
                td5_ai_actor_is_broken_down(slot)) {
                if (actor_fade > 90) actor_fade = 90;
            }

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
            /* [#14 2026-06-19] Skip inactive racer slots (state==3) in ALL modes
             * (see the shadow pre-pass) so empty grid slots don't render as ghost
             * cars; the original drag-only decoration gate is subsumed by this. */
            if (slot < g_traffic_slot_base) {
                int ss = td5_game_get_slot_state(slot);
                if (ss == 3)
                    continue;
            }

            /* [ARCADE] An active power-up makes the CAR itself read the effect:
             * GHOST renders the car translucent (the same look as a time-trial
             * ghost opponent — you pass through it); NITRO/INDESTRUCTIBLE/HAZARD
             * make the car SILHOUETTE glow in the effect colour (applied via the
             * effect tint around the body draw below). Holder-effect glow is
             * racer slots only (only racers can hold power-ups). [FREEZE REWORK
             * 2026-07-04] The freeze-VICTIM glow below is independent of that and
             * applies to ANY slot (racer or traffic — "whatever you crash"), and
             * takes priority since a debuff is more urgent to notice than a buff. */
            uint32_t arc_tint = 0;
            if (td5_arcade_mode_active() && td5_arcade_slot_is_freeze_victim(slot)) {
                float pu = 0.5f + 0.5f * sinf((float)td5_plat_time_ms() * 0.02f + (float)slot);
                uint32_t inten = (uint32_t)(165.0f + 90.0f * pu);   /* 165..255 — strong */
                if (inten > 255u) inten = 255u;
                arc_tint = (inten << 24) | 0x40C0FFu;   /* icy blue — slowed by FREEZE */
            } else if (td5_arcade_mode_active() && slot >= 0 && slot < g_traffic_slot_base) {
                int eff = td5_arcade_active_effect(slot);
                if (eff == TD5_PU_GHOST) {
                    actor_fade = (actor_fade * TT_GHOST_ALPHA) / 255;
                    if (actor_fade < 1) actor_fade = 1;
                } else if (eff != TD5_PU_NONE) {
                    uint32_t kc;
                    switch (eff) {
                    case TD5_PU_NITRO:          kc = 0x20E0FFu; break;   /* cyan  */
                    case TD5_PU_INDESTRUCTIBLE: kc = 0xFF3020u; break;   /* red   */
                    case TD5_PU_HAZARD:         kc = 0xFFB000u; break;   /* amber */
                    default:                    kc = 0xFFFFFFu; break;
                    }
                    float pu = 0.5f + 0.5f * sinf((float)td5_plat_time_ms() * 0.008f + (float)slot);
                    uint32_t inten = (uint32_t)(165.0f + 90.0f * pu);   /* 165..255 — strong */
                    if (inten > 255u) inten = 255u;
                    arc_tint = (inten << 24) | kc;

                    /* [ARCADE NITRO 2026-07-04] Trailing glow-orb speed
                     * effect behind the car while NITRO is active. */
                    if (eff == TD5_PU_NITRO)
                        td5_render_arcade_nitro_trail(actor);
                }
            }

            /* [#R13 ghostdiag 2026-06-19] Pin which slots actually render (the
             * "traffic ghosts" the user still sees with few opponents). The
             * state==3 gate above already drops inactive racers, so anything that
             * logs here is genuinely drawn — its state/fade/is_traffic reveals
             * whether the ghosts are active opponents, lingering traffic, etc.
             * Logs the first view's surviving slots ~every 240 frames -> engine.log. */
            {
                static uint32_t s_gd = 0;
                if (view_index == 0 && slot == 0) s_gd++;
                if (view_index == 0 && (s_gd % 240u) == 0u)
                    TD5_LOG_I(LOG_TAG,
                        "[ghostdiag] slot=%d state=%d fade=%d is_traffic=%d base=%d total=%d",
                        slot, td5_game_get_slot_state(slot), actor_fade,
                        (slot >= g_traffic_slot_base), g_traffic_slot_base, total_actors);
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
                /* [FIX 2026-06-12 traffic-wheel gate drift] racer gate must be
                 * g_traffic_slot_base (see wheel/brake gate below) — with the
                 * 16-slot TD5_MAX_RACER_SLOTS the chassis lift was wrongly
                 * applied to traffic slots 6..11 too, sinking traffic bodies
                 * 36 units into the road (orig traffic block @ 0x40BD20 skips
                 * the lift). */
                if (slot < g_traffic_slot_base) {
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

            /* [CAR DAMAGE 2026-06-28] Feed this slot's accumulated per-vertex
             * model-space deformation to the transform, then clear it right
             * after so ONLY this body deforms (wheels/overlay/track stay clean).
             * Returns 0 / leaves NULL when CarDamage is off or this car is
             * undamaged. */
            {
                const float *ddx = NULL, *ddy = NULL, *ddz = NULL; int dvc = 0;
                if (td5_damage_get_deform(slot, mesh, &ddx, &ddy, &ddz, &dvc)) {
                    s_deform_dx = ddx; s_deform_dy = ddy;
                    s_deform_dz = ddz; s_deform_count = dvc;
                }
            }
            td5_render_transform_mesh_vertices(mesh);
            s_deform_dx = s_deform_dy = s_deform_dz = NULL;
            s_deform_count = 0;
            /* Per-actor track-zone driven 3-light + ambient basis (mirrors
             * ApplyTrackLightingForVehicleSegment @ 0x00430150). Must run
             * BEFORE compute_vertex_lighting since that's the consumer of
             * s_light_dirs[]/s_ambient_intensity. */
            td5_render_apply_track_lighting(slot, actor);
            /* [AUTO LIGHTS] Capture the player's current track-zone brightness as
             * the environment probe, read one frame later by td5_render_env_is_dark()
             * to auto-toggle headlights. Ambient ALONE can't tell a tunnel from an
             * open sunlit road (both sit near the 0x40 ambient floor), so also sum
             * the directional ("sun") budget from the just-applied zone contribs —
             * that collapses to ~0 in a tunnel and is large in daylight. */
            if (slot == 0) {
                s_env_ambient = s_ambient_intensity;
                float direct = 0.0f;
                for (int li = 0; li < 3; li++) {
                    if (!s_tl_contrib[li].enabled) continue;
                    float vx = s_tl_contrib[li].vec_world[0];
                    float vy = s_tl_contrib[li].vec_world[1];
                    float vz = s_tl_contrib[li].vec_world[2];
                    direct += sqrtf(vx * vx + vy * vy + vz * vz);
                }
                s_env_direct = direct;
            }
            /* [DYNAMIC LIGHTS] actor verts are in body space; the model->world
             * basis is the interpolated world pos + the actor's body->world
             * rotation, so dynamic lights are transformed into body space. */
            { float lbo[3] = { render_pos.x, render_pos.y, render_pos.z };
              td5_render_set_light_basis(lbo, actor->rotation_matrix.m); }
            td5_render_compute_vertex_lighting(mesh, slot);

            /* Vehicle shadow is now drawn in the shadow PRE-PASS above (before
             * ANY car body in this view), not inline here. [FIX 2026-06-02
             * inter-actor overlay] Drawing all shadows first and then all bodies
             * makes EVERY opaque body (z_write=1) overwrite EVERY shadow it
             * covers — extending the 2026-06-01 per-actor "shadow before its own
             * body" fix to cover the inter-actor case ("other cars' shadows
             * render over my car") while keeping the player-self fix intact. */

            /* [dynamic-traffic] fade bracket: the setter flushes the pending
             * immediate batch on every change, so faded triangles can never be
             * batched with another actor's. The trailing reset also flushes
             * this car's tail vertices while the fade is still active. */
            td5_render_set_actor_effect_tint(arc_tint);   /* [ARCADE] silhouette glow */
            td5_render_set_actor_draw_alpha(actor_fade);
            /* [task#21] TD6 car body z-fight fix: depth snap + toward-camera pull
             * for the duration of THIS body's draw, so coplanar interior/shell
             * faces of the single de-indexed TD6 mesh stop flickering. Ported-TD6
             * cars only (s_vehicle_is_td6); TD5 cars are byte-unchanged. The
             * prepared-mesh call flushes its own batch, so resetting to 0 right
             * after cannot leak the bias into other geometry. */
            int td6_zfix = (slot >= 0 && slot < TD5_ACTOR_MAX_TOTAL_SLOTS &&
                            s_vehicle_is_td6[slot] && td6_car_zfix_enabled());
            if (td6_zfix) s_td6_car_zbias = TD6_CAR_ZFIX_PULL_VIEWZ;
            td5_render_prepared_mesh(mesh);
            if (td6_zfix) s_td6_car_zbias = 0.0f;
            td5_render_set_actor_draw_alpha(255);
            td5_render_set_actor_effect_tint(0);          /* [ARCADE] clear silhouette glow */

            /* [S23 2026-06-05] UNIFIED vehicle reflection — TD5 cars now match
             * TD6 cars. The chrome/env-map "mode 2" overlay was a PORT-ONLY
             * enhancement: the original RenderRaceActorForView @0x0040c120 ran it
             * for the PLAYER car only (gate `actor_00 == 0`), and the traffic path
             * RenderRaceActorsForView @0x0040bd20 never ran it [CONFIRMED agent
             * 2026-06-05]. The port had extended it to all racer slots, then had
             * to EXCLUDE ported TD6 cars via !s_vehicle_is_td6 because the overlay
             * painted the scrolling environs "lights" texture onto their grayscale
             * bodies (the user's "lights shader over new cars"). The user asked
             * for ONE unified look matching the TD6 cars — which never run the
             * overlay — so it is now disabled for ALL cars and the divergent
             * !s_vehicle_is_td6 gate is removed. The overlay + projection-effect
             * update are kept behind this flag for easy revert; flip to 1 to
             * restore the all-racer chrome (the TD6 grayscale caveat returns).
             * [2026-06-16] flag moved to file scope so the environs loader skips
             * fetching the textures while the overlay is off. */
            if (s_vehicle_reflection_overlay_enabled &&
                s_proj_effect_mode == 2 && slot < TD5_MAX_RACER_SLOTS &&
                !s_photobooth_active) {
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
            /* [FIX 2026-06-12 traffic-wheel gate drift] "Racer slots only"
             * must be slot < g_traffic_slot_base, NOT TD5_MAX_RACER_SLOTS:
             * that constant was 6 when the 2026-06-02 fix was written but the
             * N-way work bumped it to 16, silently re-including traffic slots
             * 6..11 — traffic got wheel billboards built from slot-0 wheel
             * geometry again (user-visible as randomly wrong traffic wheels).
             * g_traffic_slot_base is the racer/traffic boundary in every
             * field layout (6 legacy, 16 big splits). */
            /* [WHEEL OVERHAUL 2026-06-12] Unified wheel renderer for ALL
             * vehicle classes. Racers/police/TD6 cars (slot < base) always get
             * wheels; traffic gets the same procedural wheels when the overhaul
             * + traffic toggle are on (its baked mesh wheels still draw — a
             * converter pass to strip them is the follow-up). Brake lights stay
             * racer-only (faithful — traffic never drew them). */
            int is_racer = (slot < g_traffic_slot_base);
            if (wheel_overhaul_enabled()) {
                if (is_racer || wheel_traffic_enabled())
                    render_vehicle_wheels_unified(actor, slot);
            } else if (is_racer) {
                render_vehicle_wheel_billboards(actor, slot);
            }
            if (is_racer)
                render_vehicle_brake_lights(actor, slot);
            /* [DYNAMIC LIGHTS] visible front headlamp glow (racers). */
            if (is_racer)
                render_vehicle_headlights(actor, slot);

            /* (Vehicle shadow drawn in the per-view shadow pre-pass above.) */

            /* Tracked-actor marker (cop chase strobes) — orig
             * RenderRaceActorForView @ 0x0040c79c gates:
             *   wanted_mode_enabled != 0 &&
             *   g_wantedTargetTrackerActive != 0 &&
             *   slot == g_wantedTargetSlotIndex (=0)
             * The render call lives in td5_render.c:render_tracked_actor_marker
             * (port of 0x0043cde0). Visuals stay inert in non-wanted modes. */
            /* [COP-CHASE 2026-06-21] The red/blue strobe (cop lights) is TOGGLED
             * on/off with the HORN key. Gate it directly on the siren-toggle state
             * (s_siren_user_enabled) so a horn press flips the lights immediately at
             * full intensity — the marker's own phase animation supplies the flash.
             * Horn toggles the lights + siren together (as in the original); the
             * toggle persists across pause/resume. */
            extern int td5_sound_siren_is_enabled(void);
            if (g_td5.wanted_mode_enabled &&
                slot == td5_game_get_wanted_target_slot() &&
                td5_sound_siren_is_enabled()) {
                /* Pass the SAME body transform the mesh used so the strobe stays
                 * welded to the car body. */
                render_tracked_actor_marker(actor, &view_rot, &render_pos, 0x1000);
            }

            /* [POLICE rewrite 2026-06-19] A chasing police cop wears the same
             * red/blue strobe (welded to its body) whenever the POLICE option
             * is on — the chase rewrite's cop identity. Steady full intensity
             * (td5_ai_cop_glow_intensity), independent of the wanted-mode
             * tracker. Runs in this shared per-view actor loop, so it covers
             * both split-screen and single-view automatically. */
            if (g_td5.special_encounter_enabled &&
                td5_ai_cop_is_chasing(slot)) {
                render_tracked_actor_marker(actor, &view_rot, &render_pos,
                                            td5_ai_cop_glow_intensity(slot));
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
            /* [POLICE rewrite 2026-06-19] A broken-down TRAFFIC car or COP smokes
             * from its chassis. Restricted to non-racers (slot >= traffic base):
             * a chased racer / the player is also flagged broken-down to END the
             * chase on a hard crash, but the player should NOT smoke for it (that
             * read as "smoke on my own car"). Only traffic/cops get the smoke. */
            int broken_smoke_ok = !is_racer && td5_ai_actor_is_broken_down(slot);
            /* [MP COP CHASE ARREST SMOKE 2026-06-24] A fully-arrested suspect (its
             * wanted damage drained to 0) smokes prominently — the dense roof-lifted
             * WRECK plume — so the busted car visibly reads as caught. The broken-down
             * path above is restricted to non-racers (!is_racer), so the human suspect
             * racers need their own gate here. MP cop chase only; the cop never hits 0
             * (it takes no ram damage) so it never qualifies. */
            int arrested_smoke_ok = g_td5.wanted_mode_enabled != 0 &&
                                    g_td5.mp_mode_config.mode == TD5_MP_MODE_COP_CHASE &&
                                    !g_td5.network_active &&
                                    slot < TD5_MAX_RACER_SLOTS &&
                                    td5_game_cop_chase_is_suspect(slot) &&
                                    g_wanted_damage_state[slot] <= 0;
            if (!(slot == camera_target_slot && camera_preset_active)) {
                /* Orig 0x40C7A5: SpawnRandomVehicleSmokePuff(actor, slot) —
                 * engine-rev gated random smoke puff. Called per visible
                 * actor per frame, unconditional on wanted-mode (it's the
                 * "labouring engine" puff visible during slow climbs etc.).
                 * Skipped under cinematic-preset for consistency with the
                 * surrounding rear-wheel/wanted-smoke skip.
                 *
                 * [TRAFFIC CRASH SMOKE 2026-06-21] Restrict these two incidental
                 * emitters to RACERS. For TRAFFIC/cops they fired during the
                 * non-fatal crash-spin (slipping wheels / high RPM while the car
                 * tumbled) and then stopped on recovery — the "smoke during the
                 * crash, gone after recovery" the user reported. Per spec a
                 * traffic car smokes ONLY on a FATAL hit, handled by the
                 * broken-down wreck plume just below; a recovering traffic car
                 * now shows nothing. Racer smoke is unchanged. Knob
                 * TD5RE_TRAFFIC_RECOVER_SMOKE=1 restores the old behaviour. */
                if (is_racer || traffic_recover_smoke_enabled()) {
                    td5_vfx_spawn_random_smoke_puff(actor, view_index);
                    td5_vfx_spawn_rear_wheel_smoke(actor, view_index);
                }
                if (arrested_smoke_ok) {
                    /* Busted suspect: dense roof-lifted plume (same as a wreck) so
                     * the arrested car unmistakably reads as caught. */
                    td5_vfx_spawn_wreck_smoke(actor);
                } else if (broken_smoke_ok) {
                    /* [#5 2026-06-20] A wrecked traffic/cop car smokes from its
                     * ROOF (dense, lifted column) so a totalled car clearly reads
                     * as dead — not the chassis-centre exhaust wisp. */
                    td5_vfx_spawn_wreck_smoke(actor);
                } else if (wanted_smoke_ok) {
                    td5_vfx_spawn_smoke(actor);
                }

                /* [CAR DAMAGE 2026-06-28] Tiered damage smoke (light grey ->
                 * black -> black+fire/sparks) for a damaged car, independent of
                 * the cop-chase plumes above. No-op when CarDamage is off or the
                 * car is undamaged; vfx_spawn_damage_smoke respects the 100-slot
                 * per-view particle budget. */
                if (td5_damage_enabled()) {
                    int dtier = td5_damage_smoke_tier(slot);
                    if (dtier > 0)
                        td5_vfx_spawn_damage_smoke(actor, dtier);
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

    /* Focal length. [S01 2026-06-04] Lock the VERTICAL FOV (focal proportional to
     * HEIGHT) instead of the horizontal one. The original ran fixed 4:3, where
     * height*0.75 == width*0.5625 (= 720 at 1280x960) so this is byte-identical at
     * 4:3. On a widescreen / resized window it keeps the same vertical framing
     * (the car and its ground shadow stay in view) and instead widens the
     * horizontal FOV — "Hor+", the expected behavior when the window gets wider.
     * The old width*0.5625 locked the horizontal FOV, so widening the window
     * shrank the vertical FOV and pushed the car's shadow off the bottom. */
    s_focal_length = (float)height * 0.75f;
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

    /* [parallel-build] Bucket entries are consumed at flush time — AFTER the
     * pane's vertex workspace has been reused by later meshes — so copy the
     * primitive's vertices into this entry's fixed arena slot now. (This also
     * fixes the latent shared-blob hazard: a queued prim whose blob verts get
     * re-transformed by a later draw before the flush.) Flags 0x3/0x4 (and
     * the 0x8000000x variants) = 3/4 contiguous TD5_MeshVertex; the raw-N>4
     * path has no live callers and passes through with original semantics. */
    {
        uint32_t f  = flags & 0x7FFFFFFFu;
        int      nv = (f == 0x3u) ? 3 : (f == 0x4u) ? 4 : (int)(flags & 0x7Fu);
        if (nv >= 3 && nv <= 4) {
            TD5_MeshVertex *copy = &g_rs->prim_copy[(size_t)idx * 4];
            memcpy(copy, entry, (size_t)nv * sizeof(TD5_MeshVertex));
            entry = copy;
        }
    }

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
/* s_in_reflection_overlay moved to RenderScratch (Phase B Stage 1). */

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
void td5_render_apply_page_blend_preset(int page_id)
{
    int t = td5_asset_get_page_transparency(page_id);
    TD5_RenderPreset p;
    if (s_in_reflection_overlay) return; /* preserve caller's ADDITIVE preset */
    if (s_in_sky_draw)           return; /* preserve caller's SKY preset */
    if (t == 3)      p = TD5_PRESET_ADDITIVE;
    else if (t == 2) p = TD5_PRESET_TRANSLUCENT_ANISO;
    else             p = TD5_PRESET_OPAQUE_LINEAR;
    /* [dynamic-traffic] A fading car body must alpha-blend regardless of page
     * type. Additive pages keep ONE/ONE (the fade is folded into their RGB by
     * the flush fixup); opaque/color-key pages remap to the fade preset whose
     * alpha_ref=1 won't clip sub-50% fade alphas like ANISO's 0x80 would. */
    if (s_actor_draw_alpha < 255 && p != TD5_PRESET_ADDITIVE)
        p = TD5_PRESET_VEHICLE_FADE;
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

    /* [Phase B render-transform] Recording (worker building a pane list): touch
     * NO shared GPU state. Track the per-pane current page (so flush_immediate's
     * bind records correctly) and record the bind intent; the cache lookup + GPU
     * bind + blend preset are resolved on the serial replay. */
    if (td5_rcmd_recording()) {
        s_previous_texture_page = s_current_texture_page;
        s_current_texture_page  = page_id;
        td5_rcmd_bind_page(page_id);
        return 1;
    }

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

    /* [parallel-build] proj_u/proj_v writes target the pane workspace copy
     * (mode 3 also READS view_x/view_y, which only exist there).
     * [LIGHT2 P1] mask the derived-normals tag (bit 0). */
    verts      = rs_vtx_rebase((void *)(uintptr_t)mesh->vertices_offset);
    normals    = (TD5_VertexNormal *)((uintptr_t)mesh->normals_offset & ~(uintptr_t)1);
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
        const float *mv  = s_render_transform.m;
        const float *cam = s_camera_basis;
        /* Anchor in view space: TransformVector3ByBasis(DAT_004aafe0, slot.anchor - camera_world).
         * The port's s_camera_basis mirrors DAT_004aafe0 (camera world-to-view rotation).
         * Use the PER-PANE baked camera pos (s_camera_pos) not the shared g_cameraPos. */
        float dx = pe->anchor_x - s_camera_pos[0];
        float dy = pe->anchor_y - s_camera_pos[1];
        float dz = pe->anchor_z - s_camera_pos[2];
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

    /* [2026-06-16] Only fetch the environs REFLECTION textures when the chrome
     * overlay that consumes them is enabled — it's disabled (unified car look),
     * so skip the load entirely (the textures were loaded-but-never-drawn). The
     * per-span light-zone setup above (s_environs_level / s_actor_light_zone) is
     * a SEPARATE system and stays unconditional. */
    if (s_vehicle_reflection_overlay_enabled) {
        s_envmap_page_count = td5_asset_load_environs_pages(
            level_number, ENVMAP_TEXTURE_PAGE_BASE, ENVMAP_MAX_PAGES, s_envmap_pages);
    } else {
        s_envmap_page_count = 0;
    }

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
