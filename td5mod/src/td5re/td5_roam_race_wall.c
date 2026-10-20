/**
 * td5_roam_race_wall.c -- GEO FREE ROAM: the racer-only barrier collision layer (PORT-ONLY, F6-B).
 * Contract: td5_roam_race.h ("the racer-only barrier collision layer"). Barriers: td5_roam_barrier.h.
 *
 * WHAT IT IS. During a sprint (COUNTDOWN, RACING) every MOUTH barrier of the route is a wall for the
 * racer slots (0..5) and nothing for traffic. It is not a second resolver: td5_roam_resolve_walls asks
 * this module for every body probe and keeps the DEEPER of the road push and the barrier push, so the
 * car still gets ONE combined push per tick, and the pinch / wall-tip rules see barrier and kerb alike.
 *
 * ONE-SIDED PANELS. A barrier segment (kerb to kerb across a closed arm) has an OPEN side (the junction
 * the route crosses) and a CLOSED side (the arm). The open side is decided once per barrier, the first
 * time a racer probe comes near it (its tiles are resident then):
 *   1. patch test: 2 m either side of the panel's midpoint, the side whose road hit is the junction
 *      patch of the barrier's own cluster (cluster_key) is open; else the side that is ANY patch while
 *      the other is a ribbon;
 *   2. side memory: else the side the racer's CHASSIS is on at that moment (a racer reaches a barrier
 *      from the route, i.e. from the open side).
 * A probe within the segment (ends included), less than RW_THICK in front of the panel or up to
 * RW_DEPTH behind it, is pushed back out to RW_THICK on the open side. The depth window keeps a car
 * on the far side of a long-past barrier (another street behind it) free, while a fast probe that went
 * through the panel in one tick (< RW_DEPTH, ~860 km/h) is still pushed back the way it came.
 *
 * ONE file-scope struct (W). Main thread only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_platform.h"
#include "td5_config.h"
#include "td5_fp.h"
#include "td5re.h"
#include "td5_types.h"
#include "td5_race_state.h"
#include "td5_roam_mode.h"
#include "td5_roam_locate.h"
#include "td5_roam_barrier.h"
#include "td5_roam_race.h"
#include "td5_roam_ai.h"        /* [F7] finished opponents pass through */
#include "../../../re/include/td5_actor_struct.h"

#define LOG_TAG "roam"

#define RW_M          430.0
#define RW_RACERS     6
#define RW_THICK      (0.15 * RW_M)     /* panel half thickness, units */
#define RW_DEPTH      (8.0 * RW_M)      /* a probe this far behind the panel is no longer caught */
#define RW_NEAR       (6.0 * RW_M)      /* decide a barrier's open side when a probe is this close */
#define RW_SIDE_PROBE (2.0 * RW_M)      /* patch test offset either side of the midpoint */
#define RW_CHASSIS_BACK (3.0 * RW_M)    /* [HE] a panel only catches a car whose chassis is at most this far past it */
#define RW_Y_BELOW    (2.0 * RW_M * 256.0)                       /* 24.8: probe this far under the base = other level */
#define RW_Y_ABOVE    ((TD5_ROAM_BARRIER_H_M + 1.0) * RW_M * 256.0)

typedef struct {
    double ax, az, ux, uz, len;         /* units: start, unit direction, length */
    double minx, minz, maxx, maxz;      /* bbox grown by RW_DEPTH */
    double y0, y1;                      /* 24.8 ground y at the ends */
    int8_t side;                        /* open side: +1 = left of a->b, -1 = right, 0 = not decided */
    uint8_t mouth;
} RwSeg;

static struct {
    const TD5_RoamBarrier *src;
    int      nsrc;
    RwSeg   *seg;
    int      nseg, cap;
    long     last_hit_tick[RW_RACERS];
    long     chassis_tick[RW_RACERS];
    long     dbg_tick[RW_RACERS];          /* [F8b] DEV: contact log rate limit */
    int32_t  org_x, org_z;                 /* [HE] world origin the segments were derived for */
    TD5_RoamRaceWallStats st;
} W;

