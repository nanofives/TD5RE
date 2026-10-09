/**
 * td5_tg_geo_street.c -- GEO TRACK street-level reality from real OSM data
 *                        (PORT-ONLY, no original counterpart).
 *
 * Three emitters, one module, because they share one shape: a table of REAL
 * WORLD POSITIONS read from the place cache, snapped to the route in a
 * single-threaded prepass, then drawn by a per-span emitter that only scans a
 * read-only table. That is the contract tg_geo_signals_prepare /
 * tg_emit_geo_signals established (td5_tg_furniture.c:1161-1333) and this
 * follows it exactly, including the drop-counter census, because a new roadside
 * emitter that is gated wrong produces NOTHING while every other number in the
 * log looks healthy -- the R11 SIGNS lesson.
 *
 *   GEO CROSSINGS   a zebra at the position OSM says a crossing is, with the
 *                   paint OSM says it has, replacing the generator's own
 *                   spacing rule on the spans it covers.
 *   GEO BUS STOPS   a pole, and a shelter / bench / bin where a mapper
 *                   surveyed one, on the correct kerb.
 *   GEO FOOTPATHS   mapped plaza, park and pedestrian-street paving drawn
 *                   from the real polyline instead of inferred from a biome.
 *
 * BYTE IDENTITY WITH SYNTHETIC. Every prepass here begins with
 * `if (!td5_geo_loaded()) return;` BEFORE it touches anything else, and every
 * emitter early-outs on an empty table. That order is not decoration: work done
 * ahead of the geo gate runs on a synthetic build too, where a biome's
 * floors_extra is 0 and a `% floors_extra` is a divide by zero -- the failure
 * the 2026-10-08 geo probe hit. The synthetic MODELS.DAT hash is the gate that
 * proves it, and it is checked for this module in the round report.
 *
 * NO NEW TEXTURE PAGES. TD5_TG_PAGE_COUNT is written for EVERY build, synthetic
 * included, so one added page breaks that hash for a reason unrelated to tags.
 * Everything below reuses a shipped page and re-tints it from the vertex
 * diffuse, the trick the geo-signals round established (td5_geo_signals.h:26-30).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5re.h"
#include "td5_platform.h"
#include "td5_config.h"
#include "td5_geo.h"
#include "td5_geo_signals.h"
#include "td5_geo_footways.h"
#include "td5_trackgen.h"
#include "td5_trackgen_internal.h"

#define LOG_TAG "track"

/* Units per metre in the geo frame. The same 430.0 the signals emitter uses
 * (td5_tg_furniture.c:1118); the cache's own units_per_metre is 430 for every
 * place shipped and the radii below are coarse gates, not measurements. */
#define TG_GEOST_UPM        430.0

/* How close to a route node a cache feature must be to be OURS. 30 m, the same
 * threshold and for the same measured reason as TD5_TG_SIG_NEAR_MAX: on
 * la_plata the crossing count within 15/30/50/100 m of the route is
 * 23/30/31/32, so the knee is at 30 m and a wider radius reaches across a
 * block to buy two nodes. */
#define TG_GEOST_NEAR_MAX   (30.0 * TG_GEOST_UPM)

/* ====================================================================== */
/* SECTION: geo crossings -- a zebra where OSM says one is                */
/* ====================================================================== */

/* Two crossing nodes of the same junction (one per approach arm) must not
 * stack two bands of paint on one span. One band per this many spans. */
#define TG_GEOX_SPAN_SEP    2

/* Half-length of the painted band as an in-span fraction. The generator's own
 * zebra spans f 0.22..0.62 (td5_tg_city.c:4606), i.e. 0.40 of a span; this is
 * the same extent, centred on the node instead of on the span. */
#define TG_GEOX_HALF_F      0.20
/* Keep the band off the span seam: a quad that reaches f=0 or f=1 shares an
 * edge with the next span's road quad and flickers along it. */
#define TG_GEOX_F_MARGIN    0.03

/* Same lift, and the same reason, as TD5_TG_CROSS_LIFT: the renderer has no
 * polygon-offset path, so a decal wins the depth test by sitting 20 raw above
 * the tarmac -- enough at the distance a crossing is visible from, too little
 * to read as a step. Identical to the generator's own value on purpose, so a
 * real zebra and a synthetic one never z-fight with each other on a span the
 * suppression window misses. */
#define TG_GEOX_LIFT        TD5_TG_CROSS_LIFT

typedef struct {
    int    si;
    double f;        /* centre of the band, in-span fraction */
    int    paint;    /* TD5_GEO_XP_MARKED or _SIGNALS */
} TG_GeoXing;

static TG_GeoXing *s_gx;
static int         s_gx_n;

/* Spans the real data OWNS. The generator's own crossing rule stands down on
 * these, because two authorities painting the same junction is worse than
 * either alone: the synthetic rule would add a band 8 spans along from a real
 * one and the street would read as a crossing every 30 m. Sized to the span
 * count and allocated in the prepass; NULL on a synthetic build, which is what
 * makes tg_geo_xing_owns_span() answer 0 there without a branch on the place. */
static unsigned char *s_gx_own;
static int            s_gx_own_n;

/* Census. All filled in the SINGLE-THREADED prepass, so no worker increments
 * them -- the one exception is the budget drop, which carries the same caveat
 * the existing furniture counters do. */
static long s_gx_in_cache, s_gx_placed;
static long s_gx_drop_far, s_gx_drop_grid, s_gx_drop_struct;
static long s_gx_drop_paint, s_gx_drop_surface, s_gx_drop_dup, s_gx_drop_budget;

/* Longitudinal position of (wx,wz) within span si, as a fraction of that span's
 * own length, UNCLAMPED. Projection onto the span tangent, which is the same
 * frame tg_road_edge's `f` parameter walks. A negative answer means the point
 * lies behind node si, which is the half of the time tg_guard_nearest_node
 * hands back the node AFTER the point rather than the one before it. */
static double tg_gx_frac_raw(const TG_NodeList *nl, int si,
                             double wx, double wz)
{
    const TG_Node *a = &nl->v[si];
    const TG_Node *b = &nl->v[si + 1];
    const double dx = b->x - a->x, dz = b->z - a->z;
    const double len2 = dx * dx + dz * dz;

    if (len2 <= 0.0) return 0.5;
    return ((wx - a->x) * dx + (wz - a->z) * dz) / len2;
}

