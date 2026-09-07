/**
 * td5_tg_furniture.c -- auto-track roadside FURNITURE: guardrails, start/finish gantry, curve direction signage
 *
 * Split from the td5_trackgen.c monolith (2026-09-06); shared declarations
 * live in td5_trackgen_internal.h. Element map: docs/plans/AUTOTRACK_ELEMENT_CATALOG.md.
 */
#include "td5_trackgen_internal.h"

/* [R8 G1 "different guardrails"] Which page the roadside barrier wears.
 *
 * Every generated track wore TD5_TG_PAGE_RAIL, one procedural armco, on every
 * span of every biome -- and the only other rail art in the generator was made
 * for the BRIDGE parapet, whose quad maps its axes the other way round, so it
 * was never available here. The R8 survey found real photographic rails in the
 * shipped levels instead (scored by horizontal banding plus a keyed band, see
 * gen_trackgen_r8var_tex.py) and this picks among four of them.
 *
 * Choice is BIOME-led, then varied by a per-biome-run hash. Biome-led because a
 * guardrail is not decoration: a timber post-and-rail belongs on a forest road
 * and a dark steel double rail belongs in a city, and rolling them at random
 * would swap "one rail everywhere" for "the wrong rail everywhere". `urbanity`
 * (0 wilderness .. 3 dense urban) is the axis the biome table already carries
 * for exactly this kind of question. Each urbanity picks a PAIR, and the run
 * hash picks inside the pair, so two consecutive runs in one biome differ while
 * both stay plausible for it.
 *
 * All four pages are alpha-keyed on index 0, which is what a guardrail has to
 * be -- real armco is mostly air, the R6 item-13 finding for the bridge rail.
 * The prism's top cap samples the page's top rows, which are keyed on all four,
 * so the cap simply drops out and the barrier reads as beam-and-post rather
 * than as the solid slab the procedural page made it.
 *
 * TD5RE_R8_VARIETY_RAILS=0 restores the one procedural page for an A/B. */
int tg_rail_page(int si)
{
    const TG_Biome *b;
    unsigned int h;
    int a, c;

    if (!td5_env_flag_on("TD5RE_R8_VARIETY_RAILS")) return TD5_TG_PAGE_RAIL;
    b = &k_biomes[tg_scenery_biome_index(si)];
    h = (unsigned)(si / TD5_TG_BIOME_RUN) * 2654435761u;
    switch (b->urbanity) {
    case 0:  a = 1; c = 0; break;   /* wilderness  timber  / steel W-beam */
    case 1:  a = 0; c = 1; break;   /* rural       W-beam  / timber       */
    case 2:  a = 3; c = 0; break;   /* edge of town white armco / W-beam  */
    default: a = 2; c = 3; break;   /* dense urban dark rail / white armco*/
    }
    {
        int off = ((h >> 15) & 1u) ? c : a;
        /* [R17 FURNITURE item 1] "This guardrail looks like TWO stacked on top
         * of each other" (page 399). MEASURED from the R8V rail pages' own alpha
         * silhouettes: index 0 (Sydney, page 399) is #x20 .x8 #x20 and index 2
         * (Moscow, page 401) is #x21 .x11 #x21 -- two separate opaque beam bands
         * with keyed sky between, i.e. a TWO-RAIL fence. On the ~760u guardrail
         * prism that gap reads as two rails stacked, exactly as reported. Only
         * index 3 (Waikiki white armco, #x44) is a single continuous beam; index
         * 1 (timber post-and-rail) is a distinct multi-rail fence, not a doubled
         * beam, and is left alone. Route the two-beam METAL pages to the single
         * beam so a guardrail reads as one rail. Variety survives: wilderness /
         * rural still roll timber vs armco; town / urban settle on white armco.
         * TD5RE_R17_RAIL_SINGLE=0 restores the two-beam pages for an A/B. */
        if (td5_env_flag_on("TD5RE_R17_RAIL_SINGLE") && (off == 0 || off == 2))
            off = 3;
        return TD5_TG_PAGE_R8V_RAIL + off;
    }
}

/* [R11 GUARD item 11] One definition of "the roadside rail uses the file's
 * top-down V convention", asked by BOTH the emitter (which maps the UVs) and
 * tg_emit_texture_page_rail (which authors the one page that was drawn for the
 * old bottom-up mapping). They have to move together or the fallback page lands
 * upside down instead. See the long note in tg_emit_guardrail. */
int tg_rail_vflip_on(void)
{
    return td5_env_flag_on("TD5RE_R11_RAIL_VFLIP");
}

int tg_r11_street_crosses_here(const TG_NodeList *nl, int si, double sg)
{
    int k;
    if (!tg_r11_xguard()) return 0;
    for (k = -TD5_TG_R11_XPAD; k <= TD5_TG_R11_XPAD; k++)
        if (si + k > 1 && tg_city_crossing_here(si + k)) return 1;
    return tg_xstreet_here(nl, si, sg, NULL) ? 1 : 0;
}

int tg_guardrails_enabled(void)
{
    /* [R8 merge] Flipped to default ON. The old default was "OFF until a frame
     * confirms it"; R8 VARIETY built four real guardrail pages for G1 ("different
     * guardrails") and framed them, so the stated precondition is met -- and with
     * the emitter off, that whole axis shipped invisible, since the rails a player
     * actually saw were the bridge parapet.
     *
     * Verified on the merged tree before flipping (seed 99991, clean env):
     * 1178 rails over 589 spans, on-road guard rejections UNCHANGED at 69 (the
     * rails add no carriageway overlap), the road/branch-road/deck/gantry/tunnel
     * invariant still 0, and the race reaches span 1699.
     *
     * TD5RE_AUTOTRACK_GUARDRAILS=0 restores the old opt-in behaviour. */
    return td5_env_flag_on("TD5RE_AUTOTRACK_GUARDRAILS");
}

/* Should span si carry a barrier? Not every span: real roads are lined on the
 * outside of bends and where the road is elevated, not down every straight, and
 * railing all 1800 spans would both look wrong and cost 1800 extra meshes. */
