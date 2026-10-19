/**
 * td5_roam_gcensus.c -- GEO FREE ROAM: planner street graph vs drivable surface census
 * (PORT-ONLY, DEV-ONLY, G2). Contract: td5_roam_gcensus.h.
 *
 * Every planner graph edge's centre line is sampled every 2 m and tested against the drivable
 * surface of the resident casco ROAMTILE set (td5_roam_collect: ribbon quads + junction patch
 * triangles, edges inclusive). Routes, traffic lanes, AI paths and cop legs all follow these
 * centre lines, so a stretch with no road under it is a place a car is steered off the road.
 *
 * Off-road RUN = consecutive samples with no drivable primitive under them. Cause class of a run:
 *   NOSURF  the edge is off-road for >= 90 % of its length (no usable ribbon under the line)
 *   MEDIAN  the line is bounded by drivable surface on both sides within 12 m (a divided
 *           carriageway's centre line sitting in the median)
 *   END     the run touches an end of the edge (within 30 m): a junction jog / ribbon vs cut mismatch
 *   MID     anything else (ribbon outside the cut, the line leaving its own ribbon)
 * The A* joins two edges through the node (rr_connector): the TURN PATHS (arm end -> via points -> arm end, every turn a car can make)
 * are sampled the same way. A node next to a tile with no file (the casco border) has no patch in the set: reported as BORDER, not judged.
 * TD5RE_ROAM_GRAPH_CENSUS=1 runs it and exits. Output: <log dir>/roam_graph_census.txt (+ .csv),
 * one `GCENSUS:` summary line in the log. Verdict PASS = 0 edges with a run > 5 m and 0 interior nodes with a turn run > 15 m.
 */
#ifndef TD5RE_RELEASE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_platform.h"
#include "td5_config.h"
#include "td5_geo.h"
#include "td5_roam_tile.h"
#include "td5_roam_locate.h"
#include "td5_roam_route.h"
#include "td5_roam_gcensus.h"

#define LOG_TAG "roam"
#define GC_UPM        430.0
#define GC_STEP_M     2.0
#define GC_RUN_OK_M   5.0
#define GC_END_M      30.0
#define GC_JUNC_OK_M  15.0          /* a junction patch with an island may leave this much of a straight connector off the paved surface */
#define GC_MAX_TILES  2048

typedef struct {
    int      edge;
    uint64_t key;
    double   off_m, len_m, run_m, run_at_m;   /* off-road metres, edge length, longest run, its start */
    int      cls;
    double   px, pz;                           /* middle of the longest run, place frame */
    int      klass, lanes, oneway;
} GcRow;

enum { GC_NOSURF, GC_MEDIAN, GC_END, GC_MID, GC_NCLS };
static const char *const k_cls[GC_NCLS] = { "NOSURF", "MEDIAN", "END", "MID" };

static int gc_cmp_row(const void *a, const void *b)
{
    const GcRow *x = (const GcRow *)a, *y = (const GcRow *)b;
    if (x->run_m != y->run_m) return x->run_m > y->run_m ? -1 : 1;
    return x->edge < y->edge ? -1 : (x->edge > y->edge ? 1 : 0);
}

static int gc_on_road(const TD5_RoamResident *rs, double x, double z)
{
    TD5_RoamCand c[2];
    return td5_roam_collect(rs, (int32_t)floor((x - rs->org_x) * 256.0 + 0.5), (int32_t)floor((z - rs->org_z) * 256.0 + 0.5), c, 2) > 0;
}