/* The span that CONTAINS (wx,wz), and the fraction within it.
 *
 * tg_guard_nearest_node answers with the nearest NODE, which is the span
 * BOUNDARY, so by symmetry about half of all points fall behind it -- and
 * taking that node's span unconditionally pins those to f=0 and pushes the
 * painted band off the real crossing by up to a whole span. Measured on
 * la_plata before this: 9 of 14 placements reported f=0.00. Stepping back one
 * span when the projection is negative puts the band on the crossing instead
 * of at the start of the span after it. `*si` is updated in place; the
 * returned fraction is clamped to [0,1] once the span is settled. */
static double tg_gx_span_frac(const TG_NodeList *nl, int *si,
                              double wx, double wz)
{
    double t = tg_gx_frac_raw(nl, *si, wx, wz);

    if (t < 0.0 && *si > 0) {
        const int prev = *si - 1;
        const double tp = tg_gx_frac_raw(nl, prev, wx, wz);
        /* Only if it really lands in the previous span. A point off the end
         * of a bend can be behind BOTH, and then the original span is still
         * the nearest thing to the truth. */
        if (tp > 0.0) {
            *si = prev;
            t = tp;
        }
    }
    if (t < 0.0) t = 0.0;
    else if (t > 1.0) t = 1.0;
    return t;
}

int tg_geo_xing_owns_span(int si)
{
    if (!s_gx_own || si < 0 || si >= s_gx_own_n) return 0;
    return s_gx_own[si];
}

void tg_geo_xings_prepare(const TG_NodeList *nl, int nspans)
{
    int n, i;
    /* 309 of la_plata's 583 crossing nodes carry no paint tag at all.
     * Painting them would mean asserting "most crossings in this city are
     * marked", which is true of Argentina and is not a fact in the cache --
     * and the whole point of this round is to replace the generator's guess
     * with data. Opt-in, default OFF. */
    const int want_unknown = td5_env_flag_off("TD5RE_GEO_XING_UNKNOWN");

    free(s_gx);
    s_gx = NULL;
    s_gx_n = 0;
    free(s_gx_own);
    s_gx_own = NULL;
    s_gx_own_n = 0;
    s_gx_in_cache = s_gx_placed = 0;
    s_gx_drop_far = s_gx_drop_grid = s_gx_drop_struct = 0;
    s_gx_drop_paint = s_gx_drop_surface = s_gx_drop_dup = s_gx_drop_budget = 0;

    if (!td5_geo_loaded())                    return;   /* byte-identical */
    if (!nl || nl->count < 2)                 return;
    if (!td5_env_flag_on("TD5RE_GEO_XINGS"))  return;

    td5_geo_signals_sync();            /* idempotent; fills the node tables */
    n = td5_geo_crossings_count();
    s_gx_in_cache = n;
    if (n <= 0) return;

    s_gx = (TG_GeoXing *)malloc((size_t)n * sizeof(TG_GeoXing));
    if (!s_gx) return;
    s_gx_own_n = nspans > 0 ? nspans : 0;
    if (s_gx_own_n > 0) {
        s_gx_own = (unsigned char *)calloc((size_t)s_gx_own_n, 1u);
        if (!s_gx_own) s_gx_own_n = 0;
    }

    for (i = 0; i < n; i++) {
        TD5_GeoCrossing c;
        const TG_Node *nd;
        double f;
        int si, j, dup = 0;

        if (!td5_geo_crossing_get(i, &c)) continue;

        /* PAINT FIRST, before any geometry work. An unmarked crossing draws
         * nothing, so there is no point asking where it would go, and the
         * census then separates "the data says no paint" from "a placement
         * gate refused" -- which is the distinction the R11 SIGNS lesson is
         * about. */
        if (c.paint != TD5_GEO_XP_MARKED &&
            !(want_unknown && c.paint == TD5_GEO_XP_UNKNOWN) &&
            c.paint != TD5_GEO_XP_SIGNALS) {
            s_gx_drop_paint++;
            continue;
        }

        si = tg_guard_nearest_node(nl, 0, nl->count - 1, c.x, c.z);
        if (si < 0 || si + 1 >= nl->count) { s_gx_drop_far++; continue; }

        nd = &nl->v[si];
        {
            const double dx = c.x - nd->x, dz = c.z - nd->z;
            if (dx * dx + dz * dz > TG_GEOST_NEAR_MAX * TG_GEOST_NEAR_MAX) {
                s_gx_drop_far++;
                continue;
            }
        }
        /* Settle WHICH span before any per-span gate runs: the correction can
         * move the placement back one span, and a gate asked about the wrong
         * span is a gate asked about the wrong road. */
        f = tg_gx_span_frac(nl, &si, c.x, c.z);

        /* The lead-in is synthetic straight road the conditioner prepended,
         * not geography, and the start grid must stay clear of paint
         * regardless. Same two gates the signal heads take. */
        if (si <= TD5_TG_GRID_SPAN) { s_gx_drop_grid++; continue; }
        if (si >= nspans)           { s_gx_drop_far++;  continue; }
        if (tg_span_in_bridge_run(si) || tg_span_in_tunnel(si) ||
            tg_up_clear_span(si))   { s_gx_drop_struct++; continue; }
        /* [R5 CROSS item 12] white paint reads correctly only on tarmac; on
         * the pale gravel and cobble surfaces it clashes. The generator's own
         * zebra takes this gate (td5_tg_city.c:4614) and a real one has no
         * better claim to ignore it. */
        if (!tg_span_surface_is_tarmac(si)) { s_gx_drop_surface++; continue; }

        for (j = 0; j < s_gx_n; j++) {
            if (abs(s_gx[j].si - si) < TG_GEOX_SPAN_SEP) { dup = 1; break; }
        }
        if (dup) { s_gx_drop_dup++; continue; }

        s_gx[s_gx_n].si    = si;
        s_gx[s_gx_n].f     = f;
        s_gx[s_gx_n].paint = (c.paint == TD5_GEO_XP_SIGNALS)
                             ? TD5_GEO_XP_SIGNALS : TD5_GEO_XP_MARKED;
        s_gx_n++;
    }
    s_gx_placed = s_gx_n;

    /* The suppression window. A real crossing stands the synthetic rule down
     * over the band it paints AND the spans either side, because
     * tg_city_crossing_here's own minimum gap is 8 spans -- without the
     * window the synthetic rule simply paints 8 spans along and the street
     * gets twice the crossings it should. */
    if (s_gx_own) {
        for (i = 0; i < s_gx_n; i++) {
            int j;
            for (j = s_gx[i].si - TD5_TG_XMIN_GAP;
                 j <= s_gx[i].si + TD5_TG_XMIN_GAP; j++)
                if (j >= 0 && j < s_gx_own_n) s_gx_own[j] = 1;
        }
    }
}

