/**
 * td5_light.c -- Dynamic light system (foundation) + vehicle headlight emitter
 *
 * See td5_light.h for the RE basis and faithfulness notes. In short: the
 * original engine has only directional lights + scalar ambient and NO point
 * lights or headlights; this is a port-only extension that registers world-space
 * positional lights and lets the existing per-vertex lighting pass accumulate
 * their attenuated contribution (the renderer-side consumer lives in
 * td5_render_compute_vertex_lighting).
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_types.h"                          /* g_traffic_slot_base, TD5_Mat3x3 */
#include "../../../re/include/td5_actor_struct.h" /* full TD5_Actor (world_pos, rotation_matrix) */
#include "td5_race_state.h"  /* [LAYERING 2026-07-06] read-only race queries (was td5_game.h) */
#include "td5_light.h"
#include "td5_platform.h"
#include "td5_track.h"       /* td5_track_probe_height — static per-lamp floor Y */
#include "td5_rt.h"          /* [RT2 P7] td5_rt_active() — HIGH street-lamp default */

#define LOG_TAG "render"   /* routes to engine.log */

/* ---- Registry ---------------------------------------------------------- */
static TD5_DynLight s_lights[TD5_LIGHT_MAX];
static int          s_light_count   = 0;
static int          s_enabled       = 1;   /* master enable (set from INI at boot) */
static int          s_headlights    = 1;   /* manual headlight emitter enable */
static int          s_auto          = 1;   /* auto-enable in poorly-lit environments */
static int          s_env_dark      = 0;   /* set per-frame by the render env-brightness probe (street lamps + single-value fallback) */
/* Per-actor-slot env-dark verdict — split-screen players can be in different
 * lighting simultaneously, so vehicle headlight emission follows THIS array,
 * not the single s_env_dark above (see td5_light_set_env_dark_for_slot). */
static int          s_env_dark_slot[TD5_ACTOR_MAX_TOTAL_SLOTS];

void td5_light_set_enabled(int on)      { s_enabled = on ? 1 : 0; }
int  td5_light_enabled(void)            { return s_enabled; }
void td5_light_set_headlights(int on)   { s_headlights = on ? 1 : 0; }
int  td5_light_headlights_enabled(void) { return s_headlights; }
void td5_light_set_auto(int on)         { s_auto = on ? 1 : 0; }
int  td5_light_auto(void)               { return s_auto; }
void td5_light_set_env_dark(int dark)   { s_env_dark = dark ? 1 : 0; }
void td5_light_set_env_dark_for_slot(int slot, int dark)
{
    if (slot < 0 || slot >= TD5_ACTOR_MAX_TOTAL_SLOTS) return;
    s_env_dark_slot[slot] = dark ? 1 : 0;
}

/* Effective headlight emission for one actor slot: in AUTO mode follow that
 * slot's OWN environment-dark verdict; otherwise (manual mode) follow the
 * global manual headlight toggle, same for every car. */
static int td5_light_headlights_active_for_slot(int slot)
{
    if (!s_enabled) return 0;
    if (!s_auto) return s_headlights;
    if (slot < 0 || slot >= TD5_ACTOR_MAX_TOTAL_SLOTS) return s_env_dark;
    return s_env_dark_slot[slot];
}

void td5_light_begin_frame(void)
{
    s_light_count = 0;
}

void td5_light_add_point(float x, float y, float z,
                         float range, float intensity,
                         float r, float g, float b)
{
    if (!s_enabled) return;
    if (range <= 0.0f || intensity <= 0.0f) return;
    if (s_light_count >= TD5_LIGHT_MAX) return;

    TD5_DynLight *L = &s_lights[s_light_count++];
    L->x = x; L->y = y; L->z = z;
    L->range = range;
    L->intensity = intensity;
    L->r = r; L->g = g; L->b = b;
    L->dx = L->dy = L->dz = 0.0f; L->cone = -1.0f;   /* omni */
}