static void rw_rebuild(const TD5_RoamBarrier *b, int n)
{
    int i;
    if (n > W.cap) {
        RwSeg *ns = (RwSeg *)realloc(W.seg, (size_t)n * sizeof *ns);
        if (!ns) { W.nseg = 0; return; }
        W.seg = ns; W.cap = n;
    }
    W.nseg = n;
    W.src = b; W.nsrc = n;
    td5_roam_world_origin(&W.org_x, &W.org_z);
    W.st.barriers = n; W.st.mouths = 0; W.st.side_route = 0;
    for (i = 0; i < n; i++) {
        RwSeg *s = &W.seg[i];
        const double ax = b[i].x0 / 256.0, az = b[i].z0 / 256.0, bx = b[i].x1 / 256.0, bz = b[i].z1 / 256.0;
        const double dx = bx - ax, dz = bz - az, l = sqrt(dx * dx + dz * dz);
        memset(s, 0, sizeof *s);
        s->ax = ax; s->az = az; s->len = l;
        s->ux = l > 0.0 ? dx / l : 1.0; s->uz = l > 0.0 ? dz / l : 0.0;
        s->minx = (ax < bx ? ax : bx) - RW_DEPTH; s->maxx = (ax > bx ? ax : bx) + RW_DEPTH;
        s->minz = (az < bz ? az : bz) - RW_DEPTH; s->maxz = (az > bz ? az : bz) + RW_DEPTH;
        s->y0 = (double)b[i].y0; s->y1 = (double)b[i].y1;
        s->mouth = b[i].kind == TD5_ROAM_BARRIER_MOUTH && l > 1.0;
        W.st.mouths += s->mouth;
        {   /* [HE] the open side comes from the barrier build (the closed arm's own ribbon direction), never from where
             * a car happens to be: the old chassis fallback could pick the ARM side, and then the panel pushed a car
             * that came from the junction through into the closed street (one barrier got +1 and -1 in two runs) */
            double ox, oz;
            int from_build = 1;
#ifndef TD5RE_RELEASE
            from_build = td5_env_int("TD5RE_ROAM_WALL_SIDE_FROM_BUILD", 1, 0, 1);   /* DEV A/B: 0 = the F6-B patch / chassis rule */
#endif
            if (from_build && s->mouth && td5_roam_barrier_open_dir(i, &ox, &oz)) {
                const double dot = ox * -s->uz + oz * s->ux;
                if (fabs(dot) > 0.2) { s->side = (int8_t)(dot > 0.0 ? 1 : -1); W.st.side_route++; }
            }
        }
    }
    TD5_LOG_I(LOG_TAG, "ROAM_RACE_WALL: %d barriers (%d solid mouths) armed for racers", n, W.st.mouths);
}

static int rw_sync(void)
{
    int n = 0;
    const TD5_RoamBarrier *b = td5_roam_barriers(&n);
    if (!b || n <= 0) { W.nseg = 0; W.src = NULL; W.nsrc = 0; return 0; }
    if (b != W.src || n != W.nsrc) rw_rebuild(b, n);
    else {
        /* [HE] a world-origin rebase (F9b) re-derives the barriers in place (same array, same count): move the
         * segments with it, or every solid panel stays where the old frame put it (256 m+ off) while drawn right */
        int32_t ox, oz;
        td5_roam_world_origin(&ox, &oz);
        if (ox != W.org_x || oz != W.org_z) {
            int i;
            for (i = 0; i < W.nseg; i++) {
                RwSeg *g = &W.seg[i];
                const double ax = b[i].x0 / 256.0, az = b[i].z0 / 256.0, bx = b[i].x1 / 256.0, bz = b[i].z1 / 256.0;
                g->ax = ax; g->az = az;
                g->minx = (ax < bx ? ax : bx) - RW_DEPTH; g->maxx = (ax > bx ? ax : bx) + RW_DEPTH;
                g->minz = (az < bz ? az : bz) - RW_DEPTH; g->maxz = (az > bz ? az : bz) + RW_DEPTH;
            }
            TD5_LOG_I(LOG_TAG, "ROAM_RACE_WALL: %d barrier segments follow the origin rebase (%+d,%+d units)", W.nseg,
                      (int)(W.org_x - ox), (int)(W.org_z - oz));
            W.org_x = ox; W.org_z = oz;
        }
    }
    return W.nseg;
}