/* at the centre of a run: road on BOTH sides of the line within 12 m */
static int gc_median(const TD5_RoamResident *rs, const TD5_RouteGEdge *ev, double at_m, double x, double z)
{
    double tx = 1.0, tz = 0.0, c = 0.0;
    int i, left = 0, right = 0;
    for (i = 0; i + 1 < ev->npt; i++) {
        const double seg = ev->cum[i + 1] - ev->cum[i];
        if (ev->cum[i + 1] / GC_UPM >= at_m || i + 2 == ev->npt) {
            tx = (double)ev->pt[2 * (i + 1)] - ev->pt[2 * i];
            tz = (double)ev->pt[2 * (i + 1) + 1] - ev->pt[2 * i + 1];
            c = seg;
            break;
        }
    }
    if (c > 0.0) { const double n = sqrt(tx * tx + tz * tz); if (n > 0.0) { tx /= n; tz /= n; } }
    for (i = 1; i <= 6; i++) {
        const double d = 2.0 * i * GC_UPM;
        if (!left  && gc_on_road(rs, x - tz * d, z + tx * d)) left = 1;
        if (!right && gc_on_road(rs, x + tz * d, z - tx * d)) right = 1;
    }
    return left && right;
}

/* longest off-road run (metres) along the segment a -> b, place frame, sampled every GC_STEP_M */
static double gc_seg_run(const TD5_RoamResident *rs, double ax, double az, double bx, double bz, double *off_total)
{
    const double len = hypot(bx - ax, bz - az) / GC_UPM;
    double d, run = 0.0, cur = 0.0;
    for (d = 0.0;; d += GC_STEP_M) {
        const double dd = d > len ? len : d, t = len > 0.0 ? dd / len : 0.0;
        if (!gc_on_road(rs, ax + t * (bx - ax), az + t * (bz - az))) {
            cur += GC_STEP_M; *off_total += GC_STEP_M;
            if (cur > run) run = cur;
        } else cur = 0.0;
        if (dd >= len) break;
    }
    return run;
}

typedef struct { int64_t key; double x, z; double run_m; int ninc; int centre_off; int nocent; int border; } GcNode;
static int gc_cmp_node(const void *a, const void *b)
{
    const GcNode *x = (const GcNode *)a, *y = (const GcNode *)b;
    if (x->run_m != y->run_m) return x->run_m > y->run_m ? -1 : 1;
    return x->key < y->key ? -1 : (x->key > y->key ? 1 : 0);
}

