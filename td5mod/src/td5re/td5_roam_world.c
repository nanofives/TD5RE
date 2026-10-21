/**
 * td5_roam_world.c -- GEO FREE ROAM: the street GRAPH (PORT-ONLY, F1 task T3).
 *
 * Contract in td5_roam_world.h. This file builds nodes and edges from the cache
 * session, classifies the nodes, fills the DEAD and JOIN edge ends, and hands
 * the junction candidates to td5_roam_junction.c, which fills the CLUSTER ends.
 *
 * Determinism (design 1.10): the selected ways are sorted by way id, vertex
 * occurrences by (key, way id, vertex), edges come out in edge-key order, and
 * every per-node list is sorted by (edge, end). Nothing iterates a hash, a
 * pointer order or the cache's load order.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5re.h"
#include "td5_platform.h"
#include "td5_config.h"
#include "td5_geo_sidewalk.h"    /* building line + carriageway (divided rule) */
#include "td5_roam_cache.h"
#include "td5_roam_world.h"
#include "td5_roam_junction.h"

#define LOG_TAG "roam"

#define ROAM_PI 3.14159265358979323846

/* ------------------------------------------------------------ params -------- */

void td5_roam_graph_params_default(TD5_RoamGraphParams *p)
{
    if (!p) return;
    memset(p, 0, sizeof *p);
    p->join_max_defl_deg = 30.0;
    p->merge_len     = 30.0 * TD5_ROAM_UPM;
    p->cap_diameter  = 60.0 * TD5_ROAM_UPM;
    p->disc_pad      = 3.0 * TD5_ROAM_UPM;
    p->arm_dir_len   = 15.0 * TD5_ROAM_UPM;
    p->parallel_deg  = 20.0;
    p->corner_max    = 40.0 * TD5_ROAM_UPM;
    p->kerb_r_minor  = 3.0 * TD5_ROAM_UPM;
    p->kerb_r_major  = 6.0 * TD5_ROAM_UPM;
    p->fillet_segs   = 3;
    p->tri_edge_max  = 20000.0;
    p->signal_r      = 15.0 * TD5_ROAM_UPM;
    /* Reference body: the widest half width (340, atp/gtr/jag) and the longest
     * rear-axle-to-front-corner (1280, day) over every carparam.json in
     * re/assets/cars (chassis_corners_front, wheel_pos_RL). World units, the
     * same units as the 1500-unit lane. Script: _archive/roam/F1/T3/car_dims.py. */
    p->body_half_w   = 340.0;
    p->body_lf       = 1280.0;
    /* [roam JA] 2 m: a kerbed island a pavement slab fits on (td5_roam_scenery.c PV_W_SKIP); 50 = the T10 0.5 m */
    p->median_min    = (double)td5_env_int("TD5RE_ROAM_MEDIAN_MIN_CM", 200, 50, 1000) * TD5_ROAM_UPM / 100.0;
    /* [integ J] OPT-IN (default 0 = roam JA): the median widens on its own side, the outer kerb (and the junction
     * corners on it) stay. Off by default: with it on, roam-cops-roadblock scenario 2 fails 3/3 (the run diverges from
     * scenario 1 and the player pins on a kerb of way 336716283), not understood yet (_archive/roam/J/INTEG_REPORT.md) */
    p->median_keep_outer = td5_env_int("TD5RE_ROAM_MEDIAN_KEEP_OUTER", 0, 0, 1);
}

/* ------------------------------------------------------------ filter -------- */

int td5_roam_way_drop_reason(const TD5_RoamWay *w)
{
    uint32_t f;
    if (!w) return TD5_ROAM_DROP_NO_ID;
    if (w->way_id < 0) return TD5_ROAM_DROP_NO_ID;
    f = w->tagf;
    if (f & TD5_ROAM_TAG_HW_BUS_STOP)       return TD5_ROAM_DROP_BUS_STOP;
    if (f & TD5_ROAM_TAG_HW_RAZED)          return TD5_ROAM_DROP_RAZED;
    if (f & TD5_ROAM_TAG_HW_SERVICES)       return TD5_ROAM_DROP_SERVICES;
    if (f & TD5_ROAM_TAG_HW_RACEWAY)        return TD5_ROAM_DROP_RACEWAY;
    if (f & TD5_ROAM_TAG_AREA)              return TD5_ROAM_DROP_AREA;
    if (f & TD5_ROAM_TAG_SVC_PARKING_AISLE) return TD5_ROAM_DROP_PARKING_AISLE;
    if (f & TD5_ROAM_TAG_SVC_DRIVE_THROUGH) return TD5_ROAM_DROP_DRIVE_THROUGH;
    return TD5_ROAM_DROP_KEEP;
}

/* ------------------------------------------------------------ helpers ------- */

static double node_kx(int64_t key) { return (double)(int32_t)(key >> 32) * 10.0; }
static double node_kz(int64_t key) { return (double)(int32_t)(uint32_t)(key & 0xFFFFFFFFu) * 10.0; }

typedef struct { int64_t key; int64_t way_id; int slot; int j; } RoamOcc;

static int occ_cmp(const void *a, const void *b)
{
    const RoamOcc *x = (const RoamOcc *)a, *y = (const RoamOcc *)b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    if (x->way_id != y->way_id) return x->way_id < y->way_id ? -1 : 1;
    return (x->j > y->j) - (x->j < y->j);
}

typedef struct { int64_t way_id; int way; } RoamSel;

static int sel_cmp(const void *a, const void *b)
{
    const RoamSel *x = (const RoamSel *)a, *y = (const RoamSel *)b;
    if (x->way_id != y->way_id) return x->way_id < y->way_id ? -1 : 1;
    return (x->way > y->way) - (x->way < y->way);
}

static int endref_cmp(const void *a, const void *b)
{
    const TD5_RoamEndRef *x = (const TD5_RoamEndRef *)a, *y = (const TD5_RoamEndRef *)b;
    if (x->edge != y->edge) return x->edge < y->edge ? -1 : 1;
    return (x->end > y->end) - (x->end < y->end);
}

