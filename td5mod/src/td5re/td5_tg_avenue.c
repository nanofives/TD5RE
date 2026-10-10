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
/* [ROUND 1015 A item 1] The ONE SPAN OF SLACK tg_geo_avenue_reach (and the rail ownership
 * rule) take on either side of an avenue's rows is there so a caller asking about a mouth
 * span does not see a narrower answer than its neighbour. At the avenue's ENTRY it made the
 * span in front of the first row claim a carriageway that starts a node later: the ground
 * skirt was pushed out and the pavement and railing stood down, over a span that has no
 * carriageway at all -- "the road at span 38 is not rendered ... it starts rendering after
 * span 46" (Diagonal 73, first span 47): a see-through hole beside the widened road. So the
 * slack AHEAD (e = +1) is only taken by a span that is itself in the avenue.
 * TD5RE_GEO_AVENUE_ENTRY_SLACK=1 restores it. */
int tg_geo_avenue_slack_ok(int si, int e)
{
    if (e != 1) return 1;
    if (td5_env_flag_off("TD5RE_GEO_AVENUE_ENTRY_SLACK")) return 1;
    return td5_geo_avenue_at(si, NULL, NULL, NULL);
}

double tg_geo_avenue_reach(const TG_NodeList *nl, int si, double side)
{
    double best = 0.0;
    int e;
    if (!nl || si < 0) return 0.0;
    if (tg_geo_avenue_n() < 1) return 0.0;
    /* [ROUND 1014 A] Over a REAL fork's corridor spans the corridor IS the far
     * carriageway, at ITS OWN lateral, which starts at the main half's edge and
     * opens out to the real position over the median slope. The real
     * carriageway's outer edge is up to a median-width (1.6 m at the mouth of
     * fork 0) beyond it there, so answering with the real edge pushed the ground
     * skirt's inner edge out by the same amount past where the corridor's
     * pavement ends: a see-through wedge beside the first corridor spans
     * ("sidewalk starts opening here ... its side is empty"). The fork loop in
     * tg_carriageway_reach answers for the corridor's actual edge. */
    if (td5_env_flag_on("TD5RE_GEO_FORK_REACH")) {
        const int fi = tg_fork_of_main(si);
        if (fi >= 0 && s_forks[fi].real > 0 && side * (double)s_forks[fi].side >= 0.0)
            return 0.0;
    }
    for (e = -1; e <= 1; e++) {
        double off = 0.0;
        int lanes = 2;
        if (!tg_geo_avenue_slack_ok(si, e)) continue;
        if (!td5_geo_avenue_at(si + e, &off, &lanes, NULL)) continue;
        /* [ROUND 1013 F2] The sidecar's offset is measured from the route
         * carriageway's own centre. Over a REAL fork's window the walk moved the
         * node toward the corridor, so the same road is that much nearer. */
        off -= tg_realfork_node_delta(si + e);
        if (side * off < 0.0) continue;         /* avenue is on the other side */
        {
            const double out = (off < 0.0 ? -off : off)
                             + (double)lanes * (double)TD5_TG_LANE_WIDTH * 0.5;
            if (out > best) best = out;
        }
    }
    return best;
}

/* [ROUND 1015 A] "there should be a break in the median (and the fork) at crossing
 * streets ... real median openings at OSM cross streets must be open (paved), not
 * grass across the street" (Mariano, picks 6 and 13). Is main span `si` inside a REAL
 * fork's window AND on a real median opening of AVENUES.JSON?
 *
 * Round 1014 A merged the blocks of an avenue into ONE long corridor so the windows of
 * neighbouring blocks stopped fighting; the cost was that the raised island and the
 * gore ran straight across every cross street. The corridor itself stays continuous (a
 * fork cannot be cut mid-way: its span records, jump table and AI choice are one
 * unit), but the median between the two carriageways is the part that is NOT road, and
 * at an opening it is paved flush and carries no island, the same treatment the
 * scenery avenue gives it (tg_av_emit_opening). Both ends of the island get a cap
 * through tg_median_at_raw, which mirrors this. TD5RE_GEO_FORK_OPENING=0 restores the
 * unbroken median. */
int tg_fork_opening_at(int si)
{
    int fi, op = 0;
    if (!td5_env_flag_on("TD5RE_GEO_FORK_OPENING")) return 0;
    fi = tg_fork_of_main(si);
    if (fi < 0 || s_forks[fi].real <= 0) return 0;
    if (tg_geo_avenue_n() < 1) return 0;
    return td5_geo_avenue_at(si, NULL, NULL, &op) && op;
}

