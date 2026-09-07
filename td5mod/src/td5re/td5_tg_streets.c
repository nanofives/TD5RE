/**
 * td5_tg_streets.c -- auto-track SIDE STREETS: direction, occupancy, intersections, parks + houses, real intersections, infill, measurement sweeps
 *
 * Split from the td5_trackgen.c monolith (2026-09-06); shared declarations
 * live in td5_trackgen_internal.h. Element map: docs/plans/AUTOTRACK_ELEMENT_CATALOG.md.
 */
#include "td5_trackgen_internal.h"

/* [R8 CROSS item 1] The reveal row has to know how deep the street it reveals
 * actually runs, so it can stand BEYOND it instead of across it. Both live
 * further down with the cross-street code; declared here rather than moved so
 * the CROSS rewrite does not shuffle another area's emitter. */
static double tg_city_crossst_reach(const TG_Biome *b, double sw);

/* ===================== [R3 BLOCK] SIDE-STREET DIRECTION =====================
 * (feedback R3 item 3). Shared by the cross-street carriageway and the sidewalk/
 * railing arms that flank it, so all three leave the kerb in the SAME direction.
 * A diagonal carriageway with square-on pavements would read as broken geometry;
 * routing every arm's outward direction through one function keeps them aligned.
 *
 * DIAGONAL variance ("investigate if the crossings can be diagonal"): the angle
 * is keyed on the facade SUPERBLOCK, not the span, so every span of one gap
 * skews by the same amount and the arm stays one straight street rather than a
 * fan. Roughly a third of junctions are angled; the rest stay square. The angle
 * is capped well under 45 deg so a leaning arm can never double back across the
 * main carriageway (which would put a side street on top of the road).
 * ========================================================================== */
/* TD5_TG_DIAG_MAX_DEG is stated with the turn map (tg_turn_map_build), because
 * [R14 item 4] the continuation path has to honour the same ceiling and the two
 * must not be able to drift apart. */

double tg_block_arm_skew(int si, int left)
{
    unsigned int block, phase, gs, gl, h;
    int av;
    /* [R8 CROSS item 9] At a turn continuation the outward direction is NOT a
     * random diagonal: it is the heading the road arrived on. Returned ahead of
     * the block hash (and ahead of the DIAG_CROSS gate, which only governs the
     * decorative skew) so the carriageway, both pavement arms and the flanking
     * massing all leave the kerb along that one bearing. */
    if (tg_turn_open(si, left)) return (double)s_turn_skew[si];
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_DIAG_CROSS")) return 0.0;
    tg_facade_block(si, left, &block, &phase, &gs, &gl, &av);
    h = (block * 2u + (unsigned)(left ? 1 : 0)) * 2654435761u;
    if ((h >> 28) >= 5u) return 0.0;             /* ~31% of gaps run angled */
    {   /* signed fraction in -1..1 from an independent hash slice */
        const double f = (double)((h >> 8) & 0xFFFFu) / 32768.0 - 1.0;
        return f * (TD5_TG_DIAG_MAX_DEG * TD5_TG_PI / 180.0);
    }
}

/* Rotate an outward unit (ux,uz) in the XZ plane by `ang` radians. */
static void tg_block_rot2(double ux, double uz, double ang,
                          double *ox, double *oz)
{
    const double c = cos(ang), s = sin(ang);
    *ox = ux * c - uz * s;
    *oz = ux * s + uz * c;
}

double tg_xstreet_drop(double d)
{
    const double w = tg_verge_reach();
    if (!td5_env_flag_on("TD5RE_R8_CROSS_REACH"))
        return TD5_TG_GROUND_DROP * d / TD5_TG_GROUND_WIDTH;
    if (!(w > 1.0)) return TD5_TG_GROUND_DROP;
    return TD5_TG_GROUND_DROP * (d < w ? d : w) / w;
}

static double tg_city_crossst_reach(const TG_Biome *b, double sw)
{
    if (td5_env_flag_on("TD5RE_R8_CROSS_REACH")) {
        const double blk = tg_facade_depth(b) + TD5_TG_BACKROW_GAP;
        const double r   = sw + blk * TD5_TG_R8_XSTREET_BLOCKS;
        return r > TD5_TG_R8_XSTREET_MAX ? TD5_TG_R8_XSTREET_MAX : r;
    }
    {
        const double gaps = td5_env_flag_on("TD5RE_AUTOTRACK_XLONG")
                          ? TD5_TG_XSTREET_GAPS : 1.0;
        return sw + tg_facade_depth(b) + TD5_TG_BACKROW_GAP * gaps;
    }
}

double tg_xstreet_reach_at(const TG_NodeList *nl, int si, double sg,
                                  double ang, const TG_Biome *b, double sw)
{
    const double r = tg_city_crossst_reach(b, sw);
    double e[10], ox, oz, floor_r, d;
    int lo, hi;

    if (!td5_env_flag_on("TD5RE_R8_CROSS_REACH")) return r;
    if (!td5_env_flag_on("TD5RE_R8_CROSS_CLAMP")) return r;

    floor_r = sw + tg_facade_depth(b) + TD5_TG_BACKROW_GAP * TD5_TG_XSTREET_GAPS;
    if (floor_r > r) floor_r = r;

    tg_city_edge_frame(nl, si, sg, e);
    tg_block_rot2(e[6], e[7], ang, &ox, &oz);
    lo = si - TD5_TG_R8_CLAMP_WIN; if (lo < 0) lo = 0;
    hi = si + TD5_TG_R8_CLAMP_WIN; if (hi > nl->count - 1) hi = nl->count - 1;

    for (d = TD5_TG_R8_CLAMP_MIN; d <= r; d += TD5_TG_R8_CLAMP_STEP) {
        const double px = e[0] + ox * d, pz = e[2] + oz * d;
        double lat, best = 1e300;
        int i, ni = -1;
        for (i = lo; i <= hi; i++) {
            double dx, dz, d2;
            if (i > si - TD5_TG_R8_CLAMP_SKIP && i < si + TD5_TG_R8_CLAMP_SKIP)
                continue;
            dx = px - nl->v[i].x; dz = pz - nl->v[i].z;
            d2 = dx * dx + dz * dz;
            if (d2 < best) { best = d2; ni = i; }
        }
        if (ni < 0) break;
        lat = (px - nl->v[ni].x) * nl->v[ni].tz
            - (pz - nl->v[ni].z) * nl->v[ni].tx;
        /* The carriageway authority is asked for the side the SAMPLE lands on at
         * that distant span, not the side the street left from -- a street off a
         * hairpin can come back at the far span's other lateral. Same rule the
         * on-road guard applies, so clamp and guard cannot disagree. */
        {
            const double lim = tg_carriageway_reach(nl, ni,
                                   (lat >= 0.0) ? 1.0 : -1.0)
                             + TD5_TG_R8_CLAMP_MARGIN;
            if ((lat < 0.0 ? -lat : lat) < lim) {
                d -= TD5_TG_R8_CLAMP_STEP;     /* last sample known clear */
                if (d < floor_r) d = floor_r;
                /* [R13 item 3] The R8 clamp asks "does this ray come back onto
                 * a carriageway"; it cannot see a street folding over ITSELF,
                 * which is what a mouth laid on a bend does. Applied after the
                 * floor because the floor is a model minimum and the fold is a
                 * geometric fact. */
                return tg_r13_fold_cap(nl, si, sg, ang, d,
                                       "TD5RE_R13_FOLD_STREET");
            }
        }
    }
    return tg_r13_fold_cap(nl, si, sg, ang, r, "TD5RE_R13_FOLD_STREET");
}

int s_r10_audit_n;

static signed char s_r10_xs_state[TD5_TG_R10_XS_MAX][2];  /* -1 ?, 0 no, 1 yes */

static double      s_r10_xs_reach[TD5_TG_R10_XS_MAX][2];

void tg_r10_xs_memo_reset(void)
{
    s_xs_fill = s_xs_hit = s_xs_fill_terr = s_xs_hit_terr = 0;
    s_xs_phase = 0;
    memset(s_r10_xs_state, -1, sizeof(s_r10_xs_state));
}

int tg_r10_xstreet_guard(void)
{
    return td5_env_flag_on("TD5RE_R10_XSTREET_GUARD");
}

/* Is a side-street carriageway laid at (si, side), and how far out does it run?
 * Mirrors tg_emit_fb_city's gate on tg_city_emit_crossstreet, then that
 * function's own per-side gates. */
int tg_xstreet_here(const TG_NodeList *nl, int si, double side,
                           double *preach)
{
    const TG_Biome *b;
    double sw;
    int s, memo = (si >= 0 && si < TD5_TG_R10_XS_MAX);
    const int mi = (side > 0.0) ? 1 : 0;

    if (memo && s_r10_xs_state[si][mi] >= 0) {
        s_xs_hit++;
        if (s_xs_phase) s_xs_hit_terr++;
        if (preach) *preach = s_r10_xs_reach[si][mi];
        return (int)s_r10_xs_state[si][mi];
    }
    /* SINGLE EXIT so every answer -- including every early "no" -- is memoised.
     * `here` stays 0 until the last gate passes. */
    {
        int here = 0;
        double reach = 0.0;
        s = (side > 0.0) ? 1 : 0;
        if (nl && si >= 0 && si + 1 < nl->count
            && td5_env_flag_on("TD5RE_AUTOTRACK_CROSS_STREETS")
            && !tg_span_in_bridge_run(si)) {
            b  = &k_biomes[tg_scenery_biome_index(si)];
            sw = tg_city_sidewalk_w(b);
            if (sw > 0.0                              /* a city frontage to break */
                && !(td5_env_flag_on("TD5RE_AUTOTRACK_XBRIDGE_GATE")
                     && tg_span_near_bridge(si, TD5_TG_XBRIDGE_CLEAR))
                && !tg_facade_built(si, s)            /* frontage closed          */
                && !tg_block_is_park(si, s)           /* a lawn, not a street     */
                && !tg_side_corridor_here(nl, si, side)) {
                here  = 1;
                reach = tg_xstreet_reach_at(nl, si, side,
                                            tg_block_arm_skew(si, s), b, sw);
            }
        }
        if (memo) {
            /* [S0] PUBLISH ORDER: reach FIRST, state LAST. state is the
             * "is this slot valid" flag, so a reader that sees it set must be
             * guaranteed to see the reach that goes with it. Written the other
             * way round (as it was), a parallel entry could read state>=0 with
             * a stale or half-written reach and place furniture against the
             * wrong carriageway. Harmless single-threaded; this is the
             * ordering the per-entry parallel build needs.
             *
             * This memo stays LAZY on purpose. Precomputing all slots would do
             * far more work than a build asks for -- its expensive half
             * marches a 48-span window outward per (span, side), and the
             * branch's own note records an unmemoised run failing to finish
             * seed 777 in 900 s -- so it is made safe by ORDER, not by being
             * made eager. */
            s_r10_xs_reach[si][mi] = reach;
            TG_COMPILER_BARRIER();
            s_r10_xs_state[si][mi] = (signed char)here;
            s_xs_fill++;
            if (s_xs_phase) s_xs_fill_terr++;
        }
        if (preach) *preach = reach;
        return here;
    }
}

/* Outermost tarmac at (si, side) for geometry that must stand on a FOOTWAY:
 * the carriageway authority, widened to include a side street's asphalt. Kept
 * separate from tg_carriageway_reach on purpose -- a facade, a back row and a
 * pavement arm legitimately BOUND a side street and must keep the narrow
 * answer; only free-standing furniture and people may not stand in one. */
double tg_footway_reach(const TG_NodeList *nl, int si, double side)
{
    double r = tg_carriageway_reach(nl, si, side), xr = 0.0;
    if (tg_r10_xstreet_guard() && tg_xstreet_here(nl, si, side, &xr)) {
        const double rr = tg_road_half_width(nl, si) + xr
                        + TD5_TG_R10_XSTREET_MARGIN;
        if (rr > r) r = rr;
    }
    /* [R14 FCROSS item 1c] A FOREST side road is tarmac by the same argument, so
     * the footway envelope has to include it or the on-road guard has no opinion
     * about a crate standing in a forest lane. Adding it here rather than to
     * tg_carriageway_reach keeps the narrow answer for the things that
     * legitimately BOUND a street (the fold walls stand on its edges). */
    if (td5_env_flag_on("TD5RE_R14_FCROSS_CLEAR")) {
        double fs = 0.0, fr = 0.0;
        if (tg_r12_fcross_at(nl, si, &fs, &fr) && fs == side) {
            const double rr = tg_road_half_width(nl, si) + fr;
            if (rr > r) r = rr;
        }
    }
    return r;
}

/* [R15 OCC] See the block comment at the forward declaration for why this is a
 * pure composition of the reaches above rather than a reservation ledger. The
 * PAVE arm is the one genuinely new surface: it is the same pair the sidewalk
 * emitter itself uses to decide the slab's width (tg_city_sidewalk_w_at for the
 * biome width, tg_pavement_side_width for the per-side narrowing over a branch
 * corridor), so this reports the slab that is actually laid rather than a
 * second model of it -- the same discipline tg_xstreet_here follows for the
 * street. */
double tg_occ_reach(const TG_NodeList *nl, int si, double side,
                    unsigned int mask)
{
    double r = 0.0;

    if (!nl || si < 0 || si >= nl->count) return 0.0;
    /* STREET implies ROAD: tg_footway_reach is carriageway-or-wider by
     * construction, so asking for it answers both. */
    if (mask & TG_OCC_STREET)      r = tg_footway_reach(nl, si, side);
    else if (mask & TG_OCC_ROAD)   r = tg_carriageway_reach(nl, si, side);

    if (mask & TG_OCC_PAVE) {
        const TG_Biome *pb = &k_biomes[tg_scenery_biome_index(si)];
        const double bw = tg_city_sidewalk_w_at(nl, si, pb);
        if (bw > 0.0) {
            const double w = tg_pavement_side_width(nl, si, side, bw);
            if (w > 0.0) {
                const double p = tg_road_half_width(nl, si) + w;
                if (p > r) r = p;
            }
        }
    }
    return r;
}

/* The PLACEMENT half alone. Separate from the master knob so the two halves can
 * be A/B'd independently -- TD5RE_R10_PROP_PLACE=0 keeps the guard and lets the
 * old placement stand, which is the run that proves the BACKSTOP would have
 * caught the shipped defect on its own. */
int tg_xstreet_occupies(const TG_NodeList *nl, int si, double side,
                               double inner_d)
{
    double reach = 0.0;
    if (!tg_r10_xstreet_guard()) return 0;
    if (!td5_env_flag_on("TD5RE_R10_PROP_PLACE")) return 0;
    if (!tg_xstreet_here(nl, si, side, &reach)) return 0;
    return inner_d < reach + TD5_TG_R10_XSTREET_MARGIN;
}

/* The instrument the diagnosis was made with: every furniture placement with the
 * laterals it was judged on, so "is this piece on tarmac" is a number per piece
 * rather than a frame of one of them. */
void tg_xstreet_audit(const TG_NodeList *nl, int si, double side,
                             double inner_d, const char *what, int placed)
{
    double reach = 0.0;
    int here;
    if (!td5_env_flag_off("TD5RE_R10_PROP_AUDIT")) return;
    if (s_r10_audit_n >= TD5_TG_R10_AUDIT_MAX) return;
    s_r10_audit_n++;
    here = tg_xstreet_here(nl, si, side, &reach);
    TD5_LOG_I(LOG_TAG, "trackgen: [R10 AUDIT] span %4d %-5s %-15s inner=%.0f "
              "main-reach=%.0f xstreet=%s reach=%.0f -> %s",
              si, side > 0.0 ? "left" : "right", what, inner_d,
              tg_carriageway_reach(nl, si, side) - tg_road_half_width(nl, si),
              here ? "YES" : "no", here ? reach : 0.0,
              placed ? "placed" : "SKIPPED");
}

/* ===== [R15 CROSS item 9] A WIDE CROSSING NEEDS A MEDIAN =====
 * "this crossing street is too wide (it has too many lanes) if you want to make
 * wide lanes like this it has to be crossing on both sides and have a median."
 *
 * MEASURED (TD5RE_R15_OCC_DIAG=253, seed 1459285111): the width is not one
 * emitter drawing a wide quad -- it is N ADJACENT SPANS each laying their own
 * one-span-wide carriageway across the same frontage gap. Spans 248..252 all
 * report xhere=1 with the same 19500 reach on the same side, so five quads sit
 * edge to edge and the street is 5 * TD5_TG_LANE_WIDTH = 7500 wide. That is
 * exactly "too many lanes", and it is why the picker saw one 8-vertex mesh with
 * a 22606 radius: the radius is the street's LENGTH, not its width.
 *
 * The report also states the acceptance test, so it is implemented literally:
 * a wide street must (a) cross on BOTH sides and (b) carry a median. (a) is
 * already true wherever both kerbs are open (the diag shows both sides open on
 * every span of that run), so this adds (b) and asserts (a) rather than
 * assuming it. The island is emitted ONCE, on the run's centre span, so it runs
 * down the middle of the street instead of once per lane.
 *
 * tg_emit_avenue_divider is NOT reused: it is reachable only from the fork gore
 * path, takes branch-corridor parameters (sh0/sh1/half0/half1/br_lanes) and
 * returns early unless tg_fork_is_avenue, so it has no crossing entry point at
 * all. This is the same shape of island expressed in the crossing's own frame. */
#define TD5_TG_R15_MEDIAN_MIN   3       /* spans of width before one is owed  */
#define TD5_TG_R15_MEDIAN_HW  260.0     /* half width, matches the avenue cap */
#define TD5_TG_R15_MEDIAN_H   150.0     /* kerbed island, not a barrier       */

static long s_r15_medians;

/* Is si the centre span of a contiguous cross-street run on this side, and how
 * many spans wide is that run? Reads tg_xstreet_here, which is the crossstreet
 * emitter's own gate set, so the run measured is the street actually laid. */
static int tg_r15_xrun_centre(const TG_NodeList *nl, int si, double sg,
                              int *plen)
{
    int lo = si, hi = si;
    double r = 0.0;

    if (!tg_xstreet_here(nl, si, sg, &r)) return 0;
    while (lo > 0 && tg_xstreet_here(nl, lo - 1, sg, &r)) lo--;
    while (hi + 1 < nl->count && tg_xstreet_here(nl, hi + 1, sg, &r)) hi++;
    if (plen) *plen = hi - lo + 1;
    return si == (lo + hi) / 2;
}

static int tg_city_emit_crossstreet(const TG_FBHook *h, double sw)
{
    double px[8], py[8], pz[8], uu[8], vv[8];
    double e[10], q[12], t[8];
    /* [R4 CROSS item 9] Perpendicular lane markings: swap in the cross-street
     * page whose centre line runs DOWN the street (see tg_emit_texture_page_r4_cross).
     * Default ON; TD5RE_AUTOTRACK_CROSS_MARKINGS=0 restores the biome road page. */
    const int marks = td5_env_flag_on("TD5RE_AUTOTRACK_CROSS_MARKINGS");
    int seg_page = marks ? (TD5_TG_PAGE_R4_CROSS + 0) : tg_road_page(h->si), seg_nq;
    int s, n = 0;

    /* [R6 CROSS item 15] no cross-street carriageway right after a bridge run. */
    if (td5_env_flag_on("TD5RE_AUTOTRACK_XBRIDGE_GATE") &&
        tg_span_near_bridge(h->si, TD5_TG_XBRIDGE_CLEAR)) return 1;

    for (s = 0; s < 2; s++) {
        const double sg = s ? 1.0 : -1.0;
        /* Only where THIS kerb is open. On an avenue both are, and the two
         * quads plus the crossing between them make one crossroads. */
        double ang, nox, noz, fox, foz, reach, drop, u_r;
        if (tg_facade_built(h->si, s)) continue;
        if (tg_block_is_park(h->si, s)) continue;   /* a park, not a through st */
        /* [R9 CITY item 4] measured, not blanket. The street, its pavement arms
         * and its flanking blocks must all use the SAME gate, or a frontage gap
         * that opens across a fork mouth gets a kerb dropped for a side street
         * that was never laid -- bare ground, which is item 4. */
        if (tg_side_corridor_here(h->nl, h->si, sg)) continue;
        tg_city_edge_frame(h->nl, h->si, sg, e);

        /* Skew the outward edge (item 3 diagonal). Both ends rotate by the same
         * per-gap angle, so the carriageway leans as one straight street. */
        ang = tg_block_arm_skew(h->si, s);
        /* [R8] Reach is now PER SIDE: the clamp depends on this side's bearing
         * and on how the road curves away from it, so the two kerbs of an
         * avenue can legitimately stop at different depths. */
        reach = tg_xstreet_reach_at(h->nl, h->si, sg, ang, h->b, sw);
        drop  = tg_xstreet_drop(reach);
        u_r   = reach / (double)TD5_TG_LANE_WIDTH;
        tg_block_rot2(e[6], e[7], ang, &nox, &noz);
        tg_block_rot2(e[8], e[9], ang, &fox, &foz);

        q[0] = e[0];                q[1]  = e[1] + TD5_TG_VERGE_LIFT;
        q[2] = e[2];
        q[3] = e[0] + nox * reach;  q[4]  = e[1] + TD5_TG_VERGE_LIFT - drop;
        q[5] = e[2] + noz * reach;
        q[6] = e[3] + fox * reach;  q[7]  = e[4] + TD5_TG_VERGE_LIFT - drop;
        q[8] = e[5] + foz * reach;
        q[9] = e[3];                q[10] = e[4] + TD5_TG_VERGE_LIFT;
        q[11] = e[5];
        t[0] = 0.0; t[1] = (double)h->si;
        t[2] = u_r; t[3] = (double)h->si;
        t[4] = u_r; t[5] = (double)h->si + 1.0;
        t[6] = 0.0; t[7] = (double)h->si + 1.0;
        tg_city_push_quad(px, py, pz, uu, vv, &n, q, t);
    }

    if (n <= 0) return 1;
    if (*h->nmesh >= h->maxmesh) return 1;
    seg_nq = n / 4;
    tg_acct_n(TG_ACCT_CROSSING, h->si, n / 4);   /* side-street mouths */
    if (marks) tg_acct_n(TG_ACCT_CROSSFURN, h->si, n / 4);  /* item 9 markings fired */
    h->moff[(*h->nmesh)++] = h->blk->len;
    if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n,
                            &seg_page, &seg_nq, 1))
        return 0;

    /* [R15 CROSS item 9] The median. Its own mesh because it is a different
     * page from the lane markings above, and its own SIDE loop because a run
     * can be wide on one kerb and narrow on the other. */
    if (td5_env_flag_on("TD5RE_R15_XWIDE_MEDIAN")) {
        double mp[12], mt[8];
        int ms;
        for (ms = 0; ms < 2; ms++) {
            const double sg = ms ? 1.0 : -1.0;
            double ang, nox, noz, fox, foz, reach, drop;
            double ax, az, alen, oxm, ozm, olen, cx, cy, cz;
            double mpx[12], mpy[12], mpz[12], muu[12], mvv[12];
            int mn = 0, mpage = TD5_TG_PAGE_SIDEWALK, mnq;
            int len = 0;
            double rr = 0.0;

            /* (b) the run must be genuinely wide, and this the centre of it. */
            if (!tg_r15_xrun_centre(h->nl, h->si, sg, &len)) continue;
            if (len < TD5_TG_R15_MEDIAN_MIN) continue;
            /* (a) the report's own condition: it must cross on BOTH sides. */
            if (!tg_xstreet_here(h->nl, h->si, -sg, &rr)) continue;
            if (*h->nmesh >= h->maxmesh) break;

            tg_city_edge_frame(h->nl, h->si, sg, e);
            ang   = tg_block_arm_skew(h->si, ms);
            reach = tg_xstreet_reach_at(h->nl, h->si, sg, ang, h->b, sw);
            drop  = tg_xstreet_drop(reach);
            tg_block_rot2(e[6], e[7], ang, &nox, &noz);
            tg_block_rot2(e[8], e[9], ang, &fox, &foz);

            /* Along-road unit and the kerb-line midpoint of this span: the
             * centre span's midpoint is the middle of the whole run. */
            ax = e[3] - e[0]; az = e[5] - e[2];
            alen = sqrt(ax * ax + az * az);
            if (alen < 1.0) continue;
            ax /= alen; az /= alen;
            oxm = 0.5 * (nox + fox); ozm = 0.5 * (noz + foz);
            olen = sqrt(oxm * oxm + ozm * ozm);
            if (olen < 0.001) continue;
            oxm /= olen; ozm /= olen;
            cx = 0.5 * (e[0] + e[3]);
            cy = 0.5 * (e[1] + e[4]) + TD5_TG_VERGE_LIFT;
            cz = 0.5 * (e[2] + e[5]);

            {
                const double hw = TD5_TG_R15_MEDIAN_HW;
                const double H  = TD5_TG_R15_MEDIAN_H;
                const double ur = reach / (double)TD5_TG_SPAN_LENGTH;
                const double uw = (2.0 * hw) / (double)TD5_TG_SPAN_LENGTH;
                int k;
                /* TOP */
                mp[0] = cx - ax*hw;              mp[1]  = cy + H;
                mp[2] = cz - az*hw;
                mp[3] = cx + ax*hw;              mp[4]  = cy + H;
                mp[5] = cz + az*hw;
                mp[6] = cx + ax*hw + oxm*reach;  mp[7]  = cy + H - drop;
                mp[8] = cz + az*hw + ozm*reach;
                mp[9] = cx - ax*hw + oxm*reach;  mp[10] = cy + H - drop;
                mp[11] = cz - az*hw + ozm*reach;
                mt[0]=0.0; mt[1]=0.0; mt[2]=uw; mt[3]=0.0;
                mt[4]=uw;  mt[5]=ur;  mt[6]=0.0; mt[7]=ur;
                tg_city_push_quad(mpx, mpy, mpz, muu, mvv, &mn, mp, mt);
                /* Two flanks, so the island reads as a kerb and not a decal. */
                for (k = 0; k < 2; k++) {
                    const double sw2 = k ? hw : -hw;
                    mp[0] = cx + ax*sw2;             mp[1]  = cy;
                    mp[2] = cz + az*sw2;
                    mp[3] = cx + ax*sw2 + oxm*reach; mp[4]  = cy - drop;
                    mp[5] = cz + az*sw2 + ozm*reach;
                    mp[6] = cx + ax*sw2 + oxm*reach; mp[7]  = cy - drop + H;
                    mp[8] = cz + az*sw2 + ozm*reach;
                    mp[9] = cx + ax*sw2;             mp[10] = cy + H;
                    mp[11] = cz + az*sw2;
                    mt[0]=0.0; mt[1]=0.0; mt[2]=ur; mt[3]=0.0;
                    mt[4]=ur;  mt[5]=H/(double)TD5_TG_SPAN_LENGTH;
                    mt[6]=0.0; mt[7]=H/(double)TD5_TG_SPAN_LENGTH;
                    tg_city_push_quad(mpx, mpy, mpz, muu, mvv, &mn, mp, mt);
                }
                mnq = mn / 4;
                h->moff[(*h->nmesh)++] = h->blk->len;
                if (!tg_write_quad_mesh(h->blk, mpx, mpy, mpz, muu, mvv, mn,
                                        &mpage, &mnq, 1))
                    return 0;
                tg_acct(TG_ACCT_CROSSFURN, h->si);
                s_r15_medians++;
            }
        }
    }
    return 1;
}