static int tg_span_needs_guardrail_raw(const TG_NodeList *nl, int si, int nspans)
{
    /* 50 = 5 deg of heading change across the span. MEASURED over 3 seeds this
     * rails 11-15% of spans, consistently -- barriers through the corners and
     * not down the straights, which is the point. The previous default of 15
     * (1.5 deg) railed 62%, i.e. nearly everything. Curvature is the stable
     * signal here: unlike the elevation gate its coverage barely moves between
     * seeds. */
    const int limit = td5_env_int("TD5RE_AUTOTRACK_RAIL_DEG10", 50, 0, 900);
    double cross, dot, ang_deg;

    if (si < 1 || si + 2 >= nl->count) return 0;

    /* EXCLUSION: inside a tunnel the walls already contain and read as a
     * boundary; a barrier there is invisible clutter. */
    if (tg_span_in_tunnel(si)) return 0;

    /* Forks used to be excluded outright here (a rail derived from the MAIN
     * road edge lands ON the branch carriageway, walling the corridor off), at
     * the cost of a rail-free hole around every fork. tg_emit_guardrail now
     * slides the right-hand barrier out to tg_carriageway_reach instead, so the
     * fork region rails like any other span and the exclusion is gone. */
    (void)nspans;

    /* EXCLUSION [R9 RAILFIX, items 8+12]. This used to be two RETURN-1 rules --
     * "rail the whole bridge run" and "rail anything lifted above local ground"
     * -- and they were correct while the roadside emitter was opt-in and OFF:
     * the rails a player actually saw on a deck were the bridge parapet, so
     * nobody noticed that the gate claimed the deck too.
     *
     * With the emitter default-ON at the R8 merge, both fired on every deck edge
     * at two different treatments, which is item 12 ("different guardrails ...
     * upside down?" -- two rails, two art conventions, one edge). MEASURED on
     * seed 99991 before this change: 320 edges carried roadside + deck.
     *
     * The parapet is the right owner of a deck edge (it follows the humped deck
     * as a sloped ribbon and seals to the deck kerb), so the roadside rail
     * yields. Note the two old rules were between them EXACTLY
     * tg_span_is_bridge_deck, so nothing loses a barrier: every span that used
     * to be railed by this test is a span the parapet rails instead. Spans
     * outside the parapet pass (fork pads, branch corridors) are NOT excluded --
     * see tg_rail_deck_here for why that distinction is the whole point. */
    /* These two rules are UNCHANGED from round 8 and stay that way ON PURPOSE.
     * The first attempt at this fix excluded decks HERE, at span level, which
     * mixed two jobs into one predicate: "should this span carry a barrier at
     * all" and "which treatment owns each of its two edges". Those are different
     * questions -- the first is per SPAN, the second per SIDE -- and merging
     * them cost the safety property: a span whose two edges are owned
     * differently cannot be described by one span-level answer, and an edge
     * whose owner never actually emits ended up with nothing.
     *
     * So this stays the round-8 coverage question, and it is also the BASELINE
     * the zero-rail invariant is measured against. Ownership is decided per side
     * in tg_emit_guardrail, where the yield can be counted at the same point it
     * is taken.
     *
     * TD5RE_R9_RAILFIX_SPANDECK=1 restores the draft's span-level exclusion, as
     * a regression probe for the ZERO-RAIL counter. Keep it: the failure it
     * reproduces is not obvious from reading the code. */
    if (td5_env_flag_off("TD5RE_R9_RAILFIX_SPANDECK") && tg_rail_deck_here(nl, si))
        return 0;
    if (tg_span_in_bridge_run(si)) return 1;
    /* [R13 RAIL item 5b] CONSEQUENCE 1. The rule directly above is `lift >=
     * +900`, and a ramp DOWN into a gorge has negative lift, so the approach to
     * every gorge crossing on the track fell through to the bend test and a
     * straight one got nothing. See the block at tg_r13_approach_build. */
    if (tg_r13_approach_span(si)) return 1;
    if (nl->v[si].y - tg_local_ground_y(nl, si) >= TD5_TG_BRIDGE_MIN_LIFT)
        return 1;

    /* Bend: heading change across this span, in tenths of a degree. */
    cross = nl->v[si].tx * nl->v[si + 1].tz - nl->v[si].tz * nl->v[si + 1].tx;
    dot   = nl->v[si].tx * nl->v[si + 1].tx + nl->v[si].tz * nl->v[si + 1].tz;
    ang_deg = atan2(fabs(cross), dot) * 180.0 / TD5_TG_PI;
    return (ang_deg * 10.0) >= (double)limit;
}

/* RUN DILATION. The raw test above is per-span, and its ELEVATION half is
 * judged against a +/-8 span average (tg_local_ground_y), so on undulating or
 * graded ground it latches for one or two spans at a time -- a crest trips it,
 * the approach and the exit do not. The result is ONE-SPAN RAIL ISLANDS: a
 * 1500-unit stub of barrier that begins and ends in mid-air on a slope, which is
 * the other half of the "fences on slope" report (a rail that stops on a grade
 * reads as stair-stepping, and one that starts mid-grade reads as floating).
 *
 * Fix: rail a span if ANY span within +/-TD5RE_AUTOTRACK_RAIL_PAD needs one.
 * That bridges the gaps between islands and extends every run past its ends, so
 * a barrier always starts and finishes on road the raw test agreed was worth
 * railing. Pure function of nl -- no state, so the streaming range emitter can
 * still ask about any span in isolation. */
int tg_span_needs_guardrail(const TG_NodeList *nl, int si, int nspans)
{
    const int pad = td5_env_int("TD5RE_AUTOTRACK_RAIL_PAD", 3, 0, 16);
    int k;

    /* Exclusions must be re-checked on THIS span, not on the neighbour that
     * satisfied the test: dilating a rail into a tunnel is exactly what that
     * exclusion exists to prevent. */
    if (tg_span_in_tunnel(si)) return 0;
    /* [R9 RAILFIX] NO deck exclusion here any more -- see the note in
     * tg_span_needs_guardrail_raw. Dilation may well pull a rail onto a deck
     * span; the per-side ownership test in tg_emit_guardrail drops it there, and
     * drops it only where the parapet really stands. */

    for (k = -pad; k <= pad; k++)
        if (tg_span_needs_guardrail_raw(nl, si + k, nspans)) return 1;
    return 0;
}

/* One barrier prism per side for span si. Caller gates with
 * tg_span_needs_guardrail.
 *
 * PITCH (2026-08-26, "fences on slope should be inclined to follow the slope").
 * The rail used to be extruded straight UP: top = edge_y + RAIL_HEIGHT, base =
 * edge_y - BASE_DROP, both purely vertical. On a graded span that leaves the
 * barrier standing plumb while the road it guards is inclined, so its inner face
 * is not perpendicular to the tarmac and its top edge is not parallel to the
 * road -- and where two spans of different grade meet, the two plumb rails join
 * at a visible kink instead of a continuous line. Real crash barriers lean with
 * the road.
 *
 * So the extrusion axis is now the road's SURFACE NORMAL in the pitch plane:
 * with grade g = dY/d(along-road), the along-road unit is (1, g)/|.| and the
 * normal is (-g, 1)/|.|, both in the (along-road, up) plane. Offsetting by
 * height*normal instead of height*up tilts the posts back by exactly the road's
 * pitch, which is what "inclined to follow the slope" means. At g = 0 the normal
 * IS up, so a flat span is bit-identical to the old behaviour and only graded
 * spans move. The lateral offsets are untouched -- camber is not modelled here,
 * and the two edges already carry their own heights.
 */
