/**
 * td5_roam_wall_test.c -- GEO FREE ROAM: the `roam-walls` test (PORT-ONLY, DEV-ONLY, round H task HE).
 * Contract: td5_roam_race.h ("DEV: the roam-walls test").
 *
 * WHY. Mariano (2026-10-19): "walls for borders of the sprint are not fool-proof, they can be driven around".
 * The F6 census counts arms (taken / barriered / linked / open) on the TOPOLOGY of the route; a car drives the
 * GEOMETRY. This test proves the race walls on the geometry, two ways:
 *
 *   FLOOD. Every 1 m cell within TW_CORRIDOR_M of the route whose centre and four points 0.8 m round it are on the
 *   drivable surface is car-passable (a gap narrower than ~1.6 m stops a car). From every passable cell under the
 *   route polyline the fill spreads 4-connected; a step that crosses a mouth barrier (the panel line, 0.3 m longer
 *   each end) is cut. A reached cell that is not on the route's own surface (a route edge's ribbon, a route
 *   cluster's patch) nor on an arm the barrier build left linked on purpose is a LEAK cell; leak cells are grouped
 *   into regions and each region's way out is listed (place frame + lat/lon + the street piece it is on).
 *
 *   ATTACK. For every mouth barrier the car is put on the open side, 5..14 m from the panel, and driven at it with
 *   full throttle for 2.5 s: square at the middle (90 km/h) and at each kerb end 30 degrees outward (50 km/h), the
 *   pursuit target 20 m behind the panel. A run LEAKS when the chassis centre gets more than 1 m past the panel
 *   line inside its length, or reaches the closed arm's own ribbon more than 1 m past the line, or ends up 25 m
 *   from both the route and its start point.
 *
 * ONE file-scope struct (T). Compiled out of RELEASE. Main thread only (the attack runs from the input hook).
 */
#ifndef TD5RE_RELEASE

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
#include "td5_damage.h"
#include "td5_roam_tile.h"
#include "td5_roam_locate.h"
#include "td5_roam_mode.h"
#include "td5_roam_route.h"
#include "td5_roam_barrier.h"
#include "td5_roam_race.h"
#include "../../../re/include/td5_actor_struct.h"

#define LOG_TAG "roam"

#define TW_M            430.0
#define TW_PI           3.14159265358979323846
#define TW_UPT_KMH      (430.0 / 30.0 / 3.6)     /* units per tick at 1 km/h */
#define TW_CELL         (1.0 * TW_M)
#define TW_CORRIDOR_M   70.0
#define TW_ERODE        (0.8 * TW_M)
#define TW_EXT          (0.3 * TW_M)             /* a barrier cuts the fill 0.3 m past each end */
#define TW_RUN_TICKS    75
#define TW_MAX_KEYS     12

/* Mariano's geo_map sprint of 2026-10-19 (routes/geo_map.json): Calle 7 / Diagonal 74 north, 2.2 km */
static const double k_default_wp[][2] = {
    { -34.92269936, -57.95484511 },
    { -34.90728248, -57.95505639 },
};

typedef struct {
    int    bar;                 /* barrier index */
    int    aim;                 /* 0 middle, 1 end A, 2 end B */
    double kmh;
    double sx, sz, hx, hz;      /* start (world units) and heading */
    double tx, tz;              /* pursuit target (world units) */
    double run_m;               /* run-up */
    /* result */
    int    ran, leak;
    double max_past_m;          /* chassis centre past the panel line inside its length */
    double max_arm_m;           /* chassis on the closed arm's ribbon, past the line */
    double end_route_m;         /* chassis distance from the route at the end */
    double ex, ez;              /* end position (world) */
    long   contacts;
    char   why[48];
} TwAttack;

typedef struct { int n; int keys; uint64_t key[TW_MAX_KEYS]; double ex, ez; int linked_only; } TwRegion;

static struct {
    int      armed, phase, done;
    long     frames, pf;
    double   ll[TD5_ROUTE_MAX_WP][2];
    int      nwp;
    /* route copy (place frame) */
    double  *rx, *rz;
    int      npt;
    uint64_t *rkeys; int nrkeys;       /* route edge keys + the clusters the route passes (patches) */
    uint64_t *lkeys; int nlkeys;       /* linked arms */
    /* flood */
    int      gw, gh;
    double   gx0, gz0;
    uint8_t *cell;                      /* bit0 corridor, bit1 passable, bit2 route surface, bit3 linked, bit4 reached, bit5 leak, bit6 grouped */
    uint64_t *ckey;
    TwRegion reg[64];
    int      nreg, nreg_leak;
    long     n_corr, n_pass, n_reach, n_leak;
    double   flood_ms;
    /* attacks */
    TwAttack *at;
    int      nat, cur, skipped;
    long     t_run;
    int      leaks;
    FILE    *acsv, *trc;
} T;

/* ------------------------------------------------------------ helpers ------------ */

static int cmp_u64(const void *a, const void *b)
{
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y);
}
static int has_key(const uint64_t *k, int n, uint64_t v)
{
    int a = 0, b = n;
    while (a < b) { const int m = (a + b) / 2; if (k[m] < v) a = m + 1; else b = m; }
    return a < n && k[a] == v;
}

static int tw_locate(double wx, double wz, TD5_RoamHit *h)
{
    const TD5_RoamResident *rs = td5_roam_resident();
    if (!rs) return 0;
    if (!td5_roam_locate(rs, (int32_t)floor(wx * 256.0 + 0.5), (int32_t)floor(wz * 256.0 + 0.5), 0, 0, ROAM_PRIM_NONE, h))
        return 0;
    return h->prim != ROAM_PRIM_NONE && h->inside;
}

