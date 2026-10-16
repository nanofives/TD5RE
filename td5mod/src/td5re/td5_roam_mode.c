/**
 * td5_roam_mode.c -- GEO FREE ROAM: the ROAM game mode (PORT-ONLY, F2 task A).
 * Contract: td5_roam_mode.h. Design: docs/plans/GEO_FREE_ROAM_DESIGN.md 0 item 7, 2.
 *
 * WHAT THIS MODULE OWNS. The session (place, spawn, chosen car, fixed world origin),
 * the arm -> active life cycle, the single-player overrides a roam race needs, the
 * player spawner, the lat/lon -> place-frame conversion the frontend click needs and
 * the dev CLI. It does NOT own physics (task B: td5_roam_set_resident) or drawing
 * (task C). The gates in the other modules ask td5_roam_active().
 *
 * WORLD FRAME. world = place - origin, in integer units of 1/430 m; the actor's
 * world_pos is the 24.8 form (FP_SCALE). The origin is the (0,0) corner of the
 * lattice tile that holds the spawn point (roam_grid_origin, the same rounding the
 * ROAMTILE files use), fixed for the whole session (F2; dynamic rebasing is F9).
 *
 * NO FILE-SCOPE STATE OUTSIDE s_roam (the mode/session struct).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_platform.h"
#include "td5_config.h"
#include "td5_geo.h"
#include "td5_fp.h"
#include "td5re.h"
#include "td5_types.h"
#include "td5_physics.h"
#include "td5_render.h"        /* AngleFromVector12 */
#include "td5_roam_tile.h"
#include "td5_roam_locate.h"
#include "td5_roam_mode.h"
#include "deps/cjson/cJSON.h"
#include "../../../re/include/td5_actor_struct.h"
#include "td5_roam_physics.h"   /* task B: the resident set (road tiles + contact provider) */

#define LOG_TAG "roam"

#define ROAM_MAX_SLOTS   32      /* == TD5_MAX_TOTAL_ACTORS */
#define ROAM_UPM         430.0   /* place units per metre */
#define ROAM_RAD         0.017453292519943295
#define ROAM_WGS_A       6378137.0
#define ROAM_WGS_F       (1.0 / 298.257223563)

/* The "projection" block of a PLACE.JSON (td5_roam_view.c rv_proj_load, same maths). */
typedef struct { double lat0, lon0, upm, mdlat, mdlon, cs, sn, offx, offz; } RoamProj;

typedef struct {
    int                             armed;     /* begin() called, end() not yet */
    int                             active;    /* a roam race session is running */
    TD5_RoamSpawn                   spawn;
    int32_t                         origin_x, origin_z;   /* PLACE units */
    const struct TD5_RoamResident  *resident;
    uint64_t                        slot_edge[ROAM_MAX_SLOTS];
    int                             host_track;           /* -1 = not resolved */
    double                          spawn_snap_m;         /* metres the spawn point moved onto the road, -1 = none found */
} RoamSession;

static RoamSession s_roam = { 0, 0, { {0}, 0, 0, 0, 0 }, 0, 0, NULL, { 0 }, -1, 0.0 };

/* ------------------------------------------------------------ small helpers -- */

static char *roam_slurp(const char *path)
{
    TD5_File *f = td5_plat_file_open(path, "rb");
    int64_t n;
    char *buf;
    if (!f) return NULL;
    n = td5_plat_file_size(f);
    if (n <= 0 || n > (int64_t)8 * 1024 * 1024) { td5_plat_file_close(f); return NULL; }
    buf = (char *)malloc((size_t)n + 1u);
    if (!buf) { td5_plat_file_close(f); return NULL; }
    if (td5_plat_file_read(f, buf, (size_t)n) != (size_t)n) {
        free(buf); td5_plat_file_close(f); return NULL;
    }
    buf[n] = '\0';
    td5_plat_file_close(f);
    return buf;
}