int tg_emit_guardrail(const TG_NodeList *nl, int si, TG_Buf *blk,
                             int *emitted)
{
    /* Road grade over this span, and the pitch-plane normal derived from it.
     * ny is the vertical part of the normal and nt the along-road part, so a
     * point offset by h is (p + nt*h*along, p_y + ny*h). */
    double grade = 0.0, gm, n_up, n_along;
    double atx = 0.0, atz = 0.0;      /* along-road unit, horizontal part */
    double nlx, nly, nlz, nrx, nry, nrz;   /* near left / right road edge */
    double flx, fly, flz, frx, fry, frz;   /* far  left / right road edge */
    double nux, nuz, fux, fuz;             /* outward lateral units */
    double len, cx, cy, cz, radius = 0.0;
    /* [R12 item 14b] 20 per side, not 12: the 3 body quads plus up to 2 end
     * caps. Sized for the worst case (both ends capped on both sides) so the
     * cap can never be the thing that overruns. */
    double px[40], py[40], pz[40], uu[40], vv[40];
    int side, i, n = 0, rails = 0;

    *emitted = 0;
    tg_road_edge(nl, si, 0.0, 0.0, 1.0, &nlx, &nly, &nlz, &nrx, &nry, &nrz);
    tg_road_edge(nl, si, 1.0, 0.0, 1.0, &flx, &fly, &flz, &frx, &fry, &frz);

    /* Grade of THIS span, from the centerline nodes rather than the edges, so a
     * width change across the span cannot be mistaken for pitch. */
    {
        double dx = nl->v[si + 1].x - nl->v[si].x;
        double dz = nl->v[si + 1].z - nl->v[si].z;
        double dh = sqrt(dx * dx + dz * dz);
        if (dh > 1e-6) {
            atx = dx / dh; atz = dz / dh;
            grade = (nl->v[si + 1].y - nl->v[si].y) / dh;
        }
    }
    gm      = sqrt(1.0 + grade * grade);
    n_up    =  1.0 / gm;            /* vertical part of the surface normal   */
    n_along = -grade / gm;          /* along-road part (leans against the climb) */

    nux = nlx - nrx; nuz = nlz - nrz;
    len = sqrt(nux * nux + nuz * nuz);
    if (len < 1e-6) { nux = 1.0; nuz = 0.0; } else { nux /= len; nuz /= len; }
    fux = flx - frx; fuz = flz - frz;
    len = sqrt(fux * fux + fuz * fuz);
    if (len < 1e-6) { fux = 1.0; fuz = 0.0; } else { fux /= len; fuz /= len; }

    for (side = 0; side < 2; side++) {
        /* side 0 = left edge (outward = +unit), side 1 = right (outward = -). */
        const double s  = side ? -1.0 : 1.0;
        const double ex = side ? nrx : nlx, ey = side ? nry : nly;
        const double ez = side ? nrz : nlz;
        const double gx = side ? frx : flx, gy = side ? fry : fly;
        const double gz = side ? frz : flz;
        /* Outboard push, from the ONE carriageway authority. Over a fork the
         * branch has bowed out beyond the main road edge these points come
         * from, so a barrier at the bare edge would stand in the corridor;
         * asking for a zero setback returns exactly how far out the outermost
         * tarmac is. Zero on the left (nothing bows that way) and zero on every
         * span off a fork, so ordinary road is bit-identical. Near and far are
         * asked separately so the rail follows the bow instead of stepping. */
        const double push_n = side
            ? TG_TD(TG_T_RAIL_CLEARGAP, tg_carriageway_clear_gap(nl, si, -1.0, 0.0, 0.0)) : 0.0;
        const double push_f = side
            ? TG_TD(TG_T_RAIL_CLEARGAP, tg_carriageway_clear_gap(nl, si + 1, -1.0, 0.0, 0.0)) : 0.0;
        const double o0 = TD5_TG_RAIL_OFFSET;
        const double o1 = TD5_TG_RAIL_OFFSET + TD5_TG_RAIL_THICK;

        /* SAFEGUARD: never leave a barrier standing on tarmac. Asked of the
         * same authority the push came from, so in a consistent world it cannot
         * fire (the rail line is reach + RAIL_OFFSET, one offset OUTSIDE the
         * reach). It exists for the inconsistent world: if some future
         * carriageway grows past its own reported reach, dropping the rail is
         * the right failure -- a missing barrier is a cosmetic loss, a barrier
         * across a live lane is a wall. The LEFT side can never trip it (no
         * corridor bows left, so left reach IS the road edge), which is what
         * keeps the mesh non-empty and the quad count below >= 3. */
        if (TG_TI(TG_T_RAIL_ONCW,
                  tg_on_carriageway(nl, si, s * (tg_road_half_width(nl, si)
                                                 + o0 + push_n), 0.0) ||
                  tg_on_carriageway(nl, si + 1, s * (tg_road_half_width(nl, si + 1)
                                                     + o0 + push_f), 0.0)))
            continue;
        /* [R9 RAILFIX, item 8] YIELD to the pedestrian kerb railing. A city span
         * on a bend satisfies both gates, so an armco prism at road half width
         * and the railing on the pavement stood a couple of hundred units apart
         * on the SAME kerb -- the "double guardrails" at span 957, which lies in
         * no bridge run at all and so is a genuinely different mechanism from
         * item 12. MEASURED on seed 99991 before this change: 376 edges carried
         * roadside + kerb-fence.
         *
         * The railing wins because it is the correct treatment for a kerbed
         * urban street (armco belongs where there is no pavement), because it
         * costs the fewer edges (376 of 1178 roadside vs 376 of 1626 fence), and
         * because dropping the railing instead would leave crash barrier
         * standing in the middle of a city footway. Per SIDE, not per span: a
         * bend with a pavement on one side only keeps its armco on the other. */
        /* [R9 RAILFIX] Past every gate above, the ROUND-8 emitter would have put
         * a rail on this edge. Record that BEFORE deciding whether to yield: it
         * is the baseline the safety invariant is measured against ("did any
         * edge that had a rail end up with none?"), and recording it at the same
         * single point where the yield is decided is what makes the two numbers
         * reconcile by construction instead of by argument. */
        TG_TV(TG_T_RAIL_EDGEWOULD, tg_rail_edge_would(si, s));

        /* OWNERSHIP, decided here and ONLY here, per side. Both predicates
         * answer "does that emitter actually run on this edge" -- see the ring
         * gate in each. If NEITHER claims it, the roadside rail stands: an edge
         * must never end up with zero rails, which is the other half of the
         * invariant and the half a doubling fix is most likely to break. */
        if (tg_railfix_on() &&
            (TG_TI(TG_T_RAIL_DECK, tg_rail_deck_here(nl, si)) ||
             TG_TI(TG_T_RAIL_KERB, tg_rail_kerbfence_here(si, s)))) {
            tg_acct(TG_ACCT_R9_RAILYIELD, si);
            continue;
        }
        /* [R11 CROSS items 9+15] A street crosses here -- no barrier, and
         * nothing else claims the edge either: this is a DELIBERATE hole, not a
         * hand-off, so it is counted in its own bucket rather than through the
         * R9 yield (which means "another emitter rails this edge instead").
         * Counted at the single point the edge would have been claimed, so the
         * numbers are of rails really dropped, not of candidate spans. */
        if (TG_TI(TG_T_RAIL_XSTREET, tg_r11_street_crosses_here(nl, si, s))) {
            if (tg_city_crossing_here(si)) s_r11_rail_on_cross++;
            else                           s_r11_rail_on_xstreet++;
            if (tg_r11_cross_diag())
                TD5_LOG_I(LOG_TAG, "trackgen: [R11 CROSS] rail span %4d %-5s "
                          "DROPPED zebra=%d xstreet=%d kerbfence=%d deck=%d",
                          si, s > 0.0 ? "left" : "right",
                          tg_city_crossing_here(si),
                          tg_xstreet_here(nl, si, s, NULL),
                          tg_rail_kerbfence_here(si, s),
                          tg_rail_deck_here(nl, si));
            continue;
        }
        /* Claim this road edge: this side is now committed to geometry. */
        tg_rail_edge_note(TG_RAIL_ROADSIDE, si, s);
        rails++;                       /* [R12 item 14b] see the tg_acct_n below */
        /* Along-road slide that goes with a given height offset, so the post
         * leans with the road's pitch instead of standing plumb. Zero on a flat
         * span (n_along = 0), which keeps flat road byte-identical. */
        const double slide_t = n_along * TD5_TG_RAIL_HEIGHT;
        const double slide_b = n_along * -TD5_TG_RAIL_BASE_DROP;
        /* near/far x inner/outer, at base and top -- the lateral offsets are the
         * same as before, the pitch lean is the extra atx/atz term on the TOP
         * and BASE rows. */
        const double nib_x = ex + s * nux * (o0 + push_n);
        const double nib_z = ez + s * nuz * (o0 + push_n);
        const double nob_x = ex + s * nux * (o1 + push_n);
        const double nob_z = ez + s * nuz * (o1 + push_n);
        const double fib_x = gx + s * fux * (o0 + push_f);
        const double fib_z = gz + s * fuz * (o0 + push_f);
        const double fob_x = gx + s * fux * (o1 + push_f);
        const double fob_z = gz + s * fuz * (o1 + push_f);
        /* Top and base rows, offset along the SURFACE NORMAL. */
        const double nibt_x = nib_x + atx * slide_t, nibt_z = nib_z + atz * slide_t;
        const double nobt_x = nob_x + atx * slide_t, nobt_z = nob_z + atz * slide_t;
        const double fibt_x = fib_x + atx * slide_t, fibt_z = fib_z + atz * slide_t;
        const double fobt_x = fob_x + atx * slide_t, fobt_z = fob_z + atz * slide_t;
        const double nibb_x = nib_x + atx * slide_b, nibb_z = nib_z + atz * slide_b;
        const double nobb_x = nob_x + atx * slide_b, nobb_z = nob_z + atz * slide_b;
        const double fibb_x = fib_x + atx * slide_b, fibb_z = fib_z + atz * slide_b;
        const double fobb_x = fob_x + atx * slide_b, fobb_z = fob_z + atz * slide_b;
        const double nyb = ey - n_up * TD5_TG_RAIL_BASE_DROP;
        const double fyb = gy - n_up * TD5_TG_RAIL_BASE_DROP;
        const double nyt = ey + n_up * TD5_TG_RAIL_HEIGHT;
        const double fyt = gy + n_up * TD5_TG_RAIL_HEIGHT;
        const double u0 = (double)si, u1 = (double)si + 1.0;
        /* [R11 GUARD item 11] "The guardrails on span 755 are upside down."
         *
         * V CONVENTION. Every page in this generator stores row 0 as the TOP of
         * the image, and every other emitter that stands art upright therefore
         * maps v = 1 AT THE BASE -- the facade cells, the kerb railing, the
         * street-furniture fence page and the branch median all say so in as
         * many words. This emitter alone mapped v = 0 at the base, i.e. it
         * sampled the page bottom-up, and its comment claimed that put "the
         * page's bottom rows at the base". That was true of exactly one page:
         * the procedural TD5_TG_PAGE_RAIL, which was authored bottom-up to suit
         * it (its "gutter under the rail" band sits at y < 8) and is now flipped
         * to the file convention with the rest.
         *
         * The four pages a default build actually uses are the R8 VARIETY
         * photographic rails, and they are top-down like everything else.
         * VERIFIED by decoding them: page 399 (the timber rail on span 755) has
         * its post FOOTING in rows 60-63 and open sky in rows 20-27, so v=0 at
         * the base put the footing at the top of the barrier and the sky at the
         * bottom. Upside down, exactly as reported.
         *
         * This is a CLASS, not a span: span 755 is one of 430 roadside edges on
         * this seed, and every one of them was flipped. It only became visible
         * at R8, when tg_rail_page swapped the procedural page (near-uniform
         * noise, flip-invariant) for real photographs that have a top and a
         * bottom.
         *
         * Fix is the UVs, not the vertex order: the winding is what makes the
         * inner and outer faces point opposite ways and is correct as it stands.
         * TD5RE_R11_RAIL_VFLIP=0 restores the old mapping for an A/B. */
        const double v_base = tg_rail_vflip_on() ? 1.0 : 0.0;
        const double v_top  = tg_rail_vflip_on() ? 0.0 : 1.0;
        /* Cap: a sliver of the page taken from just INSIDE the top edge, so the
         * cap reads as the rail's crown whichever way v runs. */
        const double v_cap  = tg_rail_vflip_on() ? 0.15 : 0.85;

        /* INNER face (towards the road). U runs along the road, V up the face. */
        px[n]=nibb_x; py[n]=nyb; pz[n]=nibb_z; uu[n]=u0; vv[n]=v_base; n++;
        px[n]=nibt_x; py[n]=nyt; pz[n]=nibt_z; uu[n]=u0; vv[n]=v_top;  n++;
        px[n]=fibt_x; py[n]=fyt; pz[n]=fibt_z; uu[n]=u1; vv[n]=v_top;  n++;
        px[n]=fibb_x; py[n]=fyb; pz[n]=fibb_z; uu[n]=u1; vv[n]=v_base; n++;

        /* OUTER face, wound the other way so it faces away from the road. */
        px[n]=fobb_x; py[n]=fyb; pz[n]=fobb_z; uu[n]=u1; vv[n]=v_base; n++;
        px[n]=fobt_x; py[n]=fyt; pz[n]=fobt_z; uu[n]=u1; vv[n]=v_top;  n++;
        px[n]=nobt_x; py[n]=nyt; pz[n]=nobt_z; uu[n]=u0; vv[n]=v_top;  n++;
        px[n]=nobb_x; py[n]=nyb; pz[n]=nobb_z; uu[n]=u0; vv[n]=v_base; n++;

        /* TOP cap, so the barrier reads as solid from a chase camera. */
        px[n]=nibt_x; py[n]=nyt; pz[n]=nibt_z; uu[n]=u0; vv[n]=v_top; n++;
        px[n]=nobt_x; py[n]=nyt; pz[n]=nobt_z; uu[n]=u0; vv[n]=v_cap; n++;
        px[n]=fobt_x; py[n]=fyt; pz[n]=fobt_z; uu[n]=u1; vv[n]=v_cap; n++;
        px[n]=fibt_x; py[n]=fyt; pz[n]=fibt_z; uu[n]=u1; vv[n]=v_top; n++;

        /* [R12 OVERPASS item 14b] END CAP AT A BRIDGE MOUTH. "Elevated
         * sidewalks and guardrails stop abruptly just before the bridge."
         *
         * The barrier is an open prism -- inner face, outer face, top cap, and
         * nothing closing either END. R9 RAILFIX made that visible: the roadside
         * rail YIELDS every deck edge to the bridge parapet (tg_rail_deck_here),
         * so the run does not fade out on a bend, it is cut off square one span
         * before the deck and you see into the hollow section.
         *
         * Scoped to the reported cause rather than to every run end on the
         * track: a run that ends because the bend straightened out ends on open
         * road where the stub reads as a rail terminal, while a run that ends
         * because the NEXT span handed its edges to another treatment ends at a
         * hard structural boundary. Tunnel mouths are the same hand-off
         * (tg_span_in_tunnel excludes the bore) and get the same cap.
         *
         * TD5RE_R12_TERMCAP=0 restores the open ends for an A/B. */
        if (td5_env_flag_on("TD5RE_R12_TERMCAP")) {
            if (si > 0 && (tg_rail_deck_here(nl, si - 1) ||
                           tg_span_in_tunnel(si - 1))) {
                px[n]=nibb_x; py[n]=nyb; pz[n]=nibb_z; uu[n]=u0; vv[n]=v_base; n++;
                px[n]=nobb_x; py[n]=nyb; pz[n]=nobb_z; uu[n]=u0; vv[n]=v_base; n++;
                px[n]=nobt_x; py[n]=nyt; pz[n]=nobt_z; uu[n]=u0; vv[n]=v_top;  n++;
                px[n]=nibt_x; py[n]=nyt; pz[n]=nibt_z; uu[n]=u0; vv[n]=v_top;  n++;
            }
            if (si + 2 < nl->count && (tg_rail_deck_here(nl, si + 1) ||
                                       tg_span_in_tunnel(si + 1))) {
                px[n]=fibb_x; py[n]=fyb; pz[n]=fibb_z; uu[n]=u1; vv[n]=v_base; n++;
                px[n]=fobb_x; py[n]=fyb; pz[n]=fobb_z; uu[n]=u1; vv[n]=v_base; n++;
                px[n]=fobt_x; py[n]=fyt; pz[n]=fobt_z; uu[n]=u1; vv[n]=v_top;  n++;
                px[n]=fibt_x; py[n]=fyt; pz[n]=fibt_z; uu[n]=u1; vv[n]=v_top;  n++;
            }
        }
    }

    /* [R9 RAILFIX] BOTH sides can now be refused (the left side was previously
     * unrefusable, which is what let the centroid loop below divide by n
     * unguarded). Emit nothing rather than a zero-vertex mesh -- and tell the
     * caller, so it does not record a mesh offset pointing at no bytes. */
    if (n == 0) return 1;
    *emitted = 1;

    cx = cy = cz = 0.0;
    for (i = 0; i < n; i++) { cx += px[i]; cy += py[i]; cz += pz[i]; }
    cx /= n; cy /= n; cz /= n;
    for (i = 0; i < n; i++) {
        double dx = px[i]-cx, dy = py[i]-cy, dz = pz[i]-cz;
        double d = sqrt(dx*dx + dy*dy + dz*dz);
        if (d > radius) radius = d;
    }
    if (!(radius > 0.0)) radius = 1.0;

    tg_put_u16(blk, 259);
    tg_put_u16(blk, 0);                    /* opaque, not a billboard */
    tg_put_u32(blk, 1);
    tg_put_u32(blk, (unsigned)n);
    tg_put_f32(blk, radius);
    tg_put_f32(blk, cx); tg_put_f32(blk, cy); tg_put_f32(blk, cz);
    tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
    tg_put_u32(blk, 0);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE + TD5_TG_CMD_SIZE);
    tg_put_u32(blk, 0);

    tg_put_u16(blk, 0);                    /* dispatch_type 0 */
    {
        const int rp = tg_rail_page(si);
        tg_put_u16(blk, (unsigned)rp);
        if (tg_page_is_r8_variety(rp)) tg_acct(TG_ACCT_R8_VARIETY, si);
        tg_var_note(TG_VAR_RAIL, rp);
    }
    tg_put_u32(blk, 0);
    tg_put_u16(blk, 0);                    /* triangle_count */
    /* 3 quads per side, but the safeguard above may have dropped the right one,
     * so DERIVE the count from the vertices actually written rather than
     * hardcoding 6 -- a quad count that overruns the vertex array is read as
     * garbage geometry, not as a missing rail. */
    tg_put_u16(blk, (unsigned)(n / 4));
    tg_put_u32(blk, 0);

    for (i = 0; i < n; i++) {
        tg_put_f32(blk, px[i]); tg_put_f32(blk, py[i]); tg_put_f32(blk, pz[i]);
        tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
        tg_put_u32(blk, 0xFFFFFFFFu);
        tg_put_f32(blk, uu[i]); tg_put_f32(blk, vv[i]);
        tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
    }
    /* One mesh, but one rail per side and the right side can be skipped over a
     * fork, so count RAILS. Counted as the loop runs rather than divided out of
     * the vertex total: since [R12 item 14b] a rail is 12 verts OR 16 OR 20
     * depending on how many of its ends are capped, so n/12 would over- or
     * under-count exactly on the spans the caps land. */
    tg_acct_n(TG_ACCT_GUARDRAIL, si, rails);
    return !blk->oom;
}

