/**
 * td5_tg_avenue.c -- GEO TRACK: a DIVIDED AVENUE built from the real map
 *                    (PORT-ONLY). Contract: td5_geo_avenues.h.
 *
 * [ROUND 1010 AVENUES] "you didn't properly catch avenues of the selected
 * road, you created branches instead of using the actual map."
 *
 * Round 1009 routed a divided avenue through the SYNTHETIC fork machinery
 * (TG_FORK_ISLAND in td5_tg_branch.c): widen the race road to
 * lanes(A)+lanes(B), split it into two drivable carriageways bowing apart by a
 * fixed `sep`, and drop an island in the gore. This module replaces that with
 * what the map actually says, and the difference is the whole point:
 *
 *   NOTHING HERE IS DRIVABLE. A fork is drivable because of three things that
 *   are not geometry -- the span records (tg_patch_span / tg_append_span), the
 *   corridor jump table, and the TG_WO_DRIVABLE occupancy paint. This module
 *   emits meshes and only meshes, so the race stays on its own carriageway with
 *   its own lanes and the other side of the kerb is scenery that looks like the
 *   road it is.
 *
 *   THE OFFSET IS MEASURED, NOT PLANNED. td5_geo_route.c samples the real
 *   opposite carriageway once per conditioned span and stores the signed
 *   lateral offset in AVENUES.JSON, so the median narrows, widens and ENDS
 *   where the OSM ways do rather than at a bow scale. On Mariano's route
 *   Diagonal 73's last run runs 12.5 -> 15.8 m across its own length; the old
 *   path gave every avenue the same sep 0.16.
 *
 * WHAT IS EMITTED, per span inside an avenue:
 *
 *   1. the OPPOSITE CARRIAGEWAY's road surface, at its real offset, with its
 *      OWN lane count, through the same tg_emit_road_quad_taper every drivable
 *      carriageway uses -- so it carries the same asphalt and the same
 *      one-tile-per-lane markings and reads as a real road;
 *   2. a KERBED MEDIAN ISLAND filling the gap between the two carriageways:
 *      real distance between the centrelines minus both half widths. Skipped
 *      where the sidecar says a real cross street cuts through (`open`), which
 *      is how a real median opens at a junction instead of walling the turn
 *      off, and skipped where the gap is too thin to carry a kerb.
 *
 * BYTE-IDENTICAL SYNTHETIC BUILDS. tg_geo_avenue_n() is the only gate and it
 * returns 0 with no geo place loaded, before any file is opened. No tg_rand /
 * tg_frand / tg_range call is added on either path, so the single RNG stream is
 * untouched -- the standing rule at td5_trackgen_internal.h:1290-1296.
 *
 * TD5RE_GEO_AVENUE_BUILD=0 pins "detect but build nothing" for an A/B.
 */
#include "td5_trackgen_internal.h"
#include "td5_geo.h"              /* is a place loaded */
#include "td5_geo_avenues.h"      /* AVENUES.JSON: the real divided avenues */

/* The island's gap must carry a kerb on both faces and still read as a strip
 * between them. 300 units is 0.7 m -- under that the two carriageways are
 * effectively touching and a raised sliver popping in and out reads worse than
 * a painted gap, the same judgement tg_emit_avenue_divider's sliver reject
 * makes at 200. */
#define TG_AV_MIN_MEDIAN_W   300.0
/* Height of the island above the carriageway. The planted treatment in
 * tg_emit_avenue_divider stands at 220 and is floored to TD5_TG_MEDIAN_MIN_H
 * by [R16 MEDIAN] ("always median with height"); the same number is used here
 * so a geo median and a synthetic one read alike. */
#define TG_AV_ISLAND_H       220.0
/* World units per metre, for the log only -- GR_UNITS_PER_METRE in
 * td5_geo_route.c, geo_common.py's one measured constant. Nothing here is
 * computed in metres; the geometry is all world units end to end. */
#define TG_AV_UNITS_PER_M    430.0

int tg_geo_avenue_n(void)
{
    if (!td5_geo_loaded()) return 0;
    if (!td5_env_flag_on("TD5RE_GEO_AVENUE_BUILD")) return 0;
    return td5_geo_avenues_sync();    /* idempotent: keyed on the loaded slug */
}