void td5_light_add_spot(float x, float y, float z,
                        float dx, float dy, float dz,
                        float range, float intensity, float cone_cos,
                        float r, float g, float b)
{
    if (!s_enabled) return;
    if (range <= 0.0f || intensity <= 0.0f) return;
    if (s_light_count >= TD5_LIGHT_MAX) return;

    float len = (float)sqrt((double)(dx*dx + dy*dy + dz*dz));
    if (len < 1e-6f) { td5_light_add_point(x, y, z, range, intensity, r, g, b); return; }
    float inv = 1.0f / len;

    TD5_DynLight *L = &s_lights[s_light_count++];
    L->x = x; L->y = y; L->z = z;
    L->range = range;
    L->intensity = intensity;
    L->r = r; L->g = g; L->b = b;
    L->dx = dx*inv; L->dy = dy*inv; L->dz = dz*inv;
    L->cone = cone_cos;
}

int td5_light_count(void) { return s_enabled ? s_light_count : 0; }

const TD5_DynLight *td5_light_get(int i)
{
    if (!s_enabled || i < 0 || i >= s_light_count) return NULL;
    return &s_lights[i];
}

const TD5_DynLight *td5_light_list(int *count)
{
    if (!s_enabled || s_light_count <= 0) { if (count) *count = 0; return NULL; }
    if (count) *count = s_light_count;
    return s_lights;
}

/* ---- Tunable headlight geometry (env knobs, read once) -----------------
 * All offsets are in float world units (/256 of the 24.8 world space). The car
 * body's chassis-to-contact lift is ~36 world units and a taillight billboard
 * half-extent is ~40, so a car spans a few hundred world units — the defaults
 * below mount the lamps a couple of car-lengths ahead with a wide range so the
 * pool of light reads as headlights sweeping the road. */
static int   s_knobs_read = 0;
static float s_hl_range     = 11000.0f; /* TD5RE_HEADLIGHT_RANGE  beam reach (world units) */
static float s_hl_intensity = 0.95f;    /* TD5RE_HEADLIGHT_INTENSITY  peak added light 0..1 */
static float s_hl_fwd       = 400.0f;   /* TD5RE_HEADLIGHT_FWD   lamp forward offset (to the front bumper) */
static float s_hl_up        = 60.0f;    /* TD5RE_HEADLIGHT_UP    raise lamp off the road (world +Y is DOWN, so subtract) */
static float s_hl_cone      = 32.0f;    /* TD5RE_HEADLIGHT_CONE  beam half-angle (degrees) */
static float s_hl_tilt      = 0.40f;    /* TD5RE_HEADLIGHT_TILT  downward beam tilt (fraction of forward) */
static float s_hl_fwd_sign  = 0.0f;     /* TD5RE_HEADLIGHT_FWD_SIGN  0 = auto (from car's rear-lamp Z); +/-1 forces it */
static int   s_hl_traffic   = 0;        /* TD5RE_HEADLIGHT_TRAFFIC   1 = traffic cars also get headlights */

static float env_f(const char *name, float def)
{
    const char *e = getenv(name);
    if (!e || !e[0]) return def;
    float v = (float)atof(e);
    return v;
}