static int tg_r4_banner_fix(void)
{
    return td5_env_flag_on("TD5RE_R4_BANNER_FIX");   /* default ON */
}

/* [R5 STRUCT item 1] Point the gantry LEGS at their own solid concrete page
 * (TD5_TG_PAGE_R5_LEG) instead of the bleeding 12% window of the shared BANNER
 * page. Default ON; =0 pins the round-4 legs-on-BANNER for a single-variable
 * A/B of the leg texture. */
static int tg_r5_leg_fix(void)
{
    return td5_env_flag_on("TD5RE_R5_STRUCT_LEG");   /* default ON */
}

/* Push one axis-aligned-in-the-road-frame quad into the caller's arrays. */
static void tg_gantry_quad(double *px, double *py, double *pz,
                           double *uu, double *vv, int *n,
                           const double *x, const double *y, const double *z,
                           double u0, double u1, double v0, double v1)
{
    const double us[4] = { u0, u1, u1, u0 };
    const double vs[4] = { v0, v0, v1, v1 };
    int i;
    for (i = 0; i < 4; i++) {
        px[*n] = x[i]; py[*n] = y[i]; pz[*n] = z[i];
        uu[*n] = us[i]; vv[*n] = vs[i];
        (*n)++;
    }
}

/* [R8 G1 "different start/finish banners"] Which banner page a gantry half
 * uses. `finish` picks FINISH over START, `half` picks the R page over the L.
 *
 * The gantry used to hardcode the four Keswick pages, so every generated track
 * in every biome at every time of day carried the same red-on-white board.
 * Three sets now exist and the choice is per TRACK (a start and a finish banner
 * that did not match each other would read as a bug, not as variety):
 *
 *   0  the shipped Keswick set          level001 337/338 + 369/370
 *   1  the ONE other word set in TD5    level003 499/500 + 439/440
 *   2  the Keswick set, palette-darkened for a night track
 *
 * Set 1 is not a guess: the R8 survey scanned every page of every shipped level
 * for banner art (see gen_trackgen_r8var_tex.py) and found the Keswick set
 * duplicated verbatim into six other levels plus exactly one genuinely
 * different word set, level003's black serif on pale blue steel. It splits each
 * word over two pages the same way, so it drops into the two-half-quad panel
 * with no layout change. level013 page 222 is a third style but puts the whole
 * word on ONE page, which this panel cannot render, so it is left out.
 *
 * A NIGHT track always takes set 2 rather than rolling: a white board under a
 * night sky was the mismatch the day/night sky work is fixing one line up, and
 * the same argument applies to the brightest object on the track.
 *
 * TD5RE_R8_VARIETY_BANNERS=0 restores the single hardcoded set for an A/B. */