/* least distance (units) from a place-frame point to the route polyline */
static double tw_route_dist(double px, double pz)
{
    double best = 1e300;
    int i;
    for (i = 0; i + 1 < T.npt; i++) {
        const double ax = T.rx[i], az = T.rz[i], ex = T.rx[i + 1] - ax, ez = T.rz[i + 1] - az;
        const double l2 = ex * ex + ez * ez;
        double t = l2 > 0.0 ? ((px - ax) * ex + (pz - az) * ez) / l2 : 0.0, dx, dz;
        if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
        dx = ax + ex * t - px; dz = az + ez * t - pz;
        if (dx * dx + dz * dz < best) best = dx * dx + dz * dz;
    }
    return sqrt(best);
}

static int seg_x(double ax, double az, double bx, double bz, double cx, double cz, double dx, double dz)
{
    const double rx = bx - ax, rz = bz - az, sx = dx - cx, sz = dz - cz;
    const double den = rx * sz - rz * sx;
    double t, u;
    if (fabs(den) < 1e-9) return 0;
    t = ((cx - ax) * sz - (cz - az) * sx) / den;
    u = ((cx - ax) * rz - (cz - az) * rx) / den;
    return t >= 0.0 && t <= 1.0 && u >= 0.0 && u <= 1.0;
}

/* ------------------------------------------------------------ route ------------ */

static int tw_plan(void)
{
    int i, ok = 1;
    td5_roam_route_clear();
    for (i = 0; i < T.nwp && ok; i++) {
        int32_t px, pz;
        if (!td5_roam_route_latlon_to_place(T.ll[i][0], T.ll[i][1], &px, &pz)) { ok = 0; break; }
        if (i == 0) ok = td5_roam_route_set_start_at(px, pz);
        else if (i == T.nwp - 1) ok = td5_roam_route_set_finish_at(px, pz);
        else ok = td5_roam_route_add_at(px, pz);
    }
    if (ok) td5_roam_route_set_type(TD5_ROUTE_SPRINT, 1);
    return ok && td5_roam_route_result()->ok;
}

static void tw_copy_route(void)
{
    const TD5_RouteResult *r = td5_roam_route_result();
    int i, n, cap;
    T.rx = (double *)malloc((size_t)r->npt * sizeof(double));
    T.rz = (double *)malloc((size_t)r->npt * sizeof(double));
    memcpy(T.rx, r->px, (size_t)r->npt * sizeof(double));
    memcpy(T.rz, r->pz, (size_t)r->npt * sizeof(double));
    T.npt = r->npt;
    /* the route's own surface: its edges + every cluster any barrier / arm record names (the passes) */
    n = td5_roam_barriers_dev_arm(-1, NULL, NULL, NULL);
    cap = r->nedge + n + 8;
    T.rkeys = (uint64_t *)malloc((size_t)cap * sizeof(uint64_t));
    T.lkeys = (uint64_t *)malloc((size_t)(n + 1) * sizeof(uint64_t));
    T.nrkeys = 0; T.nlkeys = 0;
    for (i = 0; i < r->nedge; i++) T.rkeys[T.nrkeys++] = r->edge[i].key;
    for (i = 0; i < n; i++) {
        int st;
        uint64_t ek, ck;
        td5_roam_barriers_dev_arm(i, &st, &ek, &ck);
        T.rkeys[T.nrkeys++] = ck;
        if (st == 2) T.lkeys[T.nlkeys++] = ek;
    }
    qsort(T.rkeys, (size_t)T.nrkeys, sizeof(uint64_t), cmp_u64);
    qsort(T.lkeys, (size_t)T.nlkeys, sizeof(uint64_t), cmp_u64);
}

/* ------------------------------------------------------------ flood ------------ */

#define C_CORR  1
#define C_PASS  2
#define C_ROUTE 4
#define C_LINK  8
#define C_REACH 16
#define C_LEAK  32
#define C_GRP   64