/* ==========================================================================
 * [R3 BLOCK] STREET INTERSECTIONS, PARKS & HOUSES  (feedback R3 items 3-6)
 *
 * This whole block is owned by the BLOCK work area. It sits AFTER Group A (city
 * furniture) and BEFORE the Group A dispatcher on purpose -- its own dispatcher
 * tg_emit_fb_block is wired into the scenery loop next to tg_emit_fb_city, so
 * none of the code below edits another area's emitter.
 *
 * WHAT A "GAP" IS. tg_facade_built(si, side) is false where a superblock's side
 * street opens the frontage. Round 2 filled that gap with a carriageway arm
 * (tg_city_emit_crossstreet) but the pavements and railings ran straight past
 * it, so a "crossing" was a hole in a wall, not a junction. Two things fix that:
 *   1. INTERSECTIONS (item 3): at the two ALONG-ROAD corners of a gap, a
 *      pavement + railing turn off the main kerb and run OUTWARD down the side
 *      street, so the sidewalk and the guard rail follow the street. The arm
 *      shares its outward direction with the carriageway (tg_block_arm_skew), so
 *      a diagonal side street gets diagonal pavements too.
 *   2. PARKS (items 5-6): one gap in four is a green square instead of a through
 *      street -- a lawn from the kerb out, a hedge border behind the pavement,
 *      and the occasional individual house set back on the grass. A park and a
 *      side street are mutually exclusive on a given gap (tg_block_is_park is the
 *      single predicate both the crossstreet guard and the park emitter read),
 *      so a lawn never lands on top of a carriageway.
 * ========================================================================== */

/* Is the gap on span si / side `left` a PARK rather than a through street?
 * Keyed on the facade SUPERBLOCK so every span of one gap agrees (a gap lies
 * wholly within one block's period). Avenues -- open on both kerbs -- are never
 * parks: an avenue is a through route, not a square.
 *
 * [R6 CROSS item 2] DEFAULT OFF. Every park IS a roadside gap, so from the
 * racing line a park reads as a side street whose surface is a green lawn with
 * the boundary hedge standing across the far end like a guardrail -- verbatim
 * "an intersection where instead of road I see green grass ... it still has that
 * vertical green texture as guardrail" (framedump top-down span 115, seed 99991:
 * a bright-green mouth on the right kerb). The user's instruction is explicit --
 * "fix this to be street" -- so parks no longer open a gap: with is_park false a
 * former park gap is laid as a normal through street (carriageway + crossing +
 * lined frontages) by the existing emitters, and no lawn or hedge is emitted.
 * TD5RE_AUTOTRACK_PARKS=1 restores the green squares for an A/B. */
int tg_block_is_park(int si, int left)
{
    unsigned int block, phase, gs, gl, h;
    int av;
    if (!td5_env_int("TD5RE_AUTOTRACK_PARKS", 0, 0, 1)) return 0;
    tg_facade_block(si, left, &block, &phase, &gs, &gl, &av);
    if (av) return 0;
    h = (block * 2u + (unsigned)(left ? 1 : 0)) * 0x85EBCA6Bu;
    return (h >> 30) == 0u;                       /* ~1 gap in 4 is a park */
}

/* [R7 CITY item 3] Ground + dressing under a fork-gore BUILDING backdrop. The
 * backdrop massing (tg_bg_building_box) stood on its base chord alone; where the
 * far terrain does not reach behind the corridor the block read as "buildings at
 * the back that don't reach the floor / have no ground" (span 137, seed 99991).
 * This lays a tiled PLAZA slab from the carriageway clearance out past the block
 * back, at the block's own base Y (the road-node Y), so the buildings visibly
 * stand on a floor, and dresses it with a fountain/monument or a passer-by --
 * the "plazas can contain fountains and people" request. Everything sits BEHIND
 * the carriageway clearance (routes the same tg_carriageway_clear_gap the block
 * does, so the GUARD backstop has nothing to reject). Gate TD5RE_R7_CITY_PLAZA
 * (default ON); =0 restores the bare backdrop for an A/B. */
static int tg_city_emit_forkback_plaza(const TG_FBHook *h, double side,
                                       const TG_Node *n0, const TG_Node *n1,
                                       double lx0, double lz0,
                                       double lx1, double lz1,
                                       double set0, double set1, double depth)
{
    const int si = h->si;
    /* Inner edge sits at the skirt's outer reach (measured from the road edge),
     * underlapped a little so the plaza abuts the ordinary ground with no seam
     * and does NOT overlap-and-z-fight the sloping skirt inside it. The whole
     * point is the region BEYOND the 12000 skirt, where a fork backdrop stands
     * (front ~14000-21000 out, seed 99991 span 137) over the sunk far-band and so
     * "does not reach the floor". */
    const double inner = tg_verge_reach() - 1000.0;
    double dn, df, u_d;
    double px[4], py[4], pz[4], uu[4], vv[4];
    int seg_page = TD5_TG_PAGE_GROUND, seg_nq, pp;

    if (!td5_env_flag_on("TD5RE_R7_CITY_PLAZA")) return 1;

    dn  = n0->width * 0.5 + set0 + depth;      /* near outer = block back */
    df  = n1->width * 0.5 + set1 + depth;      /* far  outer              */
    u_d = (dn - (n0->width * 0.5 + inner)) / (double)TD5_TG_SPAN_LENGTH;
    {
        const double inx0 = n0->x + lx0 * (n0->width * 0.5 + inner);
        const double inz0 = n0->z + lz0 * (n0->width * 0.5 + inner);
        const double inx1 = n1->x + lx1 * (n1->width * 0.5 + inner);
        const double inz1 = n1->z + lz1 * (n1->width * 0.5 + inner);
        px[0]=inx0;            py[0]=n0->y; pz[0]=inz0;            uu[0]=0.0; vv[0]=(double)si;
        px[1]=n0->x+lx0*dn;    py[1]=n0->y; pz[1]=n0->z+lz0*dn;   uu[1]=u_d; vv[1]=(double)si;
        px[2]=n1->x+lx1*df;    py[2]=n1->y; pz[2]=n1->z+lz1*df;   uu[2]=u_d; vv[2]=(double)si + 1.0;
        px[3]=inx1;            py[3]=n1->y; pz[3]=inz1;            uu[3]=0.0; vv[3]=(double)si + 1.0;
    }
    if (*h->nmesh < h->maxmesh) {
        seg_nq = 1;
        h->moff[(*h->nmesh)++] = h->blk->len;
        if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, 4, &seg_page, &seg_nq, 1))
            return 0;
        tg_acct(TG_ACCT_R7_CITY, si);          /* the plaza floor */
    }

    /* Dressing: a fountain/monument on a ~1-in-3 beat, a passer-by otherwise,
     * out on the plaza between the skirt end and the block front. tg_prop_one
     * adds width/2 + gap itself and accounts each as a PROP. */
    pp = ((unsigned)si % 3u == 0u) ? PP_MONUMENT : PP_PERSON0;
    if (!tg_prop_one(h->nl, si, pp, side, inner + 0.4 * (set0 - inner),
                     h->blk, h->moff, h->nmesh))
        return 0;

    /* [R9 CITY item 3] CLOSE THE PLAZA'S ENDS.
     *
     * The user asks for "buildings on its three sides" at span 358. First, what
     * IS that thing: roadside parks are default OFF (tg_block_is_park returns 0
     * unless TD5RE_AUTOTRACK_PARKS=1) and the element inventory confirms it --
     * `parks` is in the NONE-emitted list on both seeds, so there is no park
     * anywhere on this track. What stands to the right of 358 is THIS emitter's
     * plaza: the r7-city accounting run covers 311-362, span 358 included. So the
     * item is real, but it is a plaza, not a park -- an open paved square with a
     * fountain and passers-by.
     *
     * Its composition is the complaint. The forkback lays a building band along
     * the plaza's BACK edge only, so the square is walled on one side and open on
     * the other three -- which is why it reads as an undeveloped green rather
     * than a city square. The back is already built, so "three sides" means the
     * back plus the two ENDS. This closes the ends: where a forkback run STARTS
     * or ENDS (the neighbouring span has no backdrop), stand a block across the
     * plaza's depth, running from the inner edge out to the block back, square to
     * the road. Interior spans of a run add nothing, so a long plaza gets exactly
     * two end walls rather than a comb.
     *
     * Geometry is taken from the same chord this function already computed -- the
     * inner edge at `inner`, the outer at dn -- so the end wall cannot drift from
     * the floor it closes, and it is set at the plaza depth, which the backdrop
     * already cleared through tg_carriageway_clear_gap. Nothing new is placed
     * nearer the road than the existing block, so the on-road guard has nothing
     * more to reject. TD5RE_R9_CITY_PLAZA_ENDS=0 for an A/B. */
    if (td5_env_flag_on("TD5RE_R9_CITY_PLAZA_ENDS")) {
        const int prev_run = tg_span_in_fork_clear(si - 1);
        const int next_run = tg_span_in_fork_clear(si + 1);
        int end = 0;
        if (!prev_run) end = -1;            /* first span of the run */
        else if (!next_run) end = +1;       /* last span of the run  */
        if (end != 0) {
            const TG_Biome *b = h->b;
            const unsigned int eh = ((unsigned)si * 2246822519u) ^ 0x85EBCA6Bu;
            /* Along-road unit for THIS span, and the wall's own thickness. */
            double wx = n1->x - n0->x, wz = n1->z - n0->z;
            double wl = sqrt(wx * wx + wz * wz);
            const double d_in = n0->width * 0.5 + inner;
            const double span_d = dn - d_in;       /* plaza depth to close */
            int rows = b->floors_min + (int)((eh >> 13) % 4u);
            double H;
            if (wl < 1e-6 || !(span_d > 0.0)) return 1;
            wx /= wl; wz /= wl;
            if (rows > TD5_TG_FORKBACK_MAX_ROWS) rows = TD5_TG_FORKBACK_MAX_ROWS;
            H = (double)rows * tg_facade_floor_h(b);
            {   /* Base chord runs OUTWARD across the plaza at the run's end. */
                const double ex = n0->x + lx0 * d_in + wx * (end < 0 ? 0.0 : wl);
                const double ez = n0->z + lz0 * d_in + wz * (end < 0 ? 0.0 : wl);
                const double axx = lx0 * span_d, azz = lz0 * span_d;
                const int cols = tg_facade_cols_for(span_d, (double)b->cell_w, 4);
                if (!tg_bg_building_box(h->blk, h->moff, h->nmesh, h->maxmesh,
                                        ex, n0->y, ez, axx, 0.0, azz,
                                        wx * (double)end, wz * (double)end,
                                        wx * (double)end, wz * (double)end,
                                        TD5_TG_FORKBACK_DEPTH, H, cols, rows,
                                        tg_facade_page_class(eh, rows), 1, si))
                    return 0;
                tg_acct(TG_ACCT_R9_CITY, si);
            }
        }
    }
    return 1;
}

static int tg_city_emit_forkback(const TG_FBHook *h)
{
    const TG_Biome *b = h->b;
    const TG_NodeList *nl = h->nl;
    const int si = h->si;
    const double side = -1.0;               /* forks only clear the RIGHT lateral */
    const int small = td5_env_flag_on("TD5RE_AUTOTRACK_FORKBACK_SMALL");
    const double gap = small ? TD5_TG_FORKBACK_GAP : TD5_TG_FORKBACK_GAP_TALL;
    const TG_Node *n0, *n1;
    double lx0, lz0, lx1, lz1, set0, set1, bx, by, bz, ax, ay, az;
    unsigned int blk, ph, gs, gl, bh, sh;
    const int vary = td5_env_flag_on("TD5RE_AUTOTRACK_FORKBACK_VARY");
    int av;

    if (!td5_env_flag_on("TD5RE_AUTOTRACK_FORK_BACKDROP")) return 1;
    if (!tg_branches_enabled() || !tg_span_in_fork_clear(si)) return 1;
    if (tg_span_in_bridge_run(si)) return 1;
    if (si + 1 >= nl->count) return 1;
    n0 = &nl->v[si]; n1 = &nl->v[si + 1];

    lx0 = n0->tz * side; lz0 = -n0->tx * side;
    lx1 = n1->tz * side; lz1 = -n1->tx * side;

    /* Distance from the road EDGE to the front of the band: the whole corridor
     * clearance (clear_gap) plus a background gap so the block stands well back.
     * [R5 items 6/7] The clearance is taken PER ENDPOINT -- clear_gap(si) for the
     * near base and clear_gap(si+1) for the far base -- not one clear_gap(si) for
     * both. The corridor BOWS: the branch's outer edge is further out at si+1
     * than at si over the widening half of a fork, so applying si's clearance to
     * the far base set the front INBOARD of the branch there, and the front chord
     * skewed against the branch it was meant to back. That is item 6 ("background
     * building colliding with the road" at 153) and item 7 ("stretched ...
     * diagonally to the branch road" at 160): the branch bows into a backdrop
     * laid to the near span's narrower reach. Per-endpoint clearance makes the
     * band follow the bow. Default ON; TD5RE_AUTOTRACK_FORKBACK_FOLLOW=0 restores
     * the single-set behaviour for an A/B. */
    set0 = tg_carriageway_clear_gap(nl, si, side, tg_city_sidewalk_w(b),
                                    TD5_TG_CARRIAGEWAY_MARGIN) + gap;
    set1 = td5_env_flag_on("TD5RE_AUTOTRACK_FORKBACK_FOLLOW")
         ? tg_carriageway_clear_gap(nl, si + 1, side, tg_city_sidewalk_w(b),
                                    TD5_TG_CARRIAGEWAY_MARGIN) + gap
         : set0;

    /* [R6 item 3] Per-span stagger: shift this span's whole block back by a
     * span-keyed amount so the row breaks into distinct, differently-set-back
     * buildings instead of one continuous wall. Applied to both endpoints so the
     * block stays parallel; the far background band closes the ground behind the
     * gaps a stagger opens between neighbouring spans. */
    sh = ((unsigned)si * 2654435761u) ^ 0x9e3779b9u;
    if (vary) {
        const double stag = TD5_TG_FORKBACK_STAGGER_BASE
                          + (double)((sh >> 17) % TD5_TG_FORKBACK_STAGGER_RANGE);
        set0 += stag; set1 += stag;
    }

    bx = n0->x + lx0 * (n0->width * 0.5 + set0);
    by = n0->y;
    bz = n0->z + lz0 * (n0->width * 0.5 + set0);
    ax = (n1->x + lx1 * (n1->width * 0.5 + set1)) - bx;
    ay = n1->y - n0->y;
    az = (n1->z + lz1 * (n1->width * 0.5 + set1)) - bz;

    tg_facade_block(si, 0, &blk, &ph, &gs, &gl, &av);
    bh = blk * 2654435761u;

    if (tg_block_is_park(si, 0)) {
        /* PARK: a green lawn over the bare tiles, deep enough to read as open
         * ground behind the branch. One flat quad, park-lawn page, isotropic UV
         * so it tiles the same on a curve. */
        double px[4], py[4], pz[4], uu[4], vv[4], q[12];
        const double d = small ? TD5_TG_FORKBACK_DEPTH : TD5_TG_FORKBACK_DEPTH_TALL;
        int seg_page = TD5_TG_PAGE_R3_BLOCK + 0, seg_nq, n = 0;
        /* [R17 CITY item 3] "floating grass -- is this supposed to be a park?"
         * The lawn's front edge sat at the block front (set0, ~14000 out) with a
         * bare gap back to the 12000 verge skirt, so it hung FLAT over the sunk
         * far band as an isolated island. The BUILDING branch already solved the
         * identical "does not reach the floor" problem by laying a plaza floor
         * from the skirt reach out (tg_city_emit_forkback_plaza, inner =
         * tg_verge_reach()-1000); do the same here by pulling the lawn's own inner
         * edge back to that reach, so the green apron abuts the ordinary ground
         * with no gap instead of floating. TD5RE_R17_FORKPARK_FLOOR=0 restores the
         * island for an A/B. */
        const double fin = td5_env_flag_on("TD5RE_R17_FORKPARK_FLOOR")
                         ? tg_verge_reach() - 1000.0 : set0;
        const double fnx = n0->x + lx0 * (n0->width * 0.5 + fin);
        const double fnz = n0->z + lz0 * (n0->width * 0.5 + fin);
        const double ffx = n1->x + lx1 * (n1->width * 0.5 + fin);
        const double ffz = n1->z + lz1 * (n1->width * 0.5 + fin);
        const double u_d = (set0 + d - fin) / (double)TD5_TG_SPAN_LENGTH;
        q[0] = fnx;             q[1] = by;      q[2] = fnz;
        q[3] = bx + lx0 * d;    q[4] = by;      q[5] = bz + lz0 * d;
        q[6] = bx + ax + lx1*d; q[7] = by + ay; q[8] = bz + az + lz1 * d;
        q[9] = ffx;             q[10] = by + ay; q[11] = ffz;
        px[0]=q[0]; py[0]=q[1]; pz[0]=q[2];  uu[0]=0.0;  vv[0]=(double)si;
        px[1]=q[3]; py[1]=q[4]; pz[1]=q[5];  uu[1]=u_d;  vv[1]=(double)si;
        px[2]=q[6]; py[2]=q[7]; pz[2]=q[8];  uu[2]=u_d;  vv[2]=(double)si + 1.0;
        px[3]=q[9]; py[3]=q[10];pz[3]=q[11]; uu[3]=0.0;  vv[3]=(double)si + 1.0;
        n = 4;
        if (*h->nmesh >= h->maxmesh) return 1;
        seg_nq = n / 4;
        h->moff[(*h->nmesh)++] = h->blk->len;
        if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n,
                                &seg_page, &seg_nq, 1))
            return 0;
        tg_acct(TG_ACCT_FORKBACK, si);
        return 1;
    }

    /* BUILDINGS: a skyline block set behind the corridor. [R6 item 3] height is
     * keyed PER SPAN (not the superblock) so neighbouring blocks step up and down
     * -- a varied skyline, not one flat-topped wall. The district climb is
     * dropped for the backdrop: it only pinned the whole run to the cap, which is
     * what read as the uniform "line of skyscrapers". */
    {
        int rows = vary
                 ? b->floors_min + (int)((sh >> 11) % 5u)
                 : b->floors_min + tg_city_district_floors(blk)
                   + (int)((bh >> 9) % (small ? 3u : 5u)) + (small ? 0 : 2);
        const int cap = small ? TD5_TG_FORKBACK_MAX_ROWS : TD5_TG_FACADE_MAX_ROWS;
        const double depth = small ? TD5_TG_FORKBACK_DEPTH
                                   : TD5_TG_FORKBACK_DEPTH_TALL;
        double H, flen;
        int cols, page;
        if (rows > cap) rows = cap;
        H = (double)rows * tg_facade_floor_h(b);
        flen = sqrt(ax * ax + az * az);
        cols = tg_facade_cols_for(flen, (double)b->cell_w, 4);
        /* [R6 item 3] A backdrop uses a low-rise MASONRY page even when tall: the
         * glass TOWER pages (rows >= FACADE_TALL_ROWS) read as a blown-out white
         * slab at the distance a fork backdrop is usually seen from -- fork
         * 319-360 seen down the descending avenue from span 188 -- which is worse
         * than the uniform wall it replaced. Stone/brick reads correctly there. */
        page = vary ? tg_facade_page_class(bh, TD5_TG_FACADE_TALL_ROWS - 1)
                    : tg_facade_page_class(bh, rows);
        if (!tg_bg_building_box(h->blk, h->moff, h->nmesh, h->maxmesh,
                                bx, by, bz, ax, ay, az, lx0, lz0, lx1, lz1,
                                depth, H, cols, rows, page, 1, si))
            return 0;
        tg_acct(TG_ACCT_FORKBACK, si);
        /* [R7 item 3] give the backdrop a floor to stand on + plaza dressing. */
        if (!tg_city_emit_forkback_plaza(h, side, n0, n1, lx0, lz0, lx1, lz1,
                                         set0, set1, depth))
            return 0;
    }
    return 1;
}