static int tg_banner_page(int finish, int half)
{
    static const int k_orig[4] = { TD5_TG_PAGE_START_L, TD5_TG_PAGE_START_R,
                                   TD5_TG_PAGE_FINISH_L, TD5_TG_PAGE_FINISH_R };
    const int slot = (finish ? 2 : 0) + (half ? 1 : 0);
    unsigned int h;
    if (!td5_env_flag_on("TD5RE_R8_VARIETY_BANNERS")) return k_orig[slot];
    if (td5_trackgen_is_night()) return TD5_TG_PAGE_R8V_BANNER_NIGHT + slot;
    h = tg_gen_seed() * 2654435761u;
    return ((h >> 19) & 1u) ? TD5_TG_PAGE_R8V_BANNER + slot : k_orig[slot];
}

/* Gantry across span si. `finish` selects the FINISH artwork over the START
 * artwork. Returns 0 on OOM. */
static int tg_emit_gantry(const TG_NodeList *nl, int si, TG_Buf *blk, int finish)
{
    double lx, ly, lz, rx, ry, rz;     /* road edge at the span's near row */
    double dx, dz, len;                /* unit lateral, left-positive      */
    double tx, tz;                     /* unit along the road              */
    double px[64], py[64], pz[64], uu[64], vv[64];
    double qx[4], qy[4], qz[4];
    double base_y;
    /* One command per page, in vertex order: the frame first, then the two
     * halves of the word (see the panel block). [R5 item 1] a 4th segment when
     * the legs move to their own page (legs page, cap page, then the two word
     * halves). */
    int seg_page[4], seg_nq[4];
    int nseg = 3, cap_n = 0;
    int n = 0, i;
    const int    legfix = tg_r5_leg_fix();
    const double lue     = legfix ? TD5_TG_FACADE_UV_INSET : 0.0;
    /* [R4 item 1] one flag drives both halves of the fix so the A/B is single
     * variable. leg_w/leg_d square the posts up; `out` moves the feet outboard
     * to match; `ins` insets the panel UVs to kill the centre seam. */
    const int    fix   = tg_r4_banner_fix();
    const double leg_w = fix ? TD5_TG_GANTRY_LEG_W2 : TD5_TG_GANTRY_LEG_W;
    const double leg_d = fix ? TD5_TG_GANTRY_LEG_D2 : TD5_TG_GANTRY_THICK;
    const double out   = fix ? TD5_TG_GANTRY_OUT2   : TD5_TG_GANTRY_OUT;
    const double ins   = fix ? TD5_TG_FACADE_UV_INSET : 0.0;

    tg_road_edge(nl, si, 0.0, 0.0, 1.0, &lx, &ly, &lz, &rx, &ry, &rz);
    dx = lx - rx; dz = lz - rz;
    len = sqrt(dx * dx + dz * dz);
    if (len < 1e-6) return 1;          /* degenerate span: nothing to straddle */
    dx /= len; dz /= len;
    /* Along-road unit is the lateral rotated 90 deg (left of travel is
     * (tz,-tx), so travel is (-dz, dx) in the same convention). */
    tx = -dz; tz = dx;
    /* Legs stand on the LOWER of the two edges so neither foot floats on a
     * cambered or graded span. */
    base_y = (ly < ry) ? ly : ry;

    /* --- two uprights, each a 4-quad box (no top/bottom: the panel covers the
     * top and the ground covers the bottom) --- */
    for (i = 0; i < 2; i++) {
        const double ex = i ? rx : lx, ez = i ? rz : lz;
        const double s  = i ? -1.0 : 1.0;              /* outboard direction */
        const double cxx = ex + dx * s * out;
        const double czz = ez + dz * s * out;
        const double y0 = base_y - 40.0;               /* sunk, no gap        */
        const double y1 = base_y + TD5_TG_GANTRY_CLEAR + TD5_TG_GANTRY_PANEL_H;
        int face;
        /* Four faces of the post, each spanned by the lateral or the along-road
         * axis; the page's V runs up the post. */
        for (face = 0; face < 4; face++) {
            /* (a,b) = the in-plane axis, (c) = the fixed offset axis. */
            const double ax = (face < 2) ? dx : tx, az = (face < 2) ? dz : tz;
            const double ox = (face < 2) ? tx : dx, oz = (face < 2) ? tz : dz;
            const double half = (face < 2) ? leg_w : leg_d;
            const double off  = ((face & 1) ? -1.0 : 1.0)
                              * ((face < 2) ? leg_d : leg_w);
            qx[0] = cxx + ax * half + ox * off; qz[0] = czz + az * half + oz * off;
            qx[1] = cxx - ax * half + ox * off; qz[1] = czz - az * half + oz * off;
            qx[2] = qx[1];                      qz[2] = qz[1];
            qx[3] = qx[0];                      qz[3] = qz[0];
            qy[0] = y0; qy[1] = y0; qy[2] = y1; qy[3] = y1;
            /* [R5 item 1] On the dedicated leg page the whole 64x64 is concrete,
             * so map the face across the page inset by half a texel (no border
             * fetch). Without the fix, the legacy 0..0.12 window of the BANNER
             * page -- the source of the reported edge bleed. */
            tg_gantry_quad(px, py, pz, uu, vv, &n, qx, qy, qz,
                           legfix ? lue : 0.0, legfix ? 1.0 - lue : 0.12,
                           legfix ? 1.0 - lue : 1.0, legfix ? lue : 0.0);
        }
    }
    cap_n = n;   /* verts so far are all legs; the cap follows on its own page */

    /* Underside cap of the panel, emitted here so the whole steel FRAME (legs +
     * cap) is one contiguous run of quads and therefore one command. */
    {
        const double y0 = base_y + TD5_TG_GANTRY_CLEAR;
        const double ox = lx + dx * out;
        const double oz = lz + dz * out;
        const double kx = rx - dx * out;
        const double kz = rz - dz * out;
        qx[0] = ox + tx * TD5_TG_GANTRY_THICK; qz[0] = oz + tz * TD5_TG_GANTRY_THICK;
        qx[1] = kx + tx * TD5_TG_GANTRY_THICK; qz[1] = kz + tz * TD5_TG_GANTRY_THICK;
        qx[2] = kx - tx * TD5_TG_GANTRY_THICK; qz[2] = kz - tz * TD5_TG_GANTRY_THICK;
        qx[3] = ox - tx * TD5_TG_GANTRY_THICK; qz[3] = oz - tz * TD5_TG_GANTRY_THICK;
        qy[0] = y0; qy[1] = y0; qy[2] = y0; qy[3] = y0;
        tg_gantry_quad(px, py, pz, uu, vv, &n, qx, qy, qz, 0.0, 1.0, 0.9, 1.0);
    }
    /* [R5 item 1] With the leg fix the legs are one command on the LEG page and
     * the underside cap stays on the BANNER page (its chequer end block); the
     * two word halves follow. Without it, legs+cap are one BANNER command as
     * before. */
    if (legfix) {
        seg_page[0] = TD5_TG_PAGE_R5_LEG;  seg_nq[0] = cap_n / 4;       /* legs */
        seg_page[1] = TD5_TG_PAGE_BANNER;  seg_nq[1] = (n - cap_n) / 4; /* cap  */
        nseg = 4;
    } else {
        seg_page[0] = TD5_TG_PAGE_BANNER;
        seg_nq[0]   = n / 4;
        nseg = 3;
    }

    /* --- panel: a slab bridging the two legs, carrying the SHIPPED START or
     * FINISH artwork. Front and back faces are separated by
     * TD5_TG_GANTRY_THICK so they cannot z-fight.
     *
     * [FB r2 item 12] The panel used to be a procedural chequer. Shipped TD5
     * banners are photographic pages that spell the word out, and the word does
     * not fit one 64x64 page: Keswick splits START over pages 337+338 and
     * FINISH over 369+370 (each with its own chequer end block), laid side by
     * side. So the panel is TWO half quads per face, each mapping one whole
     * page -- never one page stretched across the road, which is the mistake
     * the facade survey warned about.
     *
     * [FIX 2026-08-27 -- BANNER READ "TRATS" IN FRAME] There used to be a BACK
     * face too, carrying the same word readable from behind (halves swapped
     * pages and u reversed, because from behind the world-right half is what a
     * viewer sees on their left). It was separated from the front by
     * TD5_TG_GANTRY_THICK and the comment here claimed that meant they "cannot
     * z-fight". They can, and they did: THICK is 160 raw = 0.625 world units,
     * and the banner is first seen from tens of thousands of units away against
     * a 195000 far plane, so that separation is well inside depth-buffer noise.
     * The back face won, and since its halves are page-swapped AND u-reversed,
     * what you read from the car was the whole word mirrored: "TRATS".
     * Framedump: log/R_post1.png.
     *
     * This is the same failure the shipped-track banners hit (Keswick page 338,
     * Blue Ridge page 156, both z-fighting under the wrapper's global
     * CULL_MODE_NONE); there the cure was one-sided culling, NOT more
     * separation -- which is the evidence that separation does not hold up here.
     *
     * So the back face is gone. A start/finish gantry is read by a driver
     * APPROACHING the line, the back face served only legibility from behind
     * (which nobody needs while racing the right way round), and dropping it
     * makes the z-fight impossible by construction instead of merely unlikely.
     * Scenery is submitted CULL_NONE, so the single remaining face is still
     * drawn from both sides -- it just reads mirrored from behind, exactly like
     * the shipped one-sided banners do. Two half quads, one per page, emitted
     * in page order so each page stays one contiguous command run. --- */
    {
        const double y0 = base_y + TD5_TG_GANTRY_CLEAR;
        const double y1 = y0 + TD5_TG_GANTRY_PANEL_H;
        const double ox = lx + dx * out;   /* left  end */
        const double oz = lz + dz * out;
        const double kx = rx - dx * out;   /* right end */
        const double kz = rz - dz * out;
        const double mx = 0.5 * (ox + kx), mz = 0.5 * (oz + kz);
        /* (a-end, b-end, face sign, u at a, u at b) for the two half quads.
         * Row order IS the page order: row 0 is the L page, row 1 the R page,
         * which is what makes each page one command. */
        const double ends[2][5] = {
            { 0.0, 1.0,  1.0, 0.0, 1.0 },   /* front, left  half */
            { 1.0, 2.0,  1.0, 0.0, 1.0 }    /* front, right half */
        };
        const double ptx[3] = { ox, mx, kx };
        const double ptz[3] = { oz, mz, kz };
        int q;
        for (q = 0; q < 2; q++) {
            const int    ia = (int)ends[q][0], ib = (int)ends[q][1];
            const double sf = ends[q][2];
            qx[0] = ptx[ia] + tx * sf * TD5_TG_GANTRY_THICK;
            qz[0] = ptz[ia] + tz * sf * TD5_TG_GANTRY_THICK;
            qx[1] = ptx[ib] + tx * sf * TD5_TG_GANTRY_THICK;
            qz[1] = ptz[ib] + tz * sf * TD5_TG_GANTRY_THICK;
            qx[2] = qx[1]; qz[2] = qz[1];
            qx[3] = qx[0]; qz[3] = qz[0];
            qy[0] = y0; qy[1] = y0; qy[2] = y1; qy[3] = y1;
            /* [R4 item 1] Inset the panel UVs by half a texel. Each half maps
             * one whole 64x64 page (u 0->1); at the panel centre the L page's
             * right edge (u=1) abuts the R page's left edge (u=0), and sampling
             * those extreme columns -- wrapped, they fetch the OPPOSITE edge --
             * draws the reported vertical line. e0..e1 keeps every fetch off the
             * page border. ins=0 restores the raw 0/1 for the A/B. */
            tg_gantry_quad(px, py, pz, uu, vv, &n, qx, qy, qz,
                           ends[q][3] + (ends[q][4] - ends[q][3]) * ins,
                           ends[q][4] - (ends[q][4] - ends[q][3]) * ins,
                           1.0 - ins, ins);
        }
        /* Panel halves are the last two commands; their index shifts by one when
         * the legs took their own page ahead of the cap ([R5 item 1]). */
        {
            const int ps = legfix ? 2 : 1;
            seg_page[ps]     = tg_banner_page(finish, 0);
            seg_page[ps + 1] = tg_banner_page(finish, 1);
            if (tg_page_is_r8_variety(seg_page[ps]))
                tg_acct(TG_ACCT_R8_VARIETY, si);
            tg_var_note(TG_VAR_BANNER, seg_page[ps]);
            tg_var_note(TG_VAR_BANNER, seg_page[ps + 1]);
            seg_nq[ps]       = 1;  /* one front half per page (back face dropped) */
            seg_nq[ps + 1]   = 1;
        }
    }

    tg_acct(TG_ACCT_BANNER, si);
    return tg_write_quad_mesh(blk, px, py, pz, uu, vv, n, seg_page, seg_nq, nseg);
}