int tg_emit_geo_xings(const TG_NodeList *nl, int si,
                      TG_Buf *blk, size_t *moff, int *nmesh, int maxmesh)
{
    int k;

    if (s_gx_n <= 0) return 1;              /* synthetic, or no crossings */
    if (si + 1 >= nl->count) return 1;

    for (k = 0; k < s_gx_n; k++) {
        const TG_GeoXing *p = &s_gx[k];
        double px[4], py[4], pz[4], uu[4], vv[4];
        double l0x, l0y, l0z, r0x, r0y, r0z;
        double l1x, l1y, l1z, r1x, r1y, r1z;
        const double L = (double)nl->v[si].lanes;
        double f0, f1;
        int seg_page = TD5_TG_PAGE_CROSSING, seg_nq = 1;

        if (p->si != si) continue;
        if (*nmesh >= maxmesh) { s_gx_drop_budget++; break; }

        /* Centre the band on the real node, then keep it off both span seams.
         * Clamping rather than spilling into span si+1: a decal that crosses
         * a seam needs two quads whose shared edge must agree to the raw
         * unit, and a band 0.03 shorter is invisible where a seam crack is
         * not. */
        f0 = p->f - TG_GEOX_HALF_F;
        f1 = p->f + TG_GEOX_HALF_F;
        if (f0 < TG_GEOX_F_MARGIN) {
            f0 = TG_GEOX_F_MARGIN;
            if (f1 < f0 + 0.10) f1 = f0 + 0.10;
        }
        if (f1 > 1.0 - TG_GEOX_F_MARGIN) {
            f1 = 1.0 - TG_GEOX_F_MARGIN;
            if (f0 > f1 - 0.10) f0 = f1 - 0.10;
        }

        tg_road_edge(nl, si, f0, 0.0, 1.0,
                     &l0x, &l0y, &l0z, &r0x, &r0y, &r0z);
        tg_road_edge(nl, si, f1, 0.0, 1.0,
                     &l1x, &l1y, &l1z, &r1x, &r1y, &r1z);

        px[0] = r0x; py[0] = r0y + TG_GEOX_LIFT; pz[0] = r0z;
        px[1] = l0x; py[1] = l0y + TG_GEOX_LIFT; pz[1] = l0z;
        px[2] = l1x; py[2] = l1y + TG_GEOX_LIFT; pz[2] = l1z;
        px[3] = r1x; py[3] = r1y + TG_GEOX_LIFT; pz[3] = r1z;

        if (p->paint == TD5_GEO_XP_MARKED) {
            /* A ZEBRA. u runs 0..lanes ACROSS the road, so the page -- whose
             * bars vary in x, i.e. in u (td5_tg_pages.c:729-735) -- tiles once
             * per lane and its bars run ALONG travel, which is how a zebra is
             * painted. Same convention as tg_city_emit_crossing. */
            uu[0] = 0.0; vv[0] = 0.0;
            uu[1] = L;   vv[1] = 0.0;
            uu[2] = L;   vv[2] = 1.0;
            uu[3] = 0.0; vv[3] = 1.0;
        } else {
            /* LIMIT LINES, for `crossing=traffic_signals` with no marking
             * tagged. The SAME page with u and v transposed: u now runs
             * 0..1 ALONG travel, so the page's two bars come out TRANSVERSE
             * -- the pair of lines that bound a signal-controlled crossing,
             * rather than the zebra the data does not claim is there. One
             * page, two markings, no new page and so no TD5_TG_PAGE_COUNT
             * change. */
            uu[0] = 0.0; vv[0] = 0.0;
            uu[1] = 0.0; vv[1] = L;
            uu[2] = 1.0; vv[2] = L;
            uu[3] = 1.0; vv[3] = 0.0;
        }

        tg_acct(TG_ACCT_CROSSING, si);
        moff[(*nmesh)++] = blk->len;
        {   /* [R8 GUARD] Authored kerb to kerb, so the on-road guard's
             * coverage test would drop it on sight. DECAL, not exempt:
             * covering the road is licensed only flush with the tarmac. */
            const size_t d0 = blk->len;
            const int r = tg_write_quad_mesh(blk, px, py, pz, uu, vv, 4,
                                             &seg_page, &seg_nq, 1);
            tg_guard_mark(d0, blk->len, TG_GK_DECAL, si);
            if (!r) return 0;
        }
    }
    return 1;
}

/* ====================================================================== */
/* SECTION: geo bus stops -- a pole, and the furniture a mapper surveyed  */
/* ====================================================================== */

#define TD5_TG_BS_POLE_H     (2.45 * TD5_TG_INFRA_M)
#define TD5_TG_BS_POLE_W     (0.09 * TD5_TG_INFRA_M)
#define TD5_TG_BS_FLAG_H     (0.34 * TD5_TG_INFRA_M)
#define TD5_TG_BS_FLAG_W     (0.40 * TD5_TG_INFRA_M)
#define TD5_TG_BS_FLAG_D     (0.04 * TD5_TG_INFRA_M)
/* A shelter is the one piece here big enough to foul a facade, so it is sized
 * to the narrow end of what a real one is: 3.2 m of frontage, 1.3 m deep. */
#define TD5_TG_BS_SHEL_W     (3.20 * TD5_TG_INFRA_M)
#define TD5_TG_BS_SHEL_D     (1.30 * TD5_TG_INFRA_M)
#define TD5_TG_BS_SHEL_H     (2.35 * TD5_TG_INFRA_M)
#define TD5_TG_BS_ROOF_T     (0.10 * TD5_TG_INFRA_M)
#define TD5_TG_BS_BACK_T     (0.07 * TD5_TG_INFRA_M)
#define TD5_TG_BS_BENCH_W    (1.55 * TD5_TG_INFRA_M)
#define TD5_TG_BS_BENCH_H    (0.46 * TD5_TG_INFRA_M)
#define TD5_TG_BS_BENCH_D    (0.40 * TD5_TG_INFRA_M)
#define TD5_TG_BS_BIN_W      (0.34 * TD5_TG_INFRA_M)
#define TD5_TG_BS_BIN_H      (0.88 * TD5_TG_INFRA_M)