static int tg_block_emit_arm(const TG_FBHook *h,
                             double cx, double cy, double cz,
                             double ox, double oz, double bx, double bz,
                             double reach, double sw)
{
    double px[8], py[8], pz[8], uu[8], vv[8], q[12], t[8];
    const double drop = tg_xstreet_drop(reach);    /* [R8] matches the street */
    const double ur = reach / (double)TD5_TG_SPAN_LENGTH;
    const double uw = sw / (double)TD5_TG_SPAN_LENGTH;
    const double back = (sw * 0.35 < TD5_TG_ARM_SET) ? sw * 0.35 : TD5_TG_ARM_SET;
    int seg_page, seg_nq, n;

    /* Slab top at kerb height: kerb-corner, kerb-outer, back-outer, back-corner.
     * "back" thickens the slab by sw away from the carriageway. */
    n = 0; seg_page = TD5_TG_PAGE_SIDEWALK;
    q[0]  = cx;                     q[1]  = cy + TD5_TG_KERB_H;
    q[2]  = cz;
    q[3]  = cx + ox * reach;        q[4]  = cy + TD5_TG_KERB_H - drop;
    q[5]  = cz + oz * reach;
    q[6]  = cx + ox * reach + bx * sw; q[7] = cy + TD5_TG_KERB_H - drop;
    q[8]  = cz + oz * reach + bz * sw;
    q[9]  = cx + bx * sw;           q[10] = cy + TD5_TG_KERB_H;
    q[11] = cz + bz * sw;
    t[0] = 0.0; t[1] = 0.0;  t[2] = ur; t[3] = 0.0;
    t[4] = ur;  t[5] = uw;   t[6] = 0.0; t[7] = uw;
    tg_city_push_quad(px, py, pz, uu, vv, &n, q, t);
    if (*h->nmesh >= h->maxmesh) return 1;
    seg_nq = n / 4;
    tg_acct(TG_ACCT_SIDEWALK, h->si);
    h->moff[(*h->nmesh)++] = h->blk->len;
    {   /* [R9 CITY item 4] provenance: proves the kerb turns down this mouth */
        const size_t p0 = h->blk->len;
        const int r = tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n,
                                         &seg_page, &seg_nq, 1);
        tg_pave_mark(p0, h->blk->len, TG_PVS_ARM, h->si);
        if (!r) return 0;
    }

    /* Kerb face down to the carriageway along the kerb-corner..kerb-outer edge. */
    n = 0;
    q[0]  = cx;                q[1]  = cy;                    q[2]  = cz;
    q[3]  = cx + ox * reach;   q[4]  = cy - drop;             q[5]  = cz + oz * reach;
    q[6]  = cx + ox * reach;   q[7]  = cy + TD5_TG_KERB_H - drop;
    q[8]  = cz + oz * reach;
    q[9]  = cx;                q[10] = cy + TD5_TG_KERB_H;    q[11] = cz;
    t[0] = 0.0; t[1] = 0.0; t[2] = ur; t[3] = 0.0;
    t[4] = ur; t[5] = TD5_TG_KERB_H / (double)TD5_TG_SPAN_LENGTH;
    t[6] = 0.0; t[7] = TD5_TG_KERB_H / (double)TD5_TG_SPAN_LENGTH;
    tg_city_push_quad(px, py, pz, uu, vv, &n, q, t);
    if (*h->nmesh >= h->maxmesh) return 1;
    seg_nq = n / 4;
    h->moff[(*h->nmesh)++] = h->blk->len;
    if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n, &seg_page, &seg_nq, 1))
        return 0;

    /* Railing along the same edge, set back onto the slab (posts off the road). */
    n = 0; seg_page = TD5_TG_PAGE_FENCE;
    q[0]  = cx + bx * back;              q[1]  = cy + TD5_TG_KERB_H;
    q[2]  = cz + bz * back;
    q[3]  = cx + bx * back + ox * reach; q[4]  = cy + TD5_TG_KERB_H - drop;
    q[5]  = cz + bz * back + oz * reach;
    q[6]  = cx + bx * back + ox * reach; q[7]  = cy + TD5_TG_KERB_H - drop + TD5_TG_FENCE_H;
    q[8]  = cz + bz * back + oz * reach;
    q[9]  = cx + bx * back;              q[10] = cy + TD5_TG_KERB_H + TD5_TG_FENCE_H;
    q[11] = cz + bz * back;
    t[0] = 0.0; t[1] = 1.0; t[2] = ur; t[3] = 1.0;
    t[4] = ur;  t[5] = 0.0; t[6] = 0.0; t[7] = 0.0;
    tg_city_push_quad(px, py, pz, uu, vv, &n, q, t);
    if (*h->nmesh >= h->maxmesh) return 1;
    seg_nq = n / 4;
    tg_acct(TG_ACCT_FENCE, h->si);
    h->moff[(*h->nmesh)++] = h->blk->len;
    return tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n, &seg_page, &seg_nq, 1);
}

/* Intersections (item 3): pavement + railing arms at the two along-road corners
 * of every side-street gap, so the sidewalks and guard rails TURN to follow the
 * street. Only at the gap's boundary spans (where the neighbour on that side is
 * built) -- the interior of the gap is the carriageway itself. Parks and forks
 * are skipped: a park has no through street, and a fork's half-width road has no
 * room for a junction. */
/* [R11 CITY item 6] The SPAN-level half of tg_block_emit_intersection's gate --
 * everything that decides "can this span host a junction at all", with no side
 * in it. Split out (rather than duplicated) so tg_r11_arm_side below and the
 * emitter itself read one authority and cannot drift. */
static int tg_block_arm_span_ok(int si)
{
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_INTERSECTIONS")) return 0;
    if (tg_span_in_bridge_run(si)) return 0;
    if (td5_env_flag_on("TD5RE_AUTOTRACK_XBRIDGE_GATE") &&   /* item 15 */
        tg_span_near_bridge(si, TD5_TG_XBRIDGE_CLEAR)) return 0;
    if (tg_branches_enabled() && tg_span_in_fork_clear(si) &&
        !td5_env_flag_on("TD5RE_R8_CROSS_FORKARM")) return 0;
    return 1;
}

/* Which arms stand on side `s` of span si -- see the forward declaration. The
 * per-side gates and the two corner tests are exactly the emitter's, below. */
int tg_r11_arm_side(const TG_NodeList *nl, int si, int s)
{
    const double sg = s ? 1.0 : -1.0;
    if (si <= 0) return 0;
    if (!(tg_city_sidewalk_w(&k_biomes[tg_scenery_biome_index(si)]) > 0.0))
        return 0;
    if (!tg_block_arm_span_ok(si)) return 0;
    if (tg_facade_built(si, s)) return 0;          /* built = no junction */
    if (tg_block_is_park(si, s)) return 0;         /* a park, not a street */
    if (tg_side_corridor_here(nl, si, sg)) return 0;
    /* [R16 CITY item 1] pavement corner, blind to the outskirts density ramp:
     * a ramp-retracted frontage still carries the raised pavement, so its arm
     * still wraps the sidewalk down the side street (tg_r16_pave_corner_stands
     * == tg_r11_corner_stands when TD5RE_R16_RAMP_JUNCTION is off). */
    return (tg_r16_pave_corner_stands(si - 1, s) ? 1 : 0)
         | (tg_r16_pave_corner_stands(si + 1, s) ? 2 : 0);
}

static int tg_block_emit_intersection(const TG_FBHook *h)
{
    double e[10];
    const double sw = tg_city_sidewalk_w(h->b);
    int s;

    if (!(sw > 0.0)) return 1;
    if (!tg_block_arm_span_ok(h->si)) return 1;
    /* [R8 CROSS, carried-in open item] "8 side-street mouths inside forks lack
     * flanking pavement." This blanket return was the cause and it was never
     * matched by the emitter it flanks: tg_city_emit_crossstreet gates on
     * tg_side_blocked ALONE, which suppresses only the side a fork corridor
     * actually eats (side < 0). So inside a fork-clear region the carriageway
     * was still laid on the untouched side while its pavement + railing arms
     * were dropped for BOTH sides -- a street with no kerbs, which is exactly
     * the reported symptom. Dropping the blanket gate leaves the per-side
     * tg_side_blocked test in the loop below as the single authority, so the two
     * emitters now agree span for span on where a junction exists.
     * TD5RE_R8_CROSS_FORKARM=0 restores the blanket fork drop for an A/B.
     * [R11 CITY item 6] Both that gate and the four per-side tests below now
     * live in tg_block_arm_span_ok / tg_r11_arm_side above; this loop reads the
     * shared answer so the frontage trim can never disagree with the arm. */

    for (s = 0; s < 2; s++) {
        const int arms = tg_r11_arm_side(h->nl, h->si, s);
        const double sg = s ? 1.0 : -1.0;
        double reach, ang, ax, az, alen, ox, oz;
        int near_corner, far_corner;

        if (!arms) continue;
        near_corner = (arms & 1);                     /* gap starts here */
        far_corner  = (arms & 2);                     /* gap ends here   */

        tg_city_edge_frame(h->nl, h->si, sg, e);
        ang   = tg_block_arm_skew(h->si, s);
        /* [R8] Same per-side clamped reach the carriageway uses, so a pavement
         * arm can never outrun (or fall short of) the street it flanks. */
        reach = tg_xstreet_reach_at(h->nl, h->si, sg, ang, h->b, sw);
        /* Along-road unit, near -> far. */
        ax = e[3] - e[0]; az = e[5] - e[2];
        alen = sqrt(ax * ax + az * az);
        if (alen < 1e-6) { ax = 0.0; az = 1.0; } else { ax /= alen; az /= alen; }

        if (near_corner) {                            /* corner at the near node */
            tg_block_rot2(e[6], e[7], ang, &ox, &oz);
            /* back = -along (onto the built side, off the carriageway). */
            if (!tg_block_emit_arm(h, e[0], e[1], e[2], ox, oz, -ax, -az,
                                   reach, sw))
                return 0;
        }
        if (far_corner) {                             /* corner at the far node */
            tg_block_rot2(e[8], e[9], ang, &ox, &oz);
            if (!tg_block_emit_arm(h, e[3], e[4], e[5], ox, oz, ax, az,
                                   reach, sw))
                return 0;
        }
    }
    return 1;
}

/* One small house box (4 walls + a roof) as a single 2-segment mesh, centred
 * along the span and set back on the lawn. Walls on the house page, roof on the
 * roof page. */
static int tg_block_emit_house(const TG_FBHook *h, const double *e, double set)
{
    double px[20], py[20], pz[20], uu[20], vv[20];
    double ax, az, alen, ox, oz, y0, drop;
    double flx, flz, frx, frz, blx, blz, brx, brz;
    const double w = TD5_TG_HOUSE_W, d = TD5_TG_HOUSE_D, H = TD5_TG_HOUSE_H;
    int seg_page[2], seg_nq[2], n = 0;

    /* Along-road unit and length of this span's edge. */
    ax = e[3] - e[0]; az = e[5] - e[2];
    alen = sqrt(ax * ax + az * az);
    if (alen < 1e-6) return 1;
    ax /= alen; az /= alen;
    ox = e[6]; oz = e[7];                     /* near outward (square-on house) */
    drop = TD5_TG_GROUND_DROP * set / TD5_TG_GROUND_WIDTH;
    y0 = e[1] + TD5_TG_VERGE_LIFT - drop;

    {   /* front-left base, centred along the span. */
        const double off = (alen - w) > 0.0 ? (alen - w) * 0.5 : 0.0;
        const double fx = e[0] + ax * off + ox * set;
        const double fz = e[2] + az * off + oz * set;
        flx = fx;            flz = fz;
        frx = fx + ax * w;   frz = fz + az * w;
        blx = flx + ox * d;  blz = flz + oz * d;
        brx = frx + ox * d;  brz = frz + oz * d;
    }

    /* Four walls (near-bottom, far-bottom, far-top, near-top per quad). */
    #define HQ(x0,z0,x1,z1) \
        do { \
            px[n]=x0; py[n]=y0;   pz[n]=z0; uu[n]=0.0; vv[n]=1.0; n++; \
            px[n]=x1; py[n]=y0;   pz[n]=z1; uu[n]=1.0; vv[n]=1.0; n++; \
            px[n]=x1; py[n]=y0+H; pz[n]=z1; uu[n]=1.0; vv[n]=0.0; n++; \
            px[n]=x0; py[n]=y0+H; pz[n]=z0; uu[n]=0.0; vv[n]=0.0; n++; \
        } while (0)
    HQ(flx, flz, frx, frz);      /* front */
    HQ(brx, brz, blx, blz);      /* back  */
    HQ(blx, blz, flx, flz);      /* left  */
    HQ(frx, frz, brx, brz);      /* right */
    #undef HQ

    /* Roof: flat deck at the top (near a low pitch would need a ridge; a deck
     * reads fine at the distance a park house is ever seen). */
    px[n]=flx; py[n]=y0+H; pz[n]=flz; uu[n]=0.0; vv[n]=1.0; n++;
    px[n]=frx; py[n]=y0+H; pz[n]=frz; uu[n]=1.0; vv[n]=1.0; n++;
    px[n]=brx; py[n]=y0+H; pz[n]=brz; uu[n]=1.0; vv[n]=0.0; n++;
    px[n]=blx; py[n]=y0+H; pz[n]=blz; uu[n]=0.0; vv[n]=0.0; n++;

    if (*h->nmesh >= h->maxmesh) return 1;
    seg_page[0] = TD5_TG_PAGE_R3_BLOCK + 2;   /* house wall */
    seg_nq[0]   = 4;
    seg_page[1] = TD5_TG_PAGE_R3_BLOCK + 3;   /* house roof */
    seg_nq[1]   = 1;
    tg_acct(TG_ACCT_HOUSE, h->si);
    h->moff[(*h->nmesh)++] = h->blk->len;
    return tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n, seg_page, seg_nq, 2);
}

/* Park green + hedge (+ a house on some spans) for every open, non-street gap
 * side. Default ON; houses behind their own knob so a plain green park is one
 * A/B away. */
static int tg_block_emit_park(const TG_FBHook *h)
{
    const double sw = tg_city_sidewalk_w(h->b);
    int s;

    if (!(sw > 0.0)) return 1;
    if (tg_span_in_bridge_run(h->si)) return 1;
    if (tg_branches_enabled() && tg_span_in_fork_clear(h->si)) return 1;

    for (s = 0; s < 2; s++) {
        const double sg = s ? 1.0 : -1.0;
        double e[10], q[12], t[8], px[8], py[8], pz[8], uu[8], vv[8];
        double reach, drop, u_r;
        int seg_page, seg_nq, n;

        if (tg_facade_built(h->si, s)) continue;
        if (!tg_block_is_park(h->si, s)) continue;    /* only park gaps */
        if (tg_side_blocked(h->si, sg)) continue;
        tg_city_edge_frame(h->nl, h->si, sg, e);

        reach = tg_city_crossst_reach(h->b, sw);
        /* [R17 CITY item 1] "avoid this grass, this should be a street crossing."
         * A park is a bounded SQUARE, not a through-street corridor, but this lawn
         * borrowed tg_city_crossst_reach -- the R8 side-STREET reach, up to
         * TD5_TG_R8_XSTREET_MAX (21000). Past the 12000 verge skirt the lawn hung
         * near-FLAT (its drop uses the 24000 GROUND_WIDTH, ~61 units at 21000)
         * over the far band that has sunk toward the track floor, so a park read
         * as a giant floating green "side street". Bound it to one block's depth,
         * which is also inside the skirt, so the park sits on the ground as a
         * compact square. TD5RE_R17_PARK_REACH=0 restores the street-length reach
         * for an A/B. */
        if (td5_env_flag_on("TD5RE_R17_PARK_REACH")) {
            const double sq = sw + tg_facade_depth(h->b) + TD5_TG_BACKROW_GAP;
            if (reach > sq) reach = sq;
        }
        drop  = TD5_TG_GROUND_DROP * reach / TD5_TG_GROUND_WIDTH;
        u_r   = reach / (double)TD5_TG_SPAN_LENGTH;

        /* Lawn: kerb -> reach, sinking with the skirt, isotropic UV so it tiles
         * the same on curves. Same winding as the verge band. */
        n = 0; seg_page = TD5_TG_PAGE_R3_BLOCK + 0;   /* park lawn */
        q[0] = e[0];               q[1]  = e[1] + TD5_TG_VERGE_LIFT;
        q[2] = e[2];
        q[3] = e[0] + e[6] * reach; q[4] = e[1] + TD5_TG_VERGE_LIFT - drop;
        q[5] = e[2] + e[7] * reach;
        q[6] = e[3] + e[8] * reach; q[7] = e[4] + TD5_TG_VERGE_LIFT - drop;
        q[8] = e[5] + e[9] * reach;
        q[9] = e[3];               q[10] = e[4] + TD5_TG_VERGE_LIFT;
        q[11] = e[5];
        t[0] = 0.0; t[1] = (double)h->si;
        t[2] = u_r; t[3] = (double)h->si;
        t[4] = u_r; t[5] = (double)h->si + 1.0;
        t[6] = 0.0; t[7] = (double)h->si + 1.0;
        tg_city_push_quad(px, py, pz, uu, vv, &n, q, t);
        if (*h->nmesh >= h->maxmesh) return 1;
        seg_nq = n / 4;
        tg_acct(TG_ACCT_PARK, h->si);
        h->moff[(*h->nmesh)++] = h->blk->len;
        if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n,
                                &seg_page, &seg_nq, 1))
            return 0;

        /* Hedge border. [R5 CROSS item 4] The user reported "a green texture
         * around 1m height" that "looks out of place" near crossings: it is this
         * hedge, which stood ONE pavement-width (sw ~= 350 raw) off the kerb and
         * so read as a bright green ~1m WALL right at the roadside wherever a
         * park gap fell in the city (confirmed by framedump at span 92, page
         * R3_BLOCK+1). A real park shows open lawn to the kerb with its boundary
         * hedge at the BACK, so the hedge is set back onto the far part of the
         * lawn (hset) and stood on the sloped lawn surface there (verge lift less
         * the ground-skirt drop at that distance), turning "green wall at the
         * road" into "hedge at the back of the park". Still CULL_NONE so one
         * plane reads both sides. Knob for a single-variable A/B. */
        if (td5_env_flag_on("TD5RE_AUTOTRACK_PARK_HEDGE")) {
            const double hset  = sw + (reach - sw) * 0.75;
            const double hdrop = TD5_TG_GROUND_DROP * hset / TD5_TG_GROUND_WIDTH;
            const double hy    = TD5_TG_VERGE_LIFT - hdrop;
            n = 0; seg_page = TD5_TG_PAGE_R3_BLOCK + 1;   /* park hedge */
            q[0] = e[0] + e[6] * hset;  q[1]  = e[1] + hy;
            q[2] = e[2] + e[7] * hset;
            q[3] = e[3] + e[8] * hset;  q[4]  = e[4] + hy;
            q[5] = e[5] + e[9] * hset;
            q[6] = e[3] + e[8] * hset;  q[7]  = e[4] + hy + TD5_TG_HEDGE_H;
            q[8] = e[5] + e[9] * hset;
            q[9] = e[0] + e[6] * hset;  q[10] = e[1] + hy + TD5_TG_HEDGE_H;
            q[11] = e[2] + e[7] * hset;
            t[0] = 0.0; t[1] = 1.0; t[2] = 1.0; t[3] = 1.0;
            t[4] = 1.0; t[5] = 0.0; t[6] = 0.0; t[7] = 0.0;
            tg_city_push_quad(px, py, pz, uu, vv, &n, q, t);
            if (*h->nmesh >= h->maxmesh) return 1;
            seg_nq = n / 4;
            tg_acct(TG_ACCT_PARK, h->si);
            h->moff[(*h->nmesh)++] = h->blk->len;
            if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n,
                                    &seg_page, &seg_nq, 1))
                return 0;
        }

        /* One house on ~1 park span in 4, set back in the middle of the lawn. */
        if (td5_env_flag_on("TD5RE_AUTOTRACK_PARK_HOUSES")) {
            const unsigned int hh = ((unsigned)h->si * 2654435761u
                                     + (unsigned)s * 40503u) * 0x9E3779B9u;
            if ((hh >> 30) == 0u) {
                const double set = sw + (reach - sw) * 0.45;
                if (!tg_block_emit_house(h, e, set)) return 0;
            }
        }
    }
    return 1;
}

/* [R16 CITY item 2] "if you want to create big plazas where buildings are
 * retracted ... add some elements to these plazas so they look more alive like
 * park sections."
 *
 * The outskirts DENSITY ramp (tg_town_ramp_open) retracts a run's buildings
 * across the leading spans of a wilderness-to-town run, leaving the raised
 * pavement in place (tg_facade_built is still 1, so the sidewalk hook keeps the
 * slab) but BARE GROUND where the wall would have stood -- the empty ground
 * slab the user picked (skirt / GROUND). This dresses that freed footprint with
 * a park section: a lawn from the pavement back edge out to where the wall would
 * have been, a boundary hedge at the back, and the occasional house -- reusing
 * the R3 park geometry rather than inventing any. Everything sits BEHIND the
 * raised pavement (setback >= sw) and stops at the wall line (sw + facade
 * depth), which is well inside where the back rows stand (sw + depth + 2*3200),
 * so it overlaps neither the road nor the backrow band.
 *
 * ONLY on ramp-retracted, pattern-built, paved sides -- never a side-street
 * mouth (tg_facade_built == 0, handled by the intersection/park emitters) and
 * never where a real building actually stands (tg_town_ramp_open == 0). Default
 * ON; TD5RE_R16_PLAZA_PARK=0 leaves the plaza bare for an A/B. */
static int tg_r16_emit_outskirt_park(const TG_FBHook *h)
{
    /* Hardened scenery biome, the same one tg_city_span_paved, tg_facade_built
     * and tg_town_ramp_open key off, so the dressing lands exactly where the
     * pavement and the retracted-frontage decision do (h->b is the BLENDED
     * biome and can disagree over the leading blend band). */
    const TG_Biome *b = &k_biomes[tg_scenery_biome_index(h->si)];
    const double sw = tg_city_sidewalk_w(b);
    const double depth = tg_facade_depth(b);
    int s;

    if (!td5_env_flag_on("TD5RE_R16_PLAZA_PARK")) return 1;
    if (!(sw > 0.0) || !(depth > 0.0)) return 1;
    if (tg_span_in_bridge_run(h->si)) return 1;
    if (tg_branches_enabled() && tg_span_in_fork_clear(h->si)) return 1;

    for (s = 0; s < 2; s++) {
        const double sg = s ? 1.0 : -1.0;
        double e[10], q[12], t[8], px[8], py[8], pz[8], uu[8], vv[8];
        const double inner = sw;                 /* behind the raised slab      */
        const double outer = sw + depth;         /* the retracted wall line     */
        const double d_in  = TD5_TG_GROUND_DROP * inner / TD5_TG_GROUND_WIDTH;
        const double d_out = TD5_TG_GROUND_DROP * outer / TD5_TG_GROUND_WIDTH;
        const double u_d = depth / (double)TD5_TG_SPAN_LENGTH;
        int seg_page, seg_nq, n;

        /* Only a slot the ramp emptied: pattern built, wall retracted. */
        if (!tg_facade_built(h->si, s)) continue;
        if (!tg_town_ramp_open(h->si, s)) continue;
        if (tg_side_blocked(h->si, sg)) continue;
        tg_city_edge_frame(h->nl, h->si, sg, e);

        /* Lawn, pavement back edge -> wall line, sinking with the skirt. */
        n = 0; seg_page = TD5_TG_PAGE_R3_BLOCK + 0;   /* park lawn */
        q[0]  = e[0] + e[6] * inner; q[1]  = e[1] + TD5_TG_VERGE_LIFT - d_in;
        q[2]  = e[2] + e[7] * inner;
        q[3]  = e[0] + e[6] * outer; q[4]  = e[1] + TD5_TG_VERGE_LIFT - d_out;
        q[5]  = e[2] + e[7] * outer;
        q[6]  = e[3] + e[8] * outer; q[7]  = e[4] + TD5_TG_VERGE_LIFT - d_out;
        q[8]  = e[5] + e[9] * outer;
        q[9]  = e[3] + e[8] * inner; q[10] = e[4] + TD5_TG_VERGE_LIFT - d_in;
        q[11] = e[5] + e[9] * inner;
        t[0] = 0.0; t[1] = (double)h->si;
        t[2] = u_d; t[3] = (double)h->si;
        t[4] = u_d; t[5] = (double)h->si + 1.0;
        t[6] = 0.0; t[7] = (double)h->si + 1.0;
        tg_city_push_quad(px, py, pz, uu, vv, &n, q, t);
        if (*h->nmesh >= h->maxmesh) return 1;
        seg_nq = n / 4;
        tg_acct(TG_ACCT_PARK, h->si);
        h->moff[(*h->nmesh)++] = h->blk->len;
        if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n,
                                &seg_page, &seg_nq, 1))
            return 0;

        /* Boundary hedge at the back, on the sloped lawn -- same page and height
         * as the R3 park hedge, so a former building line reads as a garden
         * boundary rather than bare ground. */
        if (td5_env_flag_on("TD5RE_AUTOTRACK_PARK_HEDGE")) {
            const double hset  = sw + depth * 0.85;
            const double hdrop = TD5_TG_GROUND_DROP * hset / TD5_TG_GROUND_WIDTH;
            const double hy    = TD5_TG_VERGE_LIFT - hdrop;
            n = 0; seg_page = TD5_TG_PAGE_R3_BLOCK + 1;   /* park hedge */
            q[0]  = e[0] + e[6] * hset; q[1]  = e[1] + hy;
            q[2]  = e[2] + e[7] * hset;
            q[3]  = e[3] + e[8] * hset; q[4]  = e[4] + hy;
            q[5]  = e[5] + e[9] * hset;
            q[6]  = e[3] + e[8] * hset; q[7]  = e[4] + hy + TD5_TG_HEDGE_H;
            q[8]  = e[5] + e[9] * hset;
            q[9]  = e[0] + e[6] * hset; q[10] = e[1] + hy + TD5_TG_HEDGE_H;
            q[11] = e[2] + e[7] * hset;
            t[0] = 0.0; t[1] = 1.0; t[2] = 1.0; t[3] = 1.0;
            t[4] = 1.0; t[5] = 0.0; t[6] = 0.0; t[7] = 0.0;
            tg_city_push_quad(px, py, pz, uu, vv, &n, q, t);
            if (*h->nmesh >= h->maxmesh) return 1;
            seg_nq = n / 4;
            tg_acct(TG_ACCT_PARK, h->si);
            h->moff[(*h->nmesh)++] = h->blk->len;
            if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n,
                                    &seg_page, &seg_nq, 1))
                return 0;
        }

        /* A house on ~1 dressed span in 4, set back in the middle of the lawn --
         * the same beat and emitter the R3 park uses. */
        if (td5_env_flag_on("TD5RE_AUTOTRACK_PARK_HOUSES")) {
            const unsigned int hh = ((unsigned)h->si * 2654435761u
                                     + (unsigned)s * 40503u) * 0x9E3779B9u;
            if ((hh >> 30) == 0u)
                if (!tg_block_emit_house(h, e, sw + depth * 0.5)) return 0;
        }
    }
    return 1;
}