static int roam_proj_load(const char *slug, RoamProj *p)
{
    char path[512];
    char *json;
    cJSON *root, *pr, *a, *o, *u, *r, *ox, *oz;
    double e2, s, w, phi, rot;
    int ok = 0;
    td5_geo_source_path(path, sizeof path, slug, "PLACE.JSON");
    json = roam_slurp(path);
    if (!json) return 0;
    root = cJSON_Parse(json);
    free(json);
    if (!root) return 0;
    pr = cJSON_GetObjectItem(root, "projection");
    if (pr && cJSON_IsObject(pr)) {
        a  = cJSON_GetObjectItem(pr, "lat0");
        o  = cJSON_GetObjectItem(pr, "lon0");
        u  = cJSON_GetObjectItem(pr, "units_per_metre");
        r  = cJSON_GetObjectItem(pr, "rotation_rad");
        ox = cJSON_GetObjectItem(pr, "offset_x");
        oz = cJSON_GetObjectItem(pr, "offset_z");
        if (cJSON_IsNumber(a) && cJSON_IsNumber(o)) {
            p->lat0 = a->valuedouble;
            p->lon0 = o->valuedouble;
            p->upm  = cJSON_IsNumber(u) ? u->valuedouble : 430.0;
            rot     = cJSON_IsNumber(r) ? r->valuedouble : 0.0;
            p->cs   = cos(rot);
            p->sn   = sin(rot);
            p->offx = cJSON_IsNumber(ox) ? ox->valuedouble : 0.0;
            p->offz = cJSON_IsNumber(oz) ? oz->valuedouble : 0.0;
            e2  = ROAM_WGS_F * (2.0 - ROAM_WGS_F);
            phi = p->lat0 * ROAM_RAD;
            s   = sin(phi);
            w   = 1.0 - e2 * s * s;
            p->mdlat = ROAM_RAD * ROAM_WGS_A * (1.0 - e2) / pow(w, 1.5);
            p->mdlon = ROAM_RAD * ROAM_WGS_A * cos(phi) / sqrt(w);
            ok = 1;
        }
    }
    cJSON_Delete(root);
    return ok;
}

static int roam_grid_load(const char *slug, RoamGrid *g)
{
    char path[512];
    char *json;
    int ok;
    td5_geo_source_path(path, sizeof path, slug, "roam/ROAMGRID.JSON");
    json = roam_slurp(path);
    if (!json) return 0;
    ok = roam_grid_parse(json, g);
    free(json);
    return ok;
}

int td5_roam_place_has_tiles(const char *slug)
{
    char path[512];
    if (!slug || !slug[0]) return 0;
    td5_geo_source_path(path, sizeof path, slug, "roam/ROAMGRID.JSON");
    return td5_plat_file_exists(path);
}

int td5_roam_latlon_to_place(const char *slug, double lat, double lon, int32_t *px, int32_t *pz)
{
    RoamProj p;
    double e, n;
    if (!slug || !px || !pz || !roam_proj_load(slug, &p)) return 0;
    e = (lon - p.lon0) * p.mdlon;
    n = (lat - p.lat0) * p.mdlat;
    *px = (int32_t)floor((e * p.cs - n * p.sn) * p.upm + p.offx + 0.5);
    *pz = (int32_t)floor((e * p.sn + n * p.cs) * p.upm + p.offz + 0.5);
    return 1;
}

int td5_roam_place_for_latlon(double lat, double lon, char *slug, size_t cap,
                             int32_t *px, int32_t *pz)
{
    char cand[64], path[512];
    int n, pass;
    if (!slug || cap < 2 || !px || !pz) return 0;
    n = td5_geo_places_count();
    /* the shipped roam place first, then every registered place */
    for (pass = 0; pass <= n; pass++) {
        const char *s = (pass == 0) ? "la_plata_partido" : td5_geo_places_slug(pass - 1);
        RoamGrid g;
        int tx, tz, ok;
        int32_t x, z;
        if (!s || !s[0]) continue;
        snprintf(cand, sizeof cand, "%s", s);
        if (!td5_roam_place_has_tiles(cand)) continue;
        if (!td5_roam_latlon_to_place(cand, lat, lon, &x, &z)) continue;
        if (!roam_grid_load(cand, &g)) continue;
        roam_grid_tile_of(&g, (double)x, (double)z, &tx, &tz);
        td5_roam_tile_path(path, sizeof path, NULL, cand, tx, tz);
        ok = td5_plat_file_exists(path);
        if (!ok) continue;
        snprintf(slug, cap, "%s", cand);
        *px = x; *pz = z;
        return 1;
    }
    return 0;
}