/* The avenue's own lateral answer at span si, as the carriageway authority
 * wants it: a POSITIVE distance from the race centreline out to the far edge of
 * the opposite carriageway, on `side`, or 0 where this span has none.
 *
 * This is what keeps facades, trees, guardrails and bridge decks off the
 * scenery carriageway -- they all ask tg_carriageway_reach, which folds this
 * in. One span of slack at each end, for the same reason the fork path takes
 * it: a caller asking about the mouth span must not see a narrower answer than
 * its neighbour or scenery pops in for a single span. */
double tg_geo_avenue_reach(const TG_NodeList *nl, int si, double side)
{
    double best = 0.0;
    int e;
    if (!nl || si < 0) return 0.0;
    if (tg_geo_avenue_n() < 1) return 0.0;
    for (e = -1; e <= 1; e++) {
        double off = 0.0;
        int lanes = 2;
        if (!td5_geo_avenue_at(si + e, &off, &lanes, NULL)) continue;
        if (side * off < 0.0) continue;         /* avenue is on the other side */
        {
            const double out = (off < 0.0 ? -off : off)
                             + (double)lanes * (double)TD5_TG_LANE_WIDTH * 0.5;
            if (out > best) best = out;
        }
    }
    return best;
}

/* Both ends of span si, from the sidecar. The far end falls back to the near
 * one at the last span of a run, which is what tapers the median shut against
 * the point where the real opposite carriageway stops. Returns 0 when this span
 * carries no avenue at all. */
static int tg_av_ends(int si, double *o0, double *o1, int *lanes, int *open)
{
    double a = 0.0, b = 0.0;
    int la = 2, lb = 2, op = 0;
    if (!td5_geo_avenue_at(si, &a, &la, &op)) return 0;
    if (!td5_geo_avenue_at(si + 1, &b, &lb, NULL)) b = a;
    *o0 = a; *o1 = b; *lanes = la; *open = op;
    return 1;
}

/* The opposite carriageway's road surface for span si. Pure mesh: no strip row,
 * no span record, no occupancy paint, so it is never drivable. */
static int tg_av_emit_road(const TG_NodeList *nl, int si, double o0, double o1,
                           int lanes, TG_Buf *blk, size_t *moff, int *nmesh)
{
    const double w0 = nl->v[si].width;
    const double w1 = nl->v[si + 1].width;
    const double ow = (double)lanes * (double)TD5_TG_LANE_WIDTH;
    double ws0, ws1, u_scale;

    if (w0 < 1.0 || w1 < 1.0) return 1;
    ws0 = ow / w0;
    ws1 = ow / w1;
    /* u_scale is "the U reached at the right edge when wscale == 1". With
     * TD5RE_R17_ROADMARK_UV on (the default) tg_emit_road_quad_taper derives U
     * from the PHYSICAL width instead and lands on exactly `lanes`; passing
     * lanes/wscale makes the A/B path agree with it rather than drift. */
    u_scale = (ws0 > 0.0) ? (double)lanes / ws0 : (double)lanes;
    moff[(*nmesh)++] = blk->len;
    if (!tg_emit_road_quad_taper(nl, si, u_scale, o0, o1, ws0, ws1,
                                 tg_road_page(si), blk))
        return 0;
    /* BRANCHROAD, not ROAD: span-scoped exempt is exactly what a carriageway
     * sitting outside the race road's own half width needs from the on-road
     * guard, and it is the kind the fork corridor's quad already uses. */
    tg_guard_mark(moff[*nmesh - 1], blk->len, TG_GK_BRANCHROAD, si);
    return 1;
}

/* The kerbed island between the two carriageways. Same three-quad prism as
 * tg_emit_avenue_divider -- top plus the two road-facing walls, because the
 * driver passes on one side and the scenery road is on the other and each face
 * must survive backface culling -- and the same winding, which that function
 * derives from the emitted faces rather than from trial.
 *
 * The island FILLS the gap kerb to kerb. There is no inset here, unlike the
 * fork gore: the two surfaces are separate quads with a real gap between them,
 * not one floor underlapping both, so filling it is what a real median does and
 * leaves no strip of bare ground beside either lane. */