static void tw_flood(FILE *rep)
{
    const uint64_t t0 = td5_plat_time_us();
    int32_t ox, oz;
    double minx = 1e300, minz = 1e300, maxx = -1e300, maxz = -1e300;
    int i, x, z, nb = 0;
    const TD5_RoamBarrier *b = td5_roam_barriers(&nb);
    int *queue = NULL, qh = 0, qt = 0;
    const double corr = TW_CORRIDOR_M * TW_M;
    td5_roam_world_origin(&ox, &oz);
    for (i = 0; i < T.npt; i++) {
        if (T.rx[i] < minx) minx = T.rx[i];
        if (T.rx[i] > maxx) maxx = T.rx[i];
        if (T.rz[i] < minz) minz = T.rz[i];
        if (T.rz[i] > maxz) maxz = T.rz[i];
    }
    T.gx0 = minx - corr; T.gz0 = minz - corr;
    T.gw = (int)((maxx - minx + 2.0 * corr) / TW_CELL) + 1;
    T.gh = (int)((maxz - minz + 2.0 * corr) / TW_CELL) + 1;
    T.cell = (uint8_t *)calloc((size_t)T.gw * (size_t)T.gh, 1);
    T.ckey = (uint64_t *)calloc((size_t)T.gw * (size_t)T.gh, sizeof(uint64_t));
    queue = (int *)malloc((size_t)T.gw * (size_t)T.gh * sizeof(int));
    if (!T.cell || !T.ckey || !queue) { TD5_LOG_E(LOG_TAG, "[ROAMWALLS] flood: out of memory (%dx%d)", T.gw, T.gh); free(queue); return; }
    /* the corridor: discs round route samples every 5 m */
    for (i = 0; i + 1 < T.npt; i++) {
        const double ex = T.rx[i + 1] - T.rx[i], ez = T.rz[i + 1] - T.rz[i], l = sqrt(ex * ex + ez * ez);
        const int ns = 1 + (int)(l / (5.0 * TW_M));
        int s;
        for (s = 0; s <= ns; s++) {
            const double px = T.rx[i] + ex * (double)s / ns, pz = T.rz[i] + ez * (double)s / ns;
            const int cx = (int)((px - T.gx0) / TW_CELL), cz = (int)((pz - T.gz0) / TW_CELL);
            const int rr = (int)(corr / TW_CELL);
            for (z = cz - rr; z <= cz + rr; z++) {
                if (z < 0 || z >= T.gh) continue;
                for (x = cx - rr; x <= cx + rr; x++) {
                    if (x < 0 || x >= T.gw) continue;
                    if ((double)((x - cx) * (x - cx) + (z - cz) * (z - cz)) <= (double)rr * rr) T.cell[(size_t)z * T.gw + x] |= C_CORR;
                }
            }
        }
    }
    /* passable cells + what they are on */
    for (z = 0; z < T.gh; z++)
        for (x = 0; x < T.gw; x++) {
            const size_t ci = (size_t)z * T.gw + x;
            const double px = T.gx0 + (x + 0.5) * TW_CELL, pz = T.gz0 + (z + 0.5) * TW_CELL;
            const double wx = px - ox, wz = pz - oz;
            TD5_RoamHit h, h2;
            if (!(T.cell[ci] & C_CORR)) continue;
            T.n_corr++;
            if (!tw_locate(wx, wz, &h)) continue;
            if (!tw_locate(wx + TW_ERODE, wz, &h2) || !tw_locate(wx - TW_ERODE, wz, &h2) ||
                !tw_locate(wx, wz + TW_ERODE, &h2) || !tw_locate(wx, wz - TW_ERODE, &h2)) continue;
            T.cell[ci] |= C_PASS;
            T.ckey[ci] = h.edge_key;
            T.n_pass++;
            if (has_key(T.rkeys, T.nrkeys, h.edge_key)) T.cell[ci] |= C_ROUTE;
            else if (has_key(T.lkeys, T.nlkeys, h.edge_key)) T.cell[ci] |= C_LINK;
        }
    /* seeds: passable cells under the route polyline */
    for (i = 0; i + 1 < T.npt; i++) {
        const double ex = T.rx[i + 1] - T.rx[i], ez = T.rz[i + 1] - T.rz[i], l = sqrt(ex * ex + ez * ez);
        const int ns = 1 + (int)(l / (0.5 * TW_M));
        int s;
        for (s = 0; s <= ns; s++) {
            const int cx = (int)((T.rx[i] + ex * (double)s / ns - T.gx0) / TW_CELL);
            const int cz = (int)((T.rz[i] + ez * (double)s / ns - T.gz0) / TW_CELL);
            const size_t ci = (size_t)cz * T.gw + cx;
            if (cx < 0 || cz < 0 || cx >= T.gw || cz >= T.gh) continue;
            if ((T.cell[ci] & C_PASS) && !(T.cell[ci] & C_REACH)) { T.cell[ci] |= C_REACH; queue[qt++] = (int)ci; }
        }
    }
    /* fill */
    while (qh < qt) {
        static const int dx4[4] = { 1, -1, 0, 0 }, dz4[4] = { 0, 0, 1, -1 };
        const int ci = queue[qh++], cx = ci % T.gw, cz = ci / T.gw;
        const double ax = T.gx0 + (cx + 0.5) * TW_CELL - ox, az = T.gz0 + (cz + 0.5) * TW_CELL - oz;
        int k;
        for (k = 0; k < 4; k++) {
            const int nx = cx + dx4[k], nz = cz + dz4[k];
            size_t ni;
            int j, cut = 0;
            double bx, bz;
            if (nx < 0 || nz < 0 || nx >= T.gw || nz >= T.gh) continue;
            ni = (size_t)nz * T.gw + nx;
            if (!(T.cell[ni] & C_PASS) || (T.cell[ni] & C_REACH)) continue;
            bx = ax + dx4[k] * TW_CELL; bz = az + dz4[k] * TW_CELL;
            for (j = 0; j < nb && !cut; j++) {
                double p0x, p0z, p1x, p1z, ux, uz, l;
                if (b[j].kind != TD5_ROAM_BARRIER_MOUTH) continue;
                p0x = b[j].x0 / 256.0; p0z = b[j].z0 / 256.0; p1x = b[j].x1 / 256.0; p1z = b[j].z1 / 256.0;
                if ((ax < p0x - 3000 && ax < p1x - 3000) || (ax > p0x + 3000 && ax > p1x + 3000) ||
                    (az < p0z - 3000 && az < p1z - 3000) || (az > p0z + 3000 && az > p1z + 3000)) continue;
                ux = p1x - p0x; uz = p1z - p0z; l = sqrt(ux * ux + uz * uz);
                if (l < 1.0) continue;
                ux /= l; uz /= l;
                if (seg_x(ax, az, bx, bz, p0x - ux * TW_EXT, p0z - uz * TW_EXT, p1x + ux * TW_EXT, p1z + uz * TW_EXT)) cut = 1;
            }
            if (cut) continue;
            T.cell[ni] |= C_REACH;
            queue[qt++] = (int)ni;
        }
    }
    T.n_reach = qt;
    /* leak cells + regions. A closed arm's cells on the OPEN side of its own panel, up to 25 m before it (the stub
     * between the junction and a barrier, longer when the barrier slid out along the arm), belong to the junction. */
    for (i = 0; i < qt; i++) {
        const int ci = queue[i];
        int j, stub = 0;
        if (T.cell[ci] & (C_ROUTE | C_LINK)) continue;
        for (j = 0; j < nb && !stub; j++) {
            uint64_t ak = 0;
            double p0x, p0z, ux, uz, l, onx, onz, t, d;
            const double cxw = T.gx0 + (ci % T.gw + 0.5) * TW_CELL - ox, czw = T.gz0 + (ci / T.gw + 0.5) * TW_CELL - oz;
            if (b[j].kind != TD5_ROAM_BARRIER_MOUTH || !td5_roam_barrier_arm(j, &ak, NULL) || ak != T.ckey[ci]) continue;
            if (!td5_roam_barrier_open_dir(j, &onx, &onz)) continue;
            p0x = b[j].x0 / 256.0; p0z = b[j].z0 / 256.0;
            ux = b[j].x1 / 256.0 - p0x; uz = b[j].z1 / 256.0 - p0z; l = sqrt(ux * ux + uz * uz);
            if (l < 1.0) continue;
            ux /= l; uz /= l;
            { const double nx = -uz, nz = ux, sg = onx * nx + onz * nz; onx = sg >= 0.0 ? nx : -nx; onz = sg >= 0.0 ? nz : -nz; }
            t = (cxw - p0x) * ux + (czw - p0z) * uz;
            d = (cxw - p0x) * onx + (czw - p0z) * onz;
            if (t >= -1.0 * TW_M && t <= l + 1.0 * TW_M && d >= -0.5 * TW_M && d <= 25.0 * TW_M) stub = 1;
        }
        if (stub) { T.cell[ci] |= C_ROUTE; continue; }
        T.cell[ci] |= C_LEAK; T.n_leak++;
    }
    for (i = 0; i < qt; i++) {
        const int c0 = queue[i];
        int rh = 0, rt = 0, *rq = queue + qt;      /* the queue's tail is free */
        TwRegion rg;
        double bestd = -1.0;
        if (!(T.cell[c0] & (C_LEAK | C_LINK)) || (T.cell[c0] & C_GRP)) continue;
        if ((size_t)qt * 2 > (size_t)T.gw * T.gh) break;
        memset(&rg, 0, sizeof rg);
        rg.linked_only = 1;
        T.cell[c0] |= C_GRP;
        rq[rt++] = c0;
        while (rh < rt) {
            static const int dx4[4] = { 1, -1, 0, 0 }, dz4[4] = { 0, 0, 1, -1 };
            const int ci = rq[rh++], cx = ci % T.gw, cz = ci / T.gw;
            int k, have = 0;
            rg.n++;
            if (T.cell[ci] & C_LEAK) rg.linked_only = 0;
            for (k = 0; k < rg.keys; k++) if (rg.key[k] == T.ckey[ci]) have = 1;
            if (!have && rg.keys < TW_MAX_KEYS) rg.key[rg.keys++] = T.ckey[ci];
            for (k = 0; k < 4; k++) {
                const int nx = cx + dx4[k], nz = cz + dz4[k];
                size_t ni;
                if (nx < 0 || nz < 0 || nx >= T.gw || nz >= T.gh) continue;
                ni = (size_t)nz * T.gw + nx;
                if ((T.cell[ni] & C_REACH) && (T.cell[ni] & C_ROUTE)) {
                    /* the way out: the region cell next to the route surface farthest from the route line */
                    const double px = T.gx0 + (cx + 0.5) * TW_CELL, pz = T.gz0 + (cz + 0.5) * TW_CELL;
                    const double d = tw_route_dist(px, pz);
                    if (bestd < 0.0 || d < bestd) { bestd = d; rg.ex = px; rg.ez = pz; }
                }
                if (!(T.cell[ni] & C_REACH) || !(T.cell[ni] & (C_LEAK | C_LINK)) || (T.cell[ni] & C_GRP)) continue;
                T.cell[ni] |= C_GRP;
                rq[rt++] = (int)ni;
            }
        }
        if (bestd < 0.0) { rg.ex = T.gx0 + (c0 % T.gw + 0.5) * TW_CELL; rg.ez = T.gz0 + (c0 / T.gw + 0.5) * TW_CELL; }
        if (!rg.linked_only && rg.n >= 4) T.nreg_leak++;
        if (T.nreg < 64) T.reg[T.nreg++] = rg;
    }
    T.flood_ms = (double)(td5_plat_time_us() - t0) / 1000.0;
    TD5_LOG_I(LOG_TAG, "[ROAMWALLS] flood: grid %dx%d, corridor %ld cells, passable %ld, reached %ld, leak cells %ld, regions %d "
              "(%d leaking, >= 4 cells), %.0f ms", T.gw, T.gh, T.n_corr, T.n_pass, T.n_reach, T.n_leak, T.nreg, T.nreg_leak, T.flood_ms);
    if (rep) {
        fprintf(rep, "FLOOD (1 m cells, car-passable = centre + 4 points 0.8 m away on the road, barriers cut 0.3 m past each end)\n");
        fprintf(rep, "  grid %dx%d, corridor %ld cells (%.0f m round the route), passable %ld, reached from the route %ld\n",
                T.gw, T.gh, T.n_corr, TW_CORRIDOR_M, T.n_pass, T.n_reach);
        fprintf(rep, "  leak cells (reached, not the route's own surface, not a linked arm) %ld, regions %d, leaking regions (>= 4 cells) %d\n",
                T.n_leak, T.nreg, T.nreg_leak);
        for (i = 0; i < T.nreg; i++) {
            const TwRegion *rg = &T.reg[i];
            double lat = 0, lon = 0;
            int k;
            td5_roam_route_place_to_latlon(rg->ex, rg->ez, &lat, &lon);
            fprintf(rep, "  region %d: %s %d cells, way out place (%.0f,%.0f) = %.6f,%.6f, %.1f m from the route line, edges:",
                    i, rg->linked_only ? "linked-only" : (rg->n >= 4 ? "LEAK" : "sliver"), rg->n, rg->ex, rg->ez, lat, lon,
                    tw_route_dist(rg->ex, rg->ez) / TW_M);
            for (k = 0; k < rg->keys; k++) fprintf(rep, " %016llx", (unsigned long long)rg->key[k]);
            fprintf(rep, "\n");
        }
    }
    {   /* the reached / leak cells for plotting */
        char path[512];
        const char *dir = td5_plat_log_dir();
        FILE *f;
        snprintf(path, sizeof path, "%sroam_wall_flood.csv", dir ? dir : "");
        if ((f = fopen(path, "w")) != NULL) {
            fprintf(f, "px,pz,flags\n");
            for (z = 0; z < T.gh; z++)
                for (x = 0; x < T.gw; x++) {
                    const uint8_t c = T.cell[(size_t)z * T.gw + x];
                    if (!(c & C_PASS)) continue;
                    fprintf(f, "%.0f,%.0f,%d\n", T.gx0 + (x + 0.5) * TW_CELL, T.gz0 + (z + 0.5) * TW_CELL, c);
                }
            for (i = 0; i < nb; i++)
                fprintf(f, "#bar,%d,%.0f,%.0f,%.0f,%.0f\n", b[i].kind, b[i].x0 / 256.0 + ox, b[i].z0 / 256.0 + oz,
                        b[i].x1 / 256.0 + ox, b[i].z1 / 256.0 + oz);
            for (i = 0; i < T.npt; i++) fprintf(f, "#pt,%.0f,%.0f\n", T.rx[i], T.rz[i]);
            fclose(f);
        }
    }
    free(queue);
}