/* ----------------------------------------------------------- the contract ---- */

int td5_roam_active(void) { return s_roam.active; }
int td5_roam_armed(void)  { return s_roam.armed; }

void td5_roam_begin(const TD5_RoamSpawn *s)
{
    RoamGrid g;
    int tx = 0, tz = 0;
    if (!s) return;
    s_roam.spawn = *s;
    s_roam.spawn.place[sizeof s_roam.spawn.place - 1] = '\0';
    s_roam.origin_x = s_roam.origin_z = 0;
    /* world origin = the (0,0) corner of the lattice tile holding the spawn point */
    if (roam_grid_load(s_roam.spawn.place, &g)) {
        roam_grid_tile_of(&g, (double)s->spawn_px, (double)s->spawn_pz, &tx, &tz);
        roam_grid_origin(&g, tx, tz, &s_roam.origin_x, &s_roam.origin_z);
    } else {
        TD5_LOG_W(LOG_TAG, "roam: no ROAMGRID.JSON for place '%s' (build tiles with "
                  "--RoamBuildTiles=casco); origin left at 0,0", s_roam.spawn.place);
    }
    s_roam.armed = 1;
    s_roam.active = 0;
    memset(s_roam.slot_edge, 0, sizeof s_roam.slot_edge);
    TD5_LOG_I(LOG_TAG, "roam: armed place=%s spawn=(%d,%d) yaw=%d tile=(%d,%d) origin=(%d,%d)",
              s_roam.spawn.place, (int)s->spawn_px, (int)s->spawn_pz, (int)s->spawn_yaw,
              tx, tz, (int)s_roam.origin_x, (int)s_roam.origin_z);
}

void td5_roam_end(void)
{
    if (s_roam.armed || s_roam.active)
        TD5_LOG_I(LOG_TAG, "roam: session ended (armed=%d active=%d)", s_roam.armed, s_roam.active);
    s_roam.armed = 0;
    s_roam.active = 0;
    memset(s_roam.slot_edge, 0, sizeof s_roam.slot_edge);
    td5_roam_physics_session_end();     /* task B: frees the resident set (no-op when not loaded) */
}

void td5_roam_cancel(void) { td5_roam_end(); }

void td5_roam_world_origin(int32_t *ox, int32_t *oz)
{
    if (ox) *ox = s_roam.origin_x;
    if (oz) *oz = s_roam.origin_z;
}

const struct TD5_RoamResident *td5_roam_resident(void) { return s_roam.resident; }
void td5_roam_set_resident(const struct TD5_RoamResident *rs) { s_roam.resident = rs; }
const TD5_RoamSpawn *td5_roam_spawn(void)
{
    return (s_roam.armed || s_roam.active) ? &s_roam.spawn : NULL;
}

uint64_t td5_roam_slot_edge(int slot)
{
    return (slot >= 0 && slot < ROAM_MAX_SLOTS) ? s_roam.slot_edge[slot] : 0;
}

void td5_roam_set_slot_edge(int slot, uint64_t edge_key)
{
    if (slot >= 0 && slot < ROAM_MAX_SLOTS) s_roam.slot_edge[slot] = edge_key;
}

/* ------------------------------------------------------------ host level ----- */

/* The roam world has no strip, but the race pipeline still wants a LEVEL for the
 * sky, the static texture pages and LEVELINF (fog, environs). F2 hosts on an
 * original track (schedule index, default 0 = Moscow, level023) and throws away its
 * strip / routes / MODELS / traffic queue right after the load (td5_asset.c roam
 * branch). TD5RE_ROAM_HOST_TRACK overrides. Never a generated slot (that would
 * rebuild a level under the splash) and never a registry (custom) level. */