static int tg_av_emit_island(const TG_NodeList *nl, int si, double o0, double o1,
                             int lanes, TG_Buf *blk, size_t *moff, int *nmesh)
{
    const TG_Node *a = &nl->v[si];
    const TG_Node *c = &nl->v[si + 1];
    const double ohw = (double)lanes * (double)TD5_TG_LANE_WIDTH * 0.5;
    const double sg0 = (o0 >= 0.0) ? 1.0 : -1.0;
    const double sg1 = (o1 >= 0.0) ? 1.0 : -1.0;
    /* The race road's own edge on the avenue's side, and the scenery road's
     * near edge. The median is everything between them. */
    const double in0  = sg0 * a->width * 0.5, out0 = o0 - sg0 * ohw;
    const double in1  = sg1 * c->width * 0.5, out1 = o1 - sg1 * ohw;
    const double mw0  = (out0 > in0 ? out0 - in0 : in0 - out0);
    const double mw1  = (out1 > in1 ? out1 - in1 : in1 - out1);
    /* cl is the more POSITIVE lateral, cr the more negative -- the convention
     * tg_emit_avenue_divider's winding was derived against, which is what lets
     * this work on either side of the road without a second set of faces. */
    const double cl0 = (in0 > out0) ? in0 : out0, cr0 = (in0 > out0) ? out0 : in0;
    const double cl1 = (in1 > out1) ? in1 : out1, cr1 = (in1 > out1) ? out1 : in1;
    double H = TG_AV_ISLAND_H;
    const double base0 = a->y - (double)TD5_TG_GROUND_DROP;
    const double base1 = c->y - (double)TD5_TG_GROUND_DROP;
    double px[12], py[12], pz[12], uu[12], vv[12];
    int seg_page[2], seg_nq[2];
    int n = 0;
    /* A planted top with a concrete KERB on both walls: grass up the side of a
     * median is the [R8 item 8] "grass as walls" mistake, and a real avenue
     * island is a cast kerb holding a planted strip. */
    const int page      = TD5_TG_PAGE_GREEN;
    const int side_page = TD5_TG_PAGE_BRANCH_KERB;

    if (mw0 < TG_AV_MIN_MEDIAN_W && mw1 < TG_AV_MIN_MEDIAN_W) return 1;
    if (td5_env_flag_on("TD5RE_MEDIAN_MIN_H") && H < TD5_TG_MEDIAN_MIN_H)
        H = TD5_TG_MEDIAN_MIN_H;

    /* TOP (up-facing): near +lateral, near -lateral, far -lateral, far +. */
    px[n]=a->x+a->tz*cl0; py[n]=base0+H; pz[n]=a->z-a->tx*cl0; uu[n]=0.0; vv[n]=0.0; n++;
    px[n]=a->x+a->tz*cr0; py[n]=base0+H; pz[n]=a->z-a->tx*cr0; uu[n]=1.0; vv[n]=0.0; n++;
    px[n]=c->x+c->tz*cr1; py[n]=base1+H; pz[n]=c->z-c->tx*cr1; uu[n]=1.0; vv[n]=1.0; n++;
    px[n]=c->x+c->tz*cl1; py[n]=base1+H; pz[n]=c->z-c->tx*cl1; uu[n]=0.0; vv[n]=1.0; n++;

    /* Wall facing +lateral. */
    px[n]=a->x+a->tz*cl0; py[n]=base0;   pz[n]=a->z-a->tx*cl0; uu[n]=0.0; vv[n]=1.0; n++;
    px[n]=a->x+a->tz*cl0; py[n]=base0+H; pz[n]=a->z-a->tx*cl0; uu[n]=0.0; vv[n]=0.0; n++;
    px[n]=c->x+c->tz*cl1; py[n]=base1+H; pz[n]=c->z-c->tx*cl1; uu[n]=1.0; vv[n]=0.0; n++;
    px[n]=c->x+c->tz*cl1; py[n]=base1;   pz[n]=c->z-c->tx*cl1; uu[n]=1.0; vv[n]=1.0; n++;

    /* Wall facing -lateral. */
    px[n]=a->x+a->tz*cr0; py[n]=base0;   pz[n]=a->z-a->tx*cr0; uu[n]=0.0; vv[n]=1.0; n++;
    px[n]=c->x+c->tz*cr1; py[n]=base1;   pz[n]=c->z-c->tx*cr1; uu[n]=1.0; vv[n]=1.0; n++;
    px[n]=c->x+c->tz*cr1; py[n]=base1+H; pz[n]=c->z-c->tx*cr1; uu[n]=1.0; vv[n]=0.0; n++;
    px[n]=a->x+a->tz*cr0; py[n]=base0+H; pz[n]=a->z-a->tx*cr0; uu[n]=0.0; vv[n]=0.0; n++;

    seg_page[0] = page;      seg_nq[0] = 1;
    seg_page[1] = side_page; seg_nq[1] = 2;
    /* Accounted as a FENCE (a linear median structure), the same inventory
     * bucket tg_emit_avenue_divider uses, so "how much median is on this
     * track" stays one number however it was built. */
    tg_acct_n(TG_ACCT_FENCE, si, 1);
    moff[(*nmesh)++] = blk->len;
    if (!tg_write_quad_mesh(blk, px, py, pz, uu, vv, n, seg_page, seg_nq, 2))
        return 0;
    /* TG_GK_ROAD, an EXEMPT kind, and it has to be.
     *
     * MEASURED: marked TG_GK_BRANCHSIDE (a SCENERY class) the on-road guard
     * deleted 447 of these on Mariano's route -- one for every avenue span --
     * and the median came out as flat ground between the two carriageways
     * instead of a kerbed island. Of course it did: the island sits BETWEEN
     * two carriageways, so it is inside the envelope by construction, and
     * "scenery overlapping the carriageway" is exactly what the guard is for.
     *
     * The synthetic divider is exempt the same way, just less visibly: in
     * td5_trackgen.c the whole road+gore+divider byte range is marked
     * TG_GK_ROAD in one call. This is that authored-geometry claim made
     * explicitly for the geo island. */
    tg_guard_mark(moff[*nmesh - 1], blk->len, TG_GK_ROAD, si);
    return 1;
}