/* ------------------------------------------------------------ attacks ------------ */

static int tw_runway_ok(double px, double pz, double hx, double hz, double len)
{
    int k;
    TD5_RoamHit h;
    for (k = 0; k <= 8; k++) {
        const double f = len * (double)k / 8.0;
        if (!tw_locate(px + hx * f, pz + hz * f, &h)) return 0;
    }
    /* the car's footprint at the start: 1 m either side */
    return tw_locate(px - hz * 1.0 * TW_M, pz + hx * 1.0 * TW_M, &h) && tw_locate(px + hz * 1.0 * TW_M, pz - hx * 1.0 * TW_M, &h);
}

static void tw_plan_attacks(void)
{
    int nb = 0, i, k;
    const TD5_RoamBarrier *b = td5_roam_barriers(&nb);
    T.at = (TwAttack *)calloc((size_t)(nb * 3 + 1), sizeof(TwAttack));
    T.nat = 0;
    if (!T.at) return;
    for (i = 0; i < nb; i++) {
        double ax, az, ux, uz, len, onx, onz;
        if (b[i].kind != TD5_ROAM_BARRIER_MOUTH || !td5_roam_barrier_open_dir(i, &onx, &onz)) continue;
        {   /* TD5RE_ROAM_WALL_ONLY=i,j,...: only these barriers */
            const char *only = getenv("TD5RE_ROAM_WALL_ONLY");
            if (only && only[0]) {
                char buf[16];
                const char *q = only;
                int hit = 0;
                snprintf(buf, sizeof buf, "%d", i);
                while (*q) {
                    size_t l = strcspn(q, ",");
                    if (l == strlen(buf) && !strncmp(q, buf, l)) hit = 1;
                    q += l; if (*q == ',') q++;
                }
                if (!hit) continue;
            }
        }
        ax = b[i].x0 / 256.0; az = b[i].z0 / 256.0;
        ux = b[i].x1 / 256.0 - ax; uz = b[i].z1 / 256.0 - az; len = sqrt(ux * ux + uz * uz);
        if (len < 1.0) continue;
        ux /= len; uz /= len;
        /* the open direction must be the panel normal's side: project it onto the normal */
        {
            const double nx = -uz, nz = ux, d = onx * nx + onz * nz;
            onx = d >= 0.0 ? nx : -nx; onz = d >= 0.0 ? nz : -nz;
        }
        for (k = 0; k < 3; k++) {
            TwAttack *t = &T.at[T.nat];
            const double s30 = 0.5, c30 = 0.8660254;
            double px, pz, hx, hz, run;
            int ok = 0;
            static const double k_run[4] = { 14.0, 10.0, 7.0, 5.0 };
            int r;
            if (k == 0) { px = ax + ux * len * 0.5; pz = az + uz * len * 0.5; hx = -onx; hz = -onz; }
            else if (k == 1) { px = ax + ux * 0.6 * TW_M; pz = az + uz * 0.6 * TW_M; hx = -onx * c30 - ux * s30; hz = -onz * c30 - uz * s30; }
            else { px = ax + ux * (len - 0.6 * TW_M); pz = az + uz * (len - 0.6 * TW_M); hx = -onx * c30 + ux * s30; hz = -onz * c30 + uz * s30; }
            for (r = 0; r < 4 && !ok; r++) {
                run = k_run[r] * TW_M;
                if (tw_runway_ok(px - hx * run, pz - hz * run, hx, hz, run - 1.0 * TW_M)) ok = 1;
            }
            if (!ok) { T.skipped++; continue; }
            memset(t, 0, sizeof *t);
            t->bar = i; t->aim = k; t->kmh = k == 0 ? 90.0 : 50.0;
            t->run_m = run / TW_M;
            {   /* stored in the PLACE frame: the attacks cross the route, a world-origin rebase must not move them */
                int32_t ox, oz;
                td5_roam_world_origin(&ox, &oz);
                t->sx = px - hx * run + ox; t->sz = pz - hz * run + oz; t->hx = hx; t->hz = hz;
                t->tx = px + hx * 20.0 * TW_M + ox; t->tz = pz + hz * 20.0 * TW_M + oz;
            }
            T.nat++;
        }
    }
    TD5_LOG_I(LOG_TAG, "[ROAMWALLS] %d attack runs planned over %d barriers (%d aims skipped: no 5 m run-up on the road)", T.nat, nb, T.skipped);
}