int td5_roam_host_track(void)
{
    if (s_roam.host_track < 0)
        s_roam.host_track = td5_env_int("TD5RE_ROAM_HOST_TRACK", 0, 0, 18);
    return s_roam.host_track;
}

/* ------------------------------------------------------------ session -------- */

/* Yaw along the nearest street: the direction from this ribbon quad's span origin to
 * its linked next span (or from the previous one), through the resident locator.
 * On a junction patch: out along the first arm of the patch node. -1 when there is no
 * resident set, no hit, or nothing to read a direction from. */
static int roam_street_yaw(int32_t px, int32_t pz)
{
    const TD5_RoamResident *rs = s_roam.resident;
    TD5_RoamHit hit;
    const RoamTile *t;
    const TD5_StripSpan *a, *b;
    int32_t dx, dz;
    int idx, sign = 1;
    if (!rs) return -1;
    if (!td5_roam_locate(rs, FP_SCALE(px - s_roam.origin_x), FP_SCALE(pz - s_roam.origin_z),
                         0, 0, ROAM_PRIM_NONE, &hit))
        return -1;
    if (hit.prim == ROAM_PRIM_NONE) return -1;
    if (ROAM_PRIM_SLOT(hit.prim) >= (uint32_t)ROAM_RES_MAX) return -1;
    t = rs->tile[ROAM_PRIM_SLOT(hit.prim)];
    idx = (int)ROAM_PRIM_INDEX(hit.prim);
    if (!t) return -1;
    if (hit.is_patch) {
        /* out along the first arm of the patch node. Arm bearings are (cos b, sin b) = (x, z) and a
         * yaw has (sin y, cos y) = (x, z), so y = 1024 - b */
        const RoamNodeRec *nd;
        if (idx < 0 || (uint32_t)idx >= t->ntri || t->tri[idx].patch >= t->nnode) return -1;
        nd = &t->node[t->tri[idx].patch];
        if (nd->narm < 1) return -1;
        return (1024 - (int)t->arm[nd->arm_first].bearing12) & 0xFFF;
    }
    if (idx < 0 || (uint32_t)idx >= t->nspan) return -1;
    a = &t->span[idx];
    if (a->link_next >= 0 && (uint32_t)a->link_next < t->nspan) {
        b = &t->span[a->link_next];
    } else if (a->link_prev >= 0 && (uint32_t)a->link_prev < t->nspan) {
        b = &t->span[a->link_prev];
        sign = -1;
    } else {
        return -1;
    }
    dx = (b->origin_x - a->origin_x) * sign;
    dz = (b->origin_z - a->origin_z) * sign;
    if (dx == 0 && dz == 0) return -1;
    return AngleFromVector12(dx, dz) & 0xFFF;
}

/* ---- spawn on road ---------------------------------------------------------
 * A click can land anywhere on the map (Plaza Moreno's lawn is 108 m from the nearest
 * carriageway). The spawn POINT is moved onto the drivable surface before the car is
 * placed: the locator's push vector (to the nearest boundary edge) plus a margin inward
 * along the same direction, taken at the first margin where the car's whole footprint
 * (centre + 4 body corners at the street yaw) is on the surface. A click that is on the
 * surface but too close to a kerb for the footprint gets a small ring search instead. */
#define ROAM_SNAP_HALF_W    400      /* units, ~0.93 m: footprint half width  */
#define ROAM_SNAP_HALF_L    950      /* units, ~2.2 m: footprint half length  */

static int roam_inside(const TD5_RoamResident *rs, double wx, double wz)
{
    TD5_RoamHit h;
    if (!td5_roam_locate(rs, (int32_t)(wx * 256.0), (int32_t)(wz * 256.0), 0, 0, ROAM_PRIM_NONE, &h))
        return 0;
    return h.prim != ROAM_PRIM_NONE && h.inside;
}