/* [ROUND 1011 C2] THE FOOTWAY OUTSIDE THE FAR CARRIAGEWAY.
 *
 * Round 1010 wrote this gap down rather than closing it, at
 * tg_pavement_side_width: "this suppresses the inner pavement without laying
 * one along the FAR edge of the opposite carriageway, which is where the real
 * footway is. That needs its own emitter (the pavement emitters are all
 * anchored on the race kerb)." This is that emitter.
 *
 * It has to live here and not in td5_tg_city.c for the reason that note gives:
 * every city pavement emitter measures outward from the race road's own kerb,
 * and this slab is anchored on the FAR carriageway's outer edge instead -- a
 * lateral only the avenue sidecar knows. The baseline framedump at span 267 of
 * Mariano's La Plata route shows exactly what its absence looks like: the race
 * road, a median, the oncoming carriageway, and then bare grass running up to
 * the buildings.
 *
 * WIDTH comes from the same per-side table the city slab uses, so the footway
 * across the avenue is the width the building-line rule computed for this span
 * rather than a second opinion. Height and pages match the city slab (a kerb
 * face up to TD5_TG_KERB_H, pavement on top), so the two read as one city.
 *
 * Geometry mirrors tg_av_emit_island deliberately -- same cl/cr convention
 * (cl is the more POSITIVE lateral), same two-segment top-plus-wall mesh, same
 * taper from node si to si+1 -- because that winding is already known to
 * survive backface culling from the driver's side.
 *
 * TD5RE_GEO_AVENUE_FARWALK=0 drops it for an A/B. */
static long s_av_farwalk;     /* spans that got one, for the measurement log */