/* Group E -- track furniture: start/finish banners, branch mouths, run-off.
 * Exactly two gantries per track: one on the grid span and one on the finish
 * span (tg_finish_span, the same span the last LEVELINF checkpoint sits on --
 * so the banner is over the line that actually ends the race, not near it).
 * Default ON; TD5RE_AUTOTRACK_BANNERS=0 removes both. */
int tg_emit_fb_track(const TG_FBHook *h)
{
    const int ring = (s_ring_len > 0) ? s_ring_len : h->nspans;
    const int finish = tg_finish_span(ring);

    if (h->si != TD5_TG_GRID_SPAN && h->si != finish) return 1;
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_BANNERS")) return 1;
    if (*h->nmesh + 1 >= h->maxmesh) return 1;
    h->moff[(*h->nmesh)++] = h->blk->len;
    /* The finish span wins a tie: on a circuit the grid span and the finish can
     * be the same span, and that gantry marks the end of the race. */
    return tg_emit_gantry(h->nl, h->si, h->blk, h->si == finish);
}

int tg_emit_end_wall(const TG_NodeList *nl, int si, int at_far,
                            size_t *moff, int *nmesh, int maxmesh, TG_Buf *blk)
{
    double px[4], py[4], pz[4], uu[4], vv[4];
    double lx, ly, lz, rx, ry, rz;
    int seg_page = TD5_TG_PAGE_WALL, seg_nq = 1;
    const double f = at_far ? 1.0 : 0.0;

    if (si < 0 || si + 1 >= nl->count) return 1;
    if (*nmesh >= maxmesh) return 1;
    tg_road_edge(nl, si, f, 0.0, 1.0, &lx, &ly, &lz, &rx, &ry, &rz);

    /* Quad loop: left-bottom, right-bottom, right-top, left-top. u spans the
     * road width twice so the brick reads at a believable scale; v 0..1 up. */
    px[0] = lx; py[0] = ly;                    pz[0] = lz; uu[0] = 0.0; vv[0] = 1.0;
    px[1] = rx; py[1] = ry;                    pz[1] = rz; uu[1] = 2.0; vv[1] = 1.0;
    px[2] = rx; py[2] = ry + TD5_TG_ENDWALL_H; pz[2] = rz; uu[2] = 2.0; vv[2] = 0.0;
    px[3] = lx; py[3] = ly + TD5_TG_ENDWALL_H; pz[3] = lz; uu[3] = 0.0; vv[3] = 0.0;

    moff[(*nmesh)++] = blk->len;
    return tg_write_quad_mesh(blk, px, py, pz, uu, vv, 4, &seg_page, &seg_nq, 1);
}