/* Group BLOCK dispatcher (feedback R3 items 3-6). Wired into the scenery loop
 * next to tg_emit_fb_city; keeps all BLOCK-area emitters out of another area's
 * dispatcher. */
int tg_emit_fb_block(const TG_FBHook *h)
{
    if (!tg_city_span_paved(h)) return 1;      /* only where the city is */
    if (!tg_block_emit_intersection(h)) return 0;
    if (!tg_block_emit_park(h)) return 0;
    if (!tg_r16_emit_outskirt_park(h)) return 0;   /* [R16 CITY item 2] */
    return 1;
}

/* [R5 CROSS item 3] "there's still elevated sidewalk on street crossings; those
 * should wrap around the road." R4's item 2 read "raised sidewalk break" as a
 * pedestrian THRESHOLD and stepped the built kerb UP TD5_TG_XKERB_RISE across
 * the crossing (tg_cross_emit_kerb_break). That is the elevated sidewalk the
 * user is now pointing at -- a lip that steps ALONG the kerb rather than a
 * pavement that turns the corner. The wrap the user wants already exists:
 * tg_block_emit_intersection stands a pavement + railing arm at each of the
 * gap's two along-road corners, turning off the main kerb to run down the side
 * street. So the raised break is removed outright (byte attribution: 108 meshes
 * / 23 KB) and the crossing kerb is left flush, with the corner arms doing the
 * wrap. No knob to restore it: a raised threshold at a crossing was the wrong
 * reading of the original request. */

/* item 10 -- buildings LINING a through side street, so it reads as going
 * further with more buildings on its sides. A first cut placed extra rows
 * straight out along the kerb normal (like the back rows, only deeper); a
 * single-variable A/B showed they emitted but were invisible from the racing
 * line -- too far out and occluded by the front frontage. Buildings that don't
 * show are not buildings the user asked for.
 *
 * These instead stand at the TWO along-road edges of the gap (e[0] near, e[3]
 * far) and run OUTWARD down the street, one wall each, facing in -- the left
 * and right frontages of the side street, seen straight down it through the
 * gap. They only ever extend PERPENDICULAR to the main road (down the side
 * street), so nothing here can reach the drivable corridor. The wall runs the
 * cross-street's own reach, a couple of storeys taller than the front row so
 * the canyon has depth.
 *
 * [R5 CROSS item 2] "building facades on every 'lane' of road spawned." A side
 * street is a GAP that is several spans wide, and the first cut emitted BOTH
 * along-road edges on EVERY span of that gap, so a four-span gap grew eight
 * walls -- a comb of parallel frontages marching down the street, one per span
 * boundary, exactly the "facade on every lane" report (byte attribution: 588
 * walls across 274 spans, >2 per gap-span, where a side street wants only its
 * two flanking frontages). The two frontages of a side street sit at the gap's
 * two along-MAIN-road extremes: the NEAR edge of the gap's first span and the
 * FAR edge of its last span. So each edge is now gated to its boundary corner
 * (si-1 built for the near frontage, si+1 built for the far), the same corner
 * test tg_block_emit_intersection uses -- two walls per gap side, no comb. */
/* ---- [R12 CITY item 10] ONE CROSS-STREET FRONTAGE WALL, decided in one place.
 *
 * Both halves of the item are this wall disagreeing with a NEIGHBOUR about the
 * same number, so both are decided here and the emitter and the read-only sweep
 * read the identical answer.
 *
 * (a) "one building has overlapped geometry" -- a pale slab passing through a
 *     brick block. The wall is pushed outward past the corner block's return so
 *     cap and frontage read as one building (R6 CROSS item 1), but the clearance
 *     it used was tg_facade_depth(b) -- the BIOME depth. Since R8 item 6 a run's
 *     return is tg_facade_run_depth: base + 0..2 EXTRA cells off a run hash, so
 *     two runs in three are deeper than the constant and the wall started
 *     1500-3000 raw INSIDE the block it was meant to stand behind. Clearance now
 *     comes from that run's own depth. Second hole in the same expression: where
 *     the street was too short to fit the clearance the code fell back to
 *     off = 0, i.e. it planted the wall flush at the kerb and drove it through
 *     the whole block rather than skipping it. A wall with no room is now not
 *     emitted -- these are decorative extra frontages, and a missing one costs
 *     nothing next to one growing out of a building.
 *
 * (b) "buildings at the end of a road, no crossing street and no sidewalk". The
 *     wall ran tg_city_crossst_reach -- the MODELLED street length -- while the
 *     carriageway (tg_city_emit_crossstreet) and the pavement arm
 *     (tg_block_emit_arm) both run tg_xstreet_reach_at, the R8 per-side clamp
 *     that stops a street before it re-enters the main road further along the
 *     ring. Where the clamp bites, the road and its pavement stop and the two
 *     flanking walls carry on to the modelled length -- buildings lining bare
 *     ground, facing onto nothing. The wall now takes the SAME clamped reach.
 *
 * Returns 0 when no wall stands on this edge. Knobs TD5RE_R12_CITY_XDEPTH (a)
 * and TD5RE_R12_CITY_XREACH (b), both default ON and independently A/B-able;
 * with both off this reproduces the R11 expression exactly. */
static int tg_r12_xwall_geom(const TG_NodeList *nl, int si, int s, int edge,
                             const TG_Biome *b, double sw,
                             double *off_out, double *flen_out,
                             double *blockd_out, double *reach_out)
{
    const int xwall  = td5_env_flag_on("TD5RE_AUTOTRACK_XWALL");
    const int xdepth = td5_env_flag_on("TD5RE_R12_CITY_XDEPTH");
    const int nbi    = edge ? (si + 1) : (si - 1);
    const double sg  = s ? 1.0 : -1.0;
    double reach = tg_city_crossst_reach(b, sw);
    double blockd, capd, off;
    int rows;

    if (td5_env_flag_on("TD5RE_R12_CITY_XREACH"))
        reach = tg_xstreet_reach_at(nl, si, sg, tg_block_arm_skew(si, s), b, sw);

    /* The wall's height is the neighbouring block's floor count (R6 CROSS item
     * 19) and so is its depth -- one run, one answer. blockd is reported as the
     * run's TRUE depth whatever the knob says, so the sweep can measure the
     * penetration of a wall built to the old biome clearance. */
    rows   = tg_facade_floors(nbi, s, b);
    if (rows <= 0) rows = b->floors_min + 1;
    blockd = tg_facade_run_depth(b, nbi, s, rows, NULL);
    capd   = tg_city_sidewalk_w(b)
           + (xdepth ? blockd : tg_facade_depth(b));

    if (!xwall) off = 0.0;
    else if (reach - capd < (double)b->cell_w * 0.5) {
        if (xdepth) return 0;              /* no room past the block: no wall */
        off = 0.0;
    } else off = capd;

    if (off_out)    *off_out    = off;
    if (flen_out)   *flen_out   = reach - off;
    if (blockd_out) *blockd_out = blockd;
    if (reach_out)  *reach_out  = reach;
    return (reach - off > 1.0);
}

static int tg_cross_emit_sidewalls(const TG_FBHook *h)
{
    double px[TD5_TG_FACADE_MAXQUAD * 4], py[TD5_TG_FACADE_MAXQUAD * 4];
    double pz[TD5_TG_FACADE_MAXQUAD * 4], uu[TD5_TG_FACADE_MAXQUAD * 4];
    double vv[TD5_TG_FACADE_MAXQUAD * 4];
    const TG_Biome *b = h->b;
    const double sw = tg_city_sidewalk_w(b);
    int s;

    if (!(sw > 0.0)) return 1;
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_CROSS_SIDEWALLS")) return 1;
    if (tg_span_in_bridge_run(h->si)) return 1;
    if (td5_env_flag_on("TD5RE_AUTOTRACK_XBRIDGE_GATE") &&   /* item 15 */
        tg_span_near_bridge(h->si, TD5_TG_XBRIDGE_CLEAR)) return 1;
    if (tg_branches_enabled() && tg_span_in_fork_clear(h->si)) return 1;

    for (s = 0; s < 2; s++) {
        const double sg = s ? 1.0 : -1.0;
        double e[10], ang, ox, oz;
        int edge, near_corner, far_corner;
        if (tg_facade_built(h->si, s)) continue;      /* only a through street */
        if (tg_block_is_park(h->si, s)) continue;     /* a park has no street */
        if (tg_side_blocked(h->si, sg)) continue;
        /* Emit a frontage only at the gap's own boundary spans, so one wide gap
         * grows exactly its two flanking frontages, not one per span. */
        /* [R11 CITY item 10] the corner block must actually STAND: this wall
         * takes its height from that block (tg_facade_floors below), so keyed on
         * the raw pattern it becomes a free-standing facade with nothing behind
         * it wherever the block was suppressed. */
        near_corner = tg_r11_corner_stands(h->si - 1, s);  /* gap starts here */
        far_corner  = tg_r11_corner_stands(h->si + 1, s);  /* gap ends here   */
        if (!near_corner && !far_corner) continue;    /* gap interior: no wall */
        tg_city_edge_frame(h->nl, h->si, sg, e);
        ang = tg_block_arm_skew(h->si, s);
        tg_block_rot2(e[6], e[7], ang, &ox, &oz);     /* outward down the street */

        /* One wall on each along-road edge of the gap: edge 0 = e[0] (near),
         * edge 1 = e[3] (far). Each runs outward `reach`, rising H. The near
         * frontage belongs to the gap-start span, the far to the gap-end span. */
        for (edge = 0; edge < 2; edge++) {
            /* near frontage only at the gap start, far only at the gap end. */
            if (edge == 0 && !near_corner) continue;
            if (edge == 1 && !far_corner)  continue;
            const unsigned int rh = ((unsigned)h->si * 2654435761u
                                     + (unsigned)s * 40503u
                                     + (unsigned)edge * 21841u) * 2246822519u;
            /* [R6 CROSS item 1] The corner building's cap (tg_side_geom
             * cap_near/cap_far) already returns a wall tg_facade_depth(b) down
             * this side street, height-matched to the block. The old frontage
             * started at the road edge and ran the full reach, standing ON that
             * cap for its first stretch -- the "double geometry on the buildings
             * on the side". Start it where the cap ends and line only the street
             * BEYOND the corner block, so cap + frontage read as one building. */
            /* TD5RE_AUTOTRACK_XWALL=0 reverts both halves (base offset + block
             * height) to the pre-R6 frontage for a single-variable A/B. */
            const int xwall = td5_env_flag_on("TD5RE_AUTOTRACK_XWALL");
            /* [R12 CITY item 10] where the wall starts, how long it runs and the
             * reach it lines -- all three from the shared decision above, so the
             * read-only sweep measures the wall that is actually emitted. */
            double off, flen;
            if (!tg_r12_xwall_geom(h->nl, h->si, s, edge, b, sw,
                                   &off, &flen, NULL, NULL))
                continue;
            /* [R10 CROSS 66] SIDEWALK SETBACK ON THE SIDE STREET. The frontage
             * wall stood at the along-road corner e[0]/e[3] -- which is exactly
             * the near/far edge of the cross-street carriageway
             * (tg_city_emit_crossstreet spans e[0]..e[3] and runs outward). So
             * from inside the mouth the wall rose FLUSH with the tarmac while the
             * arm pavement (tg_block_emit_arm, width sw, laid on the -/+ along
             * side of the same corner) lay hidden BEHIND the wall. Looking down
             * the side street that reads as "buildings flush to the road, no
             * visible sidewalk" (seed 99991 span 66, the user's item). Push the
             * wall OUTWARD ALONG THE ROAD by the sidewalk width -- the same sw the
             * arm slab occupies -- so the lateral order from the carriageway is
             * kerb -> pavement(sw) -> wall, and the arm sidewalk now shows in
             * front of the frontage. The offset is purely along-road and strictly
             * OUTSIDE the carriageway span (edge 0 moves by -along, edge 1 by
             * +along), so it cannot intrude on either carriageway. Default ON;
             * TD5RE_R10_XSIDEWALK=0 restores the flush wall for a single-variable
             * A/B. */
            double axr = e[3] - e[0], azr = e[5] - e[2];
            const double alr = sqrt(axr * axr + azr * azr);
            if (alr > 1e-6) { axr /= alr; azr /= alr; } else { axr = 0.0; azr = 0.0; }
            const double xset = td5_env_flag_on("TD5RE_R10_XSIDEWALK") ? sw : 0.0;
            const double xdir = edge ? 1.0 : -1.0;   /* outside the mouth */
            const double cx = (edge ? e[3] : e[0]) + ox * off + axr * xdir * xset;
            const double cz = (edge ? e[5] : e[2]) + oz * off + azr * xdir * xset;
            const double cdrop = TD5_TG_GROUND_DROP * off / TD5_TG_GROUND_WIDTH;
            const double by = (edge ? e[4] : e[1]) + TD5_TG_KERB_H - cdrop;
            const double drop = TD5_TG_GROUND_DROP * flen / TD5_TG_GROUND_WIDTH;
            /* [R6 CROSS item 19] Height comes from the adjacent BUILT block, not
             * an independent roll, so a crossing-street frontage agrees with the
             * block it fronts (was b->floors_min+1+rand, which towered over the
             * 1-floor INDUSTRIAL sheds -- "make crossing-street buildings the same
             * height"). rh still keys the page class only. */
            const int nbi = edge ? (h->si + 1) : (h->si - 1);
            int rows = xwall ? tg_facade_floors(nbi, s, b)
                             : (int)(b->floors_min + 1 + (rh >> 9) % 4u);
            int cols, page, seg_nq, n = 0;
            double H;
            if (rows <= 0) rows = b->floors_min + 1;
            H = (double)rows * tg_facade_floor_h(b);
            cols = tg_facade_cols_for(flen, (double)b->cell_w, 5);
            /* base -> outward*flen, sinking with the skirt, rising H. */
            tg_facade_push_grid(cx, by, cz, ox * flen, -drop, oz * flen,
                                0.0, H, 0.0, cols, rows, 0, rows,
                                px, py, pz, uu, vv, &n);
            if (n <= 0) continue;
            if (*h->nmesh >= h->maxmesh) return 1;
            page   = tg_facade_page_class(rh, rows);
            seg_nq = n / 4;
            h->moff[(*h->nmesh)++] = h->blk->len;
            if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n,
                                    &page, &seg_nq, 1))
                return 0;
            tg_acct_n(TG_ACCT_CROSSFURN, h->si, 1);
            if (xset > 0.0) tg_acct(TG_ACCT_R10_CROSS, h->si);  /* setback fired */
        }
    }
    return 1;
}

/* item 14 -- is span si a short built SLIVER between two crossings on a curve?
 * A run of side streets separated by a one- or two-span wall on a bend reads as
 * a stutter of separate crosswalks; joining them gives one continuous junction.
 * True when this side is BUILT here but has an open (non-park) side street both
 * within TD5_TG_XJOIN_WIN before AND after, and the centreline actually turns
 * across the sliver (a straight run of side streets is left as separate ones). */
static int tg_cross_join_side(const TG_NodeList *nl, int si, int s)
{
    int k, gb = -1, ga = -1;
    double cross;

    if (!tg_facade_built(si, s)) return 0;            /* must be the wall sliver */
    /* Nearest through-street mouth on this side within the window, each way. */
    for (k = 1; k <= TD5_TG_XJOIN_WIN; k++)
        if (si - k >= 0 && !tg_facade_built(si - k, s)
            && !tg_block_is_park(si - k, s)) { gb = si - k; break; }
    for (k = 1; k <= TD5_TG_XJOIN_WIN; k++)
        if (si + k < nl->count && !tg_facade_built(si + k, s)
            && !tg_block_is_park(si + k, s)) { ga = si + k; break; }
    if (gb < 0 || ga < 0) return 0;
    /* Turn measured across the two flanking mouths (a wider, more reliable
     * baseline than the immediate neighbours), so only a genuine bend joins. */
    cross = nl->v[gb].tx * nl->v[ga].tz - nl->v[gb].tz * nl->v[ga].tx;
    return (cross < 0.0 ? -cross : cross) >= TD5_TG_XJOIN_CURVE;
}

/* item 14 -- paint the zebra across a joining sliver span, so two nearby
 * crossings on a curve read as one. Road-surface decal only (PAGE_CROSSING,
 * lifted like tg_city_emit_crossing), no wall or corridor change. */
static int tg_cross_emit_join_zebra(const TG_FBHook *h)
{
    double px[4], py[4], pz[4], uu[4], vv[4];
    double l0x, l0y, l0z, r0x, r0y, r0z, l1x, l1y, l1z, r1x, r1y, r1z;
    const double f0 = 0.22, f1 = 0.62, L = (double)h->lanes;
    int seg_page = TD5_TG_PAGE_CROSSING, seg_nq = 1, s, joined = 0, n = 0;

    /* [R7 CROSS item 14] DEFAULT OFF (was default on). The sliver-fill was the
     * R4/R5 answer to "crossings on a curve" -- merge two near ones into one
     * continuous junction. On the tight bend past fork 2 (seed 99991 span 635)
     * it merged a whole RUN of bunched mouths into one radial fan of zebra, the
     * exact "many crossings all together" the user now reports. Min-spacing
     * thinning (tg_city_crossing_here) is the R7 replacement, so the join is
     * retired; TD5RE_AUTOTRACK_CROSS_JOIN=1 restores the legacy behaviour. */
    if (!td5_env_flag_off("TD5RE_AUTOTRACK_CROSS_JOIN")) return 1;
    if (tg_branches_enabled() && tg_span_in_fork_clear(h->si)) return 1;
    if (!tg_span_surface_is_tarmac(h->si)) return 1;   /* [R5 CROSS item 12] */
    for (s = 0; s < 2; s++)
        if (tg_cross_join_side(h->nl, h->si, s)) joined = 1;
    if (!joined) return 1;

    tg_road_edge(h->nl, h->si, f0, 0.0, 1.0, &l0x, &l0y, &l0z, &r0x, &r0y, &r0z);
    tg_road_edge(h->nl, h->si, f1, 0.0, 1.0, &l1x, &l1y, &l1z, &r1x, &r1y, &r1z);
    px[n]=r0x; py[n]=r0y+TD5_TG_CROSS_LIFT; pz[n]=r0z; uu[n]=0.0; vv[n]=0.0; n++;
    px[n]=l0x; py[n]=l0y+TD5_TG_CROSS_LIFT; pz[n]=l0z; uu[n]=L;   vv[n]=0.0; n++;
    px[n]=l1x; py[n]=l1y+TD5_TG_CROSS_LIFT; pz[n]=l1z; uu[n]=L;   vv[n]=1.0; n++;
    px[n]=r1x; py[n]=r1y+TD5_TG_CROSS_LIFT; pz[n]=r1z; uu[n]=0.0; vv[n]=1.0; n++;

    if (*h->nmesh >= h->maxmesh) return 1;
    tg_acct_n(TG_ACCT_CROSSFURN, h->si, 1);
    h->moff[(*h->nmesh)++] = h->blk->len;
    {   /* [R8 GUARD] road-surface paint: DECAL, same rule as the crossing zebra. */
        const size_t d0 = h->blk->len;
        int r = tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n,
                                   &seg_page, &seg_nq, 1);
        tg_guard_mark(d0, h->blk->len, TG_GK_DECAL, h->si);
        return r;
    }
}

static int tg_cross_emit_street_flank(const TG_FBHook *h)
{
    const TG_Biome *b = h->b;
    const double sw = tg_city_sidewalk_w(b);
    double e[10];
    int s;

    if (!(sw > 0.0)) return 1;
    if (!td5_env_flag_on("TD5RE_R8_CROSS_REACH")) return 1;   /* model pair */
    if (!td5_env_flag_on("TD5RE_R8_CROSS_FLANK")) return 1;
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_INTERSECTIONS")) return 1;
    if (tg_span_in_bridge_run(h->si)) return 1;
    if (td5_env_flag_on("TD5RE_AUTOTRACK_XBRIDGE_GATE") &&
        tg_span_near_bridge(h->si, TD5_TG_XBRIDGE_CLEAR)) return 1;
    if (h->si + 1 >= h->nl->count) return 1;

    for (s = 0; s < 2; s++) {
        const double sg = s ? 1.0 : -1.0;
        const double depth = tg_facade_depth(b);
        const double d0 = sw + depth + TD5_TG_BACKROW_GAP * 0.5;
        double ang, ax, az, alen, reach;
        int near_corner, far_corner, c;

        if (tg_facade_built(h->si, s)) continue;      /* built = no street */
        if (tg_block_is_park(h->si, s)) continue;
        /* [R9 CITY item 4] measured, not blanket -- see tg_side_corridor_here */
        if (tg_side_corridor_here(h->nl, h->si, sg)) continue;
        near_corner = tg_facade_built(h->si - 1, s);
        far_corner  = tg_facade_built(h->si + 1, s);
        if (!near_corner && !far_corner) continue;    /* gap interior */

        tg_city_edge_frame(h->nl, h->si, sg, e);
        ang = tg_block_arm_skew(h->si, s);
        reach = tg_xstreet_reach_at(h->nl, h->si, sg, ang, b, sw);
        /* [R17 CITY item 2] "these buildings are floating." A flank block's base
         * tracks tg_xstreet_drop, which stays FLAT past the 12000 verge skirt --
         * but the far band there sinks toward the track floor, so a block placed
         * beyond the skirt hangs in the air (measured mechanism: tg_topo_drop_at's
         * own note, "planted past the skirt stands at the LIP height while the
         * ground under it has descended"). Only a fork backdrop lays a plaza floor
         * out there; an ordinary side street has none, so keep the block row on
         * ground that supports it. TD5RE_R17_FLANK_GROUND_CAP=0 restores the full
         * reach for an A/B. */
        if (td5_env_flag_on("TD5RE_R17_FLANK_GROUND_CAP")) {
            const double gcap = tg_verge_reach();
            if (reach > gcap) reach = gcap;
        }
        ax = e[3] - e[0]; az = e[5] - e[2];
        alen = sqrt(ax * ax + az * az);
        if (alen < 1e-6) continue;
        ax /= alen; az /= alen;

        for (c = 0; c < 2; c++) {
            double cx, cy, cz, ox, oz, bx, bz, d;
            int k;

            if (c == 0) {
                if (!near_corner) continue;
                cx = e[0]; cy = e[1]; cz = e[2];
                tg_block_rot2(e[6], e[7], ang, &ox, &oz);
                bx = -ax; bz = -az;          /* back into the block before it */
            } else {
                if (!far_corner) continue;
                cx = e[3]; cy = e[4]; cz = e[5];
                tg_block_rot2(e[8], e[9], ang, &ox, &oz);
                bx = ax; bz = az;            /* back into the block after it  */
            }

            for (k = 0, d = d0;
                 d + TD5_TG_R8_FLANK_LEN <= reach;
                 k++, d += TD5_TG_R8_FLANK_LEN + TD5_TG_R8_FLANK_ALLEY) {
                const unsigned int rh = ((unsigned)h->si * 2654435761u
                                       + (unsigned)s * 40503u
                                       + (unsigned)c * 2246822519u
                                       + (unsigned)k * 668265263u);
                const double set = sw + TD5_TG_R8_FLANK_SET;
                double y0, H, bxp, byp, bzp;
                int rows, cols, page;

                if ((rh >> 30) == 0u) continue;      /* ~25% left as a yard */
                rows = b->floors_min + (int)((rh >> 9) % 4u);
                H    = (double)rows * tg_facade_floor_h(b);
                y0   = cy + TD5_TG_KERB_H - tg_xstreet_drop(d);
                bxp  = cx + ox * d + bx * set;
                byp  = y0;
                bzp  = cz + oz * d + bz * set;
                cols = tg_facade_cols_for(TD5_TG_R8_FLANK_LEN,
                                          (double)b->cell_w, 4);
                page = tg_facade_page_class(rh, rows);
                if (!tg_bg_building_box(h->blk, h->moff, h->nmesh, h->maxmesh,
                                        bxp, byp, bzp,
                                        ox * TD5_TG_R8_FLANK_LEN, 0.0,
                                        oz * TD5_TG_R8_FLANK_LEN,
                                        bx, bz, bx, bz,
                                        depth, H, cols, rows, page, 1, h->si))
                    return 0;
                tg_acct(TG_ACCT_R8_CROSS, h->si);
            }
        }
    }
    return 1;
}