void td5_roam_race_wall_begin(void)
{
    free(W.seg);
    memset(&W, 0, sizeof W);
    rw_sync();
}

void td5_roam_race_wall_end(void)
{
    if (W.st.hits || W.st.contacts)
        TD5_LOG_I(LOG_TAG, "ROAM_RACE_WALL: summary barriers=%d mouths=%d contacts=%ld hits=%ld (slot0 %ld) "
                  "open side by patch=%ld route=%ld chassis=%ld chassis_past=%ld (max %.2f m) max_pen=%.2f m",
                  W.st.barriers, W.st.mouths, W.st.contacts, W.st.hits, W.st.hits_slot0, W.st.side_patch,
                  W.st.side_route, W.st.side_fallback, W.st.chassis_past, W.st.max_chassis_past_m, W.st.max_pen_m);
    W.nseg = 0; W.src = NULL; W.nsrc = 0;
}

void td5_roam_race_wall_stats(TD5_RoamRaceWallStats *out)
{
    if (out) *out = W.st;
}

void td5_roam_race_wall_note_hit(int slot)
{
    if (slot < 0 || slot >= RW_RACERS) return;
    if (W.last_hit_tick[slot] == (long)g_td5.simulation_tick_counter + 1) return;
    W.last_hit_tick[slot] = (long)g_td5.simulation_tick_counter + 1;
    W.st.hits++;
    if (slot == 0) W.st.hits_slot0++;
}

/* signed distance of (x, z) from the panel line, + = left of a->b; t = along the segment */
static void rw_rel(const RwSeg *s, double x, double z, double *t, double *d)
{
    const double rx = x - s->ax, rz = z - s->az;
    *t = rx * s->ux + rz * s->uz;
    *d = rx * -s->uz + rz * s->ux;
}

/* which side of the panel is a junction patch: +1 / -1, 0 = undecided */
static int rw_patch_side(const RwSeg *s, const TD5_RoamBarrier *b)
{
    const TD5_RoamResident *rs = td5_roam_resident();
    const double mx = s->ax + s->ux * s->len * 0.5, mz = s->az + s->uz * s->len * 0.5;
    const double nx = -s->uz, nz = s->ux;
    const int32_t y = (int32_t)((b->y0 + (int64_t)b->y1) / 2);
    TD5_RoamHit h[2];
    int ok[2], own[2], any[2], k;
    if (!rs) return 0;
    for (k = 0; k < 2; k++) {
        const double sg = k == 0 ? 1.0 : -1.0;
        const int32_t qx = (int32_t)floor((mx + nx * sg * RW_SIDE_PROBE) * 256.0 + 0.5);
        const int32_t qz = (int32_t)floor((mz + nz * sg * RW_SIDE_PROBE) * 256.0 + 0.5);
        ok[k] = td5_roam_locate(rs, qx, qz, y, 1, ROAM_PRIM_NONE, &h[k]) && h[k].prim != ROAM_PRIM_NONE && h[k].inside;
        any[k] = ok[k] && h[k].is_patch;
        own[k] = any[k] && h[k].edge_key == b->cluster_key;
    }
    if (own[0] != own[1]) return own[0] ? 1 : -1;
    if (any[0] != any[1]) return any[0] ? 1 : -1;
    return 0;
}