int td5_roam_graph_census_dev_test(void)
{
    static const char place[] = "la_plata_partido";
    TD5_RoamResident *rs;
    RoamTile *tile;
    RoamGrid grid;
    char path[512], err[160], txt[4096];
    int (*list)[2];
    int nlist, ntile = 0, i, e, nedge, k, noedge_recs = 0;
    double sx = 0.0, sz = 0.0;
    FILE *f, *rf, *csv;
    GcRow *rows;
    long samples = 0, off_samples = 0;
    int bad_edges = 0, edges_off = 0;
    int cls_edges[GC_NCLS] = { 0 }, cls_runs[GC_NCLS] = { 0 };
    double cls_m[GC_NCLS] = { 0 }, total_off_m = 0.0, total_km = 0.0;
    int g_ntile = 0;
    int conn_turns = 0, conn_bad_turns = 0, conn_nodes = 0, conn_nodes_off = 0, conn_nodes_bad = 0, conn_centre_off_bad = 0;
    double conn_off_m = 0.0;
    GcNode *conn_worst = NULL;
    int verdict_bad;
    int conn_nocent = 0, conn_off_nocent = 0, conn_bad_nocent = 0, conn_bad_border = 0, conn_bad_interior = 0, conn_bad_junc = 0;

    if (!td5_env_flag_off("TD5RE_ROAM_GRAPH_CENSUS")) return 0;
    rs = (TD5_RoamResident *)calloc(1, sizeof *rs);
    tile = (RoamTile *)calloc(GC_MAX_TILES, sizeof *tile);
    list = (int (*)[2])malloc(GC_MAX_TILES * sizeof(int[2]));
    if (!rs || !tile || !list) { TD5_LOG_E(LOG_TAG, "GCENSUS: out of memory"); return 1; }

    td5_geo_source_path(path, sizeof path, place, "roam/ROAMGRID.JSON");
    rf = fopen(path, "rb");
    if (!rf) { TD5_LOG_E(LOG_TAG, "GCENSUS: no %s (run --RoamBuildTiles=casco first)", path); return 1; }
    { const size_t nr = fread(txt, 1, sizeof txt - 1, rf); fclose(rf); txt[nr] = 0; }
    if (!roam_grid_parse(txt, &grid)) { TD5_LOG_E(LOG_TAG, "GCENSUS: ROAMGRID.JSON does not parse"); return 1; }
    nlist = td5_roam_casco_tiles(&grid, list, GC_MAX_TILES);
    for (i = 0; i < nlist; i++) {
        td5_roam_tile_path(path, sizeof path, NULL, place, list[i][0], list[i][1]);
        if (td5_roam_tile_read(path, &tile[ntile], 0, err, sizeof err)) {
            sx += tile[ntile].org_x; sz += tile[ntile].org_z;
            for (k = 0; k < (int)tile[ntile].nedge; k++) if (tile[ntile].edge[k].span_count == 0) noedge_recs++;
            ntile++;
        }
    }
    if (ntile == 0) { TD5_LOG_E(LOG_TAG, "GCENSUS: no tiles loaded"); return 1; }
    td5_roam_resident_init(rs, &grid, (int32_t)(sx / ntile), (int32_t)(sz / ntile), 0);
    for (i = 0; i < ntile; i++) td5_roam_resident_add(rs, &tile[i]);

    if (!td5_roam_route_graph_ensure(place)) { TD5_LOG_E(LOG_TAG, "GCENSUS: planner graph build failed"); return 1; }
    td5_roam_route_graph_stats(&nedge, NULL, NULL, NULL, NULL);
    { int ne; td5_roam_route_graph_stats(&ne, NULL, NULL, NULL, &total_km); }
    rows = (GcRow *)calloc((size_t)(nedge > 0 ? nedge : 1), sizeof *rows);
    if (!rows) return 1;
    (void)g_ntile;

    for (e = 0; e < nedge; e++) {
        TD5_RouteGEdge ev;
        GcRow *r = &rows[e];
        double len, d, run_start = -1.0, cur = 0.0;
        int seg = 0, nrun_here = 0;
        double run_at = 0.0, run_len = 0.0;
        double first_off_x = 0, first_off_z = 0;
        int run_cls_from_run = GC_MID;
        int ncand = 0;
        if (!td5_roam_route_graph_edge_view(e, &ev) || ev.npt < 2) continue;
        r->edge = e; r->key = ev.key; r->klass = ev.klass; r->lanes = ev.lanes; r->oneway = ev.oneway;
        len = ev.plen / GC_UPM;
        r->len_m = len;
        (void)ncand; (void)first_off_x; (void)first_off_z; (void)run_cls_from_run;
        /* walk the polyline every 2 m */
        for (d = 0.0;; d += GC_STEP_M) {
            double x, z, t;
            int on;
            const double dd = d > len ? len : d;
            while (seg + 2 < ev.npt && ev.cum[seg + 1] / GC_UPM < dd) seg++;
            {
                const double a = ev.cum[seg] / GC_UPM, b = ev.cum[seg + 1] / GC_UPM;
                t = b > a ? (dd - a) / (b - a) : 0.0;
                t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
                x = ev.pt[2 * seg]     + t * ((double)ev.pt[2 * (seg + 1)]     - ev.pt[2 * seg]);
                z = ev.pt[2 * seg + 1] + t * ((double)ev.pt[2 * (seg + 1) + 1] - ev.pt[2 * seg + 1]);
            }
            on = gc_on_road(rs, x, z);
            samples++;
            if (!on) {
                off_samples++;
                r->off_m += GC_STEP_M;
                if (run_start < 0.0) run_start = dd;
                cur = dd - run_start + GC_STEP_M;
                if (cur > run_len) { run_len = cur; run_at = run_start; r->px = x; r->pz = z; }
            } else {
                run_start = -1.0; cur = 0.0;
            }
            if (dd >= len) break;
            (void)nrun_here;
        }
        if (r->off_m > len) r->off_m = len;
        total_off_m += r->off_m;
        r->run_m = run_len; r->run_at_m = run_at;
        if (r->off_m > 0.0) edges_off++;
        if (run_len > GC_RUN_OK_M) {
            /* class of the LONGEST run (the worst offender), mid point re-derived from the run */
            double mid_m = run_at + 0.5 * run_len, mx, mz;
            int s2 = 0;
            while (s2 + 2 < ev.npt && ev.cum[s2 + 1] / GC_UPM < mid_m) s2++;
            {
                const double a = ev.cum[s2] / GC_UPM, b = ev.cum[s2 + 1] / GC_UPM;
                double t2 = b > a ? (mid_m - a) / (b - a) : 0.0;
                t2 = t2 < 0.0 ? 0.0 : (t2 > 1.0 ? 1.0 : t2);
                mx = ev.pt[2 * s2]     + t2 * ((double)ev.pt[2 * (s2 + 1)]     - ev.pt[2 * s2]);
                mz = ev.pt[2 * s2 + 1] + t2 * ((double)ev.pt[2 * (s2 + 1) + 1] - ev.pt[2 * s2 + 1]);
            }
            r->px = mx; r->pz = mz;
            if (r->off_m >= 0.9 * len) r->cls = GC_NOSURF;
            else if (gc_median(rs, &ev, mid_m, mx, mz)) r->cls = GC_MEDIAN;
            else if (run_at <= GC_END_M || run_at + run_len >= len - GC_END_M) r->cls = GC_END;
            else r->cls = GC_MID;
            bad_edges++;
            cls_edges[r->cls]++;
            cls_m[r->cls] += r->off_m;
        } else {
            r->cls = -1;
        }
    }
    /* ---- the turn paths: end of edge A -> node centre -> start of edge B, as the A* builds them (rr_connector adds the
     * centre only when the edge end is > 3 m from it). One entry per node: its worst turn. */
    {
        const int nnode = td5_roam_route_graph_nnode();
        int n, turns = 0, bad_turns = 0, nodes_off = 0, nodes_bad = 0, centre_off_bad = 0, off_nocent = 0, nocent_n = 0, bad_nocent = 0, bad_border = 0, bad_interior = 0, bad_junc = 0;
        double turn_off_m = 0.0;
        GcNode *nodes = (GcNode *)calloc((size_t)(nnode > 0 ? nnode : 1), sizeof *nodes);
        for (n = 0; nodes && n < nnode; n++) {
            TD5_RouteGNode nv;
            GcNode *N = &nodes[n];
            int a, b;
            if (!td5_roam_route_graph_node_view(n, &nv)) continue;
            N->key = nv.key; N->x = nv.x; N->z = nv.z; N->ninc = nv.ninc;
            N->centre_off = !gc_on_road(rs, nv.x, nv.z);
            N->nocent = nv.x == (double)(int32_t)(nv.key >> 32) * 10.0 && nv.z == (double)(int32_t)(uint32_t)(nv.key & 0xFFFFFFFFll) * 10.0;
            if (N->centre_off) { nodes_off++; if (N->nocent) off_nocent++; }
            if (N->nocent) nocent_n++;
            {   /* BORDER: a tile within 1 of the node's tile has no file, so its patch / ribbon is not in the set */
                int tx, tz, dx, dz;
                roam_grid_tile_of(&rs->grid, nv.x, nv.z, &tx, &tz);
                for (dz = -1; dz <= 1; dz++)
                    for (dx = -1; dx <= 1; dx++)
                        if (td5_roam_resident_find(rs, tx + dx, tz + dz) < 0) N->border = 1;
            }
            for (a = 0; a < nv.ninc; a++) {
                TD5_RouteGEdge ea;
                double ax, az;
                if (!td5_roam_route_graph_edge_view(nv.inc[a] >> 1, &ea)) continue;
                { const int p = (nv.inc[a] & 1) ? ea.npt - 1 : 0; ax = ea.pt[2 * p]; az = ea.pt[2 * p + 1]; }
                for (b = 0; b < nv.ninc; b++) {
                    TD5_RouteGEdge eb;
                    double bx, bz, run, off = 0.0;
                    if (b == a || (nv.inc[a] >> 1) == (nv.inc[b] >> 1) || !td5_roam_route_graph_edge_view(nv.inc[b] >> 1, &eb)) continue;
                    {   /* a turn the A* can make: arrive through arm a, leave through arm b, one-way respected */
                        const int d_in = (nv.inc[a] & 1) ? 0 : 1, d_out = (nv.inc[b] & 1) ? 1 : 0;
                        if ((ea.oneway > 0 && d_in != 0) || (ea.oneway < 0 && d_in != 1)) continue;
                        if ((eb.oneway > 0 && d_out != 0) || (eb.oneway < 0 && d_out != 1)) continue;
                    }
                    { const int p = (nv.inc[b] & 1) ? eb.npt - 1 : 0; bx = eb.pt[2 * p]; bz = eb.pt[2 * p + 1]; }
                    {   /* the connector the A* builds (rr_connector): the via points of the turn, each added when it is > 3 m from the point before */
                        double v[4], px = ax, pz = az;
                        const int nvia = td5_roam_route_graph_turn_via(n, nv.inc[a], nv.inc[b], v);
                        int q;
                        run = 0.0;
                        for (q = 0; q < nvia; q++) {
                            double r2;
                            if (hypot(v[2 * q] - px, v[2 * q + 1] - pz) <= 3.0 * GC_UPM) continue;
                            r2 = gc_seg_run(rs, px, pz, v[2 * q], v[2 * q + 1], &off);
                            if (r2 > run) run = r2;
                            px = v[2 * q]; pz = v[2 * q + 1];
                        }
                        {
                            const double r2 = gc_seg_run(rs, px, pz, bx, bz, &off);
                            if (r2 > run) run = r2;
                        }
                    }
                    if (run > GC_RUN_OK_M && !N->border && getenv("TD5RE_GC_DEBUG")) {
                        double v[4];
                        const int nvia = td5_roam_route_graph_turn_via(n, nv.inc[a], nv.inc[b], v);
                        TD5_LOG_I(LOG_TAG, "GCDBG node %lld arm %d(e%d end%d @%.0f,%.0f ow%d) -> arm %d(e%d end%d @%.0f,%.0f ow%d) via %d (%.0f,%.0f)(%.0f,%.0f) centre (%.0f,%.0f) run %.0f",
                                  (long long)nv.key, a, nv.inc[a] >> 1, nv.inc[a] & 1, ax, az, ea.oneway, b, nv.inc[b] >> 1, nv.inc[b] & 1, bx, bz, eb.oneway,
                                  nvia, v[0], v[1], v[2], v[3], nv.x, nv.z, run);
                    }
                    turns++; turn_off_m += off;
                    if (run > N->run_m) N->run_m = run;
                    if (run > GC_RUN_OK_M) { bad_turns++; if (N->centre_off) centre_off_bad++; }
                }
            }
            if (N->run_m > GC_RUN_OK_M) { nodes_bad++; if (N->nocent) bad_nocent++; if (N->border) bad_border++; else bad_interior++; }
            if (!N->border && N->run_m > GC_JUNC_OK_M) bad_junc++;
        }
        if (nodes) qsort(nodes, (size_t)nnode, sizeof *nodes, gc_cmp_node);
        conn_turns = turns; conn_bad_turns = bad_turns; conn_nodes = nnode; conn_nodes_off = nodes_off;
        conn_nodes_bad = nodes_bad; conn_centre_off_bad = centre_off_bad; conn_off_m = turn_off_m;
        conn_worst = nodes;
        conn_nocent = nocent_n; conn_off_nocent = off_nocent; conn_bad_nocent = bad_nocent; conn_bad_border = bad_border; conn_bad_interior = bad_interior; conn_bad_junc = bad_junc;
    }
    (void)cls_runs;
    qsort(rows, (size_t)nedge, sizeof *rows, gc_cmp_row);

    snprintf(path, sizeof path, "%sroam_graph_census.txt", td5_plat_log_dir() ? td5_plat_log_dir() : "");
    f = fopen(path, "w");
    if (!f) f = stdout;
    fprintf(f, "roam planner graph vs road surface census (G2), place %s\n", place);
    fprintf(f, "tiles loaded %d of %d casco tiles, EDGE records with no quads %d\n", ntile, nlist, noedge_recs);
    fprintf(f, "graph edges %d, %.1f km, samples every %.0f m: %ld samples, %ld off-road\n", nedge, total_km, GC_STEP_M, samples, off_samples);
    fprintf(f, "off-road %.1f m of %.1f m (%.3f %%), edges with any off-road sample %d\n", total_off_m, total_km * 1000.0,
            total_km > 0 ? 100.0 * total_off_m / (total_km * 1000.0) : 0.0, edges_off);
    fprintf(f, "edges with a run > %.0f m: %d\n", GC_RUN_OK_M, bad_edges);
    fprintf(f, "turn paths (edge end -> node centre -> edge end, the A* connector): %d turns over %d nodes, %d node centres off the surface, "
            "turns with a run > %.0f m: %d (of which centre off-road %d), nodes with such a turn %d, off-road %.1f m\n",
            conn_turns, conn_nodes, conn_nodes_off, GC_RUN_OK_M, conn_bad_turns, conn_centre_off_bad, conn_nodes_bad, conn_off_m);
    fprintf(f, "  nodes with no cluster centroid (join / dead end, position = key cell): %d, of which centre off-road %d, with a bad turn %d\n",
            conn_nocent, conn_off_nocent, conn_bad_nocent);
    fprintf(f, "  bad-turn nodes next to a tile with no file (casco border, patch/ribbon not in the set): %d, interior: %d\n", conn_bad_border, conn_bad_interior);
    for (k = 0; k < GC_NCLS; k++) fprintf(f, "  %-6s edges %d, off-road %.1f m\n", k_cls[k], cls_edges[k], cls_m[k]);
    fprintf(f, "worst 20 (edge, key way_id/first, longest run m @ m along, edge len m, off m, class, klass/lanes/oneway, lat, lon):\n");
    for (i = 0; i < 20 && i < nedge && rows[i].run_m > 0.0; i++) {
        double lat = 0, lon = 0;
        td5_roam_route_place_to_latlon(rows[i].px, rows[i].pz, &lat, &lon);
        fprintf(f, "  e%-5d %llu/%llu  run %6.1f @ %6.1f  len %6.1f  off %6.1f  %-6s k%d l%d ow%d  %.6f, %.6f\n", rows[i].edge,
                (unsigned long long)(rows[i].key >> 16), (unsigned long long)(rows[i].key & 0xFFFF), rows[i].run_m, rows[i].run_at_m,
                rows[i].len_m, rows[i].off_m, rows[i].cls >= 0 ? k_cls[rows[i].cls] : "short", rows[i].klass, rows[i].lanes, rows[i].oneway,
                lat, lon);
    }
    fprintf(f, "worst 20 nodes by longest off-road turn run (key, run m, ninc, centre, lat, lon):\n");
    for (i = 0; conn_worst && i < 20 && i < conn_nodes && conn_worst[i].run_m > 0.0; i++) {
        double lat = 0, lon = 0;
        td5_roam_route_place_to_latlon(conn_worst[i].x, conn_worst[i].z, &lat, &lon);
        fprintf(f, "  n%lld  run %6.1f  ninc %d  centre %s  %.6f, %.6f", (long long)conn_worst[i].key, conn_worst[i].run_m,
                conn_worst[i].ninc, conn_worst[i].centre_off ? "OFF" : "on", lat, lon);
        if (conn_worst[i].border) fprintf(f, " [BORDER]");
        {   /* where each incident edge ends relative to the node centre */
            TD5_RouteGNode nv;
            int nn, a;
            for (nn = 0; nn < conn_nodes; nn++) {
                if (!td5_roam_route_graph_node_view(nn, &nv) || nv.key != conn_worst[i].key) continue;
                for (a = 0; a < nv.ninc; a++) {
                    TD5_RouteGEdge ea;
                    int pp;
                    if (!td5_roam_route_graph_edge_view(nv.inc[a] >> 1, &ea)) continue;
                    pp = (nv.inc[a] & 1) ? ea.npt - 1 : 0;
                    fprintf(f, " | e%d k%d len%.0f end@%.0fm", nv.inc[a] >> 1, ea.klass, ea.plen / GC_UPM,
                            hypot(ea.pt[2 * pp] - nv.x, ea.pt[2 * pp + 1] - nv.z) / GC_UPM);
                }
                break;
            }
            fprintf(f, "\n");
        }
    }
    verdict_bad = bad_edges + conn_bad_junc;
    fprintf(f, "result: %s (%d edges with an off-road run > %.0f m, %d interior nodes with a turn run > %.0f m; border nodes %d are reported, not judged)\n",
            verdict_bad == 0 ? "PASS" : "FAIL", bad_edges, GC_RUN_OK_M, conn_bad_junc, GC_JUNC_OK_M, conn_bad_border);
    if (f != stdout) fclose(f);

    snprintf(path, sizeof path, "%sroam_graph_census.csv", td5_plat_log_dir() ? td5_plat_log_dir() : "");
    csv = fopen(path, "w");
    if (csv) {
        fprintf(csv, "edge,way_id,first_vertex,len_m,off_m,longest_run_m,run_at_m,class,klass,lanes,oneway,px,pz\n");
        for (i = 0; i < nedge; i++) {
            if (rows[i].off_m <= 0.0) break;   /* sorted by run, equal-run ties keep edge order; the rest are 0 */
            fprintf(csv, "%d,%llu,%llu,%.1f,%.1f,%.1f,%.1f,%s,%d,%d,%d,%.0f,%.0f\n", rows[i].edge,
                    (unsigned long long)(rows[i].key >> 16), (unsigned long long)(rows[i].key & 0xFFFF), rows[i].len_m, rows[i].off_m,
                    rows[i].run_m, rows[i].run_at_m, rows[i].cls >= 0 ? k_cls[rows[i].cls] : "short", rows[i].klass, rows[i].lanes,
                    rows[i].oneway, rows[i].px, rows[i].pz);
        }
        fclose(csv);
    }
    TD5_LOG_I(LOG_TAG, "GCENSUS: edges=%d km=%.1f samples=%ld off=%ld off_m=%.1f bad_edges=%d NOSURF=%d MEDIAN=%d END=%d MID=%d "
              "nodes=%d centre_off=%d bad_nodes=%d bad_turns=%d/%d result=%s",
              nedge, total_km, samples, off_samples, total_off_m, bad_edges, cls_edges[GC_NOSURF], cls_edges[GC_MEDIAN], cls_edges[GC_END],
              cls_edges[GC_MID], conn_nodes, conn_nodes_off, conn_nodes_bad, conn_bad_turns, conn_turns, verdict_bad == 0 ? "PASS" : "FAIL");
    free(rows); free(conn_worst);
    for (i = 0; i < ntile; i++) td5_roam_tile_free(&tile[i]);
    free(tile); free(list); free(rs);
    return 1;
}

#endif /* !TD5RE_RELEASE */