/* Defined with the read-only report below. Forward-declared rather than moved,
 * so the measurement and the fill cannot drift into two different rules. */
static int tg_r13_rear_exposed(const TG_NodeList *nl, int si, int left,
                               const TG_Biome *b, double range, double *far_out);

/* Spans from si to the nearest span on side `s` that carries a standing corner,
 * walking by `step`. TD5_TG_R13_SCAN if none is found inside the window -- an
 * opening that long is the end of the city, and its interior is as much a gap
 * interior as one bounded by two blocks. */
static int tg_r13_gap_run(int si, int s, int step, int nspans)
{
    int k;
    for (k = 1; k <= TD5_TG_R13_SCAN; k++) {
        const int j = si + step * k;
        if (j < 1 || j >= nspans - 1) return TD5_TG_R13_SCAN;
        if (tg_r11_corner_stands(j, s)) return k;
    }
    return TD5_TG_R13_SCAN;
}

/* Is (si,s) the INTERIOR of a frontage gap -- a side that is open, is not a
 * park or a branch corridor, and is more than TD5_TG_R13_GAP_KEEP spans from the
 * nearest standing corner in BOTH directions? Stated once so the emitter below
 * and the read-only report both answer it from the same rule. */
static int tg_r13_gap_interior(int si, int s, int nspans)
{
    if (tg_facade_built(si, s)) return 0;
    if (tg_block_is_park(si, s)) return 0;
    if (tg_side_blocked(si, s ? 1.0 : -1.0)) return 0;
    if (tg_r13_gap_run(si, s, -1, nspans) <= TD5_TG_R13_GAP_KEEP) return 0;
    if (tg_r13_gap_run(si, s, +1, nspans) <= TD5_TG_R13_GAP_KEEP) return 0;
    return 1;
}

/* [R14 OVERPASS item 3] CASE C -- THE CROSSING SURROUND.
 *
 * The deck's own three spans stay clear (tg_up_clear_span, R11 item 7c: a facade
 * there intersects the deck) and this deliberately does not touch them. What it
 * fills is the ring JUST OUTSIDE that corridor, out to the same band the R12
 * no-intersection rule uses, so a crossing stands in a built-up place instead of
 * at the near end of a hole.
 *
 * It is a RELAXATION of case A, not a new emitter: the same block, the same
 * setback, the same page and height rules. The only thing it drops is case A's
 * corner-distance test (tg_r13_gap_interior's TD5_TG_R13_GAP_KEEP), which is
 * what refused the reported zone -- measured at seed 20260901 crossing 530, the
 * left kerb reads B B . . p x x x B B B B B over spans 524-536, so the open run
 * at 526-527 has a standing corner exactly two spans behind it and case A leaves
 * it as street. Two spans of street is right in the middle of a town and wrong
 * at the mouth of an overpass, which is the whole of this case.
 *
 * Every other refusal case A makes is kept verbatim -- a park, a fork-cleared
 * side (tg_side_blocked, which is why the reported crossing's right kerb inside
 * fork 2 gets nothing and should), and a side whose frontage already stands.
 *
 * TWO DELIBERATE DEPARTURES, both measured rather than guessed. The first pass
 * of this case fired on 2 sides across the whole track and on ZERO sides at the
 * reported crossing, and the per-side trace says why:
 *
 *   surround @530 span 527 L near=1 built=0 park=0 blocked=0 yard=1 -> 0
 *   surround @530 span 528 L near=1 built=1 park=0 blocked=0 yard=0 -> 0
 *
 *   1. WHAT STANDS, NOT WHAT THE PATTERN CLAIMS. Case A asks tg_facade_built,
 *      the raw run/gap pattern, because a "gap" is a pattern gap by definition.
 *      Span 528 left is pattern-built and nothing stands on it (the town map
 *      reads B B . . p x x x B B B B B over 524-536), so the pattern test
 *      refuses to fill a side that is visibly empty. tg_r11_corner_stands is the
 *      predicate for "a block is actually emitted here", and it is the right one
 *      for a case whose whole subject is a visible hole. Every reason a side can
 *      be pattern-built yet empty is a reason to fill BEHIND it, and the fork is
 *      not one of them -- tg_side_blocked already refused that two lines up.
 *   2. NO YARD ROLL. tg_r13_fill_here leaves ~12% of its blocks out as a yard,
 *      which is variety in the middle of a town and a dice throw at the mouth of
 *      a crossing: it is what left span 527 empty above. "ALWAYS fill the nearby
 *      area" is the item, so case 3 is exempted from the roll at the call site.
 */
int tg_r14_up_surround(int si, int s)
{
    if (!td5_env_flag_on("TD5RE_R14_UP_FILL")) return 0;
    if (tg_r14_up_dist(si, TD5_TG_UP_XCLEAR) < 0) return 0;
    if (tg_r11_corner_stands(si, s)) return 0;  /* a block already stands    */
    if (tg_block_is_park(si, s)) return 0;
    if (tg_side_blocked(si, s ? 1.0 : -1.0)) return 0;   /* branch corridor  */
    return 1;
}

/* THE WHOLE PER-SIDE DECISION, in one place. The emitter and the read-only
 * report BOTH call this, so "the fill landed at span N" is something the report
 * can state rather than something a reader has to re-derive from a run summary
 * and a hash. Returns 0 = nothing here, 1 = CASE A (gap interior), 2 = CASE B
 * (behind an exposed frontage run). `g` is filled for case B, `rh` is the
 * per-side hash the caller then reuses for height, page and setback jitter. */
static int tg_r13_fill_here(const TG_NodeList *nl, int si, int nspans, int s,
                            const TG_Biome *b, TG_SideGeom *g, unsigned int *rh)
{
    int kind;

    *rh = ((unsigned)si * 2654435761u
         + (unsigned)s * 374761393u) * 2246822519u;

    if (!td5_env_flag_on("TD5RE_R13_FILL")) return 0;
    if (!(tg_city_sidewalk_w(b) > 0.0)) return 0;
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_INTERSECTIONS")) return 0;
    if (tg_span_in_bridge_run(si)) return 0;
    if (tg_up_clear_span(si)) return 0;
    if (si + 1 >= nl->count) return 0;

    /* CASE A -- GAP INTERIOR. The two spans either side of a corner are the
     * street mouth the crossing furniture owns; filling them would stand a
     * block on the junction the R11/R12 corner work just cleared. */
    if (tg_r13_gap_interior(si, s, nspans)) {
        kind = 1;
    } else if (tg_r14_up_surround(si, s)) {
        kind = 3;                  /* [R14 item 3] overpass surround */
    } else {
        /* CASE B -- BEHIND AN EXPOSED FRONTAGE RUN. Measured on seed 20260901:
         * from the reported span 1677 the backs on show are spans 1709-1712
         * side R, and those sides are BUILT, not gaps. Behind a built run there
         * is nothing at all, because tg_city_emit_backrows gates itself off
         * exactly there ("a solid frontage hides whatever is behind it") --
         * true on a straight, false the moment the road turns away and the run
         * is seen broadside across the chord. So the same block this emitter
         * stands in a gap is stood behind a built run TOO, but only on the
         * sides a driver can actually see the back of: tg_r13_rear_exposed is
         * the report's own predicate, so the "exposed=" count it prints is the
         * population offered here.
         *
         * TD5RE_R13_FILL_REAR=0 keeps case A and drops case B, which is the A/B
         * that separates the two run counts under one accounting slot. */
        if (!td5_env_flag_on("TD5RE_R13_FILL_REAR")) return 0;
        tg_side_geom(nl, si, s, b, g);
        if (!g->built) return 0;
        if (tg_r13_rear_exposed(nl, si, s, b,
                                TD5_TG_R13_EXPO_RANGE, NULL) <= 0) return 0;
        kind = 2;
    }
    /* [R14 item 3] The yard roll is variety inside a town and a dice throw at a
     * crossing mouth -- measured, it is what left span 527 left empty beside the
     * reported deck. Case 3 is "ALWAYS fill", so it is exempt. */
    if (kind != 3 && (*rh >> 29) == 0u) return 0;  /* ~12% left as a yard    */
    return kind;
}

/* [R15 CITY item 7] Gap-interior blocks refused because the gap is a street. */
static long s_r15_fill_street;

static int tg_r13_emit_gap_infill(const TG_FBHook *h)
{
    const TG_Biome *b = h->b;
    const double sw = tg_city_sidewalk_w(b);
    const TG_Node *n0, *n1;
    int s;

    if (h->si + 1 >= h->nl->count) return 1;
    n0 = &h->nl->v[h->si];
    n1 = &h->nl->v[h->si + 1];

    for (s = 0; s < 2; s++) {
        const double sg = s ? 1.0 : -1.0;
        const double lx0 = n0->tz * sg, lz0 = -n0->tx * sg;
        const double lx1 = n1->tz * sg, lz1 = -n1->tx * sg;
        double set, H, ax, ay, az, bx, by, bz, flen, depth;
        int rows, cols, page, kind;
        unsigned int rh;
        TG_SideGeom g;

        kind = tg_r13_fill_here(h->nl, h->si, h->nspans, s, b, &g, &rh);
        if (!kind) continue;

        /* Behind a built run the setback must clear THAT run's own depth, which
         * varies per run (dcols * cell_w, 3000-6000 on this seed) and is not the
         * biome nominal -- using the nominal would bury the block inside a deep
         * frontage instead of standing it behind one. */
        depth = (kind == 2) ? g.depth : tg_facade_depth(b);
        set   = sw + depth + TD5_TG_R13_SET + (double)(rh % 900u);
        /* [R15 CITY item 7] "this building is on top of a street."
         *
         * MEASURED, and this emitter -- not the back rows -- is the one. The
         * picker's mesh (entry 34, page 383, 40 verts, kind "city") projects
         * onto the centreline at span ~136 RIGHT, lateral ~14800; back rows are
         * skipped there (TD5RE_R15_OCC_DIAG=137 reports blocked=1 on that side)
         * and 40 verts is tg_bg_building_box, which has SIX call sites, not one.
         * The setback above lands squarely on it: sw(1200) + depth + R13_SET +
         * 0..900 puts the box around lateral 13000-14000, while the side street
         * at that span runs out to 22800. Note the `sw` here is the biome
         * NOMINAL (tg_city_sidewalk_w at the top of this function), not the
         * per-span/per-side width the pavement and the street actually use --
         * but even the right width would not have saved it, because nothing in
         * this emitter asks about the street at all.
         *
         * REFUSE rather than push out. This block exists to fill a gap that
         * would otherwise read as bare ground; a gap with a side street running
         * down it is not bare, and shoving the box out past 22800 would just
         * duplicate the reveal row that already terminates that vista. Same
         * answer, and the same reasoning, as tg_prop_one's refusal on
         * side-street tarmac: "there is no pavement on a side-street mouth to
         * stand on, so the honest answer is to place nothing". Reuses that very
         * predicate so the street this consults is the street that was laid. */
        if (td5_env_flag_on("TD5RE_R15_FILL_STREET") &&
            tg_xstreet_occupies(h->nl, h->si, sg, set)) {
            s_r15_fill_street++;
            continue;
        }
        rows  = b->floors_min + (int)((rh >> 9) % 4u);
        {   /* [R11 BIOME item 4] thin with the town, like every other mass. */
            const double tr = tg_town_ramp(h->si);
            if (tr < 1.0) {
                int capped = 1 + (int)((double)(rows - 1) * tr + 0.5);
                if (capped >= 1 && capped < rows) rows = capped;
            }
        }
        H  = (double)rows * tg_facade_floor_h(b);
        bx = n0->x + lx0 * (n0->width * 0.5 + set);
        by = n0->y - tg_xstreet_drop(set);
        bz = n0->z + lz0 * (n0->width * 0.5 + set);
        ax = (n1->x + lx1 * (n1->width * 0.5 + set)) - bx;
        ay = (n1->y - tg_xstreet_drop(set)) - by;
        az = (n1->z + lz1 * (n1->width * 0.5 + set)) - bz;
        flen = sqrt(ax * ax + az * az);
        cols = tg_facade_cols_for(flen, (double)b->cell_w, 4);
        page = tg_facade_page_class(rh, rows);
        if (!tg_bg_building_box(h->blk, h->moff, h->nmesh, h->maxmesh,
                                bx, by, bz, ax, ay, az, lx0, lz0, lx1, lz1,
                                depth, H, cols, rows, page, 1, h->si))
            return 0;
        /* Accounted to the case that produced it, so the inventory separates
         * R13's two cases from R14's surround under one emitter. */
        tg_acct(kind == 3 ? TG_ACCT_R14_UP : TG_ACCT_R13_FILL, h->si);
    }
    return 1;
}

/* [R8 CROSS] CLASS-LEVEL REPORT. "in this and all crossings" makes the CLASS the
 * acceptance test, and a frame proves one mouth at one span. Every quantity this
 * round changes is a deterministic function of the span index, so the whole class
 * can be enumerated instead of photographed: one line per side-street MOUTH with
 * its reach, whether its flanking pavement arms are laid, whether it sits inside
 * a fork corridor, whether it is a turn continuation, how many flanking blocks
 * line it, and how far back the reveal row now stands. A sweep is then a log
 * parse over both seeds rather than a pile of screenshots.
 *
 * Read-only: it emits nothing and calls only predicates. TD5RE_R8_CROSS_REPORT=1
 * to enable (off by default -- it is one line per mouth). */
void tg_r8_cross_report(const TG_NodeList *nl, int nspans)
{
    int si, s;
    int mouths = 0, armed = 0, forkm = 0, fork_armed = 0, turns = 0, flanks = 0;
    double rmin = 1e30, rmax = -1e30, rsum = 0.0;

    if (!td5_env_int("TD5RE_R8_CROSS_REPORT", 0, 0, 1)) return;
    if (nspans > nl->count - 1) nspans = nl->count - 1;

    TD5_LOG_I(LOG_TAG, "trackgen: ---- r8cross mouth sweep ----");
    for (si = 1; si < nspans; si++) {
        const TG_Biome *b = &k_biomes[tg_scenery_biome_index(si)];
        const double sw = tg_city_sidewalk_w(b);
        if (!(sw > 0.0)) continue;              /* no facades, no side streets */
        if (tg_span_in_bridge_run(si)) continue;
        for (s = 0; s < 2; s++) {
            const double sg = s ? 1.0 : -1.0;
            double reach;
            int fork, arms, turn, nflank, corner;
            if (tg_facade_built(si, s)) continue;        /* frontage, not a gap */
            if (tg_block_is_park(si, s)) continue;
            if (tg_side_blocked(si, sg)) continue;
            if (td5_env_flag_on("TD5RE_AUTOTRACK_XBRIDGE_GATE") &&
                tg_span_near_bridge(si, TD5_TG_XBRIDGE_CLEAR)) continue;
            corner = tg_facade_built(si - 1, s) || tg_facade_built(si + 1, s);
            if (!corner) continue;              /* gap interior: not a mouth */

            reach = tg_xstreet_reach_at(nl, si, sg,
                                        tg_block_arm_skew(si, s), b, sw);
            fork  = tg_branches_enabled() && tg_span_in_fork_clear(si);
            turn  = tg_turn_open(si, s);
            /* Arms are laid unless the (now per-side) fork gate stops them. */
            arms  = !(fork && !td5_env_flag_on("TD5RE_R8_CROSS_FORKARM"));
            {   /* Flanking blocks that fit between the first station and the
                 * reach, per corner -- the same walk tg_cross_emit_street_flank
                 * makes, so the report cannot drift from the emitter. */
                const double d0 = sw + tg_facade_depth(b)
                                + TD5_TG_BACKROW_GAP * 0.5;
                double d;
                nflank = 0;
                for (d = d0; d + TD5_TG_R8_FLANK_LEN <= reach;
                     d += TD5_TG_R8_FLANK_LEN + TD5_TG_R8_FLANK_ALLEY)
                    nflank++;
                if (tg_facade_built(si - 1, s) && tg_facade_built(si + 1, s))
                    nflank *= 2;                /* both corners build a flank */
            }
            mouths++;
            if (arms) armed++;
            if (fork) { forkm++; if (arms) fork_armed++; }
            if (turn) turns++;
            flanks += nflank;
            rsum += reach;
            if (reach < rmin) rmin = reach;
            if (reach > rmax) rmax = reach;
            TD5_LOG_I(LOG_TAG,
                "trackgen:   r8cross mouth si=%d side=%s reach=%.0f arms=%d "
                "fork=%d turn=%d flank=%d row=%.0f",
                si, s ? "L" : "R", reach, arms, fork, turn, nflank,
                td5_env_flag_on("TD5RE_R8_CROSS_REACH")
                    ? reach + TD5_TG_BACKROW_GAP
                    : sw + tg_facade_depth(b) + TD5_TG_BACKROW_GAP);
        }
    }
    TD5_LOG_I(LOG_TAG,
        "trackgen: r8cross SUMMARY mouths=%d armed=%d fork_mouths=%d "
        "fork_armed=%d turns=%d flank_blocks=%d reach min/mean/max=%.0f/%.0f/%.0f",
        mouths, armed, forkm, fork_armed, turns, flanks,
        mouths ? rmin : 0.0, mouths ? rsum / (double)mouths : 0.0,
        mouths ? rmax : 0.0);
}

/* ============ [R10 CROSS] SIDE-STREET SIDEWALK-SETBACK SWEEP ==============
 * ROUND 10 item 1, the OPEN remainder after the R10 furniture guard: at a
 * side-street crossing the frontage buildings sit hard against the tarmac with
 * no sidewalk (seed 99991 span 66). R9 CITY's assembled massing sweep measures
 * every upright's |lateral| in the MAIN-ROAD frame and found nothing near the
 * main pavement line at 66 -- correct, because the offending walls line the
 * SIDE STREET and run OUTWARD from the main road, so their main-road |lateral|
 * is large. The defect lives on the side-street axis, which that sweep does not
 * measure; this one does.
 *
 * WHAT IS MEASURED. A side-street mouth's carriageway (tg_city_emit_crossstreet)
 * spans the gap's two along-road corners e[0]..e[3] and runs outward. The
 * frontage walls (tg_cross_emit_sidewalls) stand at those same corners. The
 * SETBACK is the along-road distance from the carriageway edge to the wall base;
 * the arm sidewalk (tg_block_emit_arm) occupies the first `sw` of it. A wall with
 * setback < sw stands ON the pavement / flush with the kerb -- the offender. So
 * the class count is: side-street frontage walls whose setback from the
 * carriageway edge is under the sidewalk width.
 *
 * COVERAGE. Unlike the R8 CROSS mouth sweep, this visits GAP-INTERIOR spans too
 * (span 66 is interior to the 65-68 gap): each interior span is attributed the
 * setback of the two corner walls that flank the view down its street, so the
 * span the user named appears in the windowed dump. The gates below mirror
 * tg_cross_emit_sidewalls exactly, so the report cannot drift from the emitter.
 *
 * Read-only. TD5RE_R10_CROSS_REPORT=1 enables it; TD5RE_R10_CROSS_SPAN=N (+/-
 * _PAD) dumps every span-side in a window even when it is not an offender. The
 * SUMMARY (the acceptance number) is always logged when the report is on. */
static double tg_r10_wall_setback(const TG_Biome *b)
{
    /* The along-road setback tg_cross_emit_sidewalls now gives the frontage wall:
     * the sidewalk width when the R10 fix is on, else flush at the kerb. Read the
     * SAME knob the emitter reads, so this reports the geometry actually built. */
    return td5_env_flag_on("TD5RE_R10_XSIDEWALK") ? tg_city_sidewalk_w(b) : 0.0;
}

static int tg_r10_cross_gates(const TG_NodeList *nl, int si, int s, double sg,
                              double sw)
{
    if (!(sw > 0.0)) return 0;
    if (tg_span_in_bridge_run(si)) return 0;
    if (td5_env_flag_on("TD5RE_AUTOTRACK_XBRIDGE_GATE") &&
        tg_span_near_bridge(si, TD5_TG_XBRIDGE_CLEAR)) return 0;
    if (tg_branches_enabled() && tg_span_in_fork_clear(si)) return 0;
    if (tg_facade_built(si, s)) return 0;          /* frontage, not a gap */
    if (tg_block_is_park(si, s)) return 0;
    if (tg_side_blocked(si, sg)) return 0;
    (void)nl;
    return 1;
}

void tg_r10_cross_report(const TG_NodeList *nl, int nspans)
{
    const int verbose = td5_env_int("TD5RE_R10_CROSS_REPORT", 0, 0, 1);
    const int wspan = td5_env_int("TD5RE_R10_CROSS_SPAN", -1, -1, 100000);
    const int wpad  = td5_env_int("TD5RE_R10_CROSS_PAD", 6, 0, 4000);
    int si, s, ring = (s_ring_len > 0) ? s_ring_len : nspans;
    int walls = 0, flush = 0, gapspans = 0;
    const double margin = 100.0;    /* tolerance: setback within this of sw is OK */

    if (!verbose && wspan < 0) return;
    if (ring > nspans) ring = nspans;
    if (ring > TD5_TG_MAX_SPANS) ring = TD5_TG_MAX_SPANS;

    TD5_LOG_I(LOG_TAG, "trackgen: ---- r10cross side-street setback sweep ----");
    for (si = 1; si < ring; si++) {
        const TG_Biome *b = &k_biomes[tg_scenery_biome_index(si)];
        const double sw = tg_city_sidewalk_w(b);
        for (s = 0; s < 2; s++) {
            const double sg = s ? 1.0 : -1.0;
            int near_c, far_c, interior, in_win;
            double set, reach;
            if (!tg_r10_cross_gates(nl, si, s, sg, sw)) continue;
            /* This side is an open gap. Corners emit a wall; interiors inherit. */
            near_c = tg_facade_built(si - 1, s);
            far_c  = tg_facade_built(si + 1, s);
            interior = (!near_c && !far_c);
            set = tg_r10_wall_setback(b);
            reach = tg_xstreet_reach_at(nl, si, sg, tg_block_arm_skew(si, s), b, sw);
            gapspans++;
            in_win = (wspan >= 0 && si >= wspan - wpad && si <= wspan + wpad);
            /* Corner walls: count each and flag it if it stands within the
             * sidewalk of the carriageway (setback under sw). */
            if (near_c || far_c) {
                int nw = (near_c ? 1 : 0) + (far_c ? 1 : 0);
                int nf = (set < sw - margin) ? nw : 0;
                walls += nw;
                flush += nf;
                if ((verbose && nf > 0 && flush <= 80) || in_win)
                    TD5_LOG_I(LOG_TAG,
                        "trackgen:   r10cross MOUTH si=%d side=%s reach=%.0f "
                        "sw=%.0f wall_setback=%.0f walls=%d flush=%d -> %s",
                        si, s ? "L" : "R", reach, sw, set, nw, nf,
                        nf ? "FLUSH (no sidewalk)" : "ok");
            } else if (in_win) {
                /* Interior span: attribute the flanking corner walls' setback. */
                TD5_LOG_I(LOG_TAG,
                    "trackgen:   r10cross INTERIOR si=%d side=%s reach=%.0f "
                    "sw=%.0f flanking_wall_setback=%.0f -> %s",
                    si, s ? "L" : "R", reach, sw, set,
                    (set < sw - margin) ? "FLUSH (no sidewalk)" : "ok");
            }
            (void)interior;
        }
    }
    TD5_LOG_I(LOG_TAG,
        "trackgen: r10cross SUMMARY gap_span_sides=%d frontage_walls=%d "
        "flush_no_sidewalk=%d", gapspans, walls, flush);
}