/* Fork whose MAIN half-carriageway covers main-ring span si, or -1. */
int tg_fork_of_main(int si)
{
    int i;
    for (i = 0; i < s_fork_count; i++)
        if (si > s_forks[i].F && si <= s_forks[i].F + s_forks[i].len) return i;
    return -1;
}

/* Fork whose appended CORRIDOR covers span si (sets *k = corridor step), or -1. */
int tg_fork_of_corridor(int si, int *k)
{
    int i;
    for (i = 0; i < s_fork_count; i++) {
        int lo = s_forks[i].cbase, hi = lo + s_forks[i].len - 1;
        if (si >= lo && si <= hi) { if (k) *k = si - lo; return i; }
    }
    return -1;
}

long s_r11_signs;            /* panels emitted, reported per build */

long s_r11_sign_skip_lamp;   /* dropped: a lamp already stands there */

long s_r11_sign_skip_street; /* dropped: the mouth of a side street  */

long s_r11_sign_skip_side;   /* dropped: fork corridor on that side  */

long s_r11_sign_left;        /* arrow census, per page               */

long s_r11_sign_right;

int tg_r11_signs_enabled(void) { return td5_env_flag_on("TD5RE_R11_SIGNS"); }

/* The bend this span's sign describes, or 0 if there is none. Returns the
 * s_turn_side value at the bend (+1 left of travel, -1 right). */
static int tg_r11_sign_turn_for(int si, int nspans)
{
    const int t = si + TD5_TG_R11_SIGN_LEAD;
    if (si <= 0 || t >= nspans || t >= TD5_TG_MAX_SPANS) return 0;
    return (int)s_turn_side[t];
}

/* One direction sign at span si: a fixed panel facing back down the road, on a
 * modelled post. Appends 0, or 2 meshes (post then panel) and records both
 * offsets. Returns 0 only on a buffer failure. */