/* Clearance past the carriageway half-width, asked for the same way the
 * signal head and the direction sign ask: about the FOOTPRINT'S INNER EDGE,
 * with the half width added back. */
#define TD5_TG_BS_GAP        TD5_TG_R11_SIGN_GAP

/* Two stops of the same route pair (one each side) are legitimate; two stops
 * on the SAME kerb within this many spans are one stop mapped twice. */
#define TD5_TG_BS_SPAN_SEP   4

#define TD5_TG_BS_MESHES     6   /* pole + flag + roof + back + bench + bin */

typedef struct {
    int    si;
    double side;     /* +1 left of travel, -1 right */
    double gap;      /* clearance past the carriageway half-width */
    double base_y;
    int    shelter, bench, bin;
} TG_BusStopPlace;

static TG_BusStopPlace *s_bs;
static int              s_bs_n;

static long s_bs_in_cache, s_bs_placed;
static long s_bs_drop_far, s_bs_drop_grid, s_bs_drop_struct;
static long s_bs_drop_side, s_bs_drop_street, s_bs_drop_dup, s_bs_drop_budget;
static long s_bs_shelters, s_bs_benches, s_bs_bins;

/* Signed lateral offset of (wx,wz) from span si's centreline; positive is the
 * +1 side. The same cross product every placement in the generator uses, so
 * "side" means here what it means to tg_side_blocked and to the guard. */
static double tg_bs_lateral(const TG_NodeList *nl, int si,
                            double wx, double wz)
{
    const TG_Node *n = &nl->v[si];
    return (wx - n->x) * n->tz - (wz - n->z) * n->tx;
}

void tg_geo_bus_stops_prepare(const TG_NodeList *nl, int nspans)
{
    int n, i;

    free(s_bs);
    s_bs = NULL;
    s_bs_n = 0;
    s_bs_in_cache = s_bs_placed = 0;
    s_bs_drop_far = s_bs_drop_grid = s_bs_drop_struct = 0;
    s_bs_drop_side = s_bs_drop_street = s_bs_drop_dup = s_bs_drop_budget = 0;
    s_bs_shelters = s_bs_benches = s_bs_bins = 0;

    if (!td5_geo_loaded())                       return;   /* byte-identical */
    if (!nl || nl->count < 2)                    return;
    if (!td5_env_flag_on("TD5RE_GEO_BUSSTOPS"))  return;

    td5_geo_signals_sync();
    n = td5_geo_bus_stops_count();
    s_bs_in_cache = n;
    if (n <= 0) return;

    s_bs = (TG_BusStopPlace *)malloc((size_t)n * sizeof(TG_BusStopPlace));
    if (!s_bs) return;

    for (i = 0; i < n; i++) {
        TD5_GeoBusStop b;
        const TG_Node *nd;
        const TG_Biome *bio;
        double lat, gap, sw, side, base_y;
        int si, j, dup = 0;

        if (!td5_geo_bus_stop_get(i, &b)) continue;

        si = tg_guard_nearest_node(nl, 0, nl->count - 1, b.x, b.z);
        if (si < 0 || si + 1 >= nl->count) { s_bs_drop_far++; continue; }

        nd = &nl->v[si];
        {
            const double dx = b.x - nd->x, dz = b.z - nd->z;
            if (dx * dx + dz * dz > TG_GEOST_NEAR_MAX * TG_GEOST_NEAR_MAX) {
                s_bs_drop_far++;
                continue;
            }
        }
        if (si <= TD5_TG_GRID_SPAN) { s_bs_drop_grid++; continue; }
        if (si >= nspans)           { s_bs_drop_far++;  continue; }
        if (tg_span_in_bridge_run(si) || tg_span_in_tunnel(si) ||
            tg_up_clear_span(si))   { s_bs_drop_struct++; continue; }

        /* THE SIDE IS THE DATA'S, NOT A GUESS. A bus stop node sits on the
         * kerb it serves, so its lateral sign IS which kerb -- unlike a
         * signal head, where two nodes of one junction can land either side
         * and the choice is ours. A stop on the wrong kerb would face traffic
         * going the other way. */
        lat  = tg_bs_lateral(nl, si, b.x, b.z);
        side = (lat >= 0.0) ? 1.0 : -1.0;

        if (tg_side_blocked(si, side)) { s_bs_drop_side++; continue; }

        gap = tg_carriageway_clear_gap(nl, si, side,
                                       TD5_TG_BS_GAP - TD5_TG_BS_POLE_W * 0.5,
                                       TD5_TG_CARRIAGEWAY_MARGIN)
              + TD5_TG_BS_POLE_W * 0.5;

        /* A shelter is 1.3 m deep and stands BEHIND the pole, so the mouth
         * test has to be asked about the whole footprint, not about the pole.
         * Asking about the pole alone is how a shelter ends up across a side
         * street's asphalt. */
        if (tg_xstreet_occupies(nl, si, side,
                                gap - TD5_TG_BS_POLE_W * 0.5
                                + (b.shelter ? TD5_TG_BS_SHEL_D : 0.0))) {
            s_bs_drop_street++;
            continue;
        }

        for (j = 0; j < s_bs_n; j++) {
            if (s_bs[j].side == side &&
                abs(s_bs[j].si - si) < TD5_TG_BS_SPAN_SEP) { dup = 1; break; }
        }
        if (dup) { s_bs_drop_dup++; continue; }

        bio = &k_biomes[tg_scenery_biome_index(si)];
        sw  = tg_city_sidewalk_w_at(nl, si, bio);
        /* Paved: stand on the kerb. Unpaved: the skirt drops away from the
         * road, so road height would hang the pole in the air -- the R7 FLORA
         * rule the direction sign and the signal head both follow. */
        if (sw > 0.0) base_y = nd->y + tg_city_kerb_h(bio);
        else          base_y = nd->y - tg_infra_ground_dy(nl, si, side,
                                                          gap, 0.0);

        s_bs[s_bs_n].si      = si;
        s_bs[s_bs_n].side    = side;
        s_bs[s_bs_n].gap     = gap;
        s_bs[s_bs_n].base_y  = base_y;
        s_bs[s_bs_n].shelter = b.shelter;
        s_bs[s_bs_n].bench   = b.bench;
        s_bs[s_bs_n].bin     = b.bin;
        s_bs_shelters += b.shelter;
        s_bs_benches  += b.bench;
        s_bs_bins     += b.bin;
        s_bs_n++;
    }
    s_bs_placed = s_bs_n;
}