static void tw_start_run(TD5_Actor *a, TwAttack *t)
{
    int32_t ox, oz;
    const double v = t->kmh * TW_UPT_KMH;
    const int yaw = (int)floor(atan2(t->hx, t->hz) * 4096.0 / (2.0 * TW_PI) + 0.5) & 0xFFF;
    TD5_RoamRaceWallStats ws;
    td5_roam_world_origin(&ox, &oz);
    td5_damage_repair_actor(0);
    td5_roam_place_actor(0, a, (int32_t)floor(t->sx + 0.5), (int32_t)floor(t->sz + 0.5), yaw);
    a->linear_velocity_x = (int32_t)(t->hx * v * 256.0);
    a->linear_velocity_z = (int32_t)(t->hz * v * 256.0);
    td5_roam_race_wall_stats(&ws);
    t->contacts = -ws.contacts;
    t->ran = 1;
    T.t_run = 0;
}

static void tw_measure(TD5_Actor *a, TwAttack *t)
{
    int nb = 0;
    const TD5_RoamBarrier *b = td5_roam_barriers(&nb);
    const TD5_RoamBarrier *br = &b[t->bar];
    const double cx = a->world_pos.x / 256.0, cz = a->world_pos.z / 256.0;
    const double ax = br->x0 / 256.0, az = br->z0 / 256.0;
    double ux = br->x1 / 256.0 - ax, uz = br->z1 / 256.0 - az, len = sqrt(ux * ux + uz * uz), onx, onz, tt, d;
    uint64_t arm = 0;
    TD5_RoamHit h;
    ux /= len; uz /= len;
    td5_roam_barrier_open_dir(t->bar, &onx, &onz);
    { const double nx = -uz, nz = ux, s = onx * nx + onz * nz; onx = s >= 0.0 ? nx : -nx; onz = s >= 0.0 ? nz : -nz; }
    tt = (cx - ax) * ux + (cz - az) * uz;
    d = (cx - ax) * onx + (cz - az) * onz;          /* + = open side */
    if (tt >= 0.0 && tt <= len && -d / TW_M > t->max_past_m) t->max_past_m = -d / TW_M;
    td5_roam_barrier_arm(t->bar, &arm, NULL);
    if (d < 0.0 && tw_locate(cx, cz, &h) && !h.is_patch && h.edge_key == arm && -d / TW_M > t->max_arm_m) t->max_arm_m = -d / TW_M;
    if (T.trc) {
        TD5_RoamRaceWallStats ws;
        const double vx = a->linear_velocity_x / 256.0, vz = a->linear_velocity_z / 256.0;
        td5_roam_race_wall_stats(&ws);
        fprintf(T.trc, "%d,%ld,%.2f,%.2f,%.1f,%ld,%ld,%d\n", (int)(t - T.at), T.t_run, tt / TW_M, d / TW_M,
                sqrt(vx * vx + vz * vz) / TW_UPT_KMH, ws.contacts, ws.hits, (int)(a->display_angles.yaw & 0xFFF));
    }
}