/* centre + 4 corners of a car at (wx, wz) heading `yaw12` all on the surface */
static int roam_footprint_ok(const TD5_RoamResident *rs, double wx, double wz, int yaw12)
{
    const double y = (double)(yaw12 & 0xFFF) * (2.0 * 3.14159265358979323846 / 4096.0);
    const double fx = sin(y), fz = cos(y), rx = fz, rz = -fx;
    int i;
    if (!roam_inside(rs, wx, wz)) return 0;
    for (i = 0; i < 4; i++) {
        const double l = (i & 1) ? ROAM_SNAP_HALF_L : -ROAM_SNAP_HALF_L;
        const double w = (i & 2) ? ROAM_SNAP_HALF_W : -ROAM_SNAP_HALF_W;
        if (!roam_inside(rs, wx + fx * l + rx * w, wz + fz * l + rz * w)) return 0;
    }
    return 1;
}

/* PLACE-frame spawn point -> the nearest drivable spot. Writes the moved point to *px,*pz and
 * returns the distance moved in metres (0 = already fine), or -1 when no road could be found
 * (no resident set, no tile under the click: the point is left alone). */
static double roam_snap_to_road(int32_t *px, int32_t *pz, int fixed_yaw)
{
    static const int k_margin[] = { 645, 1075, 1505, 2150, 3010 };   /* 1.5 .. 7 m past the kerb */
    const TD5_RoamResident *rs = s_roam.resident;
    const double ox = (double)s_roam.origin_x, oz = (double)s_roam.origin_z;
    const double wx = (double)*px - ox, wz = (double)*pz - oz;
    TD5_RoamHit h;
    double bx = wx, bz = wz;
    int k, found = 0;
    if (!rs) return -1.0;
    if (!td5_roam_locate(rs, (int32_t)(wx * 256.0), (int32_t)(wz * 256.0), 0, 0, ROAM_PRIM_NONE, &h) ||
        h.prim == ROAM_PRIM_NONE)
        return -1.0;
    if (!h.inside) {
        const double m = sqrt((double)h.push_x * h.push_x + (double)h.push_z * h.push_z);
        const double ux = m > 0.5 ? h.push_x / m : 1.0, uz = m > 0.5 ? h.push_z / m : 0.0;
        for (k = 0; k < (int)(sizeof k_margin / sizeof k_margin[0]) && !found; k++) {
            const double cx = wx + h.push_x + ux * k_margin[k], cz = wz + h.push_z + uz * k_margin[k];
            int yaw = fixed_yaw >= 0 ? fixed_yaw : roam_street_yaw((int32_t)(cx + ox), (int32_t)(cz + oz));
            if (yaw < 0) yaw = 0;
            if (roam_footprint_ok(rs, cx, cz, yaw)) { bx = cx; bz = cz; found = 1; }
        }
    } else {
        int yaw = fixed_yaw >= 0 ? fixed_yaw : roam_street_yaw(*px, *pz);
        int r, a;
        double best = 1e30;
        if (yaw < 0) yaw = 0;
        if (roam_footprint_ok(rs, wx, wz, yaw)) return 0.0;
        /* on the surface but the footprint hangs over a kerb: nearest fitting spot on a ring */
        for (r = 1; r <= 12 && !found; r++) {
            for (a = 0; a < 16; a++) {
                const double t = (double)a * (2.0 * 3.14159265358979323846 / 16.0);
                const double cx = wx + cos(t) * r * 215.0, cz = wz + sin(t) * r * 215.0;
                const double d = (cx - wx) * (cx - wx) + (cz - wz) * (cz - wz);
                if (d < best && roam_footprint_ok(rs, cx, cz, yaw)) { best = d; bx = cx; bz = cz; found = 1; }
            }
        }
    }
    if (!found) return -1.0;
    *px = (int32_t)floor(bx + ox + 0.5);
    *pz = (int32_t)floor(bz + oz + 0.5);
    return sqrt((bx - wx) * (bx - wx) + (bz - wz) * (bz - wz)) / ROAM_UPM;
}

/* The single-player overrides a roam race needs (no AI, no traffic, no cops, no net,
 * no laps / checkpoints). Idempotent: run at session start and again after
 * init_race_modes_and_seed, which may re-derive some of them from the menu state. */