int tg_emit_geo_bus_stops(const TG_NodeList *nl, int si,
                          TG_Buf *blk, size_t *moff, int *nmesh, int maxmesh)
{
    int k;

    if (s_bs_n <= 0) return 1;
    if (si + 1 >= nl->count) return 1;

    for (k = 0; k < s_bs_n; k++) {
        const TG_BusStopPlace *p = &s_bs[k];
        const TG_Node *n = &nl->v[si];
        double lx, lz, cx, cz;

        if (p->si != si) continue;
        if (*nmesh + TD5_TG_BS_MESHES > maxmesh) { s_bs_drop_budget++; break; }

        /* Outward normal on this side, and the pole's footing on the kerb. */
        lx =  n->tz * p->side;
        lz = -n->tx * p->side;
        cx = n->x + lx * (n->width * 0.5 + p->gap);
        cz = n->z + lz * (n->width * 0.5 + p->gap);

        /* POLE. A 9 cm box on the sign-post page, re-tinted to galvanised
         * grey from the vertex diffuse rather than given a page of its own. */
        moff[(*nmesh)++] = blk->len;
        if (!tg_emit_box_mesh(blk, cx, p->base_y + TD5_TG_BS_POLE_H * 0.5, cz,
                              TD5_TG_BS_POLE_W * 0.5,
                              TD5_TG_BS_POLE_H * 0.5,
                              TD5_TG_BS_POLE_W * 0.5,
                              n->tx, n->tz, TD5_TG_PAGE_R11_SIGN_POST,
                              TD5_TG_BS_POLE_W, 0xFF6E7276u))
            return 0;

        /* FLAG. The plate at the top that says a bus stops here, broadside to
         * the road so it reads from a car. Red-tinted, the way a stop flag is
         * on nearly every network, and the one piece that makes the pole
         * legible as a BUS STOP rather than as another sign post. */
        moff[(*nmesh)++] = blk->len;
        if (!tg_emit_box_mesh(blk, cx, p->base_y + TD5_TG_BS_POLE_H
                                        - TD5_TG_BS_FLAG_H * 0.5, cz,
                              TD5_TG_BS_FLAG_W * 0.5,
                              TD5_TG_BS_FLAG_H * 0.5,
                              TD5_TG_BS_FLAG_D * 0.5,
                              n->tx, n->tz, TD5_TG_PAGE_R11_SIGN_POST,
                              TD5_TG_BS_FLAG_W, 0xFFC03028u))
            return 0;

        if (p->shelter) {
            /* ROOF and BACK PANEL, both pushed OUTWARD from the pole so the
             * shelter stands behind the kerb line rather than over it. The
             * back panel sits at the far edge of the roof, which is where a
             * real shelter puts it: open to the road, closed to the
             * pavement behind. */
            const double mid = n->width * 0.5 + p->gap
                               + TD5_TG_BS_SHEL_D * 0.5;
            const double bak = n->width * 0.5 + p->gap + TD5_TG_BS_SHEL_D;
            const double rx = n->x + lx * mid, rz = n->z + lz * mid;
            const double bx = n->x + lx * bak, bz = n->z + lz * bak;

            moff[(*nmesh)++] = blk->len;
            if (!tg_emit_box_mesh(blk, rx,
                                  p->base_y + TD5_TG_BS_SHEL_H
                                  + TD5_TG_BS_ROOF_T * 0.5, rz,
                                  TD5_TG_BS_SHEL_W * 0.5,
                                  TD5_TG_BS_ROOF_T * 0.5,
                                  TD5_TG_BS_SHEL_D * 0.5,
                                  n->tx, n->tz, TD5_TG_PAGE_R11_SIGN_POST,
                                  TD5_TG_BS_SHEL_W, 0xFF3A3E44u))
                return 0;

            moff[(*nmesh)++] = blk->len;
            if (!tg_emit_box_mesh(blk, bx,
                                  p->base_y + TD5_TG_BS_SHEL_H * 0.5, bz,
                                  TD5_TG_BS_SHEL_W * 0.5,
                                  TD5_TG_BS_SHEL_H * 0.5,
                                  TD5_TG_BS_BACK_T * 0.5,
                                  n->tx, n->tz, TD5_TG_PAGE_R11_SIGN_POST,
                                  TD5_TG_BS_SHEL_W, 0xFF8A9096u))
                return 0;
        }

        if (p->bench) {
            /* Under the shelter when there is one, against the kerb when
             * there is not -- which is where an unsheltered bench is. */
            const double d = n->width * 0.5 + p->gap
                             + (p->shelter ? TD5_TG_BS_SHEL_D * 0.62
                                           : TD5_TG_BS_BENCH_D);
            const double ex = n->x + lx * d + n->tx * TD5_TG_BS_SHEL_W * 0.22;
            const double ez = n->z + lz * d + n->tz * TD5_TG_BS_SHEL_W * 0.22;

            moff[(*nmesh)++] = blk->len;
            if (!tg_emit_box_mesh(blk, ex,
                                  p->base_y + TD5_TG_BS_BENCH_H * 0.5, ez,
                                  TD5_TG_BS_BENCH_W * 0.5,
                                  TD5_TG_BS_BENCH_H * 0.5,
                                  TD5_TG_BS_BENCH_D * 0.5,
                                  n->tx, n->tz, TD5_TG_PAGE_R11_SIGN_POST,
                                  TD5_TG_BS_BENCH_W, 0xFF6B4A2Eu))
                return 0;
        }

        if (p->bin) {
            /* Beside the pole, one flag-width along the kerb, which keeps it
             * clear of both the pole and the shelter's frontage. */
            const double ix = cx - n->tx * TD5_TG_BS_SHEL_W * 0.40;
            const double iz = cz - n->tz * TD5_TG_BS_SHEL_W * 0.40;

            moff[(*nmesh)++] = blk->len;
            if (!tg_emit_box_mesh(blk, ix,
                                  p->base_y + TD5_TG_BS_BIN_H * 0.5, iz,
                                  TD5_TG_BS_BIN_W * 0.5,
                                  TD5_TG_BS_BIN_H * 0.5,
                                  TD5_TG_BS_BIN_W * 0.5,
                                  n->tx, n->tz, TD5_TG_PAGE_R11_SIGN_POST,
                                  TD5_TG_BS_BIN_W, 0xFF2F4A34u))
                return 0;
        }
    }
    return 1;
}

