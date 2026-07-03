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
#include "td5_game.h"                           /* td5_game_get_actor / total count */
#include "td5_light.h"
#include "td5_platform.h"

#define LOG_TAG "render"   /* routes to engine.log */

/* ---- Registry ---------------------------------------------------------- */
static TD5_DynLight s_lights[TD5_LIGHT_MAX];
static int          s_light_count   = 0;
static int          s_enabled       = 1;   /* master enable (set from INI at boot) */
static int          s_headlights    = 1;   /* manual headlight emitter enable */
static int          s_auto          = 1;   /* auto-enable in poorly-lit environments */
static int          s_env_dark      = 0;   /* set per-frame by the render env-brightness probe */

void td5_light_set_enabled(int on)      { s_enabled = on ? 1 : 0; }
int  td5_light_enabled(void)            { return s_enabled; }
void td5_light_set_headlights(int on)   { s_headlights = on ? 1 : 0; }
int  td5_light_headlights_enabled(void) { return s_headlights; }
void td5_light_set_auto(int on)         { s_auto = on ? 1 : 0; }
int  td5_light_auto(void)               { return s_auto; }
void td5_light_set_env_dark(int dark)   { s_env_dark = dark ? 1 : 0; }

/* Effective headlight emission: in AUTO mode follow the environment-dark probe;
 * otherwise follow the manual headlight toggle. */
static int td5_light_headlights_active(void)
{
    if (!s_enabled) return 0;
    return s_auto ? s_env_dark : s_headlights;
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
    if (!td5_light_headlights_active()) return;
    read_knobs_once();

    int total = td5_game_get_total_actor_count();
    if (total <= 0) return;

    int emitted = 0;
    for (int slot = 0; slot < total; slot++) {
        /* Racers always; traffic only when the knob is set. */
        if (slot >= g_traffic_slot_base && !s_hl_traffic) continue;

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
        memcpy(&car_def, ap + 0x1B8, sizeof(void *));

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

static float s_lamp_pos[TD5_LAMP_MAX][3];
static int   s_lamp_count     = 0;
static int   s_street_lights  = 1;

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
    s_lamp_count++;
}

/* Lamp look knobs (env, read once): warm sodium-ish pools. */
static int   s_lamp_knobs_read = 0;
static float s_lamp_range      = 2200.0f;  /* TD5RE_LAMP_RANGE      pool radius (world units) */
static float s_lamp_intensity  = 0.60f;    /* TD5RE_LAMP_INTENSITY  peak added light 0..1     */
static int   s_lamp_budget     = 10;       /* TD5RE_LAMP_COUNT      nearest-N promoted/frame  */

void td5_light_emit_street_lamps(void)
{
    if (!s_enabled || !s_street_lights || s_lamp_count <= 0) return;
    /* Same verdict as auto headlights: lamps light up in rain/dusk/dark zones
     * and stay off in bright daylight. */
    if (!s_env_dark) return;

    if (!s_lamp_knobs_read) {
        s_lamp_knobs_read = 1;
        s_lamp_range     = env_f("TD5RE_LAMP_RANGE",     s_lamp_range);
        s_lamp_intensity = env_f("TD5RE_LAMP_INTENSITY", s_lamp_intensity);
        {
            const char *e = getenv("TD5RE_LAMP_COUNT");
            if (e && e[0]) { int v = atoi(e); if (v >= 0 && v <= TD5_LIGHT_MAX) s_lamp_budget = v; }
        }
        TD5_LOG_I(LOG_TAG, "street lamps: %d registered, range=%.0f intensity=%.2f budget=%d",
                  s_lamp_count, (double)s_lamp_range, (double)s_lamp_intensity, s_lamp_budget);
    }
    if (s_lamp_budget <= 0 || s_lamp_intensity <= 0.0f) return;

    /* Reference point: player slot 0 (lights are shared across panes). */
    TD5_Actor *p = td5_game_get_actor(0);
    if (!p) return;
    float px = (float)p->world_pos.x * (1.0f / 256.0f);
    float pz = (float)p->world_pos.z * (1.0f / 256.0f);

    /* Nearest-N selection (insertion into a small sorted list — lamp counts
     * are a few hundred, budget ~10; runs once per frame). XZ distance only:
     * lamp heads sit high, vertical offset is irrelevant for "near me". */
    int   best_idx[TD5_LIGHT_MAX];
    float best_d2[TD5_LIGHT_MAX];
    int   nbest = 0;
    float cutoff2 = (s_lamp_range * 6.0f) * (s_lamp_range * 6.0f);
    for (int i = 0; i < s_lamp_count; i++) {
        float dx = s_lamp_pos[i][0] - px;
        float dz = s_lamp_pos[i][2] - pz;
        float d2 = dx * dx + dz * dz;
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

    for (int k = 0; k < nbest; k++) {
        const float *L = s_lamp_pos[best_idx[k]];
        /* Warm sodium-vapor tint. */
        td5_light_add_point(L[0], L[1], L[2],
                            s_lamp_range, s_lamp_intensity,
                            1.0f, 0.82f, 0.55f);
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