static void tw_end_run(TD5_Actor *a, TwAttack *t)
{
    int32_t ox, oz;
    TD5_RoamRaceWallStats ws;
    double ds;
    td5_roam_world_origin(&ox, &oz);
    t->ex = a->world_pos.x / 256.0 + ox; t->ez = a->world_pos.z / 256.0 + oz;     /* place frame */
    t->end_route_m = tw_route_dist(t->ex, t->ez) / TW_M;
    ds = sqrt((t->ex - t->sx) * (t->ex - t->sx) + (t->ez - t->sz) * (t->ez - t->sz)) / TW_M;
    td5_roam_race_wall_stats(&ws);
    t->contacts += ws.contacts;
    if (t->max_past_m > 1.0) { t->leak = 1; snprintf(t->why, sizeof t->why, "chassis %.1f m past the panel", t->max_past_m); }
    else if (t->max_arm_m > 1.0) { t->leak = 1; snprintf(t->why, sizeof t->why, "on the closed arm %.1f m past", t->max_arm_m); }
    else if (t->end_route_m > 25.0 && ds > 25.0) { t->leak = 1; snprintf(t->why, sizeof t->why, "%.0f m from the route", t->end_route_m); }
    else snprintf(t->why, sizeof t->why, "held");
    if (t->leak) T.leaks++;
    if (T.acsv) {
        int nb = 0;
        const TD5_RoamBarrier *b = td5_roam_barriers(&nb);
        fprintf(T.acsv, "%d,%d,%016llx,%d,%.0f,%.1f,%.0f,%.0f,%.0f,%.0f,%.2f,%.2f,%.1f,%ld,%d,%s\n", (int)(t - T.at), t->bar,
                (unsigned long long)b[t->bar].cluster_key, t->aim, t->kmh, t->run_m, t->sx, t->sz, t->ex, t->ez,
                t->max_past_m, t->max_arm_m, t->end_route_m, t->contacts, t->leak, t->why);
    }
    if (t->leak)
        TD5_LOG_W(LOG_TAG, "[ROAMWALLS] attack %d LEAK: barrier %d aim %d at %.0f km/h: %s (end place %.0f,%.0f)", (int)(t - T.at),
                  t->bar, t->aim, t->kmh, t->why, t->ex, t->ez);
}