/* ====================================================================== */
/* SECTION: geo footpaths -- mapped plaza and park paving                 */
/* ====================================================================== */

/* How far from the route a mapped path is still worth drawing. Wider than the
 * 30 m feature radius because a path is GEOMETRY rather than a point: a plaza
 * path that starts 25 m out and runs away from the road is exactly the thing
 * this is for, and the lateral gate below, not this radius, is what keeps
 * paving off the carriageway. */
#define TG_GEOFP_NEAR_MAX   (55.0 * TG_GEOST_UPM)

/* Clearance from the carriageway edge a path must keep. A mapped footway that
 * crosses the road (a desire line through a junction, or a crossing way whose
 * subtype a mapper left off) would otherwise paint slabs over the tarmac. */
#define TG_GEOFP_ROAD_CLEAR (1.20 * TD5_TG_INFRA_M)

/* Default half-width when OSM tags no width. 1.6 m of path is the narrow end
 * of a city plaza walk and the wide end of a park path, and it is a DRAWN
 * width, not a claim about the real one -- 619 of la_plata's 865 pedestrian
 * ways carry no width tag at all. */
#define TG_GEOFP_DEF_W      (1.60 * TD5_TG_INFRA_M)
#define TG_GEOFP_MIN_W      (0.80 * TD5_TG_INFRA_M)
#define TG_GEOFP_MAX_W      (8.00 * TD5_TG_INFRA_M)

/* Lift off the terrain. Larger than the decal lift because this quad sits on
 * the GROUND skirt, which is itself interpolated between span nodes and can
 * rise a few raw between them -- the same reason TD5_TG_VERGE_LIFT is 16. */
#define TG_GEOFP_LIFT       (16.0)

/* A long path chopped into ribbon quads is the bulk of this emitter's mesh
 * cost, so it is capped rather than trusted. 1200 is roughly four quads per
 * span over a 300-span route. */
#define TG_GEOFP_MAX_SEG    1200

typedef struct {
    int    si;                 /* span that draws it                    */
    double ax, az, bx, bz;     /* the segment, world units              */
    double ay, by;             /* ground height at each end             */
    double half;               /* half width, world units               */
    int    page;
} TG_FootSeg;

static TG_FootSeg *s_fp;
static int         s_fp_n;

static long s_fp_ways, s_fp_segs_seen, s_fp_placed;
static long s_fp_drop_far, s_fp_drop_road, s_fp_drop_struct;
static long s_fp_drop_kind, s_fp_drop_cap, s_fp_drop_budget;

/* Which kinds are OURS. SIDEWALK is excluded on purpose: a kerbside pavement
 * is already drawn by the city pavement path, and round 1011's C2 is making
 * that per-side from the same data -- drawing a second slab over it would put
 * two surfaces in one place and z-fight. CROSSING is excluded because the
 * zebra emitter above owns it. What is left is the paving nothing else draws:
 * plaza walks, park paths, pedestrianised streets, and the cycle paths that
 * run beside neither. */
#define TG_GEOFP_KIND_MASK  ((1u << TD5_GEO_FW_FOOTWAY)    | \
                             (1u << TD5_GEO_FW_PATH)       | \
                             (1u << TD5_GEO_FW_PEDESTRIAN) | \
                             (1u << TD5_GEO_FW_CYCLEWAY))

void tg_geo_footpaths_prepare(const TG_NodeList *nl, int nspans)
{
    int n, i;

    free(s_fp);
    s_fp = NULL;
    s_fp_n = 0;
    s_fp_ways = s_fp_segs_seen = s_fp_placed = 0;
    s_fp_drop_far = s_fp_drop_road = s_fp_drop_struct = 0;
    s_fp_drop_kind = s_fp_drop_cap = s_fp_drop_budget = 0;

    if (!td5_geo_loaded())                        return;   /* byte-identical */
    if (!nl || nl->count < 2)                     return;
    if (!td5_env_flag_on("TD5RE_GEO_FOOTPATHS"))  return;

    n = td5_geo_footways_sync(td5_geo_place_slug());
    s_fp_ways = n;
    if (n <= 0) return;

    s_fp = (TG_FootSeg *)malloc((size_t)TG_GEOFP_MAX_SEG * sizeof(TG_FootSeg));
    if (!s_fp) return;

    for (i = 0; i < n; i++) {
        const TD5_GeoFootway *f = td5_geo_footways_get(i);
        double half;
        int k;

        if (!f) continue;
        if (f->kind < 0 || f->kind >= TD5_GEO_FW_KINDS ||
            !(TG_GEOFP_KIND_MASK & (1u << (unsigned)f->kind))) {
            s_fp_drop_kind++;
            continue;
        }
        /* A measured width is a measurement and wins; the default is a drawn
         * width standing in for one, so it is clamped the same way either
         * came out of a hand-edited cache. */
        half = (f->width_m > 0.0)
               ? f->width_m * TG_GEOST_UPM * 0.5 : TG_GEOFP_DEF_W * 0.5;
        if (half < TG_GEOFP_MIN_W * 0.5) half = TG_GEOFP_MIN_W * 0.5;
        if (half > TG_GEOFP_MAX_W * 0.5) half = TG_GEOFP_MAX_W * 0.5;

        for (k = 0; k + 1 < f->count; k++) {
            double ax, az, bx, bz, mx, mz, lat, reach, d;
            const TG_Node *nd;
            double side, ay, by;
            int si;

            if (!td5_geo_footways_point(f, k, &ax, &az)) continue;
            if (!td5_geo_footways_point(f, k + 1, &bx, &bz)) continue;
            s_fp_segs_seen++;

            if (s_fp_n >= TG_GEOFP_MAX_SEG) { s_fp_drop_cap++; continue; }

            mx = (ax + bx) * 0.5;
            mz = (az + bz) * 0.5;

            si = tg_guard_nearest_node(nl, 0, nl->count - 1, mx, mz);
            if (si < 0 || si + 1 >= nl->count) { s_fp_drop_far++; continue; }
            if (si >= nspans)                  { s_fp_drop_far++; continue; }

            nd = &nl->v[si];
            {
                const double dx = mx - nd->x, dz = mz - nd->z;
                if (dx * dx + dz * dz >
                    TG_GEOFP_NEAR_MAX * TG_GEOFP_NEAR_MAX) {
                    s_fp_drop_far++;
                    continue;
                }
            }
            /* A bridge deck and a tunnel bore carry their own floor; paving
             * laid at terrain height there lands inside or under it. */
            if (tg_span_in_bridge_run(si) || tg_span_in_tunnel(si)) {
                s_fp_drop_struct++;
                continue;
            }

            /* THE GATE THAT MATTERS. Both ends must clear the carriageway on
             * this side, not just the midpoint: a segment that runs diagonally
             * out of a junction has a midpoint well off the road and an end
             * squarely on it. */
            lat   = tg_bs_lateral(nl, si, mx, mz);
            side  = (lat >= 0.0) ? 1.0 : -1.0;
            reach = tg_carriageway_reach(nl, si, side) + TG_GEOFP_ROAD_CLEAR;
            {
                const double la = fabs(tg_bs_lateral(nl, si, ax, az));
                const double lb = fabs(tg_bs_lateral(nl, si, bx, bz));
                if (la < reach + half || lb < reach + half) {
                    s_fp_drop_road++;
                    continue;
                }
            }

            /* Ground height at each end, asked at that end's OWN lateral
             * distance: a path running away from the road crosses the skirt,
             * and one height for both ends would bury the far end or float
             * it. */
            d  = fabs(tg_bs_lateral(nl, si, ax, az));
            ay = nd->y - tg_infra_ground_dy(nl, si, side, d, 0.0);
            d  = fabs(tg_bs_lateral(nl, si, bx, bz));
            by = nd->y - tg_infra_ground_dy(nl, si, side, d, 0.0);

            s_fp[s_fp_n].si   = si;
            s_fp[s_fp_n].ax   = ax;
            s_fp[s_fp_n].az   = az;
            s_fp[s_fp_n].bx   = bx;
            s_fp[s_fp_n].bz   = bz;
            s_fp[s_fp_n].ay   = ay;
            s_fp[s_fp_n].by   = by;
            s_fp[s_fp_n].half = half;
            /* A loose-surfaced park path is not paving slabs. The three
             * surface buckets map onto pages that already exist: SMOOTH and
             * COBBLE both read as pavement at this scale, LOOSE takes the
             * biome's own ground page so a dirt path through a park looks
             * like the park it is in. */
            s_fp[s_fp_n].page =
                (f->surface == TD5_GEO_FW_SURF_LOOSE)
                ? k_biomes[tg_scenery_biome_index(si)].ground_page
                : TD5_TG_PAGE_SIDEWALK;
            s_fp_n++;
        }
    }
    s_fp_placed = s_fp_n;
}

