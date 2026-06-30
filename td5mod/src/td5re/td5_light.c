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
static int          s_headlights    = 1;   /* headlight emitter enable */

void td5_light_set_enabled(int on)      { s_enabled = on ? 1 : 0; }
int  td5_light_enabled(void)            { return s_enabled; }
void td5_light_set_headlights(int on)   { s_headlights = on ? 1 : 0; }
int  td5_light_headlights_enabled(void) { return s_headlights; }

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
static float s_hl_range     = 600.0f;  /* TD5RE_HEADLIGHT_RANGE     */
static float s_hl_intensity = 165.0f;  /* TD5RE_HEADLIGHT_INTENSITY (0..255 luminance bump) */
static float s_hl_fwd       = 220.0f;  /* TD5RE_HEADLIGHT_FWD   forward mount offset */
static float s_hl_side      = 70.0f;   /* TD5RE_HEADLIGHT_SIDE  lateral half-spacing */
static float s_hl_up        = 10.0f;   /* TD5RE_HEADLIGHT_UP    model +Y is DOWN, so >0 lowers toward road */
static float s_hl_fwd_sign  = 1.0f;    /* TD5RE_HEADLIGHT_FWD_SIGN  flip to -1 if the pool lands behind the car */
static int   s_hl_traffic   = 0;       /* TD5RE_HEADLIGHT_TRAFFIC   1 = traffic cars also get headlights */

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
    s_hl_side      = env_f("TD5RE_HEADLIGHT_SIDE",      s_hl_side);
    s_hl_up        = env_f("TD5RE_HEADLIGHT_UP",        s_hl_up);
    s_hl_fwd_sign  = env_f("TD5RE_HEADLIGHT_FWD_SIGN",  s_hl_fwd_sign);
    {
        const char *e = getenv("TD5RE_HEADLIGHT_TRAFFIC");
        s_hl_traffic = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    TD5_LOG_I(LOG_TAG,
              "headlights: range=%.0f intensity=%.0f fwd=%.0f side=%.0f up=%.0f fwd_sign=%.0f traffic=%d",
              (double)s_hl_range, (double)s_hl_intensity, (double)s_hl_fwd,
              (double)s_hl_side, (double)s_hl_up, (double)s_hl_fwd_sign, s_hl_traffic);
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
    if (!s_enabled || !s_headlights) return;
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
        float fwd = s_hl_fwd * s_hl_fwd_sign;

        /* Left + right lamp: same forward + vertical, mirrored lateral (X=right). */
        for (int side = 0; side < 2; side++) {
            float ox = (side == 0) ? -s_hl_side : s_hl_side;
            float wx, wy, wz;
            body_to_world(m, ox, s_hl_up, fwd, &wx, &wy, &wz);
            td5_light_add_point(cx + wx, cy + wy, cz + wz,
                                s_hl_range, s_hl_intensity,
                                1.0f, 1.0f, 1.0f);   /* white headlights (v1) */
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