static uint32_t tw_steer(const TD5_Actor *a, double tx, double tz)
{
    const double px = a->world_pos.x / 256.0, pz = a->world_pos.z / 256.0;
    const double vx = a->linear_velocity_x / 256.0, vz = a->linear_velocity_z / 256.0, sp = sqrt(vx * vx + vz * vz);
    double hx, hz, err;
    uint32_t bits = (uint32_t)TD5_INPUT_THROTTLE;
    if (sp > 5.0 * TW_UPT_KMH) { hx = vx / sp; hz = vz / sp; }
    else { const double y = (double)(a->display_angles.yaw & 0xFFF) * (2.0 * TW_PI / 4096.0); hx = sin(y); hz = cos(y); }
    err = atan2(tx - px, tz - pz) - atan2(hx, hz);
    while (err > TW_PI) err -= 2.0 * TW_PI;
    while (err < -TW_PI) err += 2.0 * TW_PI;
    if (err > 0.03) bits |= (uint32_t)TD5_INPUT_STEER_LEFT;
    else if (err < -0.03) bits |= (uint32_t)TD5_INPUT_STEER_RIGHT;
    return bits;
}

/* ------------------------------------------------------------ report ------------ */

static void tw_report(void)
{
    char path[512];
    const char *dir = td5_plat_log_dir();
    FILE *f;
    TD5_RoamBarrierStats bs;
    TD5_RoamRaceWallStats ws;
    int i, ran = 0, held = 0;
    td5_roam_barriers_stats(&bs);
    td5_roam_race_wall_stats(&ws);
    for (i = 0; i < T.nat; i++) if (T.at[i].ran) { ran++; if (!T.at[i].leak) held++; }
    snprintf(path, sizeof path, "%sroam_wall_test.txt", dir ? dir : "");
    f = fopen(path, "a");
    if (f) {
        fprintf(f, "ATTACK: %d runs (%d aims skipped: no run-up), %d held, %d LEAKED; wall layer contacts %ld, open side from the build %ld, "
                   "by patch %ld, by chassis %ld, chassis past %ld (max %.2f m)\n",
                ran, T.skipped, held, T.leaks, ws.contacts, ws.side_route, ws.side_patch, ws.side_fallback, ws.chassis_past,
                ws.max_chassis_past_m);
        for (i = 0; i < T.nat; i++) {
            const TwAttack *t = &T.at[i];
            if (!t->ran || !t->leak) continue;
            fprintf(f, "  LEAK attack %d: barrier %d aim %d %.0f km/h run-up %.0f m: %s, end %.1f m from the route\n", i, t->bar,
                    t->aim, t->kmh, t->run_m, t->why, t->end_route_m);
        }
        fprintf(f, "VERDICT: %s (flood leaking regions %d, attack leaks %d)\n", (T.nreg_leak || T.leaks) ? "FAIL" : "PASS",
                T.nreg_leak, T.leaks);
        fclose(f);
    }
    TD5_LOG_I(LOG_TAG, "[ROAMWALLS] VERDICT %s: flood leaking regions %d (leak cells %ld), attack runs %d held %d leaked %d, "
              "barriers %d (mouth %d), arms open %d linked %d, contacts %ld, open side build/patch/chassis %ld/%ld/%ld",
              (T.nreg_leak || T.leaks) ? "FAIL" : "PASS", T.nreg_leak, T.n_leak, ran, held, T.leaks, bs.barriers_mouth + bs.barriers_turn,
              bs.barriers_mouth, bs.arms_open, bs.arms_linked, ws.contacts, ws.side_route, ws.side_patch, ws.side_fallback);
}

/* ------------------------------------------------------------ hooks ------------ */