int tg_emit_geo_footpaths(const TG_NodeList *nl, int si,
                          TG_Buf *blk, size_t *moff, int *nmesh, int maxmesh)
{
    int k;

    if (s_fp_n <= 0) return 1;
    if (si + 1 >= nl->count) return 1;

    for (k = 0; k < s_fp_n; k++) {
        const TG_FootSeg *s = &s_fp[k];
        double px[4], py[4], pz[4], uu[4], vv[4];
        double dx, dz, len, nx, nz, vrep;
        int seg_page, seg_nq = 1;

        if (s->si != si) continue;
        if (*nmesh >= maxmesh) { s_fp_drop_budget++; break; }

        dx = s->bx - s->ax;
        dz = s->bz - s->az;
        len = sqrt(dx * dx + dz * dz);
        if (len < 1.0) continue;          /* degenerate, already rare */
        nx =  dz / len * s->half;         /* left normal, scaled to half width */
        nz = -dx / len * s->half;

        px[0] = s->ax - nx; py[0] = s->ay + TG_GEOFP_LIFT; pz[0] = s->az - nz;
        px[1] = s->ax + nx; py[1] = s->ay + TG_GEOFP_LIFT; pz[1] = s->az + nz;
        px[2] = s->bx + nx; py[2] = s->by + TG_GEOFP_LIFT; pz[2] = s->bz + nz;
        px[3] = s->bx - nx; py[3] = s->by + TG_GEOFP_LIFT; pz[3] = s->bz - nz;

        /* One page repeat per slab-width across, and the SAME world scale
         * along, so the paving does not stretch on a long segment. The
         * sidewalk page is 16-texel slabs over its 64, i.e. 4 slabs per
         * repeat (td5_tg_pages.c:736-740); TD5_TG_INFRA_M of repeat puts a
         * slab at a quarter of a metre... which is small, so the repeat is
         * one page per 2 m, giving half-metre slabs. */
        vrep = len / (2.0 * TD5_TG_INFRA_M);
        if (vrep < 0.25) vrep = 0.25;
        uu[0] = 0.0; vv[0] = 0.0;
        uu[1] = 1.0; vv[1] = 0.0;
        uu[2] = 1.0; vv[2] = vrep;
        uu[3] = 0.0; vv[3] = vrep;

        seg_page = s->page;
        moff[(*nmesh)++] = blk->len;
        {   /* CITY, the pavement class: this IS pavement, and the prepass has
             * already proved every corner clears the carriageway, so the
             * guard has nothing left to decide about it. */
            const size_t d0 = blk->len;
            const int r = tg_write_quad_mesh(blk, px, py, pz, uu, vv, 4,
                                             &seg_page, &seg_nq, 1);
            tg_guard_mark(d0, blk->len, TG_GK_CITY, si);
            if (!r) return 0;
        }
    }
    return 1;
}

/* ====================================================================== */
/* SECTION: the round's acceptance report                                 */
/* ====================================================================== */

/* A count of ZERO is the failure mode this exists to make loud. The drop
 * counters say WHICH gate refused, which is the difference between "the data
 * has no marked crossings near the route" and "a gate rejected all 13". */