void td5_roam_apply_overrides(void)
{
    if (!s_roam.active) return;
    g_td5.track_index              = td5_roam_host_track();
    g_td5.reverse_direction        = 0;     /* the host level's reverse texture swap never applies */
    g_td5.num_human_players        = 1;
    g_td5.num_ai_opponents         = 0;
    g_td5.num_spectate_screens     = 0;
    g_td5.traffic_enabled          = 0;
    g_td5.special_encounter_enabled = 0;
    g_td5.wanted_mode_enabled      = 0;
    g_td5.time_trial_enabled       = 0;
    g_td5.drag_race_enabled        = 0;
    g_td5.network_active           = 0;
    g_td5.track_span_ring_length   = 0;     /* stays 0: not reset when a strip unloads (F0b finding 2) */
    g_td5.split_screen_mode        = 0;
    g_td5.mp_mode_config.mode      = TD5_MP_MODE_RACE;
}

/* Race init, before anything reads the race modes: armed -> active + the overrides. */
void td5_roam_session_start(void)
{
    if (!s_roam.armed) { s_roam.active = 0; return; }
    s_roam.active = 1;
    td5_roam_physics_session_begin();   /* task B: registers the resident set before the spawn (street yaw) */
    td5_roam_apply_overrides();
    s_roam.spawn.car_index         = g_td5.car_index;
    TD5_LOG_I(LOG_TAG, "roam: session start place=%s car=%d host_track=%d origin=(%d,%d) "
              "(no strip, ring 0, 1 racer, no traffic/cops/checkpoints)",
              s_roam.spawn.place, s_roam.spawn.car_index, g_td5.track_index,
              (int)s_roam.origin_x, (int)s_roam.origin_z);
}

/* Place the player actor (td5_game.c spawn loop, after the AI runtime init). */
void td5_roam_session_spawn(int slot, void *actor_mem)
{
    TD5_Actor *a = (TD5_Actor *)actor_mem;
    int yaw = s_roam.spawn.spawn_yaw;
    int32_t px = s_roam.spawn.spawn_px, pz = s_roam.spawn.spawn_pz;
    int32_t wx, wz;
    int auto_yaw = 0;
    double moved;
    if (!a) return;

    /* the click point -> the nearest drivable spot (the car must not start off the road) */
    moved = roam_snap_to_road(&px, &pz, yaw);
    if (moved > 0.0)
        TD5_LOG_I(LOG_TAG, "roam: spawn snap to road: click place=(%d,%d) -> (%d,%d), moved %.1f m",
                  (int)s_roam.spawn.spawn_px, (int)s_roam.spawn.spawn_pz, (int)px, (int)pz, moved);
    else if (moved == 0.0)
        TD5_LOG_I(LOG_TAG, "roam: spawn snap: click place=(%d,%d) is on the road, footprint fits",
                  (int)px, (int)pz);
    else
        TD5_LOG_W(LOG_TAG, "roam: spawn snap: no road found near place=(%d,%d) (%s), spawning on the click",
                  (int)px, (int)pz, s_roam.resident ? "no tile under the click" : "no resident set");
    s_roam.spawn_snap_m = moved;
    wx = px - s_roam.origin_x;
    wz = pz - s_roam.origin_z;

    if (yaw < 0) {
        yaw = roam_street_yaw(px, pz);
        auto_yaw = 1;
        if (yaw < 0) yaw = 0;
    }
    yaw &= 0xFFF;

    a->world_pos.x = FP_SCALE(wx);
    a->world_pos.y = (int32_t)0xC0000000;   /* reset below snaps it to the ground */
    a->world_pos.z = FP_SCALE(wz);
    a->slot_index = (uint8_t)slot;
    memset(&a->tire_track_emitter_FL, 0xFF, 4);   /* "no emitter acquired yet" (spawn loop) */
    a->euler_accum.yaw = FP_SCALE(yaw);
    td5_physics_reset_actor_state(a);

    /* every span field -1 AFTER td5_ai_invalidate_actor_track_state (it writes 0), and
     * after reset_actor_state (which walks the strip when one is loaded): the roam
     * street piece lives in the side table, not in the int16 fields */
    a->track_span_raw        = -1;
    a->track_span_normalized = -1;
    a->track_span_accumulated = -1;
    a->track_span_high_water = -1;
    a->track_sub_lane_index  = 0;
    td5_roam_set_slot_edge(slot, 0);
    td5_roam_physics_spawn_snap(a);     /* task B: stand on the street under the spawn point */

    TD5_LOG_I(LOG_TAG, "roam: spawn slot=%d place=(%d,%d) world=(%d,%d) yaw=%d%s",
              slot, (int)px, (int)pz, (int)wx, (int)wz,
              yaw, auto_yaw ? (s_roam.resident ? " (street)" : " (auto: no locator, 0)") : "");
}