int tg_realfork_walk_owned(int fi, int mb)
{
    if (!td5_env_flag_on("TD5RE_GEO_FORK_MERGE")) return 0;
    if (!td5_env_flag_on("TD5RE_GEO_AVENUE_FARWALK")) return 0;
    if (fi < 0 || fi >= s_fork_count || s_forks[fi].real <= 0) return 0;
    if (tg_geo_avenue_n() < 1) return 0;
    return tg_geo_sidewalk_w_side(mb, s_forks[fi].side > 0) > 0.0;
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
/* [ROUND 1013 F2] ONE END of the opposite carriageway, clipped.
 *
 * Over a REAL fork's window the race road is wider than the lanes it carries --
 * it holds the corridor's lanes beside its own, so the fork can split -- and that
 * widening reaches over the median into the real opposite carriageway's ground.
 * The scenery road therefore starts where the race road ENDS, never inside it.
 * Outside every window the race road is its own lanes, the clip never bites, and
 * this returns exactly the near/far edges the old code used. */
static int tg_av_end_clip(double off, int lanes, double node_w, double *centre,
                          double *width)
{
    const double sg  = (off >= 0.0) ? 1.0 : -1.0;
    const double a   = (off < 0.0) ? -off : off;
    const double ohw = (double)lanes * (double)TD5_TG_LANE_WIDTH * 0.5;
    const double re  = node_w * 0.5;
    double edge_in = a - ohw, edge_out = a + ohw;   /* (near/far are Win32 macros) */
    if (edge_in < re) edge_in = re;
    if (edge_out < edge_in + 1.0) {  /* the race road swallows it at this end */
        *centre = sg * (re + 0.5);
        *width  = 1.0;
        return 0;
    }
    *centre = sg * (edge_in + edge_out) * 0.5;
    *width  = edge_out - edge_in;
    return 1;
}

static int tg_av_emit_road(const TG_NodeList *nl, int si, double o0, double o1,
                           int lanes, TG_Buf *blk, size_t *moff, int *nmesh)
{
    const double w0 = nl->v[si].width;
    const double w1 = nl->v[si + 1].width;
    double ws0, ws1, u_scale, c0, c1, cw0, cw1;
    int live0, live1;

    if (w0 < 1.0 || w1 < 1.0) return 1;
    live0 = tg_av_end_clip(o0, lanes, w0, &c0, &cw0);
    live1 = tg_av_end_clip(o1, lanes, w1, &c1, &cw1);
    if (!live0 && !live1) return 1;          /* wholly under the race road */
    o0 = c0; o1 = c1;
    ws0 = cw0 / w0;
    ws1 = cw1 / w1;
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

/* [ROUND 1014 A] The paved gap of a real median OPENING: flush asphalt from the race
 * road's edge to the opposite carriageway's near edge, on the same road page, so the
 * two carriageways read as joined by a junction instead of separated by a slot. Pure
 * mesh, never drivable (the race stays on its own carriageway). */
static int tg_av_emit_opening(const TG_NodeList *nl, int si, double o0, double o1,
                              int lanes, TG_Buf *blk, size_t *moff, int *nmesh,
                              double min_w)
{
    const double w0 = nl->v[si].width, w1 = nl->v[si + 1].width;
    const double ohw = (double)lanes * (double)TD5_TG_LANE_WIDTH * 0.5;
    const double sg0 = (o0 >= 0.0) ? 1.0 : -1.0, sg1 = (o1 >= 0.0) ? 1.0 : -1.0;
    const double in0 = sg0 * w0 * 0.5, in1 = sg1 * w1 * 0.5;
    double out0 = o0 - sg0 * ohw, out1 = o1 - sg1 * ohw;
    double cw0, cw1, c0, c1, ws0, ws1;
    if (w0 < 1.0 || w1 < 1.0) return 1;
    if (sg0 * (out0 - in0) < 0.0) out0 = in0;
    if (sg1 * (out1 - in1) < 0.0) out1 = in1;
    cw0 = (out0 > in0) ? out0 - in0 : in0 - out0;
    cw1 = (out1 > in1) ? out1 - in1 : in1 - out1;
    if (cw0 < min_w && cw1 < min_w) return 1;
    c0 = (in0 + out0) * 0.5; c1 = (in1 + out1) * 0.5;
    ws0 = cw0 / w0; ws1 = cw1 / w1;
    if (ws0 < 0.01) ws0 = 0.01;
    if (ws1 < 0.01) ws1 = 0.01;
    moff[(*nmesh)++] = blk->len;
    if (!tg_emit_road_quad_taper(nl, si, (cw0 / (double)TD5_TG_LANE_WIDTH) / ws0,
                                 c0, c1, ws0, ws1, tg_road_page(si), blk))
        return 0;
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
/* [ROUND 1014 A] Does the SCENERY island stand on span s? The same gates
 * tg_av_emit_island and tg_emit_geo_avenue apply, asked of a neighbour so a run's two
 * ends can be CAPPED (tg_emit_avenue_divider does the same with tg_r12_median_at). An
 * uncapped prism is open at both ends: you look straight into the hollow kerb, and
 * past it there is nothing -- "there is no geometry after this median". */
static int tg_av_island_at(const TG_NodeList *nl, int s)
{
    double a = 0.0, b = 0.0;
    int la = 2, lb = 2, op = 0, fi;
    if (!nl || s < 0 || s + 1 >= nl->count) return 0;
    if (!td5_geo_avenue_at(s, &a, &la, &op)) return 0;
    if (!td5_geo_avenue_at(s + 1, &b, &lb, NULL)) b = a;
    a -= tg_realfork_node_delta(s);
    b -= tg_realfork_node_delta(s + 1);
    fi = tg_fork_of_main(s);
    if (fi >= 0 && s_forks[fi].real > 0) return 0;   /* the fork's own island */
    if (op) return 0;
    {
        const double ohw = (double)la * (double)TD5_TG_LANE_WIDTH * 0.5;
        const double sg0 = (a >= 0.0) ? 1.0 : -1.0, sg1 = (b >= 0.0) ? 1.0 : -1.0;
        double out0 = a - sg0 * ohw, out1 = b - sg1 * ohw;
        const double in0 = sg0 * nl->v[s].width * 0.5, in1 = sg1 * nl->v[s + 1].width * 0.5;
        if (sg0 * (out0 - in0) < 0.0) out0 = in0;
        if (sg1 * (out1 - in1) < 0.0) out1 = in1;
        {
            const double mw0 = (out0 > in0 ? out0 - in0 : in0 - out0);
            const double mw1 = (out1 > in1 ? out1 - in1 : in1 - out1);
            if (mw0 < TG_AV_MIN_MEDIAN_W && mw1 < TG_AV_MIN_MEDIAN_W) return 0;
        }
    }
    return 1;
}

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
    const double in0  = sg0 * a->width * 0.5;
    const double in1  = sg1 * c->width * 0.5;
    /* [ROUND 1013 F2] Where a REAL fork's widened race road reaches past the
     * opposite carriageway's near edge there is no median to build: clamp the
     * edge to the race road's, so the island tapers to nothing instead of
     * standing across tarmac (the old |out - in| read that overlap as a median). */
    double out0 = o0 - sg0 * ohw, out1 = o1 - sg1 * ohw;
    if (sg0 * (out0 - in0) < 0.0) out0 = in0;
    if (sg1 * (out1 - in1) < 0.0) out1 = in1;
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
    double px[20], py[20], pz[20], uu[20], vv[20];
    int seg_page[2], seg_nq[2];
    int n = 0;
    const int caps_on = td5_env_flag_on("TD5RE_GEO_AVENUE_CAPS");
    const int cap_in  = caps_on && !tg_av_island_at(nl, si - 1);
    const int cap_out = caps_on && !tg_av_island_at(nl, si + 1);
    /* A planted top with a concrete KERB on both walls: grass up the side of a
     * median is the [R8 item 8] "grass as walls" mistake, and a real avenue
     * island is a cast kerb holding a planted strip. */
    const int page      = TD5_TG_PAGE_GREEN;
    const int side_page = TD5_TG_PAGE_BRANCH_KERB;

    if (mw0 < TG_AV_MIN_MEDIAN_W && mw1 < TG_AV_MIN_MEDIAN_W) {
        /* [ROUND 1015 A] "the end of the median has no geometry, just void" (pick 3, span
         * 77). Where the median tapers shut (the two carriageways meet at a Y, or the
         * race road's fork ramp widens toward the far one) the island stops under 0.7 m
         * -- and the sliver of gap between the two roads beyond it had NOTHING in it: the
         * ground skirt starts outside the far carriageway, so you looked through the gap
         * to the sky. Pave the sliver flush, like an opening, however thin it is.
         * TD5RE_GEO_AVENUE_WEDGE=0 restores the empty gap. */
        if (td5_env_flag_on("TD5RE_GEO_AVENUE_WEDGE") && (mw0 > 1.0 || mw1 > 1.0))
            return tg_av_emit_opening(nl, si, o0, o1, lanes, blk, moff, nmesh, 1.0);
        return 1;
    }
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

    /* [ROUND 1014 A] END CAPS, the faces tg_emit_avenue_divider closes its runs with
     * (same winding, derived there). Leading: faces back down the road. */
    if (cap_in) {
        px[n]=a->x+a->tz*cl0; py[n]=base0;   pz[n]=a->z-a->tx*cl0; uu[n]=0.0; vv[n]=1.0; n++;
        px[n]=a->x+a->tz*cr0; py[n]=base0;   pz[n]=a->z-a->tx*cr0; uu[n]=1.0; vv[n]=1.0; n++;
        px[n]=a->x+a->tz*cr0; py[n]=base0+H; pz[n]=a->z-a->tx*cr0; uu[n]=1.0; vv[n]=0.0; n++;
        px[n]=a->x+a->tz*cl0; py[n]=base0+H; pz[n]=a->z-a->tx*cl0; uu[n]=0.0; vv[n]=0.0; n++;
    }
    if (cap_out) {
        px[n]=c->x+c->tz*cl1; py[n]=base1+H; pz[n]=c->z-c->tx*cl1; uu[n]=0.0; vv[n]=0.0; n++;
        px[n]=c->x+c->tz*cr1; py[n]=base1+H; pz[n]=c->z-c->tx*cr1; uu[n]=1.0; vv[n]=0.0; n++;
        px[n]=c->x+c->tz*cr1; py[n]=base1;   pz[n]=c->z-c->tx*cr1; uu[n]=1.0; vv[n]=1.0; n++;
        px[n]=c->x+c->tz*cl1; py[n]=base1;   pz[n]=c->z-c->tx*cl1; uu[n]=0.0; vv[n]=1.0; n++;
    }
    seg_page[0] = page;      seg_nq[0] = 1;
    seg_page[1] = side_page; seg_nq[1] = 2 + (cap_in ? 1 : 0) + (cap_out ? 1 : 0);
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
/* Spans that got one. CUMULATIVE over the whole track, unlike the per-avenue
 * accumulators below which reset at each new avenue -- the summary line says so,
 * because a reader comparing "87 / 228 / 336" against three avenues of 90 / 146
 * / 112 spans would otherwise read three per-avenue counts that nearly fit. */
static long s_av_farwalk;

/* [ROUND 1014 A] The footway's OUTER-edge lateral at both ends of span s and its width,
 * exactly as tg_av_emit_far_pavement will lay it, or 0 when span s lays none. Asked of
 * the neighbours so a run's two ends (a corridor mouth, an opening, the avenue's own
 * ends) get an END CAP: an uncapped slab is hollow at its ends -- you look straight into
 * it, which is the "side is empty" of the report. */
/* [1014 integ] Does a real street leave the far kerb at span s on the side the avenue
 * (or the real fork's corridor) is on? Round 1014 B opens the pavement for it on the
 * plain-avenue path (below); round 1014 A's real-fork path and its sidecar `open` path
 * called the footway emitter straight, on the grounds that "no cross-street arm is
 * drawn through it" -- true until B put the far-side streets there, whose first span
 * then lay UNDER a raised slab with a kerb face across the mouth. `o` is the
 * carriageway's signed lateral (the sidecar's, or the corridor's). */
static int tg_av_far_mouth(int s, double o)
{
    if (!td5_env_flag_on("TD5RE_GEO_FORK_MOUTH_OPEN")) return 0;
    return tg_net_mouth_shift(s, o > 0.0) > 0.0 &&
           tg_net_mouth_kind(s, o > 0.0) >= 0;
}

/* [ROUND 1015 A item 5] "this sidewalk is not properly aligned to the crossing road"
 * (pick e22 s15, a far footway beside Diagonal 73 where a calle meets it).
 *
 * The footway is broken on the spans the street mouth table lists (tg_av_far_mouth), but a
 * street that leaves the kerb at 45 degrees sweeps ALONG the road as it moves out, so at the
 * footway's lateral it covers part of the span NEXT to the mouth: the slab ended square at a
 * span boundary and its last metres stood on the street's tarmac, at an angle to the street's
 * own edge. Round 1015 B audited it as `branchroad over city:101` and tried dropping the whole
 * span, which leaves a 1500-unit hole.
 *
 * So the slab is CLIPPED instead: the street surface is the quad tg_city_emit_crossstreet lays
 * (the mouth's kerb segment swept along its outward bearing for the mouth's reach), the inner
 * and outer edge of the slab are sampled along the span against the mouths of the spans around
 * it, and the slab keeps its longest free run, cut along the street's own oblique edge with an
 * end cap on the cut. TD5RE_GEO_FAR_STREET_CLIP=0 restores the span-granular slab. */
#define TG_AV_CLIP_N 16
static int tg_av_pt_in_quad(const double *q, double x, double z)
{
    int i, pos = 0, neg = 0;
    for (i = 0; i < 4; i++) {
        const double ax = q[i * 2], az = q[i * 2 + 1];
        const double bx = q[((i + 1) & 3) * 2], bz = q[((i + 1) & 3) * 2 + 1];
        const double c = (bx - ax) * (z - az) - (bz - az) * (x - ax);
        if (c > 0.0) pos = 1; else if (c < 0.0) neg = 1;
    }
    return !(pos && neg);
}

/* The slab's point at fraction u along span s on the line at lateral lat0 + (lat1 - lat0) u. */
static void tg_av_slab_pt(const TG_NodeList *nl, int s, double u, double lat0, double lat1,
                          double *x, double *z, double *y)
{
    const TG_Node *a = &nl->v[s], *c = &nl->v[s + 1];
    const double lat = lat0 + (lat1 - lat0) * u;
    const double tz = a->tz + (c->tz - a->tz) * u, tx = a->tx + (c->tx - a->tx) * u;
    *x = a->x + (c->x - a->x) * u + tz * lat;
    *z = a->z + (c->z - a->z) * u - tx * lat;
    *y = a->y + (c->y - a->y) * u;
}

static int tg_av_blocked_at(const TG_NodeList *nl, int s, double u, double lat0, double lat1,
                            const double q[][8], int nq)
{
    double x, z, y;
    int k;
    tg_av_slab_pt(nl, s, u, lat0, lat1, &x, &z, &y);
    for (k = 0; k < nq; k++)
        if (tg_av_pt_in_quad(q[k], x, z)) return 1;
    return 0;
}

/* Longest free run [*from, *to] of the line (lat0 -> lat1) on span s; 0 when none,
 * 1 = the whole span is free, 2 = clipped. */
static int tg_av_line_free(const TG_NodeList *nl, int s, double lat0, double lat1,
                           const double q[][8], int nq, double *from, double *to)
{
    unsigned blocked = 0;
    int i, k, best0 = -1, best1 = -1, run0 = -1;
    double lo, hi;
    for (i = 0; i <= TG_AV_CLIP_N; i++)
        if (tg_av_blocked_at(nl, s, (double)i / TG_AV_CLIP_N, lat0, lat1, q, nq))
            blocked |= 1u << i;
    if (!blocked) { *from = 0.0; *to = 1.0; return 1; }
    for (i = 0; i <= TG_AV_CLIP_N + 1; i++) {
        const int free_i = (i <= TG_AV_CLIP_N) && !(blocked & (1u << i));
        if (free_i && run0 < 0) run0 = i;
        if (!free_i && run0 >= 0) {
            if (best0 < 0 || (i - 1 - run0) > (best1 - best0)) { best0 = run0; best1 = i - 1; }
            run0 = -1;
        }
    }
    if (best0 < 0) return 0;
    lo = (double)best0 / TG_AV_CLIP_N;
    hi = (double)best1 / TG_AV_CLIP_N;
    if (best0 > 0) {                         /* bisect the cut between sample best0-1 (blocked) and best0 */
        double f = (double)(best0 - 1) / TG_AV_CLIP_N, t = lo;
        for (k = 0; k < 6; k++) {
            const double m = 0.5 * (f + t);
            if (tg_av_blocked_at(nl, s, m, lat0, lat1, q, nq)) f = m; else t = m;
        }
        lo = t;
    }
    if (best1 < TG_AV_CLIP_N) {              /* and between best1 and best1+1 (blocked) */
        double f = hi, t = (double)(best1 + 1) / TG_AV_CLIP_N;
        for (k = 0; k < 6; k++) {
            const double m = 0.5 * (f + t);
            if (tg_av_blocked_at(nl, s, m, lat0, lat1, q, nq)) t = m; else f = m;
        }
        hi = f;
    }
    *from = lo; *to = hi;
    return 2;
}

/* The street quads of the mouths around span s on the footway's side. */
static int tg_av_street_quads(const TG_NodeList *nl, int s, double sg, double q[][8])
{
    const int left = (sg > 0.0);
    int m, nq = 0;
    for (m = s - 3; m <= s + 3 && nq < 7; m++) {
        double sk = 0.0, rr = 0.0, ef[10], no[2], fo[2], cs, sn;
        if (m < 1 || m + 1 >= nl->count) continue;
        if (!(tg_net_mouth_shift(m, left) > 0.0) || tg_net_mouth_kind(m, left) < 0) continue;
        if (tg_net_mouth(m, left, &sk, &rr) < 0 || !(rr > 0.0)) continue;
        tg_city_edge_frame(nl, m, sg, ef);
        cs = cos(sk); sn = sin(sk);
        no[0] = ef[6] * cs - ef[7] * sn; no[1] = ef[6] * sn + ef[7] * cs;
        fo[0] = ef[8] * cs - ef[9] * sn; fo[1] = ef[8] * sn + ef[9] * cs;
        q[nq][0] = ef[0];                q[nq][1] = ef[2];
        q[nq][2] = ef[0] + no[0] * rr;   q[nq][3] = ef[2] + no[1] * rr;
        q[nq][4] = ef[3] + fo[0] * rr;   q[nq][5] = ef[5] + fo[1] * rr;
        q[nq][6] = ef[3];                q[nq][7] = ef[5];
        nq++;
    }
    return nq;
}

/* Free part of span s's slab: 0 = none, 1 = whole span, 2 = clipped (ui/uo set). The slab's
 * inner edge is at e, its outer at f (signed laterals at both ends of the span). */
static int tg_av_far_clip(const TG_NodeList *nl, int s, double e0, double e1, double f0, double f1,
                          double sg, double *ui0, double *ui1, double *uo0, double *uo1)
{
    double q[7][8];
    int nq, ri, ro;
    *ui0 = *uo0 = 0.0; *ui1 = *uo1 = 1.0;
    if (!td5_env_flag_on("TD5RE_GEO_FAR_STREET_CLIP")) return 1;
    if (!nl || s < 0 || s + 1 >= nl->count) return 1;
    nq = tg_av_street_quads(nl, s, sg, q);
    if (nq < 1) return 1;
    ri = tg_av_line_free(nl, s, e0, e1, (const double (*)[8])q, nq, ui0, ui1);
    ro = tg_av_line_free(nl, s, f0, f1, (const double (*)[8])q, nq, uo0, uo1);
    if (!ri || !ro) return 0;
    if (*ui1 - *ui0 < 0.06 && *uo1 - *uo0 < 0.06) return 0;
    return (ri == 1 && ro == 1) ? 1 : 2;
}

static int tg_av_far_edges(const TG_NodeList *nl, int s, double *e0, double *e1,
                           double *sw)
{
    double a = 0.0, b = 0.0, o0, o1;
    int la = 2, op = 0, fi, lanes, in_fork = 0;
    double sg0, sg1, w;
    if (!nl || s < 0 || s + 1 >= nl->count) return 0;
    if (!td5_geo_avenue_at(s, &a, &la, &op)) return 0;
    if (!td5_geo_avenue_at(s + 1, &b, NULL, NULL)) b = a;
    a -= tg_realfork_node_delta(s);
    b -= tg_realfork_node_delta(s + 1);
    fi = tg_fork_of_main(s);
    if (fi >= 0 && s_forks[fi].real > 0) {
        const int j = s - s_forks[fi].F - 1;
        o0 = tg_fork_br_shift(fi, j,     nl->v[s].width);
        o1 = tg_fork_br_shift(fi, j + 1, nl->v[s + 1].width);
        lanes = s_forks[fi].br_lanes;
        in_fork = 1;
        if (op && !td5_env_flag_on("TD5RE_GEO_FORK_MERGE")) return 0;
    } else {
        if (op && !td5_env_flag_on("TD5RE_GEO_AVENUE_OPENING")) return 0;
        o0 = a; o1 = b; lanes = la;
    }
    if (tg_av_far_mouth(s, o0)) return 0;     /* no footway here: neighbours cap */
    sg0 = (o0 >= 0.0) ? 1.0 : -1.0;
    sg1 = (o1 >= 0.0) ? 1.0 : -1.0;
    w = tg_geo_sidewalk_w_side(s, sg0 > 0.0);
    if (!(w > 0.0)) return 0;
    {
        const double ohw = (double)lanes * (double)TD5_TG_LANE_WIDTH * 0.5;
        const double m = in_fork ? -1.0 : 50.0;
        const double x0 = o0 + sg0 * ohw, x1 = o1 + sg1 * ohw;
        if ((x0 < 0.0 ? -x0 : x0) < nl->v[s].width * 0.5 + m ||
            (x1 < 0.0 ? -x1 : x1) < nl->v[s + 1].width * 0.5 + m) return 0;
        /* [ROUND 1015 A item 5] a street takes the whole span: no slab, the neighbours cap */
        {
            double u0, u1, v0, v1;
            if (tg_av_far_clip(nl, s, x0, x1, x0 + sg0 * w, x1 + sg1 * w, sg0,
                               &u0, &u1, &v0, &v1) == 0) return 0;
        }
        *e0 = x0; *e1 = x1;
    }
    *sw = w;
    return 1;
}

/* Does span s's footway join span `other`'s at the shared node (`end` = 1: s's far
 * end meets other's near end; 0: s's near end meets other's far end)? */
static int tg_av_far_joins(const TG_NodeList *nl, int s, int other, int end)
{
    double a0, a1, aw, b0, b1, bw, ea, eb;
    if (!tg_av_far_edges(nl, other, &b0, &b1, &bw)) return 0;
    if (!tg_av_far_edges(nl, s, &a0, &a1, &aw)) return 0;
    ea = end ? a1 : a0;
    eb = end ? b0 : b1;
    return (ea - eb < 60.0 && eb - ea < 60.0 && aw - bw < 60.0 && bw - aw < 60.0);
}

/* The clipped slab: same top + two walls + caps as tg_av_emit_far_pavement, with the near and
 * far end of each of the two long edges taken at its own free fraction. A cut end always gets a
 * cap; an uncut end gets one when the neighbouring footway does not join it. */
static int tg_av_emit_far_clipped(const TG_NodeList *nl, int si, double e0, double e1,
                                  double f0, double f1, double sg, double ui0, double ui1,
                                  double uo0, double uo1, int deep, TG_Buf *blk,
                                  size_t *moff, int *nmesh)
{
    const double H = (double)TD5_TG_KERB_H;
    const double drop = deep ? (double)TD5_TG_GROUND_DROP : 0.0;
    double px[20], py[20], pz[20], uu[20], vv[20];
    double Xn[2], Zn[2], Yn[2], Xf[2], Zf[2], Yf[2];    /* [0] inner edge, [1] outer edge */
    int seg_page[2], seg_nq[2], n = 0, cap_in, cap_out, hi, lo;

    tg_av_slab_pt(nl, si, ui0, e0, e1, &Xn[0], &Zn[0], &Yn[0]);
    tg_av_slab_pt(nl, si, ui1, e0, e1, &Xf[0], &Zf[0], &Yf[0]);
    tg_av_slab_pt(nl, si, uo0, f0, f1, &Xn[1], &Zn[1], &Yn[1]);
    tg_av_slab_pt(nl, si, uo1, f0, f1, &Xf[1], &Zf[1], &Yf[1]);
    /* cl = the more POSITIVE lateral: the outer edge when the avenue is on +t */
    hi = (sg > 0.0) ? 1 : 0;
    lo = 1 - hi;
    /* TOP: near-cl, near-cr, far-cr, far-cl */
    px[n]=Xn[hi]; py[n]=Yn[hi]+H; pz[n]=Zn[hi]; uu[n]=0.0; vv[n]=0.0; n++;
    px[n]=Xn[lo]; py[n]=Yn[lo]+H; pz[n]=Zn[lo]; uu[n]=1.0; vv[n]=0.0; n++;
    px[n]=Xf[lo]; py[n]=Yf[lo]+H; pz[n]=Zf[lo]; uu[n]=1.0; vv[n]=1.0; n++;
    px[n]=Xf[hi]; py[n]=Yf[hi]+H; pz[n]=Zf[hi]; uu[n]=0.0; vv[n]=1.0; n++;
    /* wall facing +lateral (the OUTER wall, down to the ground, when the avenue is on +t) */
    {
        const double bn = (sg > 0.0) ? Yn[hi] - drop : Yn[hi];
        const double bf = (sg > 0.0) ? Yf[hi] - drop : Yf[hi];
        px[n]=Xn[hi]; py[n]=bn;       pz[n]=Zn[hi]; uu[n]=0.0; vv[n]=1.0; n++;
        px[n]=Xn[hi]; py[n]=Yn[hi]+H; pz[n]=Zn[hi]; uu[n]=0.0; vv[n]=0.0; n++;
        px[n]=Xf[hi]; py[n]=Yf[hi]+H; pz[n]=Zf[hi]; uu[n]=1.0; vv[n]=0.0; n++;
        px[n]=Xf[hi]; py[n]=bf;       pz[n]=Zf[hi]; uu[n]=1.0; vv[n]=1.0; n++;
    }
    /* wall facing -lateral */
    {
        const double bn = (sg > 0.0) ? Yn[lo] : Yn[lo] - drop;
        const double bf = (sg > 0.0) ? Yf[lo] : Yf[lo] - drop;
        px[n]=Xn[lo]; py[n]=bn;       pz[n]=Zn[lo]; uu[n]=0.0; vv[n]=1.0; n++;
        px[n]=Xf[lo]; py[n]=bf;       pz[n]=Zf[lo]; uu[n]=1.0; vv[n]=1.0; n++;
        px[n]=Xf[lo]; py[n]=Yf[lo]+H; pz[n]=Zf[lo]; uu[n]=1.0; vv[n]=0.0; n++;
        px[n]=Xn[lo]; py[n]=Yn[lo]+H; pz[n]=Zn[lo]; uu[n]=0.0; vv[n]=0.0; n++;
    }
    cap_in  = (ui0 > 0.0 || uo0 > 0.0) || (deep && !tg_av_far_joins(nl, si, si - 1, 0));
    cap_out = (ui1 < 1.0 || uo1 < 1.0) || (deep && !tg_av_far_joins(nl, si, si + 1, 1));
    if (cap_in) {
        px[n]=Xn[hi]; py[n]=Yn[hi]-drop; pz[n]=Zn[hi]; uu[n]=0.0; vv[n]=1.0; n++;
        px[n]=Xn[lo]; py[n]=Yn[lo]-drop; pz[n]=Zn[lo]; uu[n]=1.0; vv[n]=1.0; n++;
        px[n]=Xn[lo]; py[n]=Yn[lo]+H;    pz[n]=Zn[lo]; uu[n]=1.0; vv[n]=0.0; n++;
        px[n]=Xn[hi]; py[n]=Yn[hi]+H;    pz[n]=Zn[hi]; uu[n]=0.0; vv[n]=0.0; n++;
    }
    if (cap_out) {
        px[n]=Xf[hi]; py[n]=Yf[hi]+H;    pz[n]=Zf[hi]; uu[n]=0.0; vv[n]=0.0; n++;
        px[n]=Xf[lo]; py[n]=Yf[lo]+H;    pz[n]=Zf[lo]; uu[n]=1.0; vv[n]=0.0; n++;
        px[n]=Xf[lo]; py[n]=Yf[lo]-drop; pz[n]=Zf[lo]; uu[n]=1.0; vv[n]=1.0; n++;
        px[n]=Xf[hi]; py[n]=Yf[hi]-drop; pz[n]=Zf[hi]; uu[n]=0.0; vv[n]=1.0; n++;
    }
    seg_page[0] = TD5_TG_PAGE_SIDEWALK;    seg_nq[0] = 1;
    seg_page[1] = TD5_TG_PAGE_BRANCH_KERB; seg_nq[1] = 2 + (cap_in ? 1 : 0) + (cap_out ? 1 : 0);
    moff[(*nmesh)++] = blk->len;
    if (!tg_write_quad_mesh(blk, px, py, pz, uu, vv, n, seg_page, seg_nq, 2))
        return 0;
    tg_guard_mark(moff[*nmesh - 1], blk->len, TG_GK_BRANCHROAD, si);
    s_av_farwalk++;
    return 1;
}

static int tg_av_emit_far_pavement(const TG_NodeList *nl, int si,
                                   double o0, double o1, int lanes, int in_fork,
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
    double px[20], py[20], pz[20], uu[20], vv[20];
    int seg_page[2], seg_nq[2];
    int n = 0, cap_in = 0, cap_out = 0;
    /* [ROUND 1014 A] The OUTER wall stops at the ground, not at the road plane. The
     * skirt sits GROUND_DROP under the road, so a wall ending at the road plane left a
     * gap under the slab you could see into from the verge side ("doesn't have proper
     * depth"). The kerb (road-facing) wall keeps its base on the asphalt. The same
     * depth is the base of the end caps. TD5RE_GEO_AVENUE_CAPS=0 restores both. */
    const int    deep  = td5_env_flag_on("TD5RE_GEO_AVENUE_CAPS");
    const double g0    = deep ? b0 - (double)TD5_TG_GROUND_DROP : b0;
    const double g1    = deep ? b1 - (double)TD5_TG_GROUND_DROP : b1;
    const double bl0   = (sg0 > 0.0) ? g0 : b0;   /* wall at cl: outer when the avenue is on +t */
    const double bl1   = (sg1 > 0.0) ? g1 : b1;
    const double br0   = (sg0 > 0.0) ? b0 : g0;   /* wall at cr: outer when the avenue is on -t */
    const double br1   = (sg1 > 0.0) ? b1 : g1;

    if (getenv("TD5RE_GEO_FARWALK_DIAG"))
        TD5_LOG_I(LOG_TAG, "trackgen: [GEO FARWALK] span %d sw %.0f e0 %.0f e1 %.0f "
                  "node w %.0f/%.0f%s", si, sw, e0, e1, a->width, c->width,
                  (sw > 0.0) ? "" : " NO WIDTH");
    if (!(sw > 0.0)) return 1;
    if (!td5_env_flag_on("TD5RE_GEO_AVENUE_FARWALK")) return 1;
    if (tg_av_far_mouth(si, o0)) return 1;    /* a street leaves here: leave it open */
    /* [ROUND 1013 F2] Over a real fork's widened window the race road can reach
     * past the opposite carriageway's far edge; a footway there would be laid on
     * tarmac. */
    /* [ROUND 1014 A] Over a real fork's corridor the corridor's outer edge IS the
     * node's outer edge by construction (the node carries lanes(A)+lanes(B) and the
     * corridor holds the B half), so the test `edge past the race road + 50` read the
     * first corridor span (|e0| == w/2 exactly) as "the race road swallows it" and
     * dropped the footway there: a one-span pavement hole at every corridor mouth.
     * A footway is only taken out when the edge is genuinely INSIDE the race road. */
    {
        const double m = in_fork ? -1.0 : 50.0;
        if ((e0 < 0.0 ? -e0 : e0) < a->width * 0.5 + m ||
            (e1 < 0.0 ? -e1 : e1) < c->width * 0.5 + m) return 1;
    }

    /* [ROUND 1015 A item 5] a street's tarmac over part of this span: clip the slab to the
     * free run, cut along the street's own edge, and cap the cut. */
    {
        double ui0, ui1, uo0, uo1;
        const int cl = tg_av_far_clip(nl, si, e0, e1, f0, f1, sg0, &ui0, &ui1, &uo0, &uo1);
        if (cl == 0) return 1;
        if (cl == 2)
            return tg_av_emit_far_clipped(nl, si, e0, e1, f0, f1, sg0, ui0, ui1, uo0, uo1,
                                          deep, blk, moff, nmesh);
    }

    /* TOP. */
    px[n]=a->x+a->tz*cl0; py[n]=b0+H; pz[n]=a->z-a->tx*cl0; uu[n]=0.0; vv[n]=0.0; n++;
    px[n]=a->x+a->tz*cr0; py[n]=b0+H; pz[n]=a->z-a->tx*cr0; uu[n]=1.0; vv[n]=0.0; n++;
    px[n]=c->x+c->tz*cr1; py[n]=b1+H; pz[n]=c->z-c->tx*cr1; uu[n]=1.0; vv[n]=1.0; n++;
    px[n]=c->x+c->tz*cl1; py[n]=b1+H; pz[n]=c->z-c->tx*cl1; uu[n]=0.0; vv[n]=1.0; n++;

    /* Wall facing +lateral. */
    px[n]=a->x+a->tz*cl0; py[n]=bl0;  pz[n]=a->z-a->tx*cl0; uu[n]=0.0; vv[n]=1.0; n++;
    px[n]=a->x+a->tz*cl0; py[n]=b0+H; pz[n]=a->z-a->tx*cl0; uu[n]=0.0; vv[n]=0.0; n++;
    px[n]=c->x+c->tz*cl1; py[n]=b1+H; pz[n]=c->z-c->tx*cl1; uu[n]=1.0; vv[n]=0.0; n++;
    px[n]=c->x+c->tz*cl1; py[n]=bl1;  pz[n]=c->z-c->tx*cl1; uu[n]=1.0; vv[n]=1.0; n++;

    /* Wall facing -lateral. */
    px[n]=a->x+a->tz*cr0; py[n]=br0;  pz[n]=a->z-a->tx*cr0; uu[n]=0.0; vv[n]=1.0; n++;
    px[n]=c->x+c->tz*cr1; py[n]=br1;  pz[n]=c->z-c->tx*cr1; uu[n]=1.0; vv[n]=1.0; n++;
    px[n]=c->x+c->tz*cr1; py[n]=b1+H; pz[n]=c->z-c->tx*cr1; uu[n]=1.0; vv[n]=0.0; n++;
    px[n]=a->x+a->tz*cr0; py[n]=b0+H; pz[n]=a->z-a->tx*cr0; uu[n]=0.0; vv[n]=0.0; n++;

    /* [ROUND 1014 A] END CAPS where the footway does not carry on at the same lateral
     * and width (winding as tg_emit_avenue_divider's, derived there). */
    if (deep) {
        cap_in  = !tg_av_far_joins(nl, si, si - 1, 0);
        cap_out = !tg_av_far_joins(nl, si, si + 1, 1);
    }
    if (cap_in) {
        px[n]=a->x+a->tz*cl0; py[n]=g0;   pz[n]=a->z-a->tx*cl0; uu[n]=0.0; vv[n]=1.0; n++;
        px[n]=a->x+a->tz*cr0; py[n]=g0;   pz[n]=a->z-a->tx*cr0; uu[n]=1.0; vv[n]=1.0; n++;
        px[n]=a->x+a->tz*cr0; py[n]=b0+H; pz[n]=a->z-a->tx*cr0; uu[n]=1.0; vv[n]=0.0; n++;
        px[n]=a->x+a->tz*cl0; py[n]=b0+H; pz[n]=a->z-a->tx*cl0; uu[n]=0.0; vv[n]=0.0; n++;
    }
    if (cap_out) {
        px[n]=c->x+c->tz*cl1; py[n]=b1+H; pz[n]=c->z-c->tx*cl1; uu[n]=0.0; vv[n]=0.0; n++;
        px[n]=c->x+c->tz*cr1; py[n]=b1+H; pz[n]=c->z-c->tx*cr1; uu[n]=1.0; vv[n]=0.0; n++;
        px[n]=c->x+c->tz*cr1; py[n]=g1;   pz[n]=c->z-c->tx*cr1; uu[n]=1.0; vv[n]=1.0; n++;
        px[n]=c->x+c->tz*cl1; py[n]=g1;   pz[n]=c->z-c->tx*cl1; uu[n]=0.0; vv[n]=1.0; n++;
    }

    seg_page[0] = TD5_TG_PAGE_SIDEWALK;    seg_nq[0] = 1;
    seg_page[1] = TD5_TG_PAGE_BRANCH_KERB; seg_nq[1] = 2 + (cap_in ? 1 : 0) + (cap_out ? 1 : 0);
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
                  "far-side footway on %ld span(s) CUMULATIVE over the track",
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
    /* [ROUND 1013 F2] The sidecar's offset is measured from the route
     * carriageway's own centre. Over a REAL fork's window the walk moved the
     * node toward the corridor (td5_tg_realfork.c), so the same real road sits
     * that much nearer the node. Zero outside every window. */
    o0 -= tg_realfork_node_delta(si);
    o1 -= tg_realfork_node_delta(si + 1);
    {
        /* A span a REAL fork's corridor runs over: the fork draws the carriageway
         * (it is DRIVEABLE there), the gore and the kerbed island, all from the
         * same real offset, so drawing them again here would stack two roads.
         * Only the footway beyond the far edge is still this module's -- it
         * carries the real per-side width the fork's own branch pavement does
         * not know. */
        const int fi = tg_fork_of_main(si);
        if (fi >= 0 && s_forks[fi].real > 0) {
            const int    j  = si - s_forks[fi].F - 1;
            const int    bl = s_forks[fi].br_lanes;
            const double c0 = tg_fork_br_shift(fi, j,     nl->v[si].width);
            const double c1 = tg_fork_br_shift(fi, j + 1, nl->v[si + 1].width);
            tg_av_note(nl, si, c0, bl, open,
                       tg_realfork_med(s_forks[fi].real - 1, j));
            /* [ROUND 1014 A] No `open` skip here any more. A merged fork's corridor
             * runs ACROSS the real median openings (the opening is a cross street
             * the corridor carriageway passes over, not a break in it), so the
             * footway beside the corridor is continuous too -- skipping it left a
             * pavement hole at every opening inside the window. */
            if (open && !td5_env_flag_on("TD5RE_GEO_FORK_MERGE")) return 1;
            return tg_av_emit_far_pavement(nl, si, c0, c1, bl, 1, blk, moff, nmesh);
        }
    }

    {
        const double ohw = (double)lanes * (double)TD5_TG_LANE_WIDTH * 0.5;
        const double a   = (o0 < 0.0) ? -o0 : o0;
        double med = a - nl->v[si].width * 0.5 - ohw;
        if (med < 0.0) med = 0.0;     /* a real fork's window: the race road is wider */
        tg_av_note(nl, si, o0, lanes, open, med);
    }
    if (!tg_av_emit_road(nl, si, o0, o1, lanes, blk, moff, nmesh)) return 0;
    /* A real cross street cuts the median here, so it opens for the turn --
     * and so does everything beyond it. [1011 C2] The far footway takes the
     * SAME gate as the island rather than one of its own: a car turning across
     * the median crosses the far carriageway and its pavement too, so a slab
     * left standing here would be a kerb across the mouth of a real street.
     *
     * [ROUND 1014 A] But NOTHING was laid in the gap: the opening stopped the
     * island and the footway and put nothing in their place, so the ground skirt
     * (which starts past the far carriageway) left a see-through slot between
     * the two roads -- "there is no geometry after this median". The opening is
     * now paved flush between the carriageways (a junction), and the footway
     * runs on past it (no cross-street arm is drawn through it, so there is no
     * mouth for a slab to block). TD5RE_GEO_AVENUE_OPENING=0 restores the slot. */
    if (open && td5_env_flag_on("TD5RE_GEO_AVENUE_OPENING")) {
        if (!tg_av_emit_opening(nl, si, o0, o1, lanes, blk, moff, nmesh,
                                TG_AV_MIN_MEDIAN_W)) return 0;
        return tg_av_emit_far_pavement(nl, si, o0, o1, lanes, 0, blk, moff, nmesh);
    }
    if (open) return 1;
    /* [1014 B item 8] and where a street really leaves the far kerb: the sidecar's
     * `open` runs and the network's mouths are measured by different code, so
     * they can differ by a span or two, and a footway left standing across the
     * mouth is the item-6 defect again. */
    if (tg_net_mouth_shift(si, o0 > 0.0) > 0.0 &&
        tg_net_mouth_kind(si, o0 > 0.0) >= 0)
        return tg_av_emit_island(nl, si, o0, o1, lanes, blk, moff, nmesh);
    if (!tg_av_emit_far_pavement(nl, si, o0, o1, lanes, 0, blk, moff, nmesh))
        return 0;
    return tg_av_emit_island(nl, si, o0, o1, lanes, blk, moff, nmesh);
}