int td5_roam_graph_find_node(const TD5_RoamGraph *g, int64_t key)
{
    int lo = 0, hi = g ? g->nnode - 1 : -1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        const int64_t k = g->node[mid].key;
        if (k == key) return mid;
        if (k < key) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

int td5_roam_graph_find_edge(const TD5_RoamGraph *g, uint64_t key)
{
    int lo = 0, hi = g ? g->nedge - 1 : -1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        const uint64_t k = g->edge[mid].key;
        if (k == key) return mid;
        if (k < key) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

void td5_roam_edge_point(const TD5_RoamGraph *g, const TD5_RoamEdge *e, double s,
                         double *x, double *z, double *tx, double *tz)
{
    const TD5_RoamPt *p = &g->pt[e->pt_first];
    const int n = e->npt;
    double acc = 0.0;
    int i;
    for (i = 0; i + 1 < n; i++) {
        const double dx = p[i + 1].x - p[i].x, dz = p[i + 1].z - p[i].z;
        const double len = sqrt(dx * dx + dz * dz);
        if (s < acc + len || i + 2 == n) {
            double f = len > 0.0 ? (s - acc) / len : 0.0;
            if (f < 0.0) f = 0.0;
            if (f > 1.0) f = 1.0;
            /* The ends are returned exactly, so a row at s = 0 / length sits on
             * the node position bit for bit. */
            if (f == 0.0)      { if (x) *x = p[i].x;     if (z) *z = p[i].z; }
            else if (f == 1.0) { if (x) *x = p[i + 1].x; if (z) *z = p[i + 1].z; }
            else { if (x) *x = p[i].x + dx * f; if (z) *z = p[i].z + dz * f; }
            if (tx) *tx = len > 0.0 ? dx / len : 1.0;
            if (tz) *tz = len > 0.0 ? dz / len : 0.0;
            return;
        }
        acc += len;
    }
    if (x) *x = p[0].x;
    if (z) *z = p[0].z;
    if (tx) *tx = 1.0;
    if (tz) *tz = 0.0;
}

void td5_roam_edge_row(const TD5_RoamGraph *g, const TD5_RoamEdge *e, double s,
                       TD5_RoamPt *out, double *tx, double *tz)
{
    double cx, cz, dx, dz, nx, nz;
    int k;
    td5_roam_edge_point(g, e, s, &cx, &cz, &dx, &dz);
    nx = -dz;                      /* left of a->b */
    nz = dx;
    for (k = 0; k <= e->lanes; k++) {
        const double off = -e->half_w + (double)k * e->lane_w;
        out[k].x = cx + nx * off;
        out[k].z = cz + nz * off;
    }
    if (tx) *tx = dx;
    if (tz) *tz = dz;
}

/* Quantise an end's row. */
static void end_quant(TD5_RoamEdgeEnd *en)
{
    int k;
    for (k = 0; k < en->nrow; k++) {
        en->rowq[k][0] = td5_roam_quant(en->row[k].x);
        en->rowq[k][1] = td5_roam_quant(en->row[k].z);
    }
}

/* Outward unit tangent of an edge's first segment at one end. */
static void end_out_dir(const TD5_RoamGraph *g, const TD5_RoamEdge *e, int end,
                        double *ux, double *uz)
{
    const TD5_RoamPt *p = &g->pt[e->pt_first];
    double dx, dz, len;
    if (end == 0) { dx = p[1].x - p[0].x; dz = p[1].z - p[0].z; }
    else { dx = p[e->npt - 2].x - p[e->npt - 1].x; dz = p[e->npt - 2].z - p[e->npt - 1].z; }
    len = sqrt(dx * dx + dz * dz);
    *ux = len > 0.0 ? dx / len : 1.0;
    *uz = len > 0.0 ? dz / len : 0.0;
}

/* The perpendicular row of a dead end. */
static void fill_dead_end(TD5_RoamGraph *g, int ei, int end)
{
    TD5_RoamEdge *e = &g->edge[ei];
    TD5_RoamEdgeEnd *en = &e->end[end];
    en->kind = TD5_ROAM_END_DEAD;
    en->cluster = -1;
    en->arm = -1;
    en->cut = 0.0;
    en->s = end == 0 ? 0.0 : e->length;
    en->nrow = e->lanes + 1;
    td5_roam_edge_row(g, e, en->s, en->row, &en->dir_x, &en->dir_z);
    end_quant(en);
}

/* The mitred row through a JOIN node, computed once and written to both ends in
 * each edge's own a->b orientation. */
static void fill_join(TD5_RoamGraph *g, const TD5_RoamNode *nd)
{
    const TD5_RoamEndRef *r = &g->node_end[nd->end_first];
    TD5_RoamPt p[TD5_ROAM_ROW_MAX];
    double u1x, u1z, u2x, u2z, dx, dz, len, ch, m, nx, nz, hw;
    int lanes, k, q;
    end_out_dir(g, &g->edge[r[0].edge], r[0].end, &u1x, &u1z);
    end_out_dir(g, &g->edge[r[1].edge], r[1].end, &u2x, &u2z);
    /* Through direction: in along -u1, out along u2. */
    dx = u2x - u1x;
    dz = u2z - u1z;
    len = sqrt(dx * dx + dz * dz);
    if (len < 1e-9) { dx = -u1z; dz = u1x; len = 1.0; }   /* fold-back: never a JOIN */
    dx /= len;
    dz /= len;
    ch = dx * u2x + dz * u2z;                  /* cos(deflection / 2) */
    m = ch > 0.5 ? 1.0 / ch : 2.0;
    nx = -dz;
    nz = dx;
    lanes = g->edge[r[0].edge].lanes;
    hw = g->edge[r[0].edge].half_w;
    for (k = 0; k <= lanes; k++) {
        const double off = (-hw + (double)k * g->edge[r[0].edge].lane_w) * m;
        p[k].x = nd->x + nx * off;
        p[k].z = nd->z + nz * off;
    }
    for (q = 0; q < 2; q++) {
        TD5_RoamEdge *e = &g->edge[r[q].edge];
        TD5_RoamEdgeEnd *en = &e->end[r[q].end];
        double ax, az, ux, uz;
        int same;
        end_out_dir(g, e, r[q].end, &ux, &uz);
        /* The edge's a->b tangent at this node. */
        ax = r[q].end == 0 ? ux : -ux;
        az = r[q].end == 0 ? uz : -uz;
        same = (ax * dx + az * dz) > 0.0;
        en->kind = TD5_ROAM_END_JOIN;
        en->cluster = -1;
        en->arm = -1;
        en->cut = 0.0;
        en->s = r[q].end == 0 ? 0.0 : e->length;
        en->nrow = lanes + 1;
        en->dir_x = same ? dx : -dx;
        en->dir_z = same ? dz : -dz;
        for (k = 0; k <= lanes; k++)
            en->row[k] = same ? p[k] : p[lanes - k];
        end_quant(en);
    }
}

void td5_roam_graph_refill_joins(TD5_RoamGraph *g)
{
    int i;
    for (i = 0; i < g->nnode; i++)
        if (g->node[i].kind == TD5_ROAM_NODE_JOIN) fill_join(g, &g->node[i]);
}

/* ------------------------------------------------------------ build --------- */

void td5_roam_graph_free(TD5_RoamGraph *g)
{
    if (!g) return;
    free(g->node);
    free(g->node_end);
    free(g->edge);
    free(g->pt);
    free(g->cluster);
    free(g->cluster_node);
    free(g->arm);
    free(g->pvert);
    free(g->ptri);
    free(g->outline);
    free(g->drop);
    memset(g, 0, sizeof *g);
}

/* ------------------------------------------------------------ divided carriageways
 *
 * [F1 T9] THE LANE COUNT OF HALF A STREET. td5_roam_lane_count raises every La
 * Plata way to the place's 10 m carriageway (3 lanes, 10.47 m). That figure is
 * per CARRIAGEWAY of a street, so it is right for a calle and wrong for one
 * carriageway of a divided avenida: OSM draws each direction as its own one-way
 * way, often 6-10 m centre to centre, and two 10.47 m ribbons then overlap and
 * pave the median over (T7 audit, 5,698 kerb edges inside another carriageway,
 * all along the avenidas and diagonales; 20 ROW_COVERED junction patches).
 *
 * The strip met the same bug on Diagonal 73 (round 1012 D2) and fixed it in
 * gr_mark_divided_carriageways / gr_way_divided (td5_geo_route.c). This is that
 * rule, ported, and decided per WAY as the strip does:
 *   DIVIDED   a same-name, different-id, one-way, non-ring partner whose nearest
 *             point to the way at 1/4, 1/2 and 3/4 of its arc length is 4..45 m
 *             away, ANTI-parallel within 35 deg, on the same side for at least
 *             two of the three samples, and the cross-section (gap + both
 *             carriageways at their OSM count + two class-default pavements)
 *             fits the place's building line. Takes its OWN OSM lane count.
 *   AVENIDA / DIAGONAL, TWO-WAY, not divided: a whole avenue in one way, so
 *             twice the place figure (the strip's GR_AV_WIDE_X10 = 20).
 *             Deviation: the strip doubles any undivided avenida way; a ONE-WAY
 *             way carries one direction only, so here it keeps the floor.
 *   a tagged `width` wins in every case; a counted lane number is never lowered
 *   by the doubling.
 *
 * [F1-T10] THE MEDIAN CAP IS PER EDGE, not per way. T9 capped a divided
 * carriageway so it fitted the smallest of three centre-gap samples of the WHOLE
 * way, so one convergence near a junction narrowed every block of a long avenue
 * (72 ways in the casco). Now the way keeps its OSM count and each EDGE (the piece
 * between two graph nodes, about a block) is capped by the centre gap measured
 * along that piece only: every half row, to the nearest same-name anti-parallel
 * partner (div_edge_gap), the first and last 10 m left out (they lie under the
 * junction patch). Where two pieces end up with different counts the node between
 * them is a TAPER (a 2-arm patch), the strip's own lane-change rule.
 *
 * WINDOW INDEPENDENCE. The verdict reads only the way's own polyline and the
 * ways within 45 m of it. td5_roam_graph_build loads every cache tile that meets
 * (every selected way's box + 45 m) before deciding, and the candidates are
 * sorted by (name, way id), so a way gets the same lanes in every tile build
 * that holds it, whatever else the session has read. */

#define ROAM_MED_MIN_M         4.0    /* GR_MED_MIN_M          */
#define ROAM_MED_MAX_M        45.0    /* GR_MED_MAX_M          */
#define ROAM_MED_ANTI_TOL_DEG 35.0    /* GR_MED_ANTI_TOL_DEG   */
#define ROAM_AV_WIDE_X10      20      /* GR_AV_WIDE_X10        */
#define ROAM_MED_KERB_M        0.5    /* least median kept between two carriageways */
#define ROAM_MED_END_SKIP_M   10.0    /* [T10] gap samples skip this much at each edge end */

typedef struct { int name_id; int64_t way_id; int way; } RoamNameRef;

static int nameref_cmp(const void *a, const void *b)
{
    const RoamNameRef *x = (const RoamNameRef *)a, *y = (const RoamNameRef *)b;
    if (x->name_id != y->name_id) return x->name_id < y->name_id ? -1 : 1;
    if (x->way_id != y->way_id) return x->way_id < y->way_id ? -1 : 1;
    return (x->way > y->way) - (x->way < y->way);
}

/* gr_bearing: atan2(dx, dz), 0 = +Z. */
static double div_bearing(double ax, double az, double bx, double bz)
{
    return atan2(bx - ax, bz - az);
}

/* gr_way_at_frac over the way's cache points. */
static int div_way_at_frac(const TD5_RoamWay *w, double f, double *px, double *pz, double *br)
{
    double total = 0.0, acc = 0.0, want;
    int i;
    const int n = w->road.count;
    for (i = 0; i + 1 < n; i++) {
        double ax, az, bx, bz;
        td5_roam_cache_point(w, i, &ax, &az, NULL);
        td5_roam_cache_point(w, i + 1, &bx, &bz, NULL);
        total += hypot(bx - ax, bz - az);
    }
    if (total <= 1e-9) return 0;
    want = total * f;
    for (i = 0; i + 1 < n; i++) {
        double ax, az, bx, bz, seg, t;
        td5_roam_cache_point(w, i, &ax, &az, NULL);
        td5_roam_cache_point(w, i + 1, &bx, &bz, NULL);
        seg = hypot(bx - ax, bz - az);
        if (seg <= 1e-9) continue;
        if (acc + seg < want && i + 2 < n) { acc += seg; continue; }
        t = (want - acc) / seg;
        if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
        *px = ax + (bx - ax) * t;
        *pz = az + (bz - az) * t;
        *br = div_bearing(ax, az, bx, bz);
        return 1;
    }
    return 0;
}

/* gr_road_near_at: closest approach of way w to P, that segment's bearing and the
 * closest point. */
static double div_near_at(const TD5_RoamWay *w, double px, double pz, double *bear,
                          double *outx, double *outz)
{
    double best = 1e30;
    int i;
    for (i = 0; i + 1 < w->road.count; i++) {
        double ax, az, bx, bz, dx, dz, l2, t, cx, cz, d;
        td5_roam_cache_point(w, i, &ax, &az, NULL);
        td5_roam_cache_point(w, i + 1, &bx, &bz, NULL);
        dx = bx - ax; dz = bz - az;
        l2 = dx * dx + dz * dz;
        if (l2 <= 1e-12) continue;
        t = ((px - ax) * dx + (pz - az) * dz) / l2;
        if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
        cx = ax + dx * t; cz = az + dz * t;
        d = hypot(px - cx, pz - cz);
        if (d < best) {
            best = d;
            *bear = div_bearing(ax, az, bx, bz);
            *outx = cx;
            *outz = cz;
        }
    }
    return best;
}

static int div_partner_ok(const TD5_RoamWay *b)
{
    return b->road.oneway && !b->road.roundabout && b->road.count >= 2 &&
           td5_roam_way_drop_reason(b) == TD5_ROAM_DROP_KEEP;
}

/* gr_way_divided over the name group cand[g0..g1) (sorted by way id). */
static int div_way_divided(const TD5_RoamWay *A, const RoamNameRef *cand, int g0, int g1,
                           double *gap_out)
{
    const double lo = ROAM_MED_MIN_M * TD5_ROAM_UPM, hi = ROAM_MED_MAX_M * TD5_ROAM_UPM;
    const double line = td5_geo_sw_building_line_for(TD5_GEO_RC_UNKNOWN, A->road.namek) * TD5_ROAM_UPM;
    const double pav = td5_geo_roads_pavement_default_m(TD5_GEO_RC_SECONDARY) * TD5_ROAM_UPM;
    const double own_h = (double)(A->lanes_osm > 0 ? A->lanes_osm : 2) * TD5_ROAM_LANE_W * 0.5;
    int t, hits = 0, side_sum = 0;
    double gap = 1e30;
    if (!div_partner_ok(A)) return 0;
    for (t = 1; t <= 3; t++) {
        double px = 0.0, pz = 0.0, rbr = 0.0, lx, lz;
        int b, side = 0, found = 0;
        if (!div_way_at_frac(A, (double)t * 0.25, &px, &pz, &rbr)) continue;
        lx = cos(rbr); lz = -sin(rbr);      /* left of travel, gr_bearing frame */
        for (b = g0; b < g1 && !found; b++) {
            const TD5_RoamWay *B = td5_roam_cache_way(cand[b].way);
            double pbr = 0.0, cx = 0.0, cz = 0.0, d, dot, dd;
            if (!B || B->way_id == A->way_id) continue;
            d = div_near_at(B, px, pz, &pbr, &cx, &cz);
            if (d < lo || d > hi) continue;
            dd = (rbr + ROAM_PI) - pbr;
            dd = atan2(sin(dd), cos(dd));
            if (fabs(dd) * 180.0 / ROAM_PI > ROAM_MED_ANTI_TOL_DEG) continue;
            if (line > 0.0) {
                const double far_h = (double)(B->lanes_osm > 0 ? B->lanes_osm : 2) * TD5_ROAM_LANE_W * 0.5;
                if (d + own_h + far_h + 2.0 * pav > line) continue;
            }
            dot = (cx - px) * lx + (cz - pz) * lz;
            side = (dot > 0.0) ? 1 : -1;
            found = 1;
            if (d < gap) gap = d;
        }
        if (found) { hits++; side_sum += side; }
    }
    *gap_out = gap;
    return hits >= 2 && (side_sum == hits || side_sum == -hits);
}

/* The lane count of every selected way (sel order). A divided carriageway gets its
 * OSM count here; div_edge_lanes caps each of its edges. grp_out[2*i] / [2*i+1] =
 * the way's name group in *cand_out (g0, g1), or -1 when the way is not divided.
 * The caller frees *cand_out. Returns 0 on out of memory. */
static int div_way_lanes(const RoamSel *sel, int nsel, const double ubox[4], int *lanes_out,
                         int *grp_out, RoamNameRef **cand_out, TD5_RoamGraphCensus *c, int logm)
{
    const int ncache = td5_roam_cache_way_count();
    RoamNameRef *cand = (RoamNameRef *)malloc((size_t)(ncache > 0 ? ncache : 1) * sizeof *cand);
    int ncand = 0, i;
    *cand_out = cand;
    if (!cand) return 0;
    for (i = 0; i < ncache; i++) {
        const TD5_RoamWay *w = td5_roam_cache_way(i);
        if (!w || w->road.name_id < 0 || !div_partner_ok(w)) continue;
        if (w->road.maxx < ubox[0] || w->road.minx > ubox[2] ||
            w->road.maxz < ubox[1] || w->road.minz > ubox[3]) continue;
        cand[ncand].name_id = w->road.name_id;
        cand[ncand].way_id = w->way_id;
        cand[ncand].way = i;
        ncand++;
    }
    qsort(cand, (size_t)ncand, sizeof *cand, nameref_cmp);
    for (i = 0; i < nsel; i++) {
        const TD5_RoamWay *w = td5_roam_cache_way(sel[i].way);
        int lanes = w->road.lanes;
        grp_out[2 * i] = grp_out[2 * i + 1] = -1;
        if (!w->width_tagged) {
            int divided = 0, g0 = 0, g1 = 0;
            double gap = 1e30;
            if (w->road.name_id >= 0 && div_partner_ok(w)) {
                int lo = 0, hi = ncand;
                while (lo < hi) {               /* first entry of the name group */
                    const int mid = lo + (hi - lo) / 2;
                    if (cand[mid].name_id < w->road.name_id) lo = mid + 1; else hi = mid;
                }
                g0 = lo;
                for (g1 = g0; g1 < ncand && cand[g1].name_id == w->road.name_id; g1++) {}
                divided = g1 - g0 >= 2 && div_way_divided(w, cand, g0, g1, &gap);
            }
            if (divided) {
                if (w->lanes_osm > 0) lanes = w->lanes_osm;
                {
                    /* The T9 whole-way cap, kept as a census figure only (the cap
                     * itself is per edge now, div_edge_lanes). */
                    const int fit = (int)floor((gap - ROAM_MED_KERB_M * TD5_ROAM_UPM) / TD5_ROAM_LANE_W);
                    if (fit < lanes) {
                        c->median_capped++;
                        if (logm)
                            TD5_LOG_I(LOG_TAG, "ROAM_MEDIAN_T9 way %lld '%s' osm lanes %d, T9 whole-way lanes %d (gap %.2f m)",
                                      (long long)w->way_id, td5_roam_cache_name(w->road.name_id) ? td5_roam_cache_name(w->road.name_id) : "?", lanes,
                                      fit < 1 ? 1 : fit, gap / TD5_ROAM_UPM);
                    }
                }
                grp_out[2 * i] = g0;
                grp_out[2 * i + 1] = g1;
                c->divided_ways++;
            } else if (!w->road.oneway && (w->road.namek == TD5_GEO_NAMEK_AVENIDA ||
                                           w->road.namek == TD5_GEO_NAMEK_DIAGONAL)) {
                const double tm = td5_geo_sw_carriageway_m(TD5_GEO_RC_UNKNOWN, w->road.namek);
                if (tm > 0.0) {
                    int want = (int)floor(tm * (double)ROAM_AV_WIDE_X10 * 0.1 * TD5_ROAM_UPM /
                                          TD5_ROAM_LANE_W + 0.5);
                    if (w->lanes_counted && want < w->lanes_osm) want = w->lanes_osm;
                    if (want >= 1 && want <= TD5_GEO_ROADS_LANES_MAX) { lanes = want; c->wide_ways++; }
                }
            }
        }
        lanes_out[i] = lanes;
    }
    return 1;
}

/* [F1-T10] The smallest centre gap along an edge to a same-name, anti-parallel
 * (within ROAM_MED_ANTI_TOL_DEG) partner of the way's name group, sampled every
 * half row between ROAM_MED_END_SKIP_M of each end (the whole edge when shorter
 * than three times that). Partners further than ROAM_MED_MAX_M do not count.
 * 1e30 when no sample sees a partner. Pure over the edge polyline and the cache. */
static double div_edge_gap(const TD5_RoamGraph *g, const TD5_RoamEdge *e, const TD5_RoamWay *self,
                           int rev, const RoamNameRef *cand, int g0, int g1, double *side)
{
    const double skip = ROAM_MED_END_SKIP_M * TD5_ROAM_UPM, hi = ROAM_MED_MAX_M * TD5_ROAM_UPM;
    const double step = 0.5 * TD5_ROAM_LANE_W;
    double s0 = skip, s1 = e->length - skip, s, best = 1e30;
    int last = 0;
    if (s1 - s0 < skip) { s0 = 0.0; s1 = e->length; }
    for (s = s0; !last; s += step) {
        double px, pz, tx, tz, eb;
        int b;
        if (s >= s1) { s = s1; last = 1; }
        td5_roam_edge_point(g, e, s, &px, &pz, &tx, &tz);
        eb = atan2(tx, tz);                       /* gr_bearing frame */
        if (rev) eb += ROAM_PI;                   /* the WAY's direction, not a->b */
        for (b = g0; b < g1; b++) {
            const TD5_RoamWay *B = td5_roam_cache_way(cand[b].way);
            double pbr = 0.0, cx = 0.0, cz = 0.0, d, dd;
            if (!B || B->way_id == self->way_id) continue;
            d = div_near_at(B, px, pz, &pbr, &cx, &cz);
            if (d > hi || d >= best) continue;
            dd = (eb + ROAM_PI) - pbr;
            dd = atan2(sin(dd), cos(dd));
            if (fabs(dd) * 180.0 / ROAM_PI > ROAM_MED_ANTI_TOL_DEG) continue;
            best = d;
            /* [integ J] which side of a->b the partner lies on: +1 left, -1 right */
            if (side) *side = (tx * (cz - pz) - tz * (cx - px)) > 0.0 ? 1.0 : -1.0;
        }
    }
    return best;
}

/* Is key a node? Binary search over the sorted node keys. */
static int is_node(const TD5_RoamGraph *g, int64_t key)
{
    return td5_roam_graph_find_node(g, key) >= 0;
}

int td5_roam_graph_build(TD5_RoamGraph *g, const double win[4], double halo,
                         const TD5_RoamGraphParams *prm)
{
    const uint32_t t0 = td5_plat_time_ms();
    int ncache;
    TD5_RoamGraphCensus *c;
    RoamSel *sel = NULL;
    int *wlanes = NULL;                                 /* per sel: lane count  */
    int *wgrp = NULL;                                   /* [T10] per sel: divided name group */
    RoamNameRef *dcand = NULL;                          /* [T10] the name-group candidates  */
    double ubox[4];
    int nsel = 0;
    int *vstart = NULL, *vcount = NULL, *vidx = NULL;   /* filtered vertices per sel */
    int nv = 0;
    RoamOcc *occ = NULL;
    int nocc = 0;
    int i, j, k;
    int cap_edge = 0, cap_pt = 0;

    if (!g) return 0;
    memset(g, 0, sizeof *g);
    if (prm) g->prm = *prm; else td5_roam_graph_params_default(&g->prm);
    c = &g->census;

    /* 0. [T9] Every way that can be selected, grown by the median search
     * radius, must have its whole neighbourhood in the session before the
     * divided rule reads it. A way that meets win + halo lives in a cache tile
     * whose extent meets win + halo, so this load never changes the selection. */
    {
        const int n0 = td5_roam_cache_way_count();
        const double pad = ROAM_MED_MAX_M * TD5_ROAM_UPM;
        int have = 0;
        TD5_RoamCacheCensus cc;
        for (i = 0; i < n0; i++) {
            const TD5_RoamWay *w = td5_roam_cache_way(i);
            if (!w) continue;
            if (w->road.maxx < win[0] - halo || w->road.minx > win[2] + halo ||
                w->road.maxz < win[1] - halo || w->road.minz > win[3] + halo)
                continue;
            if (!have) {
                ubox[0] = w->road.minx; ubox[1] = w->road.minz;
                ubox[2] = w->road.maxx; ubox[3] = w->road.maxz;
                have = 1;
            } else {
                if (w->road.minx < ubox[0]) ubox[0] = w->road.minx;
                if (w->road.minz < ubox[1]) ubox[1] = w->road.minz;
                if (w->road.maxx > ubox[2]) ubox[2] = w->road.maxx;
                if (w->road.maxz > ubox[3]) ubox[3] = w->road.maxz;
            }
        }
        if (!have) {
            ubox[0] = win[0] - halo; ubox[1] = win[1] - halo;
            ubox[2] = win[2] + halo; ubox[3] = win[3] + halo;
        }
        ubox[0] -= pad; ubox[1] -= pad; ubox[2] += pad; ubox[3] += pad;
        td5_roam_cache_load_window(ubox, 0.0, &cc);
    }
    ncache = td5_roam_cache_way_count();

    /* 1. Select ways (bbox meets win + halo), apply the drivable filter. */
    g->nway = ncache;
    g->drop = (int *)calloc((size_t)(ncache > 0 ? ncache : 1), sizeof(int));
    sel = (RoamSel *)malloc((size_t)(ncache > 0 ? ncache : 1) * sizeof *sel);
    if (!g->drop || !sel) goto oom;
    for (i = 0; i < ncache; i++) {
        const TD5_RoamWay *w = td5_roam_cache_way(i);
        int why;
        if (!w) continue;
        g->drop[i] = -1;                       /* -1 = outside the window */
        if (w->road.maxx < win[0] - halo || w->road.minx > win[2] + halo ||
            w->road.maxz < win[1] - halo || w->road.minz > win[3] + halo)
            continue;
        c->ways_in++;
        why = td5_roam_way_drop_reason(w);
        g->drop[i] = why;
        if (why != TD5_ROAM_DROP_KEEP) { c->drop[why]++; continue; }
        sel[nsel].way_id = w->way_id;
        sel[nsel].way = i;
        nsel++;
    }
    qsort(sel, (size_t)nsel, sizeof *sel, sel_cmp);
    /* Duplicate ids: the first (lowest cache index) keeps the id. */
    for (i = 1, j = 1; i < nsel; i++) {
        if (sel[i].way_id == sel[j - 1].way_id) {
            g->drop[sel[i].way] = TD5_ROAM_DROP_DUP_ID;
            c->drop[TD5_ROAM_DROP_DUP_ID]++;
            TD5_LOG_W(LOG_TAG, "ROAM_DUP_WAY id %lld (cache way %d dropped)",
                      (long long)sel[i].way_id, sel[i].way);
            continue;
        }
        sel[j++] = sel[i];
    }
    if (nsel > 0) nsel = j;
    wlanes = (int *)malloc((size_t)(nsel + 1) * sizeof(int));
    wgrp = (int *)malloc((size_t)(2 * nsel + 2) * sizeof(int));
    if (!wlanes || !wgrp || !div_way_lanes(sel, nsel, ubox, wlanes, wgrp, &dcand, c, g->prm.log_median)) goto oom;

    /* 2. Filtered vertices: the first vertex of each run of equal node keys. */
    vstart = (int *)malloc((size_t)(nsel + 1) * sizeof(int));
    vcount = (int *)malloc((size_t)(nsel + 1) * sizeof(int));
    {
        int tot = 0;
        for (i = 0; i < nsel; i++) tot += td5_roam_cache_way(sel[i].way)->road.count;
        vidx = (int *)malloc((size_t)(tot + 1) * sizeof(int));
        occ = (RoamOcc *)malloc((size_t)(tot + 1) * sizeof(RoamOcc));
    }
    if (!vstart || !vcount || !vidx || !occ) goto oom;
    for (i = 0; i < nsel; i++) {
        const TD5_RoamWay *w = td5_roam_cache_way(sel[i].way);
        int64_t last = 0;
        int have = 0;
        vstart[i] = nv;
        for (k = 0; k < w->road.count; k++) {
            int64_t key;
            td5_roam_cache_point(w, k, NULL, NULL, &key);
            if (have && key == last) continue;
            vidx[nv++] = k;
            last = key;
            have = 1;
        }
        vcount[i] = nv - vstart[i];
        if (vcount[i] < 2) {
            g->drop[sel[i].way] = TD5_ROAM_DROP_DEGENERATE;
            c->drop[TD5_ROAM_DROP_DEGENERATE]++;
            nv = vstart[i];
            vcount[i] = 0;
            continue;
        }
        c->ways_kept++;
        for (j = 0; j < vcount[i]; j++) {
            int64_t key;
            td5_roam_cache_point(w, vidx[vstart[i] + j], NULL, NULL, &key);
            occ[nocc].key = key;
            occ[nocc].way_id = w->way_id;
            occ[nocc].slot = i;
            occ[nocc].j = j;
            nocc++;
        }
    }
    qsort(occ, (size_t)nocc, sizeof *occ, occ_cmp);

    /* 3. Nodes: a way end, or a key held by 2+ occurrences. */
    g->node = (TD5_RoamNode *)calloc((size_t)(nocc + 1), sizeof(TD5_RoamNode));
    if (!g->node) goto oom;
    for (i = 0; i < nocc; i = j) {
        int isn = 0, cnt, lay_set = 0, lay_int = 0;
        int cross = 0;
        for (j = i; j < nocc && occ[j].key == occ[i].key; j++) {
            const int last = vcount[occ[j].slot] - 1;
            if (occ[j].j == 0 || occ[j].j == last) isn = 1;
        }
        cnt = j - i;
        if (cnt >= 2) isn = 1;
        if (!isn) continue;
        /* Ways of different layers passing THROUGH the same cell: not a real
         * junction (OSM does not join them), a snap coincidence. */
        for (k = i; k < j; k++) {
            const int last = vcount[occ[k].slot] - 1;
            const int lay = td5_roam_cache_way(sel[occ[k].slot].way)->road.layer;
            if (occ[k].j == 0 || occ[k].j == last) continue;
            if (!lay_set) { lay_int = lay; lay_set = 1; }
            else if (lay != lay_int) cross = 1;
        }
        {
            TD5_RoamNode *nd = &g->node[g->nnode++];
            nd->key = occ[i].key;
            nd->x = node_kx(nd->key);
            nd->z = node_kz(nd->key);
            nd->cluster = -1;
            nd->layer_cross = cross;
            if (cross) {
                c->layer_cross_nodes++;
                TD5_LOG_W(LOG_TAG, "ROAM_LAYER_NODE key %lld at (%.0f, %.0f): ways of "
                          "different layers pass through one snap cell",
                          (long long)nd->key, nd->x, nd->z);
            }
        }
    }
    c->nodes = g->nnode;

    /* 4. Edges: each way cut at every node vertex. */
    for (i = 0; i < nsel; i++) {
        const TD5_RoamWay *w = td5_roam_cache_way(sel[i].way);
        int start = 0, ord = 0;
        if (vcount[i] < 2) continue;
        for (j = 1; j < vcount[i]; j++) {
            int64_t kj;
            td5_roam_cache_point(w, vidx[vstart[i] + j], NULL, NULL, &kj);
            if (j != vcount[i] - 1 && !is_node(g, kj)) continue;
            /* Piece [start .. j]. */
            {
                TD5_RoamEdge *e;
                int64_t ka, kb;
                int n = j - start + 1, rev, q, out0;
                double len = 0.0;
                if (g->nedge + 1 > cap_edge) {
                    void *nb;
                    cap_edge = cap_edge ? cap_edge * 2 : 4096;
                    nb = realloc(g->edge, (size_t)cap_edge * sizeof *g->edge);
                    if (!nb) goto oom;
                    g->edge = (TD5_RoamEdge *)nb;
                }
                if (g->npt + n > cap_pt) {
                    void *nb;
                    while (g->npt + n > cap_pt) cap_pt = cap_pt ? cap_pt * 2 : 65536;
                    nb = realloc(g->pt, (size_t)cap_pt * sizeof *g->pt);
                    if (!nb) goto oom;
                    g->pt = (TD5_RoamPt *)nb;
                }
                td5_roam_cache_point(w, vidx[vstart[i] + start], NULL, NULL, &ka);
                kb = kj;
                rev = ka > kb;
                e = &g->edge[g->nedge];
                memset(e, 0, sizeof *e);
                e->way_id = w->way_id;
                /* The piece "ordinal" is the index of its first vertex in the
                 * way, not a running piece count: a count depends on which
                 * OTHER ways are loaded (a long way gets more pieces when the
                 * streets crossing it far away are in the halo), so it would
                 * change an edge's key between two tile builds. A vertex index
                 * depends on the way alone. */
                {
                    const int first_vertex = vidx[vstart[i] + start];
                    e->key = ((uint64_t)w->way_id << 16) | (uint64_t)(first_vertex & 0xFFFF);
                }
                (void)ord;
                e->way = sel[i].way;
                e->node_a = td5_roam_graph_find_node(g, rev ? kb : ka);
                e->node_b = td5_roam_graph_find_node(g, rev ? ka : kb);
                /* Polyline a -> b: node positions at the ends, interior points
                 * raw; an interior point within 1 unit of its neighbour is
                 * dropped so no segment is degenerate. */
                e->pt_first = g->npt;
                out0 = g->npt;
                for (q = 0; q < n; q++) {
                    const int jj = rev ? (j - q) : (start + q);
                    double x, z;
                    if (q == 0) {
                        const TD5_RoamNode *nd = &g->node[e->node_a];
                        x = nd->x; z = nd->z;
                    } else if (q == n - 1) {
                        const TD5_RoamNode *nd = &g->node[e->node_b];
                        x = nd->x; z = nd->z;
                        /* the last interior point too close to the end node */
                        if (g->npt - out0 >= 2) {
                            const TD5_RoamPt *pp = &g->pt[g->npt - 1];
                            if (fabs(pp->x - x) < 1.0 && fabs(pp->z - z) < 1.0) g->npt--;
                        }
                    } else {
                        td5_roam_cache_point(w, vidx[vstart[i] + jj], &x, &z, NULL);
                        {
                            const TD5_RoamPt *pp = &g->pt[g->npt - 1];
                            if (fabs(pp->x - x) < 1.0 && fabs(pp->z - z) < 1.0) continue;
                        }
                    }
                    g->pt[g->npt].x = x;
                    g->pt[g->npt].z = z;
                    g->npt++;
                }
                e->npt = g->npt - out0;
                for (q = 0; q + 1 < e->npt; q++) {
                    const double dx = g->pt[out0 + q + 1].x - g->pt[out0 + q].x;
                    const double dz = g->pt[out0 + q + 1].z - g->pt[out0 + q].z;
                    len += sqrt(dx * dx + dz * dz);
                }
                e->length = len;
                /* Attributes. */
                e->klass = w->road.klass;
                e->lanes = wlanes[i] < 1 ? 1 : wlanes[i];
                if (e->lanes > TD5_GEO_ROADS_LANES_MAX) e->lanes = TD5_GEO_ROADS_LANES_MAX;
                e->lane_w = TD5_ROAM_LANE_W;
                if (wgrp[2 * i] >= 0 && e->npt >= 2 && len > 0.0) {
                    /* [F1-T10] the median of THIS piece: the carriageway must fit the
                     * centre gap measured along it, with a 0.5 m kerb, i.e. a width of
                     * gap - kerb (both carriageways of a pair take the same rule, so
                     * half each side). The OSM lane count is kept while each lane can
                     * be at least TD5_ROAM_LANE_MIN_M wide; lanes are narrowed first,
                     * dropped only below that. Never below one lane of the minimum. */
                    double gap, side = 0.0;
                    e->length = len;
                    gap = div_edge_gap(g, e, w, rev, dcand, wgrp[2 * i], wgrp[2 * i + 1], &side);
                    c->divided_edges++;
                    if (gap < 1e29) {
                        const double avail = gap - ROAM_MED_KERB_M * TD5_ROAM_UPM;
                        const double lmin = TD5_ROAM_LANE_MIN_M * TD5_ROAM_UPM;
                        if (avail < (double)e->lanes * TD5_ROAM_LANE_W) {
                            int fit = (int)floor(avail / lmin);
                            if (fit < 1) fit = 1;
                            if (fit < e->lanes) {
                                c->median_capped_edges++;
                                c->median_capped_len += len;
                                e->lanes = fit;
                            }
                            e->lane_w = avail / (double)e->lanes;
                            if (e->lane_w > TD5_ROAM_LANE_W) e->lane_w = TD5_ROAM_LANE_W;
                            if (e->lane_w < lmin) e->lane_w = lmin;
                        }
                        /* [roam JA] THE MEDIAN ITSELF. The 0.5 m kerb above only keeps two carriageways from
                         * overlapping: a pair whose lanes fit the centre gap kept them at full width and left
                         * a median of 0.5-1 m (Avenida 7 at Calle 49: 11.2 m centre to centre, 3 x 3.49 m lanes
                         * a side, 0.79 m between), too narrow for any pavement slab, so the lot showed in the
                         * slot (Mariano: "there's no median in between these roads"). The lanes now narrow (never
                         * under lmin, the lane count is never lowered for it) until the median is median_min. */
                        if (g->prm.median_min > ROAM_MED_KERB_M * TD5_ROAM_UPM && g->prm.median_keep_outer && side != 0.0) {
                            /* [integ J] THE OUTER KERB STAYS. Narrowed about its way, a carriageway lost half its
                             * width on the OUTER side too, so every junction corner on that kerb moved into the
                             * junction (the acute block tip at Calle 14 / 55 by Plaza Moreno came onto the line
                             * into the next street: the roam-ram car hit it). The outer kerb is kept where the
                             * full lanes put it and only the median side moves: each carriageway gives up
                             * W - W' with W' = (gap + W - median_min) / 2 (wider lanes than the centred rule, the
                             * same median), and shifts (W - W') / 2 away from its partner. */
                            const double wfull = (double)e->lanes * e->lane_w;
                            const double wnew = 0.5 * (gap + wfull - g->prm.median_min);
                            if (wnew < wfull) {
                                double nw = wnew / (double)e->lanes;
                                if (nw < lmin) nw = lmin;
                                if (nw < e->lane_w) {
                                    e->med_shift = 0.5 * (double)e->lanes * (e->lane_w - nw);
                                    e->lane_w = nw;
                                    c->median_widened_edges++;
                                }
                            }
                        } else if (g->prm.median_min > ROAM_MED_KERB_M * TD5_ROAM_UPM) {
                            const double want = (gap - g->prm.median_min) / (double)e->lanes;
                            if (e->lane_w > want) {
                                const double nw = want > lmin ? want : lmin;
                                if (nw < e->lane_w) { e->lane_w = nw; c->median_widened_edges++; }
                            }
                        }
                        if (e->med_shift > 0.0) {
                            /* every point, the two ends included (the nodes follow in step 5b), moves
                             * med_shift away from the partner along its vertex normal */
                            TD5_RoamPt *P = &g->pt[out0];
                            const int np_ = e->npt;
                            TD5_RoamPt *Q = (TD5_RoamPt *)malloc((size_t)np_ * sizeof *Q);
                            if (!Q) goto oom;
                            for (q = 0; q < np_; q++) {
                                const int qa = q > 0 ? q - 1 : q, qb = q + 1 < np_ ? q + 1 : q;
                                double tx = P[qb].x - P[qa].x, tz = P[qb].z - P[qa].z;
                                const double tl = sqrt(tx * tx + tz * tz);
                                if (tl > 0.0) { tx /= tl; tz /= tl; }
                                /* the left normal of a->b is (-tz, tx); outward = away from the partner */
                                Q[q].x = P[q].x + side * e->med_shift * tz;
                                Q[q].z = P[q].z - side * e->med_shift * tx;
                            }
                            len = 0.0;
                            for (q = 0; q < np_; q++) {
                                P[q] = Q[q];
                                if (q > 0) len += sqrt((P[q].x - P[q - 1].x) * (P[q].x - P[q - 1].x) + (P[q].z - P[q - 1].z) * (P[q].z - P[q - 1].z));
                            }
                            free(Q);
                            e->length = len;
                            c->median_shifted_edges++;
                            if (e->med_shift > c->median_shift_max) c->median_shift_max = e->med_shift;
                        }
                        if (e->lane_w < TD5_ROAM_LANE_W) c->median_narrow_edges++;
                    }
                    if (g->prm.log_median) {
                        double mx, mz;
                        td5_roam_edge_point(g, e, 0.5 * len, &mx, &mz, NULL, NULL);
                        TD5_LOG_I(LOG_TAG, "ROAM_MEDIAN_EDGE way %lld edge %llu lanes %d of %d, lane %.2f m, gap %.2f m, "
                                  "length %.0f m, at (%.0f, %.0f)", (long long)w->way_id,
                                  (unsigned long long)e->key, e->lanes, wlanes[i], e->lane_w / TD5_ROAM_UPM,
                                  gap < 1e29 ? gap / TD5_ROAM_UPM : -1.0, len / TD5_ROAM_UPM, mx, mz);
                    }
                }
                e->half_w = (double)e->lanes * e->lane_w * 0.5;
                if (w->road.oneway) {
                    const int d = w->road.oneway_dir ? w->road.oneway_dir : 1;
                    e->oneway = rev ? -d : d;
                }
                e->surface = w->road.surface;
                e->layer = w->road.layer;
                e->bridge = w->road.bridge;
                e->tunnel = w->road.tunnel
                          ? ((w->tagf & TD5_ROAM_TAG_TUNNEL_PASSAGE) ? 2 : 1) : 0;
                e->roundabout = w->road.roundabout;
                e->namek = w->road.namek;
                e->name_id = w->road.name_id;
                e->maxspeed_kph = w->road.maxspeed_kph;
                e->end[0].node = e->node_a;
                e->end[1].node = e->node_b;
                e->end[0].cluster = e->end[1].cluster = -1;
                e->end[0].arm = e->end[1].arm = -1;
                if (e->npt >= 2 && e->length > 0.0) g->nedge++;
                else g->npt = out0;
            }
            start = j;
            ord++;
        }
    }
    c->edges = g->nedge;

    /* 5. Per-node edge-end lists, sorted by (edge, end). Edges are already in
     * key order (ways by id, pieces by ordinal), which the lookups rely on. */
    for (i = 1; i < g->nedge; i++)
        if (g->edge[i].key <= g->edge[i - 1].key) {
            TD5_LOG_E(LOG_TAG, "roam graph: edge keys out of order at %d", i);
            goto oom;
        }
    g->node_end = (TD5_RoamEndRef *)malloc((size_t)(2 * g->nedge + 1) * sizeof *g->node_end);
    if (!g->node_end) goto oom;
    for (i = 0; i < g->nedge; i++) {
        g->node[g->edge[i].node_a].nend++;
        g->node[g->edge[i].node_b].nend++;
    }
    for (i = 0, k = 0; i < g->nnode; i++) {
        g->node[i].end_first = k;
        k += g->node[i].nend;
        g->node[i].nend = 0;
    }
    g->nnode_end = k;
    for (i = 0; i < g->nedge; i++) {
        int q;
        for (q = 0; q < 2; q++) {
            TD5_RoamNode *nd = &g->node[q == 0 ? g->edge[i].node_a : g->edge[i].node_b];
            g->node_end[nd->end_first + nd->nend].edge = i;
            g->node_end[nd->end_first + nd->nend].end = q;
            nd->nend++;
        }
    }
    for (i = 0; i < g->nnode; i++) {
        TD5_RoamNode *nd = &g->node[i];
        qsort(&g->node_end[nd->end_first], (size_t)nd->nend, sizeof *g->node_end, endref_cmp);
        nd->layer_min = 127;
        nd->layer_max = -128;
        for (k = 0; k < nd->nend; k++) {
            const int lay = g->edge[g->node_end[nd->end_first + k].edge].layer;
            if (lay < nd->layer_min) nd->layer_min = lay;
            if (lay > nd->layer_max) nd->layer_max = lay;
        }
    }

    /* 5b. [integ J] The nodes of carriageways shifted outward (step 4, median_keep_outer) follow them: each such node
     * moves by the mean shift its shifted edge ends asked for (two carriageways of one pair meeting at one node ask
     * opposite shifts and cancel: the merge point stays), then every edge end there is pinned back to its node. */
    {
        double *acc = (double *)calloc((size_t)(3 * g->nnode + 1), sizeof *acc);
        if (!acc) goto oom;
        for (i = 0; i < g->nedge; i++) {
            const TD5_RoamEdge *e = &g->edge[i];
            int q;
            if (e->med_shift <= 0.0) continue;
            for (q = 0; q < 2; q++) {
                const int nd = q == 0 ? e->node_a : e->node_b;
                const TD5_RoamPt *pp = &g->pt[q == 0 ? e->pt_first : e->pt_first + e->npt - 1];
                acc[3 * nd] += pp->x - g->node[nd].x;
                acc[3 * nd + 1] += pp->z - g->node[nd].z;
                acc[3 * nd + 2] += 1.0;
            }
        }
        for (i = 0; i < g->nnode; i++)
            if (acc[3 * i + 2] > 0.0) {
                g->node[i].x += acc[3 * i] / acc[3 * i + 2];
                g->node[i].z += acc[3 * i + 1] / acc[3 * i + 2];
                c->median_moved_nodes++;
            }
        for (i = 0; i < g->nedge; i++) {
            TD5_RoamEdge *e = &g->edge[i];
            TD5_RoamPt *P = &g->pt[e->pt_first];
            double len = 0.0;
            int q;
            if (acc[3 * e->node_a + 2] <= 0.0 && acc[3 * e->node_b + 2] <= 0.0) continue;
            P[0].x = g->node[e->node_a].x; P[0].z = g->node[e->node_a].z;
            P[e->npt - 1].x = g->node[e->node_b].x; P[e->npt - 1].z = g->node[e->node_b].z;
            for (q = 1; q < e->npt; q++)
                len += sqrt((P[q].x - P[q - 1].x) * (P[q].x - P[q - 1].x) + (P[q].z - P[q - 1].z) * (P[q].z - P[q - 1].z));
            e->length = len;
        }
        free(acc);
    }

    /* 6. Classify nodes; DEAD and JOIN ends. */
    {
        const double cos_join = cos(g->prm.join_max_defl_deg * ROAM_PI / 180.0);
        for (i = 0; i < g->nnode; i++) {
            TD5_RoamNode *nd = &g->node[i];
            const TD5_RoamEndRef *r = &g->node_end[nd->end_first];
            if (nd->nend == 0) { nd->kind = 0; continue; }
            if (nd->nend == 1) {
                nd->kind = TD5_ROAM_NODE_END;
                c->dead_ends++;
                fill_dead_end(g, r[0].edge, r[0].end);
                continue;
            }
            if (nd->nend == 2) {
                const TD5_RoamEdge *e1 = &g->edge[r[0].edge], *e2 = &g->edge[r[1].edge];
                double u1x, u1z, u2x, u2z, cd;
                end_out_dir(g, e1, r[0].end, &u1x, &u1z);
                end_out_dir(g, e2, r[1].end, &u2x, &u2z);
                cd = -(u1x * u2x + u1z * u2z);       /* cos(deflection) */
                if (e1->lanes != e2->lanes || fabs(e1->lane_w - e2->lane_w) > 0.5) {
                    nd->kind = TD5_ROAM_NODE_TAPER;
                    c->tapers++;
                } else if (cd < cos_join) {
                    nd->kind = TD5_ROAM_NODE_BEND;
                    c->bends++;
                } else {
                    nd->kind = TD5_ROAM_NODE_JOIN;
                    c->joins++;
                    fill_join(g, nd);
                }
                continue;
            }
            nd->kind = TD5_ROAM_NODE_JUNCTION;
            c->junction_nodes++;
        }
    }

    /* 7. Junction clusters, patches and the CLUSTER ends. */
    if (!td5_roam_junction_build(g)) goto oom;

    free(sel);
    free(wlanes);
    free(wgrp);
    free(dcand);
    free(vstart);
    free(vcount);
    free(vidx);
    free(occ);
    c->ms = td5_plat_time_ms() - t0;
    TD5_LOG_I(LOG_TAG, "roam graph: %d way(s) in window, %d kept, %d node(s) "
              "(%d junction, %d join, %d taper, %d bend, %d dead end), %d edge(s), "
              "%d cluster(s), %d divided carriageway(s) at their own lanes (%d would be narrowed "
              "whole by the T9 rule; per edge: %d of %d edges lose lanes, %.0f m, %d keep them on "
              "narrower lanes, %d of them narrowed for a %.1f m median, %d shifted outward (max %.2f m, %d nodes moved)), %d wide avenue(s), %u ms",
              c->ways_in, c->ways_kept, c->nodes, c->junction_nodes, c->joins,
              c->tapers, c->bends, c->dead_ends, c->edges, c->clusters, c->divided_ways,
              c->median_capped, c->median_capped_edges, c->divided_edges,
              c->median_capped_len / TD5_ROAM_UPM, c->median_narrow_edges, c->median_widened_edges,
              g->prm.median_min / TD5_ROAM_UPM, c->median_shifted_edges, c->median_shift_max / TD5_ROAM_UPM,
              c->median_moved_nodes, c->wide_ways, (unsigned)c->ms);
    return 1;

oom:
    TD5_LOG_E(LOG_TAG, "roam graph: build failed (out of memory or bad order)");
    free(sel);
    free(wlanes);
    free(wgrp);
    free(dcand);
    free(vstart);
    free(vcount);
    free(vidx);
    free(occ);
    td5_roam_graph_free(g);
    return 0;
}

/* ------------------------------------------------------------ signals ------- */

int td5_roam_graph_attach_signals(TD5_RoamGraph *g, const double *xz, int n)
{
    int i, s, k, flagged = 0;
    double r2;
    if (!g || !xz || n <= 0) return 0;
    r2 = g->prm.signal_r * g->prm.signal_r;
    for (i = 0; i < g->nnode; i++) {
        const TD5_RoamNode *nd = &g->node[i];
        int hit = 0;
        for (s = 0; s < n && !hit; s++) {
            const double dx = xz[2 * s] - nd->x, dz = xz[2 * s + 1] - nd->z;
            if (dx * dx + dz * dz <= r2) hit = 1;
        }
        if (!hit) continue;
        for (k = 0; k < nd->nend; k++) {
            const TD5_RoamEndRef *r = &g->node_end[nd->end_first + k];
            TD5_RoamEdgeEnd *en = &g->edge[r->edge].end[r->end];
            if (!en->signal) { en->signal = 1; flagged++; }
        }
    }
    g->census.signals_attached = flagged;
    return flagged;
}