double td5_roam_spawn_snap_m(void) { return s_roam.spawn_snap_m; }

/* ------------------------------------------------------------ dev CLI -------- */

#ifndef TD5RE_RELEASE
static const char *roam_cli_value(const char *cmd, const char *key)
{
    const char *p = strstr(cmd, key);
    return p ? p + strlen(key) : NULL;
}

void td5_roam_dev_cli(const char *cmd)
{
    const char *v;
    TD5_RoamSpawn sp;
    double lat, lon;
    char slug[64] = "la_plata_partido";
    int yaw = -1, n;
    if (!cmd || !*cmd) return;
    v = roam_cli_value(cmd, "--Roam=");
    if (!v || atoi(v) == 0) return;

    v = roam_cli_value(cmd, "--RoamPlace=");
    if (v) {
        n = 0;
        while (v[n] && v[n] != ' ' && v[n] != '\t' && n < (int)sizeof slug - 1) { slug[n] = v[n]; n++; }
        slug[n] = '\0';
    }
    v = roam_cli_value(cmd, "--RoamYaw=");
    if (v) yaw = atoi(v);
    v = roam_cli_value(cmd, "--RoamSpawn=");
    if (!v || sscanf(v, "%lf,%lf", &lat, &lon) != 2) {
        TD5_LOG_E(LOG_TAG, "roam: --Roam=1 needs --RoamSpawn=<lat>,<lon>");
        return;
    }
    memset(&sp, 0, sizeof sp);
    snprintf(sp.place, sizeof sp.place, "%s", slug);
    if (!td5_roam_latlon_to_place(slug, lat, lon, &sp.spawn_px, &sp.spawn_pz)) {
        TD5_LOG_E(LOG_TAG, "roam: no PLACE.JSON projection for '%s'", slug);
        return;
    }
    sp.spawn_yaw = (yaw >= 0) ? (yaw & 0xFFF) : -1;
    sp.car_index = g_td5.ini.default_car;
    TD5_LOG_W(LOG_TAG, "roam: dev CLI --Roam=1 spawn=%.6f,%.6f -> place (%d,%d) (dev build only)",
              lat, lon, (int)sp.spawn_px, (int)sp.spawn_pz);
    td5_roam_begin(&sp);

    /* straight into the race with the default car, like AutoRace */
    g_td5.ini.auto_race        = 1;
    g_td5.ini.skip_intro       = 1;
    g_td5.ini.default_track    = td5_roam_host_track();
    g_td5.ini.default_opponents = 0;
    g_td5.ini.default_game_type = 0;
}

int td5_roam_selftest_arm(double lat, double lon)
{
    TD5_RoamSpawn sp;
    memset(&sp, 0, sizeof sp);
    snprintf(sp.place, sizeof sp.place, "%s", "la_plata_partido");
    if (!td5_roam_place_has_tiles(sp.place) ||
        !td5_roam_latlon_to_place(sp.place, lat, lon, &sp.spawn_px, &sp.spawn_pz)) {
        TD5_LOG_E(LOG_TAG, "roam selftest: no roam tiles / projection for '%s'", sp.place);
        return 0;
    }
    sp.spawn_yaw = -1;
    sp.car_index = g_td5.ini.default_car;
    td5_roam_begin(&sp);
    return 1;
}
#endif