/* Rear-plane midpoint of the run at (si,left), and its outward unit. Returns 0
 * when no wall actually stands on that side. */
static int tg_r13_rear_plane(const TG_NodeList *nl, int si, int left,
                             const TG_Biome *b, double *px, double *pz,
                             double *ux, double *uz, double *rows_out)
{
    TG_SideGeom g;
    double ox, oz, len;
    if (si < 1 || si + 1 >= nl->count) return 0;
    tg_side_geom(nl, si, left, b, &g);
    if (!g.built) return 0;
    ox = (g.lx0 + g.lx1) * 0.5;
    oz = (g.lz0 + g.lz1) * 0.5;
    len = sqrt(ox * ox + oz * oz);
    if (!(len > 1e-9)) return 0;
    ox /= len; oz /= len;
    *px = g.bx + g.ax * 0.5 + ox * g.depth;
    *pz = g.bz + g.az * 0.5 + oz * g.depth;
    *ux = ox; *uz = oz;
    if (rows_out) *rows_out = (double)g.rows;
    return 1;
}

/* Distance at which the rear plane of the run at (si,left) is visible from a
 * camera standing on the centreline at span c, or 0.0 if it is not. Four
 * conditions, all of which the driver's eye imposes:
 *   OUTWARD  the camera is on the far side of the rear plane
 *   AHEAD    the rear is in front of a car driving the ring forwards
 *   CONE     it is inside the forward half-angle, not out of the side window
 *   RANGE    it is close enough to be drawn rather than fogged out */
/* [R14 GENPERF] THE PLANE IS A PARAMETER, NOT A RECOMPUTATION. It is a function
 * of (si,left,b) alone and the caller's camera sweep holds all three fixed, so
 * deriving it here cost 81 identical tg_r13_rear_plane calls per exposure test
 * -- and each one runs tg_side_geom, which is the facade pipeline (built/ramp/
 * isolated predicates, floors, per-run depth, carriageway clearance). Measured
 * at 44% of the whole generation. Hoisted into tg_r13_rear_exposed below; the
 * arithmetic that remains is what actually varies with the camera. */
static double tg_r13_rear_seen_from(const TG_NodeList *nl, int si, int c,
                                    double range, double p_x, double p_z,
                                    double o_x, double o_z)
{
    double vx, vz, d, fx, fz;
    const TG_Node *nc;
    if (c < 1 || c > nl->count - 2) return 0.0;
    if (c > si - TD5_TG_R13_EXPO_NEAR && c < si + TD5_TG_R13_EXPO_NEAR)
        return 0.0;
    nc = &nl->v[c];
    vx = nc->x - p_x; vz = nc->z - p_z;
    if (vx * o_x + vz * o_z <= 0.0) return 0.0;        /* camera is inside  */
    fx = p_x - nc->x; fz = p_z - nc->z;
    d = sqrt(fx * fx + fz * fz);
    if (!(d > 1.0) || d > range) return 0.0;
    if ((fx * nc->tx + fz * nc->tz) / d < TD5_TG_R13_EXPO_COS) return 0.0;
    return d;
}

/* From how many centreline spans is that rear plane visible, and from the
 * furthest such camera, how far away is it? */
static int tg_r13_rear_exposed(const TG_NodeList *nl, int si, int left,
                               const TG_Biome *b, double range, double *far_out)
{
    int c, lo, hi, n = 0;
    double fdist = 0.0;
    double p_x, p_z, o_x, o_z, rows;

    if (far_out) *far_out = 0.0;
    /* [R14 GENPERF] ONCE, not once per camera. No wall on that side means no
     * camera can see one, which is exactly the answer every iteration of the
     * old loop computed independently before returning 0.0. */
    if (!tg_r13_rear_plane(nl, si, left, b, &p_x, &p_z, &o_x, &o_z, &rows))
        return 0;
    lo = si - TD5_TG_R13_EXPO_BACK;   if (lo < 1) lo = 1;
    hi = si + TD5_TG_R13_EXPO_BACK;   if (hi > nl->count - 2) hi = nl->count - 2;
    for (c = lo; c <= hi; c++) {
        const double d = tg_r13_rear_seen_from(nl, si, c, range,
                                               p_x, p_z, o_x, o_z);
        if (!(d > 0.0)) continue;
        n++;
        if (d > fdist) fdist = d;
    }
    if (far_out) *far_out = fdist;
    return n;
}

void tg_r13_fill_report(const TG_NodeList *nl, int nspans)
{
    const int wspan = td5_env_int("TD5RE_R13_FILL_SPAN", -1, -1, 100000);
    const int wpad  = td5_env_int("TD5RE_R13_FILL_PAD", 12, 0, 4000);
    int si, s, ring = (s_ring_len > 0) ? s_ring_len : nspans;
    int fronts = 0, expo = 0, expo_noback = 0, expo_nofill = 0;
    int gaps = 0, interior = 0, fill_gap = 0, fill_rear = 0, fill_up = 0;
    double expo_far_max = 0.0;
    char win[900];
    int wpos = 0;
    win[0] = '\0';

    /* td5_env_flag_on returns 1 when the variable is UNSET, so an "opt-in"
     * report gated on it is in fact always on (which is why the older dumps in
     * this file fill race.log on every run and truncated this one). The opt-in
     * accessor is td5_env_flag_off. */
    if (!td5_env_flag_off("TD5RE_R13_FILL_REPORT")) return;
    if (ring > nspans) ring = nspans;
    if (ring > TD5_TG_MAX_SPANS) ring = TD5_TG_MAX_SPANS;

    TD5_LOG_I(LOG_TAG, "trackgen: ---- r13fill exposed-rear dump ----");
    for (si = 1; si < ring - 1; si++) {
        const TG_Biome *b = &k_biomes[tg_scenery_biome_index(si)];
        const int in_win = (wspan < 0)
                        || (si >= wspan - wpad && si <= wspan + wpad);
        if (!(tg_city_sidewalk_w(b) > 0.0)) continue;
        for (s = 0; s < 2; s++) {
            TG_SideGeom g, gf;
            double fdist = 0.0, dep;
            int nsee, has_back, backrow, kind;
            unsigned int rh;
            /* The EMITTER's own decision, not a restatement of it: same
             * function, same arguments. A "fill=" of 1 or 2 on a row is proof
             * the block stands at that span-side, which a run summary and a
             * hash cannot give a reader. */
            kind = tg_r13_fill_here(nl, si, nspans, s, b, &gf, &rh);
            if (kind == 1) fill_gap++;
            else if (kind == 2) fill_rear++;
            else if (kind == 3) fill_up++;   /* [R14 item 3] surround */
            tg_side_geom(nl, si, s, b, &g);
            if (!g.built) {
                /* GAP side: count it, and record which of its spans the R13
                 * infill claims, so "the fill landed in the user's zone" is a
                 * span list rather than an inference off the run summary. */
                if (!tg_facade_built(si, s)) {
                    gaps++;
                    if (tg_r13_gap_interior(si, s, ring)) interior++;
                }
                if (kind && in_win && wpos < (int)sizeof(win) - 16)
                    wpos += snprintf(win + wpos, sizeof(win) - (size_t)wpos,
                                     "%s%d%c%d", wpos ? "," : "",
                                     si, s ? 'L' : 'R', kind);
                continue;
            }
            fronts++;
            nsee = tg_r13_rear_exposed(nl, si, s, b,
                                       TD5_TG_R13_EXPO_RANGE, &fdist);
            has_back = (g.rows >= TD5_TG_FACADE_TALL_ROWS);
            /* What tg_city_emit_backrows would do on this side: it is GATED
             * off wherever the frontage stands, so a built run has nothing
             * behind it at all. */
            backrow = !(td5_env_flag_on("TD5RE_AUTOTRACK_BACKROW_STREETS")
                        && tg_facade_built(si, s));
            dep = g.depth;
            if (nsee > 0) {
                expo++;
                if (!has_back)  expo_noback++;
                if (!backrow)   expo_nofill++;
                if (fdist > expo_far_max) expo_far_max = fdist;
            }
            if (kind && wpos < (int)sizeof(win) - 16 && in_win)
                wpos += snprintf(win + wpos, sizeof(win) - (size_t)wpos,
                                 "%s%d%c%d", wpos ? "," : "",
                                 si, s ? 'L' : 'R', kind);
            if (in_win)
                TD5_LOG_I(LOG_TAG,
                    "trackgen: r13fill si=%-5d side=%c rows=%-2d depth=%-7.0f "
                    "back=%d backrow=%d seen_from=%-3d fdist=%.0f bend=%.3f "
                    "fill=%d",
                    si, s ? 'L' : 'R', g.rows, dep, has_back, backrow,
                    nsee, fdist, tg_turn_bend(si), kind);
        }
    }
    TD5_LOG_I(LOG_TAG,
        "trackgen: r13fill SUMMARY fronts=%d exposed=%d exposed_noback=%d "
        "exposed_nofill=%d far_max=%.0f", fronts, expo, expo_noback,
        expo_nofill, expo_far_max);
    TD5_LOG_I(LOG_TAG,
        "trackgen: r13fill GAPS gap_sides=%d gap_interior=%d fill_gap=%d "
        "fill_rear=%d fill_up=%d fill_total=%d", gaps, interior, fill_gap,
        fill_rear, fill_up, fill_gap + fill_rear + fill_up);
    TD5_LOG_I(LOG_TAG,
        "trackgen: r13fill FILLED window[%d+/-%d] (span/side/case): %s",
        wspan, wpad, win[0] ? win : "-");

    /* CAMERA-CENTRIC pass: stand at TD5RE_R13_FILL_CAM and list every rear the
     * driver can actually see from there. This is the screenshot's own viewing
     * condition, so it is the one that has to reproduce the complaint. */
    {
        const int cam = td5_env_int("TD5RE_R13_FILL_CAM", wspan, -1, 100000);
        int seen = 0;
        if (cam > 0 && cam < ring - 1) {
            for (si = 1; si < ring - 1; si++) {
                const TG_Biome *b = &k_biomes[tg_scenery_biome_index(si)];
                if (!(tg_city_sidewalk_w(b) > 0.0)) continue;
                for (s = 0; s < 2; s++) {
                    const TG_Node *nc = &nl->v[cam];
                    TG_SideGeom g;
                    double p_x, p_z, o_x, o_z, rows, fx, fz, d, cosf, rear;
                    if (!tg_r13_rear_plane(nl, si, s, b, &p_x, &p_z,
                                           &o_x, &o_z, &rows)) continue;
                    fx = p_x - nc->x; fz = p_z - nc->z;
                    d = sqrt(fx * fx + fz * fz);
                    if (!(d > 1.0) || d > TD5_TG_R13_CAM_RANGE) continue;
                    cosf = (fx * nc->tx + fz * nc->tz) / d;
                    if (cosf < TD5_TG_R13_CAM_COS) continue;
                    rear = (nc->x - p_x) * o_x + (nc->z - p_z) * o_z;
                    tg_side_geom(nl, si, s, b, &g);
                    seen++;
                    TD5_LOG_I(LOG_TAG,
                        "trackgen: r13fill CAM=%d sees si=%-5d side=%c rows=%-2d "
                        "d=%-7.0f cos=%.3f face=%s back=%d",
                        cam, si, s ? 'L' : 'R', g.rows, d, cosf,
                        rear > 0.0 ? "REAR" : "front",
                        g.rows >= TD5_TG_FACADE_TALL_ROWS);
                }
            }
            TD5_LOG_I(LOG_TAG,
                "trackgen: r13fill CAM=%d rears_visible=%d", cam, seen);
        }
    }
}

/* ============== [R11 CITY] FRONTAGE / JUNCTION DUMP (read-only) =============
 * Items 6 and 10 are both "what does the frontage do next to a junction", one at
 * an ordinary intersection and one on a curve, so both need the same per-span
 * facts: the run/gap pattern, whether a sharp-bend TURN CONTINUATION opened this
 * side, the frontage segment the wall is actually built on, its caps, and which
 * of the junction emitters claim the span. Nothing here decides anything -- it
 * only prints what the emitters read, so a conclusion can be measured instead of
 * inferred.
 *
 * TD5RE_R11_CITY_REPORT=1 turns it on; TD5RE_R11_CITY_SPAN=N (+/- _PAD, default
 * 10) windows the dump. Off by default, so a normal build logs nothing. */
void tg_r11_city_report(const TG_NodeList *nl, int nspans)
{
    const int wspan = td5_env_int("TD5RE_R11_CITY_SPAN", -1, -1, 100000);
    const int wpad  = td5_env_int("TD5RE_R11_CITY_PAD", 10, 0, 4000);
    int si, s, ring = (s_ring_len > 0) ? s_ring_len : nspans;
    int orphan = 0, phantom = 0, overhang = 0, corners = 0;
    int xwalls = 0, xdive = 0, xnostreet = 0;   /* [R12 CITY item 10] */

    if (!tg_report_wanted("TD5RE_R11_CITY_REPORT")) return;
    if (ring > nspans) ring = nspans;
    if (ring > TD5_TG_MAX_SPANS) ring = TD5_TG_MAX_SPANS;

    TD5_LOG_I(LOG_TAG, "trackgen: ---- r11city frontage/junction dump ----");
    for (si = 1; si < ring - 1; si++) {
        const TG_Biome *b = &k_biomes[tg_scenery_biome_index(si)];
        const double sw = tg_city_sidewalk_w(b);
        const int in_win = (wspan < 0)
                        || (si >= wspan - wpad && si <= wspan + wpad);
        /* tg_side_geom is only ever called on a facade biome (tg_emit_street_wall
         * is the tree-billboard branch's alternative), and tg_facade_floors
         * divides by that biome's floors_extra -- 0 outside the city table. */
        if (!(sw > 0.0)) continue;
        for (s = 0; s < 2; s++) {
            const double sg = s ? 1.0 : -1.0;
            TG_SideGeom g;
            double flen;
            tg_side_geom(nl, si, s, b, &g);
            flen = g.built ? sqrt(g.ax * g.ax + g.az * g.az) : 0.0;
            /* CLASS COUNTERS, whole ring, window-independent.
             *  orphan   -- the run/gap pattern says BUILT but no wall stands, so
             *              every junction emitter keyed on the pattern anchors
             *              itself to a building that is not there (item 10).
             *  phantom  -- an arm corner claimed against such a span.
             *  overhang -- a standing frontage whose end runs into the pavement
             *              arm of the junction next door (item 6). */
            if (tg_facade_built(si, s) && tg_facade_isolated(si, s)) orphan++;
            {
                const int arms = tg_r11_arm_side(nl, si, s);
                TG_SideGeom gn, gf;
                tg_side_geom(nl, si - 1, s, b, &gn);
                tg_side_geom(nl, si + 1, s, b, &gf);
                if ((arms & 1) && !gn.built) phantom++;
                if ((arms & 2) && !gf.built) phantom++;
                if (arms & 1) corners++;
                if (arms & 2) corners++;
                /* RESIDUAL overhang, measured on the built geometry rather than
                 * counted as an opportunity: the along-road distance from the
                 * frontage's end to the corner node it retreats from. The arm
                 * slab occupies sw of that, so anything materially short of sw
                 * is still standing on the pavement. */
                if ((arms & 1) && gn.built) {
                    const TG_Node *nd = &nl->v[si];
                    const double dx = nd->x - (gn.bx + gn.ax);
                    const double dz = nd->z - (gn.bz + gn.az);
                    if (dx * nd->tx + dz * nd->tz < sw - 100.0) overhang++;
                }
                if ((arms & 2) && gf.built) {
                    const TG_Node *nd = &nl->v[si + 1];
                    const double dx = gf.bx - nd->x, dz = gf.bz - nd->z;
                    if (dx * nd->tx + dz * nd->tz < sw - 100.0) overhang++;
                }
            }
            /* [R12 CITY item 10] CROSS-STREET FRONTAGE WALL vs its two
             * neighbours -- the corner block behind it and the street it lines.
             * Gates mirror tg_cross_emit_sidewalls exactly, and the geometry
             * comes from the emitter's own tg_r12_xwall_geom, so neither counter
             * can drift from what is built.
             *   xwall_dive     -- the wall starts INSIDE the corner block's own
             *                     return: two building masses interpenetrating.
             *   xwall_nostreet -- the wall outruns the CLAMPED street reach the
             *                     carriageway and the pavement arm both use, so
             *                     it lines bare ground past the road's end. */
            if (!tg_facade_built(si, s) && !tg_block_is_park(si, s) &&
                !tg_side_blocked(si, sg) && !tg_span_in_bridge_run(si) &&
                !(td5_env_flag_on("TD5RE_AUTOTRACK_XBRIDGE_GATE") &&
                  tg_span_near_bridge(si, TD5_TG_XBRIDGE_CLEAR)) &&
                !(tg_branches_enabled() && tg_span_in_fork_clear(si)) &&
                td5_env_flag_on("TD5RE_AUTOTRACK_CROSS_SIDEWALLS")) {
                int edge;
                for (edge = 0; edge < 2; edge++) {
                    double off, flen, blockd, reach, clamp, pen;
                    if (!tg_r11_corner_stands(edge ? si + 1 : si - 1, s))
                        continue;
                    if (!tg_r12_xwall_geom(nl, si, s, edge, b, sw,
                                           &off, &flen, &blockd, &reach))
                        continue;
                    xwalls++;
                    pen = (sw + blockd) - off;
                    if (pen > 100.0) xdive++;
                    clamp = tg_xstreet_reach_at(nl, si, sg,
                                tg_block_arm_skew(si, s), b, sw);
                    if (reach - clamp > TD5_TG_R8_CLAMP_STEP) xnostreet++;
                    if (in_win && (pen > 100.0 || reach - clamp > 1.0))
                        TD5_LOG_I(LOG_TAG,
                            "trackgen:   r12city XWALL si=%d side=%s edge=%d "
                            "off=%.0f flen=%.0f blockd=%.0f reach=%.0f "
                            "clamp=%.0f pen=%.0f", si, s ? "L" : "R", edge,
                            off, flen, blockd, reach, clamp, pen);
                }
            }
            if (!in_win) continue;
            TD5_LOG_I(LOG_TAG,
                "trackgen:   r11city si=%d side=%s sw=%.0f built=%d stands=%d "
                "turn=%d park=%d isol=%d nb=%d/%d | wall=%d rows=%d cols=%d "
                "flen=%.0f depth=%.0f capn=%d capf=%d | arm=%d xbase=%d "
                "xhere=%d skew=%.1f reach=%.0f",
                si, s ? "L" : "R", sw,
                tg_facade_built(si, s), tg_facade_stands(si),
                tg_turn_open(si, s), tg_block_is_park(si, s),
                tg_facade_isolated(si, s),
                tg_side_built(si - 1, s), tg_side_built(si + 1, s),
                g.built, g.built ? g.rows : 0, g.built ? g.cols : 0,
                flen, g.built ? g.depth : 0.0,
                g.built ? g.cap_near : 0, g.built ? g.cap_far : 0,
                tg_r11_arm_side(nl, si, s),
                tg_crossing_base(si), tg_city_crossing_here(si),
                tg_block_arm_skew(si, s) * 180.0 / TD5_TG_PI,
                tg_xstreet_reach_at(nl, si, sg, tg_block_arm_skew(si, s), b, sw));
        }
    }
    TD5_LOG_I(LOG_TAG,
        "trackgen: r11city SUMMARY arm_corners=%d orphan_frontage=%d "
        "phantom_corners=%d overhang_ends=%d | xwalls=%d xwall_dive=%d "
        "xwall_nostreet=%d",
        corners, orphan, phantom, overhang, xwalls, xdive, xnostreet);
}

/* Carriageway intrusion of world point (wx,wz), judged in the frame of the
 * NEAREST node over a +/-WIN window with the point's own +/-3 spans excluded.
 * Positive = the point stands on that span's tarmac by that many units.
 *
 * Nearest, not deepest: a first cut took the max intrusion over the window and
 * reported hits at nodes 50000 units away, because a far point that happens to
 * lie near some distant span's tangent LINE has a small lateral in that span's
 * frame. That is the same frame misattribution the R11 guard fix names, and it
 * is why TD5_TG_GUARD_NEAR_MAX exists. *pnear reports the distance so a hit the
 * guard would have refused to judge is still visible here as a number. */
static double tg_r13_probe(const TG_NodeList *nl, int ring, int si0,
                           double wx, double wz, int *pspan, double *pnear)
{
    int lo = si0 - TD5_TG_R13_WIN, hi = si0 + TD5_TG_R13_WIN, i;
    double best = -1e30;
    if (lo < 0) lo = 0;
    if (hi > ring - 1) hi = ring - 1;
    if (pspan) *pspan = -1;
    if (pnear) *pnear = 0.0;
    for (i = lo; i <= hi; i++) {
        double dx, dz, d2, lat, r, intr;
        if (i > si0 - 3 && i < si0 + 3) continue;   /* its own span, by design */
        dx = wx - nl->v[i].x; dz = wz - nl->v[i].z;
        d2 = dx * dx + dz * dz;
        /* Bounded exactly like the guard's own nearest-node search: past this
         * distance no lateral computed in that span's frame describes anything
         * real, and an unbounded search reported hits at nodes 50000 away. */
        if (d2 > TD5_TG_GUARD_NEAR_MAX * TD5_TG_GUARD_NEAR_MAX) continue;
        lat = (wx - nl->v[i].x) * nl->v[i].tz - (wz - nl->v[i].z) * nl->v[i].tx;
        r = tg_carriageway_reach(nl, i, (lat >= 0.0) ? 1.0 : -1.0);
        intr = r - (lat < 0.0 ? -lat : lat);
        if (intr > best) {
            best = intr;
            if (pspan) *pspan = i;
            if (pnear) *pnear = sqrt(d2);
        }
    }
    return best;
}

/* SELF-FOLD of an outward-laid quad. Every lateral surface this generator emits
 * -- the ground slab, a side-street mouth -- is built as a ruled quad between
 * the cross-section at node si and the one at node si+1, each a straight ray of
 * length `reach` off its own kerb point. The two rays converge on the inside of
 * a bend. Once `reach` passes the convergence point the quad's OUTER edge runs
 * BACKWARDS along the road, the quad is a bowtie, and the surface beyond that
 * point lies on top of what its neighbours laid.
 *
 * Measured directly on the two emitted corners rather than inferred from a
 * radius: the fraction of the inner edge's along-road length that survives at
 * the outer edge. 1 on a straight, 0 exactly at the convergence point, NEGATIVE
 * once the quad has folded. */
static double tg_r13_quad_fold(double nx, double nz, double fx, double fz,
                               double ox, double oz, double gx, double gz,
                               double reach)
{
    const double ix = fx - nx, iz = fz - nz;
    const double ilen = sqrt(ix * ix + iz * iz);
    double ux, uz;
    if (ilen < 1e-6) return 1.0;
    ux = ix / ilen; uz = iz / ilen;
    return (((fx + gx * reach) - (nx + ox * reach)) * ux
          + ((fz + gz * reach) - (nz + oz * reach)) * uz) / ilen;
}