static void rw_decide(int i, int slot)
{
    RwSeg *s = &W.seg[i];
    int side = rw_patch_side(s, &W.src[i]);
    const char *how = "patch";
    if (side) {
        W.st.side_patch++;
    } else {
        /* side memory: the side the racer's chassis is on now (it came from the route) */
        const TD5_Actor *a = td5_game_get_actor(slot);
        double t, d;
        if (!a) return;
        rw_rel(s, a->world_pos.x / 256.0, a->world_pos.z / 256.0, &t, &d);
        side = d >= 0.0 ? 1 : -1;
        W.st.side_fallback++;
        how = "chassis";
    }
    s->side = (int8_t)side;
    (void)how;
#ifndef TD5RE_RELEASE
    TD5_LOG_I(LOG_TAG, "ROAM_RACE_WALL: barrier %d cluster %016llx open side %+d (%s), length %.1f m",
              i, (unsigned long long)W.src[i].cluster_key, side, how, s->len / RW_M);
#endif
}

/* chassis centre past a barrier (closed side, within the segment): the barrier failed */
static void rw_chassis_check(int slot)
{
    const TD5_Actor *a = td5_game_get_actor(slot);
    const long tick = (long)g_td5.simulation_tick_counter;
    double cx, cz, worst = 0.0;
    int i;
    if (!a || W.chassis_tick[slot] == tick + 1) return;
    W.chassis_tick[slot] = tick + 1;
    cx = a->world_pos.x / 256.0; cz = a->world_pos.z / 256.0;
    for (i = 0; i < W.nseg; i++) {
        const RwSeg *s = &W.seg[i];
        double t, d;
        if (!s->mouth || !s->side || cx < s->minx || cx > s->maxx || cz < s->minz || cz > s->maxz) continue;
        rw_rel(s, cx, cz, &t, &d);
        d *= s->side;
        if (t < 0.0 || t > s->len || d >= 0.0 || d < -RW_DEPTH) continue;
        if (-d > worst) worst = -d;
    }
    if (worst > 0.0) {
        W.st.chassis_past++;
        if (worst / RW_M > W.st.max_chassis_past_m) W.st.max_chassis_past_m = worst / RW_M;
        if (W.st.chassis_past <= 20)
            TD5_LOG_W(LOG_TAG, "ROAM_RACE_WALL: slot %d chassis %.2f m past a barrier (tick %ld)", slot, worst / RW_M, tick);
    }
}