static void read_knobs_once(void)
{
    if (s_knobs_read) return;
    s_knobs_read   = 1;
    s_hl_range     = env_f("TD5RE_HEADLIGHT_RANGE",     s_hl_range);
    s_hl_intensity = env_f("TD5RE_HEADLIGHT_INTENSITY", s_hl_intensity);
    s_hl_fwd       = env_f("TD5RE_HEADLIGHT_FWD",       s_hl_fwd);
    s_hl_up        = env_f("TD5RE_HEADLIGHT_UP",        s_hl_up);
    s_hl_cone      = env_f("TD5RE_HEADLIGHT_CONE",      s_hl_cone);
    s_hl_tilt      = env_f("TD5RE_HEADLIGHT_TILT",      s_hl_tilt);
    s_hl_fwd_sign  = env_f("TD5RE_HEADLIGHT_FWD_SIGN",  s_hl_fwd_sign);
    {
        const char *e = getenv("TD5RE_HEADLIGHT_TRAFFIC");
        s_hl_traffic = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    TD5_LOG_I(LOG_TAG,
              "headlights(deferred): range=%.0f intensity=%.2f fwd=%.0f up=%.0f cone=%.0fdeg tilt=%.2f fwd_sign=%.0f traffic=%d",
              (double)s_hl_range, (double)s_hl_intensity, (double)s_hl_fwd,
              (double)s_hl_up, (double)s_hl_cone, (double)s_hl_tilt,
              (double)s_hl_fwd_sign, s_hl_traffic);
}

/* Transform a body-space offset through the actor's body->world rotation matrix
 * (row-major: world.i = sum_j m[3*i+j] * offset_j), matching the convention used
 * by tl_commit_to_render_globals / the per-actor mesh transform in td5_render.c. */
static void body_to_world(const float *m, float ox, float oy, float oz,
                          float *wx, float *wy, float *wz)
{
    *wx = ox * m[0] + oy * m[1] + oz * m[2];
    *wy = ox * m[3] + oy * m[4] + oz * m[5];
    *wz = ox * m[6] + oy * m[7] + oz * m[8];
}

void td5_light_emit_vehicle_headlights(void)
{
    if (!s_enabled) return;
    read_knobs_once();

    int total = td5_game_get_total_actor_count();
    if (total <= 0) return;

    int emitted = 0;
    for (int slot = 0; slot < total; slot++) {
        /* Racers always; traffic only when the knob is set. */
        if (slot >= g_traffic_slot_base && !s_hl_traffic) continue;
        /* [AUTO LIGHTS] Per-slot verdict — split-screen players can be in
         * different lighting at once, so each car's headlights follow only
         * its OWN zone/manual state, never a sibling viewport's. */
        if (!td5_light_headlights_active_for_slot(slot)) continue;

        TD5_Actor *a = td5_game_get_actor(slot);
        if (!a) continue;

        /* World centre in float world units (render_pos space = world_pos/256). */
        float cx = (float)a->world_pos.x * (1.0f / 256.0f);
        float cy = (float)a->world_pos.y * (1.0f / 256.0f);
        float cz = (float)a->world_pos.z * (1.0f / 256.0f);

        const float *m = a->rotation_matrix.m;

        /* Derive the car's forward axis from its REAR taillight hardpoint (the
         * same model-space int16[3] the brake-light renderer reads at
         * car_def+0x60). The front is the opposite Z, so no forward-axis guess is
         * needed. hp gives the lamp height + lateral spacing too. Falls back to a
         * synthetic mount if car_def / the hardpoint is unavailable. */
        const uint8_t *ap = (const uint8_t *)a;
        void *car_def = NULL;
        memcpy(&car_def, &((const TD5_Actor *)ap)->car_definition_ptr, sizeof(void *));

        int16_t hp0[3] = { 150, 0, 200 }, hp1[3] = { -150, 0, 200 };
        if (car_def) {
            memcpy(hp0, (const uint8_t *)car_def + 0x60, 6);   /* left rear lamp  */
            memcpy(hp1, (const uint8_t *)car_def + 0x68, 6);   /* right rear lamp */
        }

        /* forward sign: opposite the rear-lamp Z (env override wins if != 0). */
        float fsign;
        if (s_hl_fwd_sign > 0.5f || s_hl_fwd_sign < -0.5f) {
            fsign = (s_hl_fwd_sign > 0.0f) ? 1.0f : -1.0f;
        } else {
            float rear_z = 0.5f * ((float)hp0[2] + (float)hp1[2]);
            fsign = (rear_z >= 0.0f) ? -1.0f : 1.0f;
        }

        /* World forward axis (body +Z * front-sign), for the beam direction. */
        float ffx, ffy, ffz;
        body_to_world(m, 0.0f, 0.0f, fsign, &ffx, &ffy, &ffz);
        /* Beam dir = forward, tilted DOWN toward the road (+Y is world down). */
        float bdx = ffx, bdy = ffy + s_hl_tilt, bdz = ffz;

        float cone_cos = (float)cos((double)s_hl_cone * 3.14159265358979 / 180.0);

        /* Two spot lamps at the car's front lamp positions (lateral + height from
         * the car's own hardpoints), each casting a forward+down cone. */
        const int16_t *hp[2] = { hp0, hp1 };
        for (int lamp = 0; lamp < 2; lamp++) {
            float ox = (float)hp[lamp][0];                 /* lateral (X = right)   */
            float oy = (float)hp[lamp][1] - s_hl_up;       /* raise off road (up=-Y) */
            float oz = fsign * s_hl_fwd;                   /* mount at the front bumper */
            float wx, wy, wz;
            body_to_world(m, ox, oy, oz, &wx, &wy, &wz);
            td5_light_add_spot(cx + wx, cy + wy, cz + wz,
                               bdx, bdy, bdz,
                               s_hl_range, s_hl_intensity, cone_cos,
                               1.0f, 1.0f, 1.0f);   /* white headlights */
        }
        emitted++;
    }

    /* One-time confirmation that the emitter is actually feeding the registry. */
    static int s_logged = 0;
    if (!s_logged && emitted > 0) {
        TD5_LOG_I(LOG_TAG, "headlight emitter: %d car(s) -> %d dynamic light(s)",
                  emitted, s_light_count);
        s_logged = 1;
    }
}

/* ---- Street lamps (static world lights) -------------------------------- */

#define TD5_LAMP_MAX 4096

static float   s_lamp_pos[TD5_LAMP_MAX][3];
static uint8_t s_lamp_tunnel[TD5_LAMP_MAX];  /* 1 = tunnel wall lamp: cool-white, tighter range, always-on (bypasses s_env_dark) */
/* [STATIC LAMP Y 2026-08-12] Resolved-once emit Y for a tunnel lamp (see the
 * TD5RE_TUNNEL_LAMP_DROP note below). A tunnel lamp is authored up on the
 * wall/ceiling but must POOL ON THE ROAD, so its emit Y is dropped toward the
 * floor. That drop used to be recomputed every frame against the PLAYER's Y,
 * which made every lamp's height track the car: the pools read as a cluster
 * hugging the player instead of fixtures fixed in the world, and lamps further
 * along a gradient were dragged to the player's elevation rather than to the
 * floor beneath themselves. Now the floor is probed ONCE per lamp, at the
 * lamp's OWN x/z, and cached — so the emitter is world-static. Same
 * resolve-once-and-cache shape as TD6 props' ground_y/ground_done. */
static float   s_lamp_emit_y[TD5_LAMP_MAX];
static uint8_t s_lamp_emit_done[TD5_LAMP_MAX];
static int   s_lamp_count     = 0;
static int   s_street_lights  = 0;   /* OFF by default: parked pending a lamp look-dev session */

void td5_light_lamps_reset(void)          { s_lamp_count = 0; }
int  td5_light_lamps_count(void)          { return s_lamp_count; }
void td5_light_set_street_lights(int on)  { s_street_lights = on ? 1 : 0; }
int  td5_light_street_lights(void)        { return s_street_lights; }

void td5_light_lamps_add(float x, float y, float z)
{
    if (s_lamp_count >= TD5_LAMP_MAX) return;
    s_lamp_pos[s_lamp_count][0] = x;
    s_lamp_pos[s_lamp_count][1] = y;
    s_lamp_pos[s_lamp_count][2] = z;
    s_lamp_tunnel[s_lamp_count] = 0;
    s_lamp_emit_y[s_lamp_count]    = y;   /* until resolved, emit where authored */
    s_lamp_emit_done[s_lamp_count] = 0;
    s_lamp_count++;
}

static void td5_light_lamps_capture_impl(float x, float y, float z, int tunnel)
{
    /* Dedupe: the same halo is captured every frame it is on screen, and the
     * big soft glows are assembled from 2-4 quadrant quads whose centers sit
     * a few hundred units apart — 650 merges a fixture's pieces while staying
     * under the closest real post spacing (~1400). */
    for (int i = 0; i < s_lamp_count; i++) {
        float dx = s_lamp_pos[i][0] - x;
        float dy = s_lamp_pos[i][1] - y;
        float dz = s_lamp_pos[i][2] - z;
        if (dx * dx + dy * dy + dz * dz < 650.0f * 650.0f)
            return;
    }
    int idx = s_lamp_count;
    td5_light_lamps_add(x, y, z);
    if (s_lamp_count > idx)              /* add() may no-op at the cap */
        s_lamp_tunnel[idx] = tunnel ? 1 : 0;
    static int s_cap_log = 0;
    if (s_cap_log < 8) {
        s_cap_log++;
        TD5_LOG_I(LOG_TAG, "lamp capture[%d]%s: (%.0f,%.0f,%.0f)",
                  s_lamp_count - 1, tunnel ? " tunnel" : "", x, y, z);
    }
}

void td5_light_lamps_capture(float x, float y, float z)
{
    td5_light_lamps_capture_impl(x, y, z, 0);
}

/* Tunnel wall "fake-light" patches (e.g. Keswick pages 152/153) → real
 * cool-white point lights. Same shared registry as street lamps, but tagged so
 * the emitter gives them a tighter, cool tint and skips the dark-env gate. */
void td5_light_lamps_capture_tunnel(float x, float y, float z)
{
    td5_light_lamps_capture_impl(x, y, z, 1);
}

/* ---- Content-classified glow pages (per level) --------------------------
 * Generated offline from the extracted texture pages: a page whose image is a
 * bright radial blob with smooth falloff IS a lamp glow (Moscow: 474 post-tip
 * halo + 253..256 big-glow quadrants, eyeball-verified). The capture gate in
 * clip_and_submit_polygon consults this — texture CONTENT is the only signal
 * that survived six rounds of geometry/class heuristics. */
#include "td5_lamp_pages.inc"

static const int *s_halo_pages      = NULL;
static int        s_halo_page_count = 0;

void td5_light_lamps_set_level(int level)
{
    s_halo_pages = NULL;
    s_halo_page_count = 0;
    for (size_t i = 0; i < sizeof(k_lamp_page_table) / sizeof(k_lamp_page_table[0]); i++) {
        if (k_lamp_page_table[i].level == level) {
            s_halo_pages = k_lamp_page_table[i].pages;
            s_halo_page_count = k_lamp_page_table[i].count;
            break;
        }
    }
    TD5_LOG_I(LOG_TAG, "street lamps: level %d has %d classified glow pages",
              level, s_halo_page_count);
}

int td5_light_lamp_page_is_halo(int page)
{
    for (int i = 0; i < s_halo_page_count; i++)
        if (s_halo_pages[i] == page) return 1;
    return 0;
}

/* Lamp look knobs (env, read once): warm sodium-ish pools. */
static int   s_lamp_knobs_read = 0;
static float s_lamp_range      = 2400.0f;  /* TD5RE_LAMP_RANGE      pool radius (world units) */
static float s_lamp_intensity  = 1.00f;    /* TD5RE_LAMP_INTENSITY  peak added light 0..1     */
static int   s_lamp_budget     = 10;       /* TD5RE_LAMP_COUNT      nearest-N promoted/frame  */
static float s_tlamp_range     = 3500.0f;  /* TD5RE_TUNNEL_LAMP_RANGE  fills the tunnel corridor (1200 = tight pools hugging the car) */
static float s_tlamp_intensity = 1.35f;    /* TD5RE_TUNNEL_LAMP_INTENSITY  peak added light (raised so the road/car read) */
/* TD5RE_TUNNEL_LAMP_DROP: the wall/ceiling patches sit ~1600 units ABOVE the road,
 * so a point light left there barely reaches the floor or a passing car (falloff
 * (1-d/range)^2 collapses at that distance). Move each tunnel lamp DOWN toward the
 * road by this fraction of (lamp_y -> floor_y), so the warm pool lands ON the road
 * and lights cars under it. 0 = keep at the ceiling; 1 = at road level.
 * Default 0.6 (mid-tunnel, biased low).
 * floor_y is the TRACK SURFACE under the lamp's own x/z, probed once and cached
 * (s_lamp_emit_y) — NOT the player's Y, which would make the lamp follow the car. */
static float s_tlamp_drop      = 0.60f;
/* TD5RE_TUNNEL_LAMP_STATIC_Y: 1 (default) = resolve each tunnel lamp's emit Y once
 * from the track floor beneath it, so the light is fixed in the world. 0 = the old
 * per-frame drop toward the player's Y (kept as an A/B escape hatch; that path is
 * what made the lamps appear to spawn on top of the car). */
static int   s_tlamp_static_y  = 1;
/* TD5RE_TUNNEL_LAMP_COUNT: cap on how many tunnel wall lamps promote to real
 * point lights per frame (nearest-first). Measured cost on a fast GPU is ~0 ms
 * for 8 lamps, so the DEFAULT is 8 (light a stretch of the tunnel ahead, not
 * just the nearest few hugging the camera). LOWER it (e.g. 3) as a perf lever on
 * a GPU-bound card. Independent of the street-lamp TD5RE_LAMP_COUNT budget. */
static int   s_tlamp_budget    = 8;
/* Tunnel lamp colour — WARM tungsten (env-tunable while art-directing). */
static float s_tlamp_r         = 1.00f;    /* TD5RE_TUNNEL_LAMP_R */
static float s_tlamp_g         = 0.85f;    /* TD5RE_TUNNEL_LAMP_G */
static float s_tlamp_b         = 0.62f;    /* TD5RE_TUNNEL_LAMP_B */

void td5_light_emit_street_lamps(void)
{
    /* [RT2 P7] HIGH default-ON: with ray-traced lighting the city lamp pools get
     * real occlusion (a car under a lamp casts a lamp shadow), so street lamps
     * default ON in HIGH. LOW keeps s_street_lights (default OFF) untouched —
     * no lamps enter the registry in LOW, so the LOW light system is byte-
     * identical. (The dark-only + nearest-N budget gates below still apply.) */
    int want_lamps = s_street_lights || td5_rt_active();
    if (!s_enabled || !want_lamps || s_lamp_count <= 0) return;
    /* Street lamps follow the auto-headlight verdict: on in rain/dusk/dark
     * zones, off in bright daylight. Tunnel lamps live INSIDE a tunnel, so they
     * ignore the sky-based dark probe — if any are registered we keep going and
     * emit only those when the env reads bright. */
    int any_tunnel = 0;
    for (int i = 0; i < s_lamp_count; i++) if (s_lamp_tunnel[i]) { any_tunnel = 1; break; }
    if (!s_env_dark && !any_tunnel) return;

    if (!s_lamp_knobs_read) {
        s_lamp_knobs_read = 1;
        s_lamp_range      = env_f("TD5RE_LAMP_RANGE",     s_lamp_range);
        s_lamp_intensity  = env_f("TD5RE_LAMP_INTENSITY", s_lamp_intensity);
        s_tlamp_range     = env_f("TD5RE_TUNNEL_LAMP_RANGE",     s_tlamp_range);
        s_tlamp_intensity = env_f("TD5RE_TUNNEL_LAMP_INTENSITY", s_tlamp_intensity);
        {
            const char *e = getenv("TD5RE_LAMP_COUNT");
            if (e && e[0]) { int v = atoi(e); if (v >= 0 && v <= TD5_LIGHT_MAX) s_lamp_budget = v; }
        }
        {
            const char *e = getenv("TD5RE_TUNNEL_LAMP_COUNT");
            if (e && e[0]) { int v = atoi(e); if (v >= 0 && v <= TD5_LIGHT_MAX) s_tlamp_budget = v; }
        }
        s_tlamp_r = env_f("TD5RE_TUNNEL_LAMP_R", s_tlamp_r);
        s_tlamp_g = env_f("TD5RE_TUNNEL_LAMP_G", s_tlamp_g);
        s_tlamp_b = env_f("TD5RE_TUNNEL_LAMP_B", s_tlamp_b);
        s_tlamp_drop = env_f("TD5RE_TUNNEL_LAMP_DROP", s_tlamp_drop);
        if (s_tlamp_drop < 0.0f) s_tlamp_drop = 0.0f; else if (s_tlamp_drop > 1.0f) s_tlamp_drop = 1.0f;
        {
            const char *e = getenv("TD5RE_TUNNEL_LAMP_STATIC_Y");
            if (e && e[0] == '0') s_tlamp_static_y = 0;
        }
        TD5_LOG_I(LOG_TAG, "street lamps: %d registered, range=%.0f intensity=%.2f budget=%d tunnel_budget=%d",
                  s_lamp_count, (double)s_lamp_range, (double)s_lamp_intensity, s_lamp_budget, s_tlamp_budget);
    }
    if (s_lamp_budget <= 0 || s_lamp_intensity <= 0.0f) return;

    /* Reference point: player slot 0 (lights are shared across panes). */
    TD5_Actor *p = td5_game_get_actor(0);
    if (!p) return;
    float px = (float)p->world_pos.x * (1.0f / 256.0f);
    float py = (float)p->world_pos.y * (1.0f / 256.0f);
    float pz = (float)p->world_pos.z * (1.0f / 256.0f);

    /* Nearest-N selection (insertion into a small sorted list — lamp counts
     * are a few hundred, budget ~10; runs once per frame). FULL 3D distance:
     * Moscow's riverside quay glows sit ~3000 units BELOW the road — with
     * XZ-only ranking they hogged every budget slot while lighting nothing
     * the player can see (the 'only one lit post on the map' bug). In 3D
     * they rank beyond the road-level post halos and lose. */
    int   best_idx[TD5_LIGHT_MAX];
    float best_d2[TD5_LIGHT_MAX];
    int   nbest = 0;
    float cutoff2 = (s_lamp_range * 6.0f) * (s_lamp_range * 6.0f);
    for (int i = 0; i < s_lamp_count; i++) {
        /* In bright daylight only tunnel lamps emit (street lamps gated off);
         * don't let daylight street lamps consume the nearest-N budget. */
        if (!s_env_dark && !s_lamp_tunnel[i]) continue;
        float dx = s_lamp_pos[i][0] - px;
        float dy = s_lamp_pos[i][1] - py;
        float dz = s_lamp_pos[i][2] - pz;
        /* Vertical gap alone beyond the light's range => it can never touch
         * the player's plane (quay glows 3000 below the embankment road). */
        if (dy > s_lamp_range * 1.25f || dy < -s_lamp_range * 1.25f) continue;
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > cutoff2) continue;
        int j = nbest;
        if (nbest < s_lamp_budget) nbest++;
        else if (d2 >= best_d2[nbest - 1]) continue;
        else j = nbest - 1;
        while (j > 0 && best_d2[j - 1] > d2) {
            best_d2[j] = best_d2[j - 1]; best_idx[j] = best_idx[j - 1]; j--;
        }
        best_d2[j] = d2; best_idx[j] = i;
    }

    int tunnel_emitted = 0;   /* SAFETY CAP: nearest s_tlamp_budget tunnel lamps only */
    for (int k = 0; k < nbest; k++) {
        int li = best_idx[k];
        const float *L = s_lamp_pos[li];
        if (s_lamp_tunnel[li]) {
            /* best_idx is distance-sorted, so this keeps the NEAREST N tunnel
             * lamps and drops farther ones (per-light RT-pass work headroom). */
            if (tunnel_emitted >= s_tlamp_budget) continue;
            tunnel_emitted++;
            /* Drop the emitter DOWN toward the FLOOR so the pool lands on the road
             * and lights cars under it — the captured patch sits up on the
             * wall/ceiling where falloff starves the road. The floor is probed at
             * the LAMP's own x/z and cached, so the lamp stays put in the world as
             * the player drives past (and under it) rather than riding the car's Y. */
            float ly;
            if (s_tlamp_static_y) {
                if (!s_lamp_emit_done[li]) {
                    int gy = 0, gst = 0;
                    /* Seed the span walk with the player's span: the probe steps
                     * outward from there, and a tunnel lamp inside the nearest-N
                     * set is by definition near the player's stretch of road. */
                    float floor_y = td5_track_probe_height(
                                        (int)(L[0] * 256.0f), (int)(L[2] * 256.0f),
                                        (int)p->track_span_raw, &gy, &gst)
                                    ? (float)gy * (1.0f / 256.0f)
                                    : py;   /* off-mesh: fall back to the old reference */
                    s_lamp_emit_y[li]    = L[1] + s_tlamp_drop * (floor_y - L[1]);
                    s_lamp_emit_done[li] = 1;
                }
                ly = s_lamp_emit_y[li];
            } else {
                ly = L[1] + s_tlamp_drop * (py - L[1]);   /* legacy A/B path */
            }
            /* Warm tungsten fixture, tighter pool — reads as a tunnel lamp. */
            td5_light_add_point(L[0], ly, L[2],
                                s_tlamp_range, s_tlamp_intensity,
                                s_tlamp_r, s_tlamp_g, s_tlamp_b);
        } else {
            /* Warm sodium-vapor tint. */
            td5_light_add_point(L[0], L[1], L[2],
                                s_lamp_range, s_lamp_intensity,
                                1.0f, 0.82f, 0.55f);
        }
    }

    static int s_lamp_logged = 0;
    if (!s_lamp_logged && nbest > 0) {
        TD5_LOG_I(LOG_TAG, "street lamps: emitting %d/%d nearest (registry now %d lights)",
                  nbest, s_lamp_count, s_light_count);
        s_lamp_logged = 1;
    }

    /* TD5RE_LAMP_LOG=1: periodic dump of the player position + the nearest
     * emitted lamps (world coords + distance) — position/emission debugging. */
    static int s_lamp_dbg = -1;
    if (s_lamp_dbg < 0) { const char *e = getenv("TD5RE_LAMP_LOG"); s_lamp_dbg = (e && e[0] && e[0] != '0') ? 1 : 0; }
    if (s_lamp_dbg) {
        static int s_tick = 0;
        if ((s_tick++ % 90) == 0) {
            float py = (float)p->world_pos.y * (1.0f / 256.0f);
            /* global nearest lamp, ignoring the cutoff — position debugging */
            int   gi = -1;
            float gd2 = 1e30f;
            for (int i = 0; i < s_lamp_count; i++) {
                float dx = s_lamp_pos[i][0] - px;
                float dz = s_lamp_pos[i][2] - pz;
                float d2 = dx * dx + dz * dz;
                if (d2 < gd2) { gd2 = d2; gi = i; }
            }
            TD5_LOG_I(LOG_TAG, "lamp dbg: player=(%.0f,%.0f,%.0f) emitting=%d "
                      "global-nearest=%d at (%.0f,%.0f,%.0f) dxz=%.0f",
                      px, py, pz, nbest, gi,
                      gi >= 0 ? s_lamp_pos[gi][0] : 0.0f,
                      gi >= 0 ? s_lamp_pos[gi][1] : 0.0f,
                      gi >= 0 ? s_lamp_pos[gi][2] : 0.0f,
                      gi >= 0 ? (double)sqrtf(gd2) : -1.0);
        }
    }
}