static int tg_av_emit_far_pavement(const TG_NodeList *nl, int si,
                                   double o0, double o1, int lanes,
                                   TG_Buf *blk, size_t *moff, int *nmesh)
{
    const TG_Node *a = &nl->v[si];
    const TG_Node *c = &nl->v[si + 1];
    const double ohw = (double)lanes * (double)TD5_TG_LANE_WIDTH * 0.5;
    const double sg0 = (o0 >= 0.0) ? 1.0 : -1.0;
    const double sg1 = (o1 >= 0.0) ? 1.0 : -1.0;
    /* The pavement's own width for THIS span and THIS side, world units. The
     * avenue is on the side sg points to, which is the `left` flag the geo
     * prepass indexed its table by. 0 means the prepass had no answer here --
     * no mapped way in range, or a synthetic build -- and then there is nothing
     * to lay rather than a guess to lay. */
    const double sw = tg_geo_sidewalk_w_side(si, sg0 > 0.0);
    /* OUTER edge of the far carriageway, and the back edge of the slab. */
    const double e0 = o0 + sg0 * ohw, f0 = e0 + sg0 * sw;
    const double e1 = o1 + sg1 * ohw, f1 = e1 + sg1 * sw;
    const double cl0 = (e0 > f0) ? e0 : f0, cr0 = (e0 > f0) ? f0 : e0;
    const double cl1 = (e1 > f1) ? e1 : f1, cr1 = (e1 > f1) ? f1 : e1;
    const double H = (double)TD5_TG_KERB_H;
    const double b0 = a->y, b1 = c->y;      /* the far carriageway's own plane */
    double px[12], py[12], pz[12], uu[12], vv[12];
    int seg_page[2], seg_nq[2];
    int n = 0;

    if (!(sw > 0.0)) return 1;
    if (!td5_env_flag_on("TD5RE_GEO_AVENUE_FARWALK")) return 1;

    /* TOP. */
    px[n]=a->x+a->tz*cl0; py[n]=b0+H; pz[n]=a->z-a->tx*cl0; uu[n]=0.0; vv[n]=0.0; n++;
    px[n]=a->x+a->tz*cr0; py[n]=b0+H; pz[n]=a->z-a->tx*cr0; uu[n]=1.0; vv[n]=0.0; n++;
    px[n]=c->x+c->tz*cr1; py[n]=b1+H; pz[n]=c->z-c->tx*cr1; uu[n]=1.0; vv[n]=1.0; n++;
    px[n]=c->x+c->tz*cl1; py[n]=b1+H; pz[n]=c->z-c->tx*cl1; uu[n]=0.0; vv[n]=1.0; n++;

    /* Wall facing +lateral. */
    px[n]=a->x+a->tz*cl0; py[n]=b0;   pz[n]=a->z-a->tx*cl0; uu[n]=0.0; vv[n]=1.0; n++;
    px[n]=a->x+a->tz*cl0; py[n]=b0+H; pz[n]=a->z-a->tx*cl0; uu[n]=0.0; vv[n]=0.0; n++;
    px[n]=c->x+c->tz*cl1; py[n]=b1+H; pz[n]=c->z-c->tx*cl1; uu[n]=1.0; vv[n]=0.0; n++;
    px[n]=c->x+c->tz*cl1; py[n]=b1;   pz[n]=c->z-c->tx*cl1; uu[n]=1.0; vv[n]=1.0; n++;

    /* Wall facing -lateral. */
    px[n]=a->x+a->tz*cr0; py[n]=b0;   pz[n]=a->z-a->tx*cr0; uu[n]=0.0; vv[n]=1.0; n++;
    px[n]=c->x+c->tz*cr1; py[n]=b1;   pz[n]=c->z-c->tx*cr1; uu[n]=1.0; vv[n]=1.0; n++;
    px[n]=c->x+c->tz*cr1; py[n]=b1+H; pz[n]=c->z-c->tx*cr1; uu[n]=1.0; vv[n]=0.0; n++;
    px[n]=a->x+a->tz*cr0; py[n]=b0+H; pz[n]=a->z-a->tx*cr0; uu[n]=0.0; vv[n]=0.0; n++;

    seg_page[0] = TD5_TG_PAGE_SIDEWALK;    seg_nq[0] = 1;
    seg_page[1] = TD5_TG_PAGE_BRANCH_KERB; seg_nq[1] = 2;
    moff[(*nmesh)++] = blk->len;
    if (!tg_write_quad_mesh(blk, px, py, pz, uu, vv, n, seg_page, seg_nq, 2))
        return 0;
    /* BRANCHROAD, the same span-scoped exempt kind tg_av_emit_road takes, and
     * for the same reason it gives: this sits outside the race road's own half
     * width, where a SCENERY class would be read as scenery on the carriageway
     * and deleted. */
    tg_guard_mark(moff[*nmesh - 1], blk->len, TG_GK_BRANCHROAD, si);
    s_av_farwalk++;
    return 1;
}

/* ---- the MEASUREMENT, logged ---------------------------------------------
 *
 * The one question this whole round is about is "is the median the width the
 * map says", and it cannot be answered from a frame: a kerb at 3.8 m and a kerb
 * at 6.0 m look the same through a windscreen. So the emitter reports what it
 * built, per avenue, in metres, beside what the sidecar asked for.
 *
 * The ACCUMULATOR is reset when a span is seen at or before the previous run's
 * start, which is what a rebuild (or the second entry of a two-pass build)
 * looks like from here -- there is no end-of-build hook in this module and
 * inventing one would couple it to the generator's loop structure. The summary
 * fires on the avenue's LAST span, which td5_geo_avenue_range already knows.
 *
 * TD5RE_GEO_AVENUE_DIAG=1 adds a line per span (the full table); the per-avenue
 * summary always runs, because a silent median that is the wrong width is the
 * defect this round exists to stop shipping. */