int td5_roam_race_wall_probe(int slot, int probe, int32_t x, int32_t z, int32_t y,
                             int32_t *push_x, int32_t *push_z, int32_t *push_dist)
{
    const double px = x / 256.0, pz = z / 256.0;
    double best_pen = 0.0, bnx = 0.0, bnz = 0.0, chx = 0.0, chz = 0.0;
    int i, hit = 0, have_ch = 0;
    if (slot < 0 || slot >= RW_RACERS || !td5_roam_race_barriers_live()) return 0;
    if (slot > 0 && td5_roam_ai_slot_finished(slot)) return 0;   /* [F7] a finished opponent is not racing */
    if (!rw_sync()) return 0;
    if (probe == 0) rw_chassis_check(slot);
    {
        const TD5_Actor *a = td5_game_get_actor(slot);
        if (a) { chx = a->world_pos.x / 256.0; chz = a->world_pos.z / 256.0; have_ch = 1; }
    }
    for (i = 0; i < W.nseg; i++) {
        RwSeg *s = &W.seg[i];
        double t, d, gy;
        if (!s->mouth || px < s->minx || px > s->maxx || pz < s->minz || pz > s->maxz) continue;
        rw_rel(s, px, pz, &t, &d);
        if (t < 0.0 || t > s->len) continue;
        W.st.probes_tested++;
        if (!s->side) {
            if (fabs(d) > RW_NEAR) continue;
            rw_decide(i, slot);
            if (!s->side) continue;
        }
        gy = s->y0 + (s->y1 - s->y0) * (s->len > 0.0 ? t / s->len : 0.0);
        if ((double)y < gy - RW_Y_BELOW || (double)y > gy + RW_Y_ABOVE) continue;
        d *= s->side;                          /* + = open side */
        if (d >= RW_THICK || d < -RW_DEPTH) continue;
        if (have_ch) {
            /* [HE] The depth window catches a probe that went through the panel in one tick. It must not catch a car
             * that is BEYOND this panel: two panels closing one short edge from both ends (a route junction and a
             * glued neighbour, 2 m apart: barriers 4 / 84 of the 2026-10-19 geo_map sprint) face each other, and a car
             * pressing on one sat 7 m inside the other's window, which threw it the other way, through the first. */
            double ct, cd;
            rw_rel(s, chx, chz, &ct, &cd);
            if (cd * s->side < -RW_CHASSIS_BACK) { W.st.beyond_skips++; continue; }
        }
        if (RW_THICK - d > best_pen) {
            best_pen = RW_THICK - d;
            bnx = -s->uz * s->side; bnz = s->ux * s->side;
            hit = 1;
#ifndef TD5RE_RELEASE
            if (W.dbg_tick[slot] + 30 <= (long)g_td5.simulation_tick_counter) {   /* [F8b] DEV: who touches which panel */
                W.dbg_tick[slot] = (long)g_td5.simulation_tick_counter;
                TD5_LOG_I(LOG_TAG, "ROAM_RACE_WALL: slot %d probe %d contact barrier %d cluster %016llx at %.1f of %.1f m, %.2f m %s "
                          "(tick %ld)", slot, probe, i, (unsigned long long)W.src[i].cluster_key, t / RW_M, s->len / RW_M,
                          fabs(d) / RW_M, d >= 0.0 ? "open side" : "closed side", (long)g_td5.simulation_tick_counter);
            }
#endif
        }
    }
    if (!hit) return 0;
    W.st.contacts++;
    if ((best_pen - RW_THICK) / RW_M > W.st.max_pen_m) W.st.max_pen_m = (best_pen - RW_THICK) / RW_M;
    *push_x = (int32_t)floor(bnx * best_pen + 0.5);
    *push_z = (int32_t)floor(bnz * best_pen + 0.5);
    *push_dist = (int32_t)ceil(best_pen);
    return 1;
}

/* [F10] a COP's body probe: cops are racer-like, a mouth barrier is a wall for them too. The open side comes from the
 * junction patch only (no racer chassis to remember it by); a barrier whose side is still undecided does not stop a cop.
 * No racer counters are touched. */
int td5_roam_race_wall_probe_cop(int32_t x, int32_t z, int32_t y, int32_t *push_x, int32_t *push_z, int32_t *push_dist)
{
    const double px = x / 256.0, pz = z / 256.0;
    double best_pen = 0.0, bnx = 0.0, bnz = 0.0;
    int i, hit = 0;
    if (!td5_roam_race_barriers_live() || !rw_sync()) return 0;
    for (i = 0; i < W.nseg; i++) {
        RwSeg *s = &W.seg[i];
        double t, d, gy;
        if (!s->mouth || px < s->minx || px > s->maxx || pz < s->minz || pz > s->maxz) continue;
        rw_rel(s, px, pz, &t, &d);
        if (t < 0.0 || t > s->len) continue;
        if (!s->side) {
            const int side = rw_patch_side(s, &W.src[i]);
            if (!side) continue;
            s->side = (int8_t)side;
            W.st.side_patch++;
        }
        gy = s->y0 + (s->y1 - s->y0) * (s->len > 0.0 ? t / s->len : 0.0);
        if ((double)y < gy - RW_Y_BELOW || (double)y > gy + RW_Y_ABOVE) continue;
        d *= s->side;
        if (d >= RW_THICK || d < -RW_DEPTH) continue;
        if (RW_THICK - d > best_pen) { best_pen = RW_THICK - d; bnx = -s->uz * s->side; bnz = s->ux * s->side; hit = 1; }
    }
    if (!hit) return 0;
    *push_x = (int32_t)floor(bnx * best_pen + 0.5);
    *push_z = (int32_t)floor(bnz * best_pen + 0.5);
    *push_dist = (int32_t)ceil(best_pen);
    return 1;
}