static double tg_r14_mass_probe(const TG_NodeList *nl, int ring, int si,
                                const TG_SideGeom *g, int *pspan, double *pnear,
                                int *plo, int *phi)
{
    const double n0x = g->bx,         n0z = g->bz;
    const double f0x = g->bx + g->ax, f0z = g->bz + g->az;
    const double n1x = n0x + g->lx0 * g->depth, n1z = n0z + g->lz0 * g->depth;
    const double f1x = f0x + g->lx1 * g->depth, f1z = f0z + g->lz1 * g->depth;
    double best = -1e30;
    int iu, iv;

    if (pspan) *pspan = -1;
    if (pnear) *pnear = 0.0;
    if (plo) *plo = -1;
    if (phi) *phi = -1;
    for (iv = 0; iv < TD5_TG_R14_GRID; iv++) {
        const double v = (double)iv / (double)(TD5_TG_R14_GRID - 1);
        /* Front and back edge points at the same along-street parameter, then
         * the segment between them: this walks the quad the emitter actually
         * lays, including the interior a folded footprint sweeps backwards. */
        const double ax2 = n0x + (f0x - n0x) * v, az2 = n0z + (f0z - n0z) * v;
        const double bx2 = n1x + (f1x - n1x) * v, bz2 = n1z + (f1z - n1z) * v;
        for (iu = 0; iu < TD5_TG_R14_GRID; iu++) {
            const double u = (double)iu / (double)(TD5_TG_R14_GRID - 1);
            const double px = ax2 + (bx2 - ax2) * u;
            const double pz = az2 + (bz2 - az2) * u;
            int sp; double nr;
            const double intr = tg_r13_probe(nl, ring, si, px, pz, &sp, &nr);
            if (sp >= 0) {
                if (plo && (*plo < 0 || sp < *plo)) *plo = sp;
                if (phi && (*phi < 0 || sp > *phi)) *phi = sp;
            }
            if (intr > best) {
                best = intr;
                if (pspan) *pspan = sp;
                if (pnear) *pnear = nr;
            }
        }
    }
    return best;
}

/* Local turn radius at span si, and which side of travel is the INSIDE.
 * *pinside is +1 when the LEFT kerb is the inside of the bend. Returns 1e30 on
 * a straight. Sign convention matches tg_turn_map_build: a LEFT turn makes the
 * tangent cross product NEGATIVE. */
static double tg_r13_radius(const TG_NodeList *nl, int si, double *pinside)
{
    const TG_Node *a = &nl->v[si], *c = &nl->v[si + 1];
    const double cr = a->tx * c->tz - a->tz * c->tx;
    const double dx = c->x - a->x, dz = c->z - a->z;
    const double seg = sqrt(dx * dx + dz * dz);
    const double s = (cr < 0.0) ? -cr : cr;
    if (pinside) *pinside = (cr < 0.0) ? 1.0 : -1.0;
    if (s < 1e-9 || seg < 1e-6) return 1e30;
    return seg / s;
}

void tg_r13_junc_report(const TG_NodeList *nl, int nspans)
{
    const int wspan = td5_env_int("TD5RE_R13_JUNC_SPAN", -1, -1, 100000);
    const int wpad  = td5_env_int("TD5RE_R13_JUNC_PAD", 12, 0, 4000);
    int si, ring = (s_ring_len > 0) ? s_ring_len : nspans;
    int gfold = 0, mfold = 0, xfold = 0;         /* reach exceeds the radius   */
    int gon = 0, mon = 0;                        /* measured on some tarmac    */
    int blind_straddle = 0, blind_far = 0;       /* why the guard said nothing */
    int gsev = 0, xsev = 0;                      /* SEVERE folds, keep < -0.25 */
    double worst_g = -1e300, worst_m = -1e300;
    double gkmin = 1e30, xkmin = 1e30;
    int worst_gs = -1, worst_ms = -1, gkmins = -1, xkmins = -1;
    /* [R14 item 4] The same mass measurement over the real footprint rather
     * than its two back corners, carried BESIDE the R13 number so the size of
     * the lower bound is itself reported instead of asserted. */
    int mon_geom = 0, worst_mgs = -1;
    double worst_mg = -1e300;
    /* [R14 item 4] ATTRIBUTION for whatever mass is still on the road. R13
     * measured a `keep` for the ground slab and for the side street but never
     * for the block body, so there was no way to tell WHICH of two mechanisms
     * a surviving intrusion came from:
     *   keep <  0  the footprint is still a bowtie, i.e. the cap did not bind.
     *              It cannot bind below TD5_TG_R13_FLOOR / TD5_TG_R13_MIN_DEPTH,
     *              which are deliberate escape hatches, so a hairpin keeps a
     *              folded block by design.
     *   keep == 0  the cap bound exactly, and the intrusion is the degenerate
     *              TIP at the centre of curvature that TD5_TG_R13_KEEP = 0
     *              leaves behind -- the mechanism R13 flagged and did not fix.
     * Without this split, raising KEEP could be credited with fixing spans it
     * never touched. */
    int mon_fold = 0, mon_tip = 0;
    double mkmin = 1e30;
    int mkmins = -1;

    if (!tg_report_wanted("TD5RE_R13_JUNC_REPORT")) return;
    if (ring > nspans) ring = nspans;
    if (ring > TD5_TG_MAX_SPANS) ring = TD5_TG_MAX_SPANS;

    TD5_LOG_I(LOG_TAG, "trackgen: ---- r13junc bend-fold dump ----");
    for (si = 1; si < ring - 2; si++) {
        const TG_Biome *b = &k_biomes[tg_scenery_biome_index(si)];
        const double sw = tg_city_sidewalk_w(b);
        const int in_win = (wspan < 0)
                        || (si >= wspan - wpad && si <= wspan + wpad);
        double inside, R = tg_r13_radius(nl, si, &inside);
        double e[10], gx, gz, greach = 0.0, gintr = -1e300, xr = 0.0;
        double gkeep = 1.0, xkeep = 1.0, mkeep = 1.0;
        double mreach = 0.0, mintr = -1e300;
        int gsp = -1, msp = -1, mstr_lo = 0, mstr_hi = 0, xhere;
        double gnear = 0.0, mnear = 0.0;
        TG_GroundProf gp;
        TG_SideGeom g;
        const int isl = (inside > 0.0) ? 1 : 0;

        g.built = 0;
        /* GROUND SKIRT, inside kerb. Same frame tg_emit_ground builds the slab
         * from: kerb point and outward unit at BOTH ends, profile outer point
         * for the reach. */
        tg_ground_side(nl, si, isl, tg_water_side(si), &gp);
        greach = gp.d[gp.n - 1];
        tg_city_edge_frame(nl, si, inside, e);
        gx = e[0] + e[6] * greach;
        gz = e[2] + e[7] * greach;
        gintr = tg_r13_probe(nl, ring, si, gx, gz, &gsp, &gnear);
        gkeep = tg_r13_quad_fold(e[0], e[2], e[3], e[5],
                                 e[6], e[7], e[8], e[9], greach);
        if (gkeep <= 0.0) gfold++;
        if (gkeep < -0.25) gsev++;
        if (gkeep < gkmin) { gkmin = gkeep; gkmins = si; }
        if (gintr > TD5_TG_GUARD_PEN) {
            gon++;
            if (gintr > worst_g) { worst_g = gintr; worst_gs = si; }
        }

        /* BUILDING MASS, inside kerb: the BACK face of the block, which is the
         * frontage line pushed `depth` further along the same inward ray. */
        if (sw > 0.0) {
            tg_side_geom(nl, si, isl, b, &g);
            if (g.built) {
                const double bx = g.bx + g.lx0 * g.depth;
                const double bz = g.bz + g.lz0 * g.depth;
                const double fx = g.bx + g.ax + g.lx1 * g.depth;
                const double fz = g.bz + g.az + g.lz1 * g.depth;
                int s1, s2;
                double n1, n2, i1, i2, mcorner;
                mreach = tg_road_half_width(nl, si) + g.depth
                       + tg_city_sidewalk_w_at(nl, si, b);
                i1 = tg_r13_probe(nl, ring, si, bx, bz, &s1, &n1);
                i2 = tg_r13_probe(nl, ring, si, fx, fz, &s2, &n2);
                if (i2 > i1) { mintr = i2; msp = s2; mnear = n2; }
                else         { mintr = i1; msp = s1; mnear = n1; }
                mstr_lo = (s1 < s2) ? s1 : s2;
                mstr_hi = (s1 > s2) ? s1 : s2;
                /* R13's own number, kept AS IT WAS so mass_on_road stays
                 * comparable across rounds no matter what the geometry probe
                 * finds. mass_on_road_geom is the honest count beside it. */
                mcorner = mintr;
                /* [R14 item 4] The block footprint's own fold, in the same
                 * terms tg_r13_quad_fold reports for the ground and the street:
                 * inner edge = the frontage segment, outward units = the two
                 * ends' inward rays, reach = the depth the cap actually left. */
                mkeep = tg_r13_quad_fold(g.bx, g.bz, g.bx + g.ax, g.bz + g.az,
                                         g.lx0, g.lz0, g.lx1, g.lz1, g.depth);
                if (mkeep < mkmin) { mkmin = mkeep; mkmins = si; }
                /* [R14 item 4] Real footprint, same threshold. Replaces the
                 * two-corner values in the per-span dump and the span-level
                 * blindness attribution, and is counted separately in the
                 * summary so the gap to the R13 number is visible. */
                if (td5_env_flag_on("TD5RE_R14_MASSGEOM")) {
                    int glo = -1, ghi = -1, gsp = -1;
                    double gnr = 0.0;
                    const double gi = tg_r14_mass_probe(nl, ring, si, &g,
                                                        &gsp, &gnr, &glo, &ghi);
                    if (gi > TD5_TG_GUARD_PEN) {
                        mon_geom++;
                        if (gi > worst_mg) { worst_mg = gi; worst_mgs = si; }
                        /* Which mechanism put it there. -1e-6 rather than 0
                         * because a bound cap lands on keep == 0 only up to
                         * rounding, and calling that a fold would blame the
                         * escape hatches for the tip. */
                        if (mkeep < -1e-6) mon_fold++; else mon_tip++;
                    }
                    if (gi > mintr) {
                        mintr = gi; msp = gsp; mnear = gnr;
                        mstr_lo = glo; mstr_hi = ghi;
                    }
                }
                if (mreach > R) mfold++;
                if (mcorner > TD5_TG_GUARD_PEN) {
                    mon++;
                    if (mcorner > worst_m) { worst_m = mcorner; worst_ms = si; }
                    /* WHY THE GUARD SAID NOTHING. Its two R11 refusals: a quad
                     * whose corners straddle more than the exemption scope is
                     * skipped outright, and a vertex further than
                     * TD5_TG_GUARD_NEAR_MAX from every node in its window is
                     * not placed at all. Both are silent. */
                    if (mstr_hi - mstr_lo > TD5_TG_GUARD_EX_SPANS)
                        blind_straddle++;
                    if (mnear > TD5_TG_GUARD_NEAR_MAX) blind_far++;
                }
            }
        }
        /* SIDE STREET, inside kerb: the same fold test on the mouth quad the
         * crossstreet emitter lays, with that emitter's own skew and clamped
         * reach so this measures the street that is actually built. */
        xhere = tg_xstreet_here(nl, si, inside, &xr);
        if (xhere) {
            const double ang = tg_block_arm_skew(si, isl);
            double nox, noz, fox, foz;
            tg_city_edge_frame(nl, si, inside, e);
            tg_block_rot2(e[6], e[7], ang, &nox, &noz);
            tg_block_rot2(e[8], e[9], ang, &fox, &foz);
            xkeep = tg_r13_quad_fold(e[0], e[2], e[3], e[5],
                                     nox, noz, fox, foz, xr);
            if (xkeep <= 0.0) xfold++;
            if (xkeep < -0.25) xsev++;
            if (xkeep < xkmin) { xkmin = xkeep; xkmins = si; }
        }

        if (!in_win) continue;
        TD5_LOG_I(LOG_TAG,
            "trackgen:   r13junc si=%d R=%.0f inside=%s bend=%.3f | "
            "g_reach=%.0f g_keep=%.2f g_intr=%.0f@%d near=%.0f | m_built=%d "
            "m_reach=%.0f m_intr=%.0f@%d near=%.0f straddle=%d | x_here=%d "
            "x_reach=%.0f x_keep=%.2f",
            si, R > 9e29 ? 999999.0 : R, isl ? "L" : "R", tg_turn_bend(si),
            greach, gkeep, gintr, gsp, gnear,
            (sw > 0.0 && g.built), mreach,
            mintr < -1e29 ? 0.0 : mintr, msp, mnear,
            mstr_hi - mstr_lo, xhere, xr, xkeep);
    }
    TD5_LOG_I(LOG_TAG,
        "trackgen: r13junc SUMMARY ground_fold=%d (severe %d, worst keep %.2f "
        "@%d) mass_fold=%d xstreet_fold=%d (severe %d, worst keep %.2f @%d) "
        "| ground_on_road=%d (worst %.0f @%d) mass_on_road=%d (worst %.0f @%d) "
        "mass_on_road_geom=%d (worst %.0f @%d, fold %d tip %d, worst mkeep "
        "%.2f @%d) keep=%.2f "
        "| guard_blind_straddle=%d guard_blind_far=%d",
        gfold, gsev, gkmin < 1e29 ? gkmin : 1.0, gkmins, mfold,
        xfold, xsev, xkmin < 1e29 ? xkmin : 1.0, xkmins,
        gon, worst_g > -1e299 ? worst_g : 0.0, worst_gs,
        mon, worst_m > -1e299 ? worst_m : 0.0, worst_ms,
        mon_geom, worst_mg > -1e299 ? worst_mg : 0.0, worst_mgs,
        mon_fold, mon_tip, mkmin < 1e29 ? mkmin : 1.0, mkmins,
        tg_r14_keep(),
        blind_straddle, blind_far);
    TD5_LOG_I(LOG_TAG,
        "trackgen: r14junc CONTINUATIONS cand=%d refused_skew=%d opened=%d "
        "worst_skew=%.1fdeg ceiling=%.1fdeg",
        s_r14_turn_cand, s_r14_turn_refused, s_r14_turn_opened,
        s_r14_turn_worst * 180.0 / TD5_TG_PI,
        (double)td5_env_float("TD5RE_R14_TURN_SKEW_MAX",
                              (float)TD5_TG_R14_SKEW_MAX_DEG, 0.0f, 90.0f));
}

static TG_R9Bands     s_r9_bands[TD5_TG_MAX_SPANS][2];

static float          s_r9_massing[TD5_TG_MAX_SPANS][2];

static unsigned short s_r9_mass_page[TD5_TG_MAX_SPANS][2];  /* who stands there */

static float          s_r9_mass_top[TD5_TG_MAX_SPANS][2];

static unsigned char  s_r9_arm[TD5_TG_MAX_SPANS][2];        /* kerb turns here  */

static TG_R14Raw s_r14_raw[TD5_TG_MAX_SPANS][2];

/* Merge only bands from the SAME emitter (one emitter's slab and its own kerb
 * face share a lateral and would otherwise self-report as an overlap). */
static void tg_r14_raw_add(int si, int s, double lo, double hi, int src)
{
    TG_R14Raw *R;
    int i;
    if (si < 0 || si >= TD5_TG_MAX_SPANS || !(hi > lo)) return;
    R = &s_r14_raw[si][s];
    for (i = 0; i < (int)R->n; i++)
        if ((int)R->src[i] == src) {
            if (lo < (double)R->lo[i]) R->lo[i] = (float)lo;
            if (hi > (double)R->hi[i]) R->hi[i] = (float)hi;
            return;
        }
    if ((int)R->n >= TD5_TG_R14_RAW_MAX) return;
    R->lo[R->n] = (float)lo;
    R->hi[R->n] = (float)hi;
    R->src[R->n] = (unsigned char)src;
    R->n++;
}

void tg_r9_city_reset(void)
{
    int i, s;
    memset(s_r14_raw, 0, sizeof s_r14_raw);
    s_r14_outer_faces = 0;          /* per BUILD, not per process */
    memset(s_r9_bands, 0, sizeof s_r9_bands);
    memset(s_r9_mass_page, 0, sizeof s_r9_mass_page);
    memset(s_r9_mass_top, 0, sizeof s_r9_mass_top);
    memset(s_r9_arm, 0, sizeof s_r9_arm);
    for (i = 0; i < TD5_TG_MAX_SPANS; i++)
        for (s = 0; s < 2; s++) s_r9_massing[i][s] = 1e30f;
}

/* Merge [lo,hi] into span si / side s's band set, joining bands that touch. */
static void tg_r9_band_add(int si, int s, double lo, double hi, int src)
{
    TG_R9Bands *B;
    int i;
    if (si < 0 || si >= TD5_TG_MAX_SPANS || !(hi > lo)) return;
    B = &s_r9_bands[si][s];
    for (i = 0; i < (int)B->n; i++)
        if (lo <= (double)B->hi[i] + TD5_TG_R9_BAND_JOIN &&
            hi >= (double)B->lo[i] - TD5_TG_R9_BAND_JOIN) {
            if (lo < (double)B->lo[i]) B->lo[i] = (float)lo;
            if (hi > (double)B->hi[i]) B->hi[i] = (float)hi;
            return;                       /* same pavement, just wider */
        }
    if ((int)B->n >= TD5_TG_R9_BANDS_MAX) return;
    B->lo[B->n] = (float)lo;
    B->hi[B->n] = (float)hi;
    B->src[B->n] = (unsigned char)src;
    B->n++;
}

/* Walk one entry's assembled meshes, before the guard compacts them (so the
 * pavement marks still map), and accumulate (A) and (B). */
void tg_r9_city_scan_entry(const TG_NodeList *nl, int ring, int s0,
                                  int ns, const TG_Buf *meshes,
                                  const size_t *moff, int nmesh)
{
    const unsigned char *b = meshes->b;
    int win_lo, win_hi, i;

    if (nmesh <= 0 || ring < 3 || !b) return;
    win_lo = s0 - TD5_TG_GUARD_WINDOW; if (win_lo < 0) win_lo = 0;
    win_hi = s0 + ns - 1 + TD5_TG_GUARD_WINDOW;
    if (win_hi > ring - 1) win_hi = ring - 1;
    /* A corridor entry sits past the ring and has no nodes of its own; measure
     * it against the MAIN spans its fork runs beside. */
    if (s0 >= ring) { win_lo = 0; win_hi = ring - 1; }
    if (win_hi < win_lo) return;

    for (i = 0; i < nmesh; i++) {
        const size_t off = moff[i];
        const size_t mlen = (off <= meshes->len)
                          ? tg_guard_mesh_len(b, off, meshes->len) : 0;
        unsigned int vtxoff, vtxcnt, vi;
        int src, mark_si = -1;

        unsigned int page0;
        if (mlen == 0 || off + mlen > meshes->len) continue;
        if (tg_rd_u16(b + off + 0x02) != 0) continue;      /* billboards: (B) only via box */
        vtxcnt = tg_rd_u32(b + off + 0x08);
        vtxoff = tg_rd_u32(b + off + 0x30);
        page0  = tg_rd_u16(b + off + TD5_TG_MESH_DISK_SIZE + 0x02);
        src = tg_pave_src_of(off, &mark_si);

        for (vi = 0; vi + 3 < vtxcnt; vi += 4) {
            double qlo = 1e30, qhi = -1e30, dymax = -1e30;
            int qsi = -1, qside = -1, k, have = 0;
            /* [R14 BRANCH item 2a] node span of the quad -- see below. */
            int qs_lo = 1 << 30, qs_hi = -1;
            for (k = 0; k < 4; k++) {
                const unsigned char *vp = b + off + vtxoff
                                        + (size_t)(vi + (unsigned)k) * TD5_TG_VTX_SIZE;
                double vx, vy, vz, lat, a, dy;
                int si;
                if ((size_t)(vp + 12 - b) > off + mlen) break;
                vx = (double)tg_rd_f32(vp);
                vy = (double)tg_rd_f32(vp + 4);
                vz = (double)tg_rd_f32(vp + 8);
                si = tg_guard_nearest_node(nl, win_lo, win_hi, vx, vz);
                if (si < 0 || si >= ring) continue;
                lat = (vx - nl->v[si].x) * nl->v[si].tz
                    - (vz - nl->v[si].z) * nl->v[si].tx;
                a  = (lat >= 0.0) ? lat : -lat;
                dy = vy - nl->v[si].y;
                if (dy > dymax) dymax = dy;
                if (si < qs_lo) qs_lo = si;
                if (si > qs_hi) qs_hi = si;
                if (!have) { qsi = si; qside = (lat >= 0.0) ? 1 : 0; have = 1; }
                if (a < qlo) qlo = a;
                if (a > qhi) qhi = a;
            }
            if (!have || qsi < 0 || qsi >= TD5_TG_MAX_SPANS) continue;

            /* (A) marked pavement only, and only where it lies near road level
             * (the kerb FACE of the city slab is the same mesh but vertical --
             * it shares the slab's lateral, so it merges into the same band). */
            if (src == TG_PVS_ARM) {
                /* An arm is pavement, but it runs OUT along the side street, so
                 * it is evidence for item 4 and noise for item 2. */
                if (mark_si >= 0 && mark_si < TD5_TG_MAX_SPANS)
                    s_r9_arm[mark_si][qside] = 1;
            } else if (src >= 0 && dymax < TD5_TG_R9_UPRIGHT) {
                tg_r9_band_add(qsi, qside, qlo, qhi, src);
                /* [R14 BRANCH item 2a] same quads, kept UNMERGED -- but NOT the
                 * cross-section ones. A pavement run's quads bridge two nodes;
                 * an R12 termination cap is a single cross-section with all
                 * four corners on ONE node, and nearest-node attribution puts
                 * it on the span AFTER the run it closes. Counting it produced
                 * two false overlaps on seed 20260901 (si=750 L and R), where
                 * the window dump shows span 750's own gate is `not-city` and
                 * so the city-slab band there cannot be its own -- it is span
                 * 749's end cap standing in the plane where the flat verge band
                 * takes over, which is R12 geometry working correctly. A cap
                 * has no run to double, so it is not evidence for this item. */
                if (qs_hi > qs_lo)
                    tg_r14_raw_add(qsi, qside, qlo, qhi, src);
            }

            /* (B) anything that stands up, whatever emitted it. */
            if (dymax >= TD5_TG_R9_UPRIGHT &&
                qlo < (double)s_r9_massing[qsi][qside]) {
                s_r9_massing[qsi][qside]   = (float)qlo;
                s_r9_mass_page[qsi][qside] = (unsigned short)page0;
                s_r9_mass_top[qsi][qside]  = (float)dymax;
            }
        }
    }
}

/* Is span si / side s inside a frontage GAP (a side-street mouth or its
 * interior)? tg_facade_built is the block model's "is there a frontage here";
 * the R8 CROSS sweep additionally required a built neighbour, which is what
 * confined it to corner spans. */
static int tg_r9_in_gap(int si, int s)
{
    return !tg_facade_built(si, s);
}

