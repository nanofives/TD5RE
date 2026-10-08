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
    tg_guard_mark(moff[*nmesh - 1], blk->len, TG_GK_BRANCHSIDE, si);
    return 1;
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

    if (!tg_av_emit_road(nl, si, o0, o1, lanes, blk, moff, nmesh)) return 0;
    /* A real cross street cuts the median here, so it opens for the turn. */
    if (open) return 1;
    return tg_av_emit_island(nl, si, o0, o1, lanes, blk, moff, nmesh);
}