int tg_emit_r11_sign(const TG_NodeList *nl, int si, int nspans,
                            TG_Buf *blk, size_t *moff, int *nmesh, int maxmesh)
{
    const TG_Node *n = &nl->v[si];
    const TG_Biome *b;
    double px[8], py[8], pz[8], uu[8], vv[8];
    double lx, lz, cx, cz, base_y, gap, sw;
    double hw = TD5_TG_R11_SIGN_W * 0.5;
    double y0, y1, ptop;
    int turn, side_i, page, seg_page, seg_nq;

    if (!tg_r11_signs_enabled())          return 1;
    if (si <= TD5_TG_GRID_SPAN)           return 1;  /* keep the start grid clear */
    if (si + 1 >= nl->count)              return 1;
    if (tg_span_in_bridge_run(si))        return 1;  /* deck carries rails only  */
    if (tg_span_in_tunnel(si))            return 1;  /* no verge inside a bore   */
    if (tg_up_clear_span(si))             return 1;  /* overpass deck flies here */
    if (*nmesh + 2 > maxmesh)             return 1;  /* budget, not an error     */

    turn = tg_r11_sign_turn_for(si, nspans);
    if (!turn) return 1;

    /* s_turn_side IS the outside of the bend, which is both where the sign
     * belongs and which way the road goes. */
    side_i = turn;
    if (tg_side_blocked(si, (double)side_i)) { s_r11_sign_skip_side++; return 1; }

    /* The lamp head is the one other thing at panel height in this band, and the
     * inventory for si is complete by the time this emitter runs. */
    if (TG_ACCT_MASK_TEST(si, TG_ACCT_LAMP)) { s_r11_sign_skip_lamp++; return 1; }

    /* No footway at a side-street mouth to stand on, so nothing is placed rather
     * than a sign being shoved down the street -- the R10 furniture rule. */
    if (tg_xstreet_occupies(nl, si, (double)side_i,
                            TD5_TG_R11_SIGN_GAP - hw)) {
        tg_xstreet_audit(nl, si, (double)side_i,
                         TD5_TG_R11_SIGN_GAP - hw, "direction-sign", 0);
        s_r11_sign_skip_street++;
        return 1;
    }
    tg_xstreet_audit(nl, si, (double)side_i, TD5_TG_R11_SIGN_GAP - hw,
                     "direction-sign", 1);

    /* Push the whole FOOTPRINT clear of any branch carriageway bowing into this
     * lateral, exactly as the pavement, the trees and the R9 furniture do.
     *
     * THE AUTHORITY IS ASKED ABOUT THE FOOTPRINT'S INNER EDGE, and the half
     * width is added back afterwards to recover the CENTRE. Passing
     * `centre + hw` and using the result as the centre -- which is what
     * tg_infra_place does -- silently moves the piece a half width further out
     * than its own constant says: measured here as gap=614 for a nominal 430,
     * i.e. a footprint of 430..800 instead of 245..615. That mattered, because
     * 800 is exactly where the tree band starts, so the "clear of the trees"
     * property this emitter's band was chosen for was not actually held. */
    gap = tg_carriageway_clear_gap(nl, si, (double)side_i,
                                   TD5_TG_R11_SIGN_GAP - hw,
                                   TD5_TG_CARRIAGEWAY_MARGIN) + hw;

    b  = &k_biomes[tg_scenery_biome_index(si)];
    sw = tg_city_sidewalk_w_at(nl, si, b);
    /* Paved: stand on the kerb. Unpaved: the skirt DROPS away from the road, so
     * road height is the wrong footing and the post would hang in the air --
     * rest on the sampled terrain, the rule R7 FLORA gave tree trunks. */
    if (sw > 0.0) base_y = n->y + tg_city_kerb_h(b);
    else          base_y = n->y - tg_infra_ground_dy(nl, si, (double)side_i,
                                                     gap, 0.0);

    lx = n->tz * (double)side_i;
    lz = -n->tx * (double)side_i;
    cx = n->x + lx * (n->width * 0.5 + gap);
    cz = n->z + lz * (n->width * 0.5 + gap);

    y0   = base_y + TD5_TG_R11_SIGN_LIFT;
    y1   = y0 + TD5_TG_R11_SIGN_H;
    ptop = y0 + TD5_TG_R11_SIGN_POST_OV;

    /* POST: two crossed quads rather than a four-face box. At 6 cm the silhouette
     * is all a driver can resolve, and a cross halves the geometry of a box for
     * an identical read. Quad A spans the lateral axis, quad B the along-road
     * axis; scenery is submitted CULL_NONE so winding only has to be
     * self-consistent. */
    {
        const double ax = n->tx, az = n->tz;
        const double h  = TD5_TG_R11_SIGN_POST_W * 0.5;
        int k = 0;
        px[k] = cx - lx * h; py[k] = base_y; pz[k] = cz - lz * h;
        uu[k] = 0.0; vv[k] = 1.0; k++;
        px[k] = cx + lx * h; py[k] = base_y; pz[k] = cz + lz * h;
        uu[k] = 1.0; vv[k] = 1.0; k++;
        px[k] = cx + lx * h; py[k] = ptop;   pz[k] = cz + lz * h;
        uu[k] = 1.0; vv[k] = 0.0; k++;
        px[k] = cx - lx * h; py[k] = ptop;   pz[k] = cz - lz * h;
        uu[k] = 0.0; vv[k] = 0.0; k++;
        px[k] = cx - ax * h; py[k] = base_y; pz[k] = cz - az * h;
        uu[k] = 0.0; vv[k] = 1.0; k++;
        px[k] = cx + ax * h; py[k] = base_y; pz[k] = cz + az * h;
        uu[k] = 1.0; vv[k] = 1.0; k++;
        px[k] = cx + ax * h; py[k] = ptop;   pz[k] = cz + az * h;
        uu[k] = 1.0; vv[k] = 0.0; k++;
        px[k] = cx - ax * h; py[k] = ptop;   pz[k] = cz - az * h;
        uu[k] = 0.0; vv[k] = 0.0; k++;
        seg_page = TD5_TG_PAGE_R11_SIGN_POST;
        seg_nq   = 2;
        moff[(*nmesh)++] = blk->len;
        if (!tg_write_quad_mesh(blk, px, py, pz, uu, vv, 8,
                                &seg_page, &seg_nq, 1))
            return 0;
    }

    /* PANEL: a FIXED quad spanning the lateral axis, so its face normal lies
     * along the road and it presents itself square to an approaching driver.
     *
     * FIXED, NOT CAMERA-FACING, and this is a deliberate choice against the
     * billboard default every other piece of verge scenery here uses. Three
     * reasons. It is bolted to a post that is modelled and cannot rotate, so a
     * panel that swivelled while its post stood still would be a visible
     * artifact rather than a subtle one. A direction sign is read from ONE
     * approach and that approach is known exactly -- it is the span tangent --
     * so there is nothing for camera-facing to buy. And scenery is submitted
     * CULL_NONE, so the back face draws too and the sign never vanishes when
     * seen from behind; this is the same conclusion the start/finish gantry
     * reached when it went double-sided instead of camera-facing. */
    page = (side_i < 0) ? TD5_TG_PAGE_R11_SIGN_LEFT
                        : TD5_TG_PAGE_R11_SIGN_RIGHT;
    if (side_i < 0) s_r11_sign_left++; else s_r11_sign_right++;

    px[0] = cx - lx * hw; py[0] = y0; pz[0] = cz - lz * hw;
    uu[0] = 0.0; vv[0] = 1.0;
    px[1] = cx + lx * hw; py[1] = y0; pz[1] = cz + lz * hw;
    uu[1] = 1.0; vv[1] = 1.0;
    px[2] = cx + lx * hw; py[2] = y1; pz[2] = cz + lz * hw;
    uu[2] = 1.0; vv[2] = 0.0;
    px[3] = cx - lx * hw; py[3] = y1; pz[3] = cz - lz * hw;
    uu[3] = 0.0; vv[3] = 0.0;
    seg_page = page;
    seg_nq   = 1;
    moff[(*nmesh)++] = blk->len;
    if (!tg_write_quad_mesh(blk, px, py, pz, uu, vv, 4, &seg_page, &seg_nq, 1))
        return 0;

    tg_acct_n(TG_ACCT_R11_SIGNS, si, 1);
    s_r11_signs++;
    if (td5_env_flag_off("TD5RE_R11_SIGNS_REPORT"))
        TD5_LOG_I(LOG_TAG, "trackgen: [R11 SIGNS] span %4d %-5s %-8s bend at "
                  "%4d gap=%.0f base=%.0f biome=%s",
                  si, side_i > 0 ? "left" : "right",
                  side_i < 0 ? "LEFT" : "RIGHT", si + TD5_TG_R11_SIGN_LEAD,
                  gap, base_y, b->name);
    return 1;
}
