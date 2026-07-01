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