static int    s_av_n;
static int    s_av_last = -1;
static int    s_av_open;
static double s_av_wlo, s_av_whi, s_av_olo, s_av_ohi;

static void tg_av_note(const TG_NodeList *nl, int si, double off, int lanes,
                       int open, double med_w)
{
    const double upm = TG_AV_UNITS_PER_M;
    const double a   = (off < 0.0) ? -off : off;
    int s0 = 0, s1 = 0, i;
    const char *name = "";

    if (si <= s_av_last) { s_av_n = 0; s_av_open = 0;
                           s_av_wlo = s_av_olo = 1e30; s_av_whi = s_av_ohi = 0.0; }
    if (s_av_n == 0)     { s_av_wlo = s_av_olo = 1e30; s_av_whi = s_av_ohi = 0.0; }
    s_av_last = si;
    s_av_n++;
    if (open) s_av_open++;
    if (med_w < s_av_wlo) s_av_wlo = med_w;
    if (med_w > s_av_whi) s_av_whi = med_w;
    if (a < s_av_olo) s_av_olo = a;
    if (a > s_av_ohi) s_av_ohi = a;

    if (getenv("TD5RE_GEO_AVENUE_DIAG"))
        TD5_LOG_I(LOG_TAG, "trackgen: [GEO AVENUE] span %4d off %+7.2f m "
                  "(%+.0f u) opp %d lane(s) own road %.2f m median %.2f m%s",
                  si, off / upm, off, lanes, nl->v[si].width / upm,
                  med_w / upm, open ? " OPEN (cross street)" : "");

    for (i = 0; i < td5_geo_avenues_count(); i++) {
        if (!td5_geo_avenue_range(i, &s0, &s1, &name)) continue;
        if (si != s1) continue;
        TD5_LOG_I(LOG_TAG, "trackgen: [GEO AVENUE] %s spans %d..%d: opposite "
                  "carriageway %.1f..%.1f m away, median built %.2f..%.2f m "
                  "over %d span(s), %d opening(s) for real cross streets; "
                  "far-side footway on %ld span(s)",
                  name, s0, s1, s_av_olo / upm, s_av_ohi / upm,
                  s_av_wlo / upm, s_av_whi / upm, s_av_n, s_av_open,
                  s_av_farwalk);
        s_av_n = 0;        /* the next avenue starts its own accumulation */
        s_av_open = 0;
        break;
    }
}

/* Span si of a geo track: build the divided avenue the map has there, if any.
 * Returns 0 only on an out-of-memory write. */
int tg_emit_geo_avenue(const TG_NodeList *nl, int si, TG_Buf *blk,
                       size_t *moff, int *nmesh)
{
    double o0 = 0.0, o1 = 0.0;
    int lanes = 2, open = 0;

    if (!nl || si < 0 || si + 1 >= nl->count) return 1;
    if (tg_geo_avenue_n() < 1) return 1;
    if (!tg_av_ends(si, &o0, &o1, &lanes, &open)) return 1;
    /* The sidecar's own floor is nominal (it only knows TD5_TG_LANE_WIDTH).
     * Re-check against the REAL node width here, which is the authority: an
     * offset inside the race road's own half width would lay the scenery
     * carriageway in the live lanes. */
    if ((o0 < 0.0 ? -o0 : o0) <= nl->v[si].width * 0.5) return 1;

    {
        const double ohw = (double)lanes * (double)TD5_TG_LANE_WIDTH * 0.5;
        const double a   = (o0 < 0.0) ? -o0 : o0;
        tg_av_note(nl, si, o0, lanes, open, a - nl->v[si].width * 0.5 - ohw);
    }
    if (!tg_av_emit_road(nl, si, o0, o1, lanes, blk, moff, nmesh)) return 0;
    /* A real cross street cuts the median here, so it opens for the turn --
     * and so does everything beyond it. [1011 C2] The far footway takes the
     * SAME gate as the island rather than one of its own: a car turning across
     * the median crosses the far carriageway and its pavement too, so a slab
     * left standing here would be a kerb across the mouth of a real street. */
    if (open) return 1;
    if (!tg_av_emit_far_pavement(nl, si, o0, o1, lanes, blk, moff, nmesh))
        return 0;
    return tg_av_emit_island(nl, si, o0, o1, lanes, blk, moff, nmesh);
}