void tg_r9_city_report(const TG_NodeList *nl, int nspans)
{
    const int verbose = td5_env_int("TD5RE_R9_CITY_REPORT", 0, 0, 1);
    /* Windowed detail: TD5RE_R9_CITY_SPAN=N (+/- _PAD) dumps EVERY span-side in
     * the window, offender or not, so the span the user named can be inspected
     * even when the offender list is long. */
    const int wspan = td5_env_int("TD5RE_R9_CITY_SPAN", -1, -1, 100000);
    const int wpad  = td5_env_int("TD5RE_R9_CITY_PAD", 6, 0, 4000);
    int si, s, ring = (s_ring_len > 0) ? s_ring_len : nspans;
    int dbl = 0, sides = 0, gaps = 0, intrude = 0, listed = 0;

    if (ring > nspans) ring = nspans;
    if (ring > TD5_TG_MAX_SPANS) ring = TD5_TG_MAX_SPANS;

    for (si = 1; si < ring; si++) {
        const TG_Biome *b = &k_biomes[tg_scenery_biome_index(si)];
        const double sw = tg_city_sidewalk_w(b);
        const double half = tg_road_half_width(nl, si);
        for (s = 0; s < 2; s++) {
            const TG_R9Bands *B = &s_r9_bands[si][s];
            if (wspan >= 0 && si >= wspan - wpad && si <= wspan + wpad) {
                char w[256];
                int k, p = 0;
                w[0] = 0;
                for (k = 0; k < (int)B->n && p < 200; k++)
                    p += snprintf(w + p, sizeof(w) - (size_t)p, " [%.0f..%.0f %s]",
                                  (double)B->lo[k], (double)B->hi[k],
                                  k_pave_src_name[B->src[k]]);
                unsigned int blk, ph, gs, gl;
                int av;
                tg_facade_block(si, s, &blk, &ph, &gs, &gl, &av);
                TD5_LOG_I(LOG_TAG, "r9city-win: si=%d side=%s gap=%d bands=%d%s "
                          "| massing nearest=%.0f page=%d top=%.0f line=%.0f "
                          "| facade turn=%d blk=%u ph=%u gap=[%u,%u) av=%d",
                          si, s ? "L" : "R", tg_r9_in_gap(si, s), (int)B->n, w,
                          (double)s_r9_massing[si][s],
                          (int)s_r9_mass_page[si][s],
                          (double)s_r9_mass_top[si][s], half + sw,
                          tg_turn_open(si, s), blk, ph, gs, gs + gl, av);
            }
            if (B->n > 0) sides++;
            if (B->n >= 2) {
                dbl++;
                if (verbose && listed++ < 60) {
                    char msg[256];
                    int k, p = 0;
                    for (k = 0; k < (int)B->n && p < 200; k++)
                        p += snprintf(msg + p, sizeof(msg) - (size_t)p,
                                      " [%.0f..%.0f %s]", (double)B->lo[k],
                                      (double)B->hi[k],
                                      k_pave_src_name[B->src[k]]);
                    TD5_LOG_W(LOG_TAG, "r9city: DOUBLE PAVEMENT si=%d side=%s "
                              "bands=%d%s", si, s ? "L" : "R", (int)B->n, msg);
                }
            }
            /* (B) only asks the question where the city has a pavement line and
             * the frontage is open -- that is what a crossing mouth is. */
            if (!(sw > 0.0) || tg_span_in_bridge_run(si)) continue;
            if (!tg_r9_in_gap(si, s)) continue;
            gaps++;
            if ((double)s_r9_massing[si][s] < half + sw) {
                intrude++;
                if (verbose && listed++ < 60)
                    TD5_LOG_W(LOG_TAG, "r9city: MOUTH MASSING si=%d side=%s "
                              "nearest=%.0f pavement_line=%.0f (%.0f inside)",
                              si, s ? "L" : "R", (double)s_r9_massing[si][s],
                              half + sw, half + sw - (double)s_r9_massing[si][s]);
            }
        }
    }
    /* [R9 CITY item 4] FORK-MOUTH SWEEP -- every fork's entry AND rejoin, not
     * just the span the user named. A mouth span-side is BARE when the city has a
     * pavement line there but nothing provides one: no along-road band and no arm
     * turning down a side street. That is the "no sidewalks on the merging of the
     * branch" condition, stated so it can be counted over both seeds. Only the
     * RIGHT side is swept -- no corridor ever bows left, so the left side has no
     * fork-related reason to lose its kerb. */
    {
        int f, bare = 0, checked = 0;
        for (f = 0; f < s_fork_count; f++) {
            int lo = s_forks[f].F - TD5_TG_BRANCH_WIDEN - 2;
            int hi = s_forks[f].R + 2;
            if (lo < 1) lo = 1;
            if (hi > ring - 1) hi = ring - 1;
            for (si = lo; si <= hi; si++) {
                const TG_Biome *b = &k_biomes[tg_scenery_biome_index(si)];
                if (!(tg_city_sidewalk_w(b) > 0.0)) continue;
                if (tg_span_in_bridge_run(si)) continue;
                /* Where the corridor really is, bare ground is CORRECT: the
                 * branch carries its own kerb out there. */
                if (tg_side_corridor_here(nl, si, -1.0)) continue;
                checked++;
                /* A gap-interior span with the side street crossing it is
                 * CORRECTLY kerbless -- that is what every junction on the track
                 * looks like. BARE means no kerb, no arm AND no street: the
                 * frontage opened and nothing serviced it. */
                {
                    const int street = !tg_facade_built(si, 0)
                                     && !tg_block_is_park(si, 0);
                    if (s_r9_bands[si][0].n == 0 && !s_r9_arm[si][0] && !street) {
                        bare++;
                        if (verbose)
                            TD5_LOG_W(LOG_TAG, "r9city: BARE FORK MOUTH si=%d "
                                      "side=R (fork %d F=%d R=%d) -- no band, "
                                      "no arm, no street", si, f,
                                      s_forks[f].F, s_forks[f].R);
                    }
                }
            }
        }
        TD5_LOG_I(LOG_TAG, "trackgen: r9city FORK MOUTHS checked=%d BARE=%d",
                  checked, bare);
    }

    TD5_LOG_I(LOG_TAG, "trackgen: r9city SUMMARY paved_span_sides=%d "
              "DOUBLE_PAVEMENT=%d | gap_span_sides=%d MASSING_INSIDE_PAVEMENT=%d",
              sides, dbl, gaps, intrude);
}

/* [R14 BRANCH item 2a] Which gate refuses the main-road pavement at (si, s)?
 * Every `continue` in tg_city_emit_sidewalk, asked in the emitter's own order,
 * so a hole is reported as the NAME of the predicate that returned false rather
 * than as "missing". Mirrors tg_r8_city_sidewalk_diag's classification; kept
 * beside the sweep that prints it so the two cannot drift apart. */
static const char *tg_r14_pave_reason(const TG_NodeList *nl, int si, int s)
{
    const TG_Biome *b;
    double sw;
    if (si < 1 || si + 1 >= nl->count)         return "off-strip";
    if (tg_span_in_bridge_run(si))             return "bridge-run";
    b  = &k_biomes[tg_scenery_biome_index(si)];
    sw = tg_city_sidewalk_w(b);
    if (!(sw > 0.0))                           return "not-city(biome)";
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_SIDEWALKS")) return "sidewalks-off";
    sw = tg_city_sidewalk_w_at(nl, si, b);
    if (!(tg_pavement_side_width(nl, si, s ? 1.0 : -1.0, sw) > 0.0))
        return "pavement_side_width=0";
    if (td5_env_flag_on("TD5RE_AUTOTRACK_XSTOP") && !tg_facade_built(si, s) &&
        !tg_r13_approach_span(si))
        return "xstop(frontage-gap)";
    return "emitted";
}

/* [R14 BRANCH items 2a] Fork-region pavement sweep: OVERLAPS and HOLES.
 *
 * OVERLAP -- the complementary measurement to R9's DOUBLE PAVEMENT (see the raw
 * store above for why R9 cannot see this): any two bands from DIFFERENT
 * emitters sharing more than TD5_TG_R14_OV_MIN of lateral.
 *
 * HOLE -- a span-side inside a fork region where the biome HAS a pavement line
 * and no emitter laid one, with a neighbour on at least one side that did. The
 * predicate that refused it is named, so "dither artefact" and "fork-specific"
 * are distinguishable from the log instead of assumed. */
void tg_r14_branch_report(const TG_NodeList *nl, int nspans)
{
    const int verbose = td5_env_int("TD5RE_R14_BRANCH_REPORT", 0, 0, 1);
    const int wspan   = td5_env_int("TD5RE_R14_BRANCH_SPAN", -1, -1, 100000);
    const int wpad    = td5_env_int("TD5RE_R14_BRANCH_PAD", 8, 0, 4000);
    int si, s, f, listed = 0;
    int ov_sides = 0, holes = 0, one_span = 0, checked = 0;
    int ring = (s_ring_len > 0) ? s_ring_len : nspans;
    double ov_max = 0.0;

    if (ring > nspans) ring = nspans;
    if (ring > TD5_TG_MAX_SPANS) ring = TD5_TG_MAX_SPANS;

    for (si = 1; si < ring; si++)
        for (s = 0; s < 2; s++) {
            const TG_R14Raw *R = &s_r14_raw[si][s];
            int i, j, hit = 0;
            char msg[288];
            int p = 0;
            msg[0] = 0;
            for (i = 0; i < (int)R->n; i++)
                for (j = i + 1; j < (int)R->n; j++) {
                    const double lo = (R->lo[i] > R->lo[j]) ? R->lo[i] : R->lo[j];
                    const double hi = (R->hi[i] < R->hi[j]) ? R->hi[i] : R->hi[j];
                    if (hi - lo <= TD5_TG_R14_OV_MIN) continue;
                    hit = 1;
                    if (hi - lo > ov_max) ov_max = hi - lo;
                    if (p < 220)
                        p += snprintf(msg + p, sizeof(msg) - (size_t)p,
                                      " %s[%.0f..%.0f] x %s[%.0f..%.0f] ov=%.0f",
                                      k_pave_src_name[R->src[i]],
                                      (double)R->lo[i], (double)R->hi[i],
                                      k_pave_src_name[R->src[j]],
                                      (double)R->lo[j], (double)R->hi[j],
                                      hi - lo);
                }
            if (hit) {
                ov_sides++;
                if (verbose && listed++ < 80)
                    TD5_LOG_W(LOG_TAG, "r14branch: PAVEMENT OVERLAP si=%d "
                              "side=%s%s", si, s ? "L" : "R", msg);
            }
            if (wspan >= 0 && si >= wspan - wpad && si <= wspan + wpad) {
                char w[288];
                int k, q = 0;
                w[0] = 0;
                for (k = 0; k < (int)R->n && q < 230; k++)
                    q += snprintf(w + q, sizeof(w) - (size_t)q, " [%.0f..%.0f %s]",
                                  (double)R->lo[k], (double)R->hi[k],
                                  k_pave_src_name[R->src[k]]);
                TD5_LOG_I(LOG_TAG, "r14branch-win: si=%d side=%s raw=%d%s "
                          "| gate=%s half=%.0f reach=%.0f sw=%.0f use=%.0f",
                          si, s ? "L" : "R", (int)R->n, w,
                          tg_r14_pave_reason(nl, si, s),
                          tg_road_half_width(nl, si),
                          tg_carriageway_reach(nl, si, s ? 1.0 : -1.0),
                          tg_city_sidewalk_w_at(nl, si,
                              &k_biomes[tg_scenery_biome_index(si)]),
                          tg_pavement_side_width(nl, si, s ? 1.0 : -1.0,
                              tg_city_sidewalk_w_at(nl, si,
                                  &k_biomes[tg_scenery_biome_index(si)])));
            }
        }

    for (f = 0; f < s_fork_count; f++) {
        int lo = s_forks[f].F - TD5_TG_BRANCH_WIDEN - 2;
        int hi = s_forks[f].R + 2;
        if (lo < 2) lo = 2;
        if (hi > ring - 2) hi = ring - 2;
        for (si = lo; si <= hi; si++) {
            if (tg_span_in_bridge_run(si)) continue;
            if (!(tg_city_sidewalk_w(&k_biomes[tg_scenery_biome_index(si)]) > 0.0))
                continue;
            for (s = 0; s < 2; s++) {
                const int prev = s_r14_raw[si - 1][s].n > 0;
                const int next = s_r14_raw[si + 1][s].n > 0;
                checked++;
                if (s_r14_raw[si][s].n > 0) continue;
                if (!prev && !next) continue;        /* run end, not a hole */
                holes++;
                if (prev && next) one_span++;
                if (verbose && listed++ < 80)
                    TD5_LOG_W(LOG_TAG, "r14branch: PAVEMENT HOLE si=%d side=%s "
                              "(fork %d F=%d R=%d) %s -- gate=%s soft=%d hard=%d",
                              si, s ? "L" : "R", f, s_forks[f].F, s_forks[f].R,
                              (prev && next) ? "ONE-SPAN" : "run-edge",
                              tg_r14_pave_reason(nl, si, s),
                              tg_biome_for_span(si), tg_biome_cell_index(si));
            }
        }
    }

    TD5_LOG_I(LOG_TAG, "trackgen: r14branch SUMMARY overlap_span_sides=%d "
              "max_overlap=%.0f | fork_span_sides=%d HOLES=%d (one-span %d) "
              "| outer_faces=%ld", ov_sides, ov_max, checked, holes, one_span,
              s_r14_outer_faces);
}

/* Group CROSS dispatcher (feedback R4 items 10, 14; R5 items 2, 3, 4, 12).
 * Wired into the scenery loop next to tg_emit_fb_block; city spans only.
 * R5 item 3 removed the raised kerb break that used to run here. */
int tg_emit_fb_cross(const TG_FBHook *h)
{
    if (!TG_SUB2(TG_SUB2_PAVED, tg_city_span_paved(h)))
        return 1;                              /* only where the city is */
    if (!TG_SUB2(TG_SUB2_XWALLS, tg_cross_emit_sidewalls(h))) return 0;
    if (!TG_SUB2(TG_SUB2_XZEBRA, tg_cross_emit_join_zebra(h))) return 0;
    if (!TG_SUB2(TG_SUB2_XFLANK, tg_cross_emit_street_flank(h))) return 0;  /* [R8 CROSS item 1] */
    if (!TG_SUB2(TG_SUB2_XINFILL, tg_r13_emit_gap_infill(h))) return 0;     /* [R13 FILL item 7b] */
    return 1;
}

/* Group A -- city: sidewalks, kerb fences, crossings, deeper building rows. */
int tg_emit_fb_city(const TG_FBHook *h)
{
    const int paved = TG_TI(TG_T_CITY_PAVED, tg_city_span_paved(h));
    /* [R7 item 7] sw from the SAME hardened city edge tg_city_span_paved uses, so
     * the slab, its kerb height and the railing all agree on where the city is.
     * [R10 WIDEWALK] width scaled to the half-road so a wide avenue reads as a
     * pavement; the facade setback (tg_side_geom) reads the SAME formula and the
     * same span, so the facade front stays on the slab back edge. */
    const double sw = tg_city_sidewalk_w_at(h->nl, h->si,
                          &k_biomes[tg_scenery_biome_index(h->si)]);

    TG_TV(TG_T_CITY_DIAG, tg_r8_city_sidewalk_diag(h));       /* [R8 CITY item 2] measure, don't guess */

    if (paved && td5_env_flag_on("TD5RE_AUTOTRACK_SIDEWALKS")) {
        if (!TG_TI(TG_T_CITY_SIDEWALK, tg_city_emit_sidewalk(h, sw))) return 0;
        if (!TG_TI(TG_T_CITY_FENCE, tg_city_emit_fence(h, sw))) return 0;
    }
    /* Outside the city the same margin is a texture band, not a slab. Bridge
     * decks are excluded exactly as the pavement is -- there is no ground beside
     * a deck for a band to lie on. */
    if (!paved && !tg_span_in_bridge_run(h->si) &&
        tg_verge_band_w(h->b) > 0.0) {
        if (!TG_TI(TG_T_CITY_VERGE, tg_city_emit_verge_band(h, tg_verge_band_w(h->b)))) return 0;
    }
    if (paved && tg_city_crossing_here(h->si) &&
        td5_env_flag_on("TD5RE_AUTOTRACK_CROSSINGS")) {
        if (!TG_TI(TG_T_CITY_CROSSING, tg_city_emit_crossing(h))) return 0;
    }
    /* The side street itself, on every span of the gap (the zebra above is only
     * on its first span). Both are gated on the pavement, since a gap in a
     * biome with no frontage is just open country. */
    /* [R12 OVERPASS item 11c] and NOT under a highway overpass. The side street
     * is the junction; its zebra is only the paint (see tg_up_xclear_span). */
    /* [R13 RAIL item 5b] CONSEQUENCE 3, and the same shape as the two gates
     * beside it: a bridge RAMP is not a place a side street can leave from. The
     * road there is already tens or hundreds of units below local ground and
     * still falling, so the street was being laid off the shoulder of a descent
     * into a gorge. Removing it is also what licenses consequence 2 -- with no
     * mouth on the ramp there is nothing for the pavement to open for, so
     * carrying the slab through cannot lay a kerb across a street. */
    if (paved && !tg_span_in_bridge_run(h->si) && !tg_up_xclear_span(h->si) &&
        !tg_r13_approach_span(h->si) &&
        td5_env_flag_on("TD5RE_AUTOTRACK_CROSS_STREETS")) {
        if (!TG_TI(TG_T_CITY_XSTREET, tg_city_emit_crossstreet(h, sw))) return 0;
    }
    /* Lamps follow the biome's prop_lamp flag (city/industrial/coast), on the
     * same 1-in-7 beat the prop layer used, so the spacing is unchanged -- only
     * the fixture under the glow is new. NIGHT ONLY (item 11): a lit lamp head
     * over a midday road is what gives the generated scenery away. */
    if (h->b->prop_lamp && (h->si % 7) == 0 && !tg_span_in_bridge_run(h->si) &&
        td5_trackgen_is_night() &&
        td5_env_flag_on("TD5RE_AUTOTRACK_LAMP_POSTS")) {
        if (!TG_TI(TG_T_CITY_LAMP, tg_city_emit_lamp(h, sw))) return 0;
    }
    /* [R11 item 7c] Backrows stand out to sw + depth + 2*3200 and the fork-back
     * massing further still -- both squarely inside the overpass arms now that
     * they reach the drawn edge. Cleared on the deck's own spans. */
    if (paved && !tg_span_in_bridge_run(h->si) && !tg_up_clear_span(h->si) &&
        td5_env_flag_on("TD5RE_AUTOTRACK_BACKROWS")) {
        if (!TG_TI(TG_T_CITY_BACKROWS, tg_city_emit_backrows(h, sw))) return 0;
    }
    /* [R4 item 6] Fill the bare flank a fork clears on the right with background
     * park/buildings set beyond the corridor. Runs on paved (city) fork spans
     * only; a no-op everywhere else. */
    if (paved && !tg_up_clear_span(h->si)) {
        if (!TG_TI(TG_T_CITY_FORKBACK, tg_city_emit_forkback(h))) return 0;
    }
    return 1;
}

/* [R11 TEX item 1] "the tree background texture looks very tall and stretched
 * out, has a black line at the top, and reads cold/wintry -- replace it with a
 * different page from the pool."
 *
 * The page is NOT the defect, so it is not replaced. MEASURED off the emitter
 * below, as it shipped:
 *   - u ran 0..1 across ONE span and v ran 0..1 over the WHOLE wall, so a single
 *     64x64 page tile was drawn TD5_TG_SPAN_LENGTH (1500) wide by `band` tall --
 *     12000 in FOREST, 14000 in ALPINE, 7000 in FIELDS. That is a 1:8 (1:8.5,
 *     1:4.7) VERTICAL stretch of a square page. Smearing a leaf canopy 8x up
 *     turns its texels into tall spikes and averages its greens toward grey,
 *     which is both "tall and stretched out" and "reads cold" -- the SAME page,
 *     drawn near 1:1 on the far ridge (tg_r8_tl_note: mean ratio 0.85), reads as
 *     a green forest skyline.
 *   - v reached EXACTLY 0.0 at the crest. Under the wrapped sampler that fetches
 *     the page's opposite edge row -- the dark foliage base -- which is the
 *     "black line at the top". The far ridge was given a half-texel V inset for
 *     precisely this at R4 item 5; this band never got it.
 * So the two reported symptoms are one UV bug and one edge-sampling bug, and
 * both already have a fix precedent on the sibling emitter.
 *
 * ASPECT: tile u by the wall's OWN height, the R8 item 14 rule, so a square page
 * is drawn square (one tile = `band` wide = band/1500 spans). u is CUMULATIVE in
 * span index rather than alternating 0/1 per span, so consecutive quads share
 * their edge u exactly and the wall stays one unbroken run; it is reduced modulo
 * one tile per quad to keep the float small, which is seam-free because the
 * sampler wraps. This drops the old per-span mirror trick: mirroring only hides
 * a page that does not tile, and it can only do so while the whole 0..1 range is
 * shown, which is exactly what causes the stretch.
 * TD5RE_R11_TREELINE_FIT=0 restores the stretched, un-inset band for an A/B. */
int tg_r11_treeline_fit(void)
{
    return td5_env_flag_on("TD5RE_R11_TREELINE_FIT");   /* default ON */
}

int tg_r12_treeline_density(void)
{
    return td5_env_flag_on("TD5RE_R12_TREELINE_DENSITY");   /* default ON */
}

/* Triangle wave of period 2 on t, so u mirrors about every tile edge instead of
 * wrapping. t is in TILES; a span advances t by SPAN_LENGTH / TILE_U = 0.5. */
double tg_r12_tl_fold(double t)
{
    double m = fmod(t, 2.0);
    if (m < 0.0) m += 2.0;
    return (m <= 1.0) ? m : (2.0 - m);
}

/* Band height for biome b, 0 = no backdrop. A forest wall stands above the
 * 5400..7200-raw billboards in front of it; alpine conifers read taller; open
 * FIELDS get a low far hedgerow line instead of a wall, which is what keeps the
 * "open horizon" the biome comment asks for. City/industrial/coast/oriental get
 * none: buildings, the sea and manicured planting close those off already. */
double tg_treeline_height(const TG_Biome *b)
{
    if (!strcmp(b->name, "FOREST")) return 12000.0;
    if (!strcmp(b->name, "ALPINE")) return 14000.0;
    if (!strcmp(b->name, "FIELDS")) return  7000.0;
    return 0.0;
}

/* Lateral setback of the band from the road EDGE. Trees sit at 800..3200 plus
 * half their own width, so the band has to be past that or it would hide them
 * instead of backing them. FIELDS puts its hedgerow line further out again, at
 * the far side of the open ground. */
double tg_treeline_back(const TG_Biome *b)
{
    return (!strcmp(b->name, "FIELDS")) ? 22000.0 : 11000.0;
}

void tg_r12_band_params(int si, double *out_h, double *out_back)
{
    int cell, off, other, dist, own;
    double h, back, ho, bo;

    if (si < 0) si = 0;
    own  = tg_biome_cell_index(si);
    h    = tg_treeline_height(&k_biomes[own]);
    back = tg_treeline_back(&k_biomes[own]);
    *out_back = back;
    *out_h    = 0.0;
    if (!(h > 0.0)) return;               /* this run carries no tree line */

    cell = si / TD5_TG_BIOME_RUN;
    off  = si - cell * TD5_TG_BIOME_RUN;
    if (off < TD5_TG_BIOME_BLEND) {
        other = tg_biome_cell_index((cell - 1) * TD5_TG_BIOME_RUN);
        dist  = off;
    } else if (off >= TD5_TG_BIOME_RUN - TD5_TG_BIOME_BLEND) {
        other = tg_biome_cell_index((cell + 1) * TD5_TG_BIOME_RUN);
        dist  = TD5_TG_BIOME_RUN - 1 - off;
    } else {
        *out_h = h; return;               /* solid core of the run */
    }
    if (other == own) { *out_h = h; return; }

    ho = tg_treeline_height(&k_biomes[other]);
    bo = tg_treeline_back(&k_biomes[other]);
    if (ho > 0.0) {
        /* Halfway at the boundary, own value at the band's far edge. The
         * neighbouring cell computes the mirror of this, so both height and
         * setback are continuous ACROSS the boundary as well as along it. */
        const double t = (double)(TD5_TG_BIOME_BLEND - dist)
                       / (double)(2 * TD5_TG_BIOME_BLEND);
        *out_h    = h    + (ho - h)    * t;
        *out_back = back + (bo - back) * t;
        return;
    }
    *out_h = (dist < TD5_TG_R12_BAND_TAPER)
           ? h * (double)dist / (double)TD5_TG_R12_BAND_TAPER : h;
}

/* [R15] Per-module half of the round-15 report. Split out of the single
 * tg_r15_sky_report the work was first written against: after the trackgen
 * split its counters live in four different modules, and a file-static
 * cannot be read from another translation unit. One report per owning
 * module keeps the counters static where they belong.  */
void tg_r15_streets_report(void)
{
    TD5_LOG_I(LOG_TAG, "[R15 CITY item 7] gap-interior infill blocks refused "
              "(the gap is a side street) = %ld (knob TD5RE_R15_FILL_STREET=%s)",
              s_r15_fill_street,
              td5_env_flag_on("TD5RE_R15_FILL_STREET") ? "on" : "off");
    TD5_LOG_I(LOG_TAG, "[R15 CROSS item 9] medians laid down wide crossings "
              "(>= %d spans of width, both kerbs open) = %ld (knob "
              "TD5RE_R15_XWIDE_MEDIAN=%s)", TD5_TG_R15_MEDIAN_MIN,
              s_r15_medians,
              td5_env_flag_on("TD5RE_R15_XWIDE_MEDIAN") ? "on" : "off");
}