void tg_geo_street_report(int nspans)
{
    if (!td5_geo_loaded()) return;           /* nothing to say on synthetic */

    /* --- crossings --- */
    if (!td5_env_flag_on("TD5RE_GEO_XINGS")) {
        TD5_LOG_I(LOG_TAG, "[GEO XINGS] DISABLED (TD5RE_GEO_XINGS=0)");
    } else if (!s_gx_in_cache) {
        TD5_LOG_I(LOG_TAG, "[GEO XINGS] place %s carries no crossing node(s) "
                  "-- nothing to place", td5_geo_place_slug());
    } else if (!s_gx_placed) {
        TD5_LOG_W(LOG_TAG, "[GEO XINGS] %ld crossing(s) in cache but NONE "
                  "placed over %d spans (paint=%ld far=%ld grid=%ld "
                  "struct=%ld surface=%ld dup=%ld) -- the generator's own "
                  "rule still owns every span",
                  s_gx_in_cache, nspans, s_gx_drop_paint, s_gx_drop_far,
                  s_gx_drop_grid, s_gx_drop_struct, s_gx_drop_surface,
                  s_gx_drop_dup);
    } else {
        int i, own = 0;
        for (i = 0; i < s_gx_own_n; i++) own += s_gx_own[i];
        TD5_LOG_I(LOG_TAG, "[GEO XINGS] cache=%ld placed=%ld over %d spans, "
                  "%d span(s) taken off the synthetic rule | dropped: "
                  "paint=%ld far=%ld grid=%ld struct=%ld surface=%ld "
                  "dup=%ld budget=%ld",
                  s_gx_in_cache, s_gx_placed, nspans, own, s_gx_drop_paint,
                  s_gx_drop_far, s_gx_drop_grid, s_gx_drop_struct,
                  s_gx_drop_surface, s_gx_drop_dup, s_gx_drop_budget);
        for (i = 0; i < s_gx_n; i++)
            TD5_LOG_I(LOG_TAG, "[GEO XINGS]   %d: span %d f=%.2f %s",
                      i, s_gx[i].si, s_gx[i].f,
                      s_gx[i].paint == TD5_GEO_XP_MARKED ? "ZEBRA"
                                                         : "limit lines");
    }

    /* --- bus stops --- */
    if (!td5_env_flag_on("TD5RE_GEO_BUSSTOPS")) {
        TD5_LOG_I(LOG_TAG, "[GEO BUSSTOPS] DISABLED (TD5RE_GEO_BUSSTOPS=0)");
    } else if (!s_bs_in_cache) {
        TD5_LOG_I(LOG_TAG, "[GEO BUSSTOPS] place %s carries no bus-stop "
                  "node(s) -- nothing to place", td5_geo_place_slug());
    } else if (!s_bs_placed) {
        TD5_LOG_W(LOG_TAG, "[GEO BUSSTOPS] %ld stop(s) in cache but NONE "
                  "placed over %d spans (far=%ld grid=%ld struct=%ld "
                  "side=%ld street=%ld dup=%ld)",
                  s_bs_in_cache, nspans, s_bs_drop_far, s_bs_drop_grid,
                  s_bs_drop_struct, s_bs_drop_side, s_bs_drop_street,
                  s_bs_drop_dup);
    } else {
        int i;
        TD5_LOG_I(LOG_TAG, "[GEO BUSSTOPS] cache=%ld placed=%ld over %d spans "
                  "(shelter=%ld bench=%ld bin=%ld) | dropped: far=%ld "
                  "grid=%ld struct=%ld side=%ld street=%ld dup=%ld "
                  "budget=%ld",
                  s_bs_in_cache, s_bs_placed, nspans, s_bs_shelters,
                  s_bs_benches, s_bs_bins, s_bs_drop_far, s_bs_drop_grid,
                  s_bs_drop_struct, s_bs_drop_side, s_bs_drop_street,
                  s_bs_drop_dup, s_bs_drop_budget);
        for (i = 0; i < s_bs_n; i++)
            TD5_LOG_I(LOG_TAG, "[GEO BUSSTOPS]   %d: span %d %s side%s%s%s",
                      i, s_bs[i].si, s_bs[i].side > 0.0 ? "left" : "right",
                      s_bs[i].shelter ? " +shelter" : "",
                      s_bs[i].bench ? " +bench" : "",
                      s_bs[i].bin ? " +bin" : "");
    }

    /* --- footpaths --- */
    if (!td5_env_flag_on("TD5RE_GEO_FOOTPATHS")) {
        TD5_LOG_I(LOG_TAG, "[GEO FOOTPATHS] DISABLED (TD5RE_GEO_FOOTPATHS=0)");
    } else if (!s_fp_ways) {
        TD5_LOG_I(LOG_TAG, "[GEO FOOTPATHS] place %s carries no FOOTWAYS.JSON "
                  "-- cache predates tag_schema 3", td5_geo_place_slug());
    } else if (!s_fp_placed) {
        TD5_LOG_W(LOG_TAG, "[GEO FOOTPATHS] %ld way(s) / %ld segment(s) in "
                  "cache but NONE drawn over %d spans (kind=%ld far=%ld "
                  "road=%ld struct=%ld cap=%ld)",
                  s_fp_ways, s_fp_segs_seen, nspans, s_fp_drop_kind,
                  s_fp_drop_far, s_fp_drop_road, s_fp_drop_struct,
                  s_fp_drop_cap);
    } else {
        int i, runs = 0, last = -99;
        TD5_LOG_I(LOG_TAG, "[GEO FOOTPATHS] ways=%ld segments=%ld drawn=%ld "
                  "over %d spans | dropped: kind=%ld far=%ld road=%ld "
                  "struct=%ld cap=%ld budget=%ld | src=%s",
                  s_fp_ways, s_fp_segs_seen, s_fp_placed, nspans,
                  s_fp_drop_kind, s_fp_drop_far, s_fp_drop_road,
                  s_fp_drop_struct, s_fp_drop_cap, s_fp_drop_budget,
                  td5_geo_footways_source());
        /* WHERE the paving is, so a framedump can be aimed at a plaza instead
         * of hunting for one -- the same reason the signal heads list their
         * spans. The table is in FOOTWAY order, not span order, so this is a
         * lowest-unseen-span sweep rather than a walk: 8 lines at most,
         * because 67 segments cluster into a handful of places and one line
         * each would bury the census above. */
        for (runs = 0; runs < 8; runs++) {
            int best = -1, n = 0;
            for (i = 0; i < s_fp_n; i++)
                if (s_fp[i].si > last && (best < 0 || s_fp[i].si < best))
                    best = s_fp[i].si;
            if (best < 0) break;
            for (i = 0; i < s_fp_n; i++)
                if (s_fp[i].si >= best && s_fp[i].si <= best + 4) n++;
            TD5_LOG_I(LOG_TAG, "[GEO FOOTPATHS]   run %d: span %d (%d quad(s) "
                      "within 4 spans)", runs, best, n);
            last = best + 4;
        }
    }
}