void td5_roam_wall_test_frame(void)
{
    if (!T.armed && !T.done && td5_roam_active()) {
        const char *e = getenv("TD5RE_ROAM_WALL_TEST");
        if (!e || e[0] != '1') { T.done = 1; return; }
        T.armed = 1;
        T.nwp = 0;
        {
            const char *w = getenv("TD5RE_ROAM_WALL_WP");
            while (w && *w && T.nwp < TD5_ROUTE_MAX_WP) {
                char *end;
                const double la = strtod(w, &end);
                if (end == w || *end != ',') break;
                T.ll[T.nwp][0] = la; T.ll[T.nwp][1] = strtod(end + 1, &end); T.nwp++;
                w = end;
                if (*w == ';') w++;
            }
        }
        if (T.nwp < 2) {
            T.nwp = 2;
            memcpy(T.ll, k_default_wp, sizeof k_default_wp);
        }
        TD5_LOG_W(LOG_TAG, "[ROAMWALLS] armed: %d waypoints", T.nwp);
    }
    if (!T.armed || T.done || !td5_roam_active()) return;
    T.frames++; T.pf++;
    switch (T.phase) {
    case 0:
        if (!td5_roam_route_graph_ready() || T.frames < 20) {
            if (T.frames > 6000) { TD5_LOG_E(LOG_TAG, "[ROAMWALLS] planner graph never ready"); T.done = 1; g_td5.quit_requested = 1; }
            break;
        }
        if (!tw_plan()) { TD5_LOG_E(LOG_TAG, "[ROAMWALLS] the route did not plan"); T.done = 1; g_td5.quit_requested = 1; break; }
        {
            TD5_RoamRaceSetup s;
            memset(&s, 0, sizeof s);
            s.laps = 1;
            if (!td5_roam_race_start(&s)) { TD5_LOG_E(LOG_TAG, "[ROAMWALLS] race start refused"); T.done = 1; g_td5.quit_requested = 1; break; }
        }
        td5_roam_race_dev_hold_finish(1);
        tw_copy_route();
        T.phase = 1; T.pf = 0;
        break;
    case 1:
        if (td5_roam_race_state() != TD5_RRACE_RACING) {
            if (T.pf > 1800) { TD5_LOG_E(LOG_TAG, "[ROAMWALLS] never reached GO"); T.done = 1; g_td5.quit_requested = 1; }
            break;
        }
        {
            char path[512];
            const char *dir = td5_plat_log_dir();
            FILE *rep;
            TD5_RoamBarrierStats bs;
            td5_roam_barriers_stats(&bs);
            snprintf(path, sizeof path, "%sroam_wall_test.txt", dir ? dir : "");
            rep = fopen(path, "w");
            if (rep) {
                int i;
                fprintf(rep, "roam-walls: route %.1f m, %d edges, hash %08X, waypoints", td5_roam_route_result()->length_m,
                        td5_roam_route_result()->nedge, (unsigned)td5_roam_route_result()->hash);
                for (i = 0; i < T.nwp; i++) fprintf(rep, " %.6f,%.6f", T.ll[i][0], T.ll[i][1]);
                fprintf(rep, "\nbarriers: mouth %d, turn %d; clusters %d; arms total %d route %d blocked %d linked %d open %d, no row %d\n",
                        bs.barriers_mouth, bs.barriers_turn, bs.clusters_passed, bs.arms_total, bs.arms_route, bs.arms_blocked,
                        bs.arms_linked, bs.arms_open, bs.arm_no_row);
            }
            tw_flood(rep);
            if (rep) fclose(rep);
            snprintf(path, sizeof path, "%sroam_wall_attack.csv", dir ? dir : "");
            T.acsv = fopen(path, "w");
            if (T.acsv) fprintf(T.acsv, "run,barrier,cluster,aim,kmh,runup_m,start_px,start_pz,end_px,end_pz,max_past_m,max_arm_m,end_route_m,contacts,leak,why\n");
            tw_plan_attacks();
            snprintf(path, sizeof path, "%sroam_wall_trace.csv", dir ? dir : "");
            T.trc = fopen(path, "w");
            if (T.trc) fprintf(T.trc, "run,t,along_m,open_m,kmh,contacts,hits,yaw\n");
            if (td5_env_int("TD5RE_ROAM_WALL_ATTACK", 1, 0, 1) == 0) T.nat = 0;
            T.cur = -1;
            T.phase = 2; T.pf = 0;
        }
        break;
    case 3:
        if (T.acsv) { fclose(T.acsv); T.acsv = NULL; }
        if (T.trc) { fclose(T.trc); T.trc = NULL; }
        tw_report();
        td5_roam_race_dev_hold_finish(0);
        T.done = 1;
        g_td5.quit_requested = 1;
        break;
    default:
        break;
    }
}

int td5_roam_wall_test_bits(TD5_Actor *a, uint32_t *bits)
{
    static long s_last = -1;
    const long tick = (long)g_td5.simulation_tick_counter;
    TwAttack *t;
    if (!T.armed || T.done || T.phase != 2 || !a) return 0;
    *bits = 0;
    if (tick == s_last) return 1;
    s_last = tick;
    if (T.cur >= 0 && T.cur < T.nat) {
        t = &T.at[T.cur];
        T.t_run++;
        tw_measure(a, t);
        if (T.t_run < TW_RUN_TICKS) {
            int32_t ox, oz;
            td5_roam_world_origin(&ox, &oz);
            *bits = tw_steer(a, t->tx - ox, t->tz - oz);
            return 1;
        }
        tw_end_run(a, t);
    }
    T.cur++;
    if (T.cur >= T.nat) { T.phase = 3; return 1; }
    tw_start_run(a, &T.at[T.cur]);
    *bits = (uint32_t)TD5_INPUT_THROTTLE;
    return 1;
}

#endif /* !TD5RE_RELEASE */
