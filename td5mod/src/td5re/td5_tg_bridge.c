/**
 * td5_tg_bridge.c -- auto-track BRIDGES, TUNNELS, OVERPASSES and WATER: water plane, bore vs underpass, portal, overpass, run coalesce, structural tie, median faces, measurement harness
 *
 * Split from the td5_trackgen.c monolith (2026-09-06); shared declarations
 * live in td5_trackgen_internal.h. Element map: docs/plans/AUTOTRACK_ELEMENT_CATALOG.md.
 */
#include "td5_trackgen_internal.h"

/* Which side of a coastal run the sea is on -- fixed per biome-run so the coast
 * does not flip sides mid-stretch. */
/* [R8 BIOME item 19] Keyed on the FIRST CELL OF THE MERGED RUN, not on the raw
 * cell. With COAST at repeat_max 1 a merged run is exactly one cell and this is
 * bit-identical to the old expression; once TD5RE_R8_BIOME_SEA lifts the cap it
 * is what stops a multi-cell coast flipping the sea from left to right halfway
 * along. See the ONE-SIDE SEA note above tg_biome_repeat_max. */
/* [R9 item 10] Does span si's biome carry a sea plane? Wraps the biome table so
 * the over-water audit up in the guard block can ask without the table. */
int tg_biome_span_has_water(int si)
{
    return k_biomes[tg_biome_for_span(si)].water ? 1 : 0;
}

double tg_water_side(int si)
{
    int a;
    unsigned int h;
    tg_biome_run_bounds(si, &a, NULL);
    h = (unsigned)(a / TD5_TG_BIOME_RUN) * 2246822519u;
    return (h & 1) ? 1.0 : -1.0;
}

/* Sea level for the coastal run containing span si -- ONE height for the whole
 * run, not per span.
 *
 * Root cause of the "marching" sea: the surface used to be n->y - WATER_DROP,
 * i.e. it followed the road's own elevation profile, so the sea rose and fell
 * with every hill. A body of water is level by definition, so take the LOWEST
 * road node in the biome run and sit below that -- below the road everywhere in
 * the run, so no low point is ever flooded. */
double tg_sea_level_y(const TG_NodeList *nl, int si)
{
    /* [R8 BIOME item 19] Lowest node of the MERGED run, not of the raw cell.
     * A no-op while COAST is capped at one cell; with TD5RE_R8_BIOME_SEA on it
     * is what keeps one body of water at ONE height across a multi-cell coast
     * instead of stepping at each cell boundary. */
    int a, b;
    double lo;
    int i;

    tg_biome_run_bounds(si, &a, &b);

    if (a > nl->count - 1) a = nl->count - 1;
    if (b > nl->count - 1) b = nl->count - 1;
    lo = nl->v[a].y;
    for (i = a + 1; i <= b; i++) if (nl->v[i].y < lo) lo = nl->v[i].y;
    return lo - (double)TD5_TG_WATER_DROP;
}

/* May a water quad be laid across span si at all?
 *
 * Reported: "i see water on top of the tunnel". Neither water emitter consulted
 * the tunnel gate. Both are called from the NON-tunnel branch of the emit loop,
 * which looks like it settles the question but does not: every water quad runs
 * from node si to node si+1, so the last open span before a portal lays its
 * plane across the FIRST span of the tunnel run. The sea gets away with it (it
 * starts TD5_TG_WATER_BEACH outboard of the road edge, so it never reaches the
 * bore) but the river spans the full TD5_TG_BRIDGE_WATER_HALF either side of the
 * centreline and goes straight through it.
 *
 * A tunnel is an enclosed section cut into rock; nothing floats over it and
 * nothing runs through it. Cheaper and more honest to refuse the quad than to
 * try to clip it. TD5RE_AUTOTRACK_WATER_TUNNEL_GATE=0 restores the old
 * behaviour for comparison. */
int tg_water_span_clear(int si)
{
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_WATER_TUNNEL_GATE")) return 1;
    return !tg_span_in_tunnel(si) && !tg_span_in_tunnel(si + 1);
}

/* [R11 WATER] How far out from the CENTRELINE the sea plane reaches at node si
 * -- the outer edge tg_emit_water lays, expressed in the same axis the bridge
 * river's BRIDGE_WATER_HALF is expressed in, so the two footprints can be
 * compared and matched instead of being two unrelated numbers. Single
 * definition: the emitter below, the diagnostic and the R11 widening all read
 * it, so "as far as the sea" cannot drift from what the sea actually does. */
double tg_r11_sea_outer(const TG_NodeList *nl, int si)
{
    if (si < 0) si = 0;
    if (si > nl->count - 1) si = nl->count - 1;
    return nl->v[si].width * 0.5 + (double)TD5_TG_WATER_BEACH
         + (double)TD5_TG_WATER_EXTENT;
}

/* Emit the sea plane beside span si on `side`, recording its mesh offset.
 *
 * ONE quad, and its UVs come from the WORLD x/z of each corner rather than
 * running 0..1 per cell. Both changes fix the same fault: a 2 x 6 cell grid per
 * span mapped the page once per cell, so every cell boundary was a seam and the
 * whole field of them slid past as a marching pattern. Projecting the page onto
 * the world XZ grid instead makes neighbouring spans agree at their shared edge
 * by construction, so the sea reads as one continuous body. It is also 4
 * vertices instead of 48. */
int tg_emit_water(const TG_NodeList *nl, int si, double side,
                         TG_Buf *m, size_t *moff, int *pn)
{
    double px[4], py[4], pz[4], uu[4], vv[4];
    const TG_Node *n0 = &nl->v[si];
    const TG_Node *n1;
    double lx0, lz0, lx1, lz1, e0, e1, wy;
    int i, seg_page = TD5_TG_PAGE_WATER, seg_nq = 1;

    if (si + 1 >= nl->count) return 1;
    if (tg_side_blocked(si, side)) return 1;
    if (!tg_water_span_clear(si)) return 1;
    n1 = &nl->v[si + 1];

    lx0 = n0->tz * side; lz0 = -n0->tx * side;
    lx1 = n1->tz * side; lz1 = -n1->tx * side;
    e0 = n0->width * 0.5 + (double)TD5_TG_WATER_BEACH;
    e1 = n1->width * 0.5 + (double)TD5_TG_WATER_BEACH;
    wy = tg_sea_level_y(nl, si);

    /* shore-near, shore-far, sea-far, sea-near: the same ring order the cell
     * grid produced, so the face keeps whatever winding was drawing before. */
    px[0] = n0->x + lx0 * e0;                        pz[0] = n0->z + lz0 * e0;
    px[1] = n1->x + lx1 * e1;                        pz[1] = n1->z + lz1 * e1;
    px[2] = px[1] + lx1 * (double)TD5_TG_WATER_EXTENT;
    pz[2] = pz[1] + lz1 * (double)TD5_TG_WATER_EXTENT;
    px[3] = px[0] + lx0 * (double)TD5_TG_WATER_EXTENT;
    pz[3] = pz[0] + lz0 * (double)TD5_TG_WATER_EXTENT;
    for (i = 0; i < 4; i++) {
        py[i] = wy;
        uu[i] = px[i] / TD5_TG_WATER_TILE;
        vv[i] = pz[i] / TD5_TG_WATER_TILE;
    }

    moff[*pn] = m->len;
    if (!tg_write_quad_mesh(m, px, py, pz, uu, vv, 4, &seg_page, &seg_nq, 1))
        return 0;
    (*pn)++;
    tg_acct_range(TG_ACCT_WATER, si, si + 1);   /* sea or river plane */
    return 1;
}

int tg_tunnel_run_len(void)
{
    /* Opt-in, paired with tg_bridge_run_len -- see the reasoning there. */
    return td5_env_flag_off("TD5RE_R8_LONGRUN")
         ? TD5_TG_TUNNEL_RUN_R8 : TD5_TG_TUNNEL_RUN_R3;
}

/* Cell-level biome, NOT tg_biome_for_span: the blended per-span biome dithers
 * across a 20-span band, and a run that was half bore and half underpass would
 * be the worse artifact by far. One run, one decision. */
static int tg_tunnel_run_biome(int si)
{
    const int t0 = (si / TD5_TG_TUNNEL_RUN) * TD5_TG_TUNNEL_RUN;
    return tg_biome_cell_index(t0 + TD5_TG_TUNNEL_RUN / 2);
}

/* The ORIGINAL gate, unchanged: hash, biome weight, bridge interlock. Split out
 * so "which runs are chosen" and "what is built on them" are separately
 * checkable, and so item 13 provably changes only the second. */
static int tg_tunnel_run_selected(int si)
{
    unsigned int h;
    /* OFF BY DEFAULT -- emitted but NEVER VERIFIED IN FRAME. A test run with
     * tunnels on showed a dark slab near the road that turned out to be a tall
     * BUILDING (tunnels off, still present), so no frame has yet confirmed a
     * tunnel appearing at all -- neither working nor broken. Off until someone
     * drives into a known tunnel run and looks.
     *
     * Known RISK, from the format survey rather than observation: there is no
     * engine support for interior darkening or occlusion, so the roof will be
     * lit from outside; and every span in the run gets an identical section,
     * so there is no MOUTH and the near end may read as a wall.
     * Enable with TD5RE_AUTOTRACK_TUNNELS=1 to work on them. */
    /* Default ON (2026-08-26); set TD5RE_AUTOTRACK_TUNNELS=0 to disable. */
    unsigned int thresh;
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_TUNNELS")) return 0;
    if (si <= TD5_TG_GRID_SPAN + 40) return 0;      /* not right off the grid */
    h = (unsigned)(si / TD5_TG_TUNNEL_RUN) * 2246822519u;
    /* Same shape as the bridge gate: 125/1000 is the old ~1-in-8, scaled by the
     * biome weight, so ALPINE at 230 bores ~29% of runs and FIELDS at 10 gets
     * ~1% ("mountains = tunnels"). */
    thresh = (125u * (unsigned)tg_biome_tunnel_pct(si)) / 100u;
    if (thresh > 1000u) thresh = 1000u;
    if (((h >> 8) % 1000u) >= thresh) return 0;     /* this run does not bore */

    /* [R4 item 19] INTERLOCK. The bridge gate (tg_span_in_bridge_run) and this
     * one are independent hash draws with no shared state, so nothing stopped a
     * bridge run and a tunnel run from overlapping -- seed 99991 has bridge run
     * 1320-1359 and tunnel run 1340-1359 both firing, and the raised deck drives
     * straight into the bore, which is illegal terrain. Resolve it one way: the
     * BRIDGE is the authored crossing and keeps its span; the tunnel YIELDS. A
     * tunnel run is suppressed if any span within CLEAR of it lies in a bridge
     * run, which forbids the overlap AND leaves a flat approach band between the
     * two. Stateless and derived only from si, so the elevation pass, the strip
     * builder and every scenery gate agree without passing anything around. A
     * bridge weight tweak could only make the collision rarer; this makes it
     * impossible. */
    {
        const int t0 = (si / TD5_TG_TUNNEL_RUN) * TD5_TG_TUNNEL_RUN;
        const int t1 = t0 + TD5_TG_TUNNEL_RUN - 1;
        int s, lo = t0 - TD5_TG_BRIDGE_TUNNEL_CLEAR;
        const int hi = t1 + TD5_TG_BRIDGE_TUNNEL_CLEAR;
        if (lo < 0) lo = 0;
        for (s = lo; s <= hi; s++)
            if (tg_span_in_bridge_run(s)) return 0;
    }
    return 1;
}

/* What a SELECTED run builds. See the item-13 block above for the rule. */
static int tg_tunnel_kind(int si)
{
    const TG_Biome *b;
    if (!tg_tunnel_run_selected(si)) return TG_TUN_NONE;
    /* A/B escape: pin the pre-R9 behaviour (every selected run is a bore,
     * whatever the biome) so the two builds can be compared byte for byte. */
    if (td5_env_flag_off("TD5RE_R9_UNDERPASS")) return TG_TUN_BORE;
    b = &k_biomes[tg_tunnel_run_biome(si)];
    /* MOUNTAINS means ALPINE: climate cold AND urbanity WILDERNESS.
     *
     * The obvious rule is climate >= 2, and it is wrong -- MEASURED, not
     * reasoned. climate 2 also selects ALPTOWN, and the first frame of a bore in
     * an ALPTOWN run (seed 777 span 473) came out as a stone portal in the
     * middle of a shopping street with railings and shopfronts either side --
     * which is the ORIGINAL COMPLAINT, reproduced by the fix meant to end it.
     * ALPTOWN is cold, but its own table comment calls it a snowy TOWN and it
     * draws on TD5_TG_PAGE_WALL, the city facade page, so it renders urban
     * whatever its climate says. A bore belongs where there is nothing but
     * hillside, and that is urbanity 0 as much as it is climate 2.
     *
     * This is the round's own method rule turned on my own first answer: when a
     * complaint survives an honest measurement, suspect the AXIS. Climate was
     * the wrong axis on its own. */
    if (b->climate >= 2 && b->urbanity == 0) return TG_TUN_BORE;
    /* Anywhere with a settlement -- town, edge, or city -- a crossing road goes
     * OVER, not through. urbanity >= 1 rather than >= 2 so ALPTOWN, FIELDS and
     * COAST are served too: a flyover across a village street, a country lane or
     * a coast road is ordinary, and restricting this to dense urbanity would
     * leave those three biomes with no crossing of any kind. FOREST (urbanity 0,
     * temperate) is the one biome that gets neither, which is right -- there is
     * nothing to tunnel through and nothing to fly over. */
    if (b->urbanity >= 1) return TG_TUN_UNDERPASS;
    return TG_TUN_NONE;
}

/* ENCLOSED: a real bore. Unchanged meaning -- see the item-13 block. */
int tg_span_in_tunnel(int si)
{
    return tg_tunnel_kind(si) == TG_TUN_BORE;
}

static int tg_r12_up_shore(void) { return td5_env_flag_on("TD5RE_R12_UP_SHORE"); }

/* Lateral, from the road EDGE, at which a water biome's shore ramp has fallen
 * through sea level -- i.e. where the ground stops and the water starts.
 * Defined with TD5_TG_SHORE_END down in the terrain code and reached through
 * this accessor because the overpass is compiled above it, the same way
 * tg_far_reach is. One definition, so the arm and the beach cannot disagree
 * about where the coastline is. */
static double tg_shore_end(void);

/* Can a crossing land on span si? Pure function of si, exactly like the
 * predicate it feeds, so no caller needs a node list to ask. */
static int tg_up_span_crossable(int si)
{
    if (si < 0) return 0;
    /* The seaward arm would end over open water -- see the block above. */
    if (tg_biome_span_has_water(si)) return 0;
    /* The road here is a deck over a river; so would the crossing be. */
    if (tg_span_near_bridge(si, TD5_TG_UP_BRIDGE_CLEAR)) return 0;
    return 1;
}

int tg_underpass_span(int si)
{
    const int t0 = (si / TD5_TG_TUNNEL_RUN) * TD5_TG_TUNNEL_RUN;
    const int c  = t0 + TD5_TG_TUNNEL_RUN / 2;
    int d;
    if (tg_tunnel_kind(si) != TG_TUN_UNDERPASS) return -1;
    if (!tg_r12_up_shore()) return c;
    for (d = 0; d <= TD5_TG_UP_SLIDE_MAX; d++) {
        if (c - d >= t0 && tg_up_span_crossable(c - d)) return c - d;
        if (c + d <= t0 + TD5_TG_TUNNEL_RUN - 1 && tg_up_span_crossable(c + d))
            return c + d;
    }
    return -1;                       /* whole run is water or deck: no crossing */
}

/* Lining page for the tunnel RUN containing si (item 16a).
 *
 * The report was that every bore wears the one concrete page, so a long
 * tunnelled stretch reads as a copy-paste. There is no per-bore art to draw on
 * (tunnels are port-original), so instead we generate several concrete linings
 * that differ in tone/grain (tg_emit_texture_page_tunnel_var) and hand each RUN
 * a different one. Keyed on si/RUN, not si, so a single bore is ONE lining end
 * to end -- a lining that changed mid-tunnel would be the worse artifact. The
 * base TD5_TG_PAGE_TUNNEL is variant 0 so the existing page is never wasted. */
static int tg_tunnel_lining_page(int si)
{
    /* [R6 TUNNEL item 8c] Repointed to the R6 cast-concrete lining family (4
     * variants) that replaces the checkerboard-reading grain of the old
     * TD5_TG_PAGE_TUNNEL(_VAR) pages. Still one lining per RUN (keyed on
     * si/RUN) so a bore is a single lining end to end. TD5RE_AUTOTRACK_
     * TUNNEL_OLDLINING=1 restores the old pages for an A/B. */
    unsigned int h = (unsigned)(si / TD5_TG_TUNNEL_RUN) * 2654435761u;
    if (td5_env_flag_off("TD5RE_AUTOTRACK_TUNNEL_OLDLINING")) {
        unsigned int ov = (h >> 13) % (unsigned)TD5_TG_TUNNEL_VARIANTS;
        if (ov == 0) return TD5_TG_PAGE_TUNNEL;
        return TD5_TG_PAGE_TUNNEL_VAR + (int)(ov - 1);
    }
    return TD5_TG_PAGE_R6_TUNNEL + (int)((h >> 13) % 4u);
}

static int tg_r12_portal_join(void)
{
    return td5_env_flag_on("TD5RE_R12_TUNNEL_PORTAL_JOIN");
}

/* Jamb width past the opening edge == the pillar's own outer edge. */
static double tg_portal_wing(void)
{
    return tg_r12_portal_join() ? TD5_TG_PORTAL_WING : TD5_TG_PORTAL_WING_R11;
}

/* Half-depth of a mouth buttress along the road. */
double tg_portal_butt_deep(void)
{
    return tg_r12_portal_join()
         ? TD5_TG_PORTAL_PROUD - TD5_TG_PORTAL_SKIN : TD5_TG_BUTT_DEEP_R11;
}

/* Lateral extent the bore has to enclose at main-ring span si: *half is the
 * clear half-width, *shift the centre offset from the road centreline (+ve =
 * LEFT of travel, matching tg_append_row's sign).
 *
 * Normally that is just the road (half = width/2, centre 0). But where a BRANCH
 * fork runs through a tunnelled stretch, the main-ring span carries only the
 * LEFT half carriageway -- shifted to +width/4 by TD5_TG_MAIN_SHIFT, so it
 * occupies [0, +width/2] -- while the branch corridor is a SECOND drivable
 * carriageway at the same span, bowing out to the right to tg_branch_shift(k)
 * (down to -(0.25 + TD5_TG_BRANCH_BOW) * width at the bow peak). A bore sized
 * for the road alone therefore puts its left wall straight through the branch,
 * which is the clipping the feedback describes.
 *
 * So enclose the UNION of the two carriageways and re-centre the bore on it:
 * one wide cavern carrying both, rather than a box one of them passes through.
 *
 * DERIVED CONSEQUENCE, worth knowing before looking at it: with BRANCH_BOW 1.20
 * the union at the bow peak runs from -1.70*width to +0.50*width, i.e. 2.2x the
 * road's own width and offset 0.6*width to the right. A fork inside a tunnel is
 * a genuinely wide cavern, not a road tunnel. Capped below so a long fork
 * cannot demand an unbuildable span. */
void tg_tunnel_bore(const TG_NodeList *nl, int si,
                           double *half, double *shift)
{
    const double w = nl->v[si].width;
    int fi;

    *half  = w * 0.5;
    *shift = 0.0;
    if (!tg_branches_enabled()) return;

    fi = tg_fork_of_main(si);
    if (fi < 0) return;                     /* no branch alongside this span */
    {
        /* Corridor step paired with this main span, same indexing tg_emit_models
         * uses for the corridor road (j = si - F - 1). */
        const int L = s_forks[fi].len;
        const int j = si - s_forks[fi].F - 1;
        const double br = tg_branch_shift(j, L, w);      /* branch centre, -ve */
        const double lo = br - w * 0.25;                 /* right edge of union */
        const double hi = w * 0.5;                       /* left edge of union */
        double h = (hi - lo) * 0.5;
        /* Cap: the roof is one flat slab, and past ~3x the road width it reads
         * as a ceiling over open ground rather than a bore. Beyond the cap the
         * branch simply runs outside the tunnel, which is at least not a wall
         * through the road. */
        const double cap = w * 1.5;
        if (h > cap) h = cap;
        *shift = (lo + hi) * 0.5;
        *half  = h;
    }
}

/* [R5 STRUCT item 9] Sweep the tunnel walls/roof between consecutive nodes.
 *
 * The box version below placed one axis-aligned box at EACH node, oriented by
 * that node's own straight tangent only. On a curving or graded bore those
 * rigid boxes diverge from the centreline: on the inside of a curve the wall
 * pulls away and daylight shows through, on the outside it juts across the road,
 * and the mismatched slabs read as tilted "slopes" -- verbatim item 9, and
 * visible in log/r5s_tunnel_b.png (road curving left, left wall gone, blocks
 * scattered). Sweeping the two side walls and the roof from node si to node si+1
 * -- the same way the road quads are built -- makes the enclosure follow the
 * road in both plan and grade. Walls are single inner-face planes (scenery is
 * CULL_NONE so they draw solid from inside), the roof caps between the wall
 * tops, and a bright lintel closes each run mouth. Default ON;
 * TD5RE_R5_TUNNEL_SWEEP=0 restores the boxes for an A/B. */
static int tg_r5_tunnel_sweep(void)
{
    return td5_env_flag_on("TD5RE_R5_TUNNEL_SWEEP");   /* default ON */
}

/* How many sub-segments [si, si+1] needs: enough that no single piece turns by
 * more than TD5RE_TUNNEL_SUBDIV_DEG degrees. Straight bores return 1 and are
 * emitted byte-identically to before, so only curves pay for this. */
static int tg_tunnel_subdiv_count(const TG_Node *a, const TG_Node *c)
{
    double dot, ang, step_deg;
    int n;

    if (!td5_env_flag_on("TD5RE_TUNNEL_SUBDIV")) return 1;

    dot = a->tx * c->tx + a->tz * c->tz;
    if (dot >  1.0) dot =  1.0;
    if (dot < -1.0) dot = -1.0;
    ang = acos(dot);                                  /* radians turned */

    /* 1 degree per piece. Measured at seed 99991 span 490: at 3 degrees the
     * wall/roof junction still showed chunky steps, at 1 degree it sweeps as a
     * smooth curve and only pixel-level edge aliasing remains. Straight bores
     * are unaffected either way (they return 1). */
    step_deg = (double)td5_env_int("TD5RE_TUNNEL_SUBDIV_DEG", 1, 1, 45);
    n = (int)ceil(ang / (step_deg * 3.14159265358979323846 / 180.0));
    if (n < 1) n = 1;
    if (n > TD5_TG_TUNNEL_SUBDIV_MAX) n = TD5_TG_TUNNEL_SUBDIV_MAX;
    return n;
}

/* Cubic Hermite between two nodes using their unit tangents, scaled by the
 * chord length. Passes through both nodes with their exact tangents, so the
 * bore stays welded to the road at every span boundary and only bows BETWEEN
 * them -- the deviation a chord was missing. Y is interpolated linearly (the
 * reported artifact is a horizontal curve, and a linear grade keeps the bore
 * from lifting off a crest). */
static void tg_tunnel_hermite(const TG_Node *a, const TG_Node *c, double m,
                              double t, TG_Node *out)
{
    const double t2 = t * t, t3 = t2 * t;
    const double h00 =  2.0*t3 - 3.0*t2 + 1.0, h10 = t3 - 2.0*t2 + t;
    const double h01 = -2.0*t3 + 3.0*t2,       h11 = t3 - t2;
    const double d00 =  6.0*t2 - 6.0*t,        d10 = 3.0*t2 - 4.0*t + 1.0;
    const double d01 = -6.0*t2 + 6.0*t,        d11 = 3.0*t2 - 2.0*t;
    double tx, tz, len;

    out->x = h00*a->x + h10*m*a->tx + h01*c->x + h11*m*c->tx;
    out->z = h00*a->z + h10*m*a->tz + h01*c->z + h11*m*c->tz;
    out->y = a->y + (c->y - a->y) * t;
    out->width = a->width + (c->width - a->width) * t;
    out->lanes = a->lanes;

    tx = d00*a->x + d10*m*a->tx + d01*c->x + d11*m*c->tx;
    tz = d00*a->z + d10*m*a->tz + d01*c->z + d11*m*c->tz;
    len = sqrt(tx*tx + tz*tz);
    if (len > 1e-9) { out->tx = tx / len; out->tz = tz / len; }
    else            { out->tx = a->tx;    out->tz = a->tz;    }
}

/* [TUNNEL BORE SMOOTHING 2026-08-31] Catmull-Rom the bore profile across
 * NEIGHBOURING spans instead of interpolating each pair linearly.
 *
 * Subdividing the centreline fixed faceting on a constant-width bore, but not
 * where the bore TAPERS. Measured at seed 99991 spans 493-496 (the reported
 * location): half goes 4500 -> 4125 -> 3750 -> 3375 -> 3000 in uniform -375
 * steps as the road narrows 9000 -> 6000 into the portal. Interpolating half
 * linearly between si and si+1 makes the width profile piecewise-linear, so the
 * wall has a CORNER at every node -- a C1 break that no amount of subdivision
 * removes, because subdividing a straight ramp still leaves the kink at its
 * ends. Catmull-Rom through (si-1, si, si+1, si+2) is C1, so the taper becomes a
 * smooth flare instead of a chain of creases.
 *
 * Endpoints are clamped (p0=p1 / p3=p2) at a run's first/last span so the bore
 * cannot be bent by geometry outside the tunnel. t=0 and t=1 still reproduce
 * the exact si / si+1 samples, so spans stay welded and a CONSTANT-width bore is
 * unchanged (all four control values equal -> the spline is flat). */
static void tg_tunnel_bore_smooth(const TG_NodeList *nl, int si, double t,
                                  double *half, double *shift)
{
    double h[4], s[4];
    const int i0 = tg_span_in_tunnel(si - 1) ? si - 1 : si;
    const int i3 = tg_span_in_tunnel(si + 2) ? si + 2 : si + 1;
    const double t2 = t * t, t3 = t2 * t;
    int i;
    const int idx[4] = { i0, si, si + 1, i3 };

    for (i = 0; i < 4; i++) tg_tunnel_bore(nl, idx[i], &h[i], &s[i]);

    if (!td5_env_flag_on("TD5RE_TUNNEL_BORE_SMOOTH")) {
        *half  = h[1] + (h[2] - h[1]) * t;      /* previous linear behaviour */
        *shift = s[1] + (s[2] - s[1]) * t;
        return;
    }

    #define TG_CR(p) (0.5 * ((2.0*(p)[1]) + (-(p)[0] + (p)[2]) * t +            \
                    (2.0*(p)[0] - 5.0*(p)[1] + 4.0*(p)[2] - (p)[3]) * t2 +      \
                    (-(p)[0] + 3.0*(p)[1] - 3.0*(p)[2] + (p)[3]) * t3))
    *half  = TG_CR(h);
    *shift = TG_CR(s);
    #undef TG_CR
}

static int tg_emit_tunnel_swept(const TG_NodeList *nl, int si, TG_Buf *blk,
                                int *added)
{
    const double wall_t = TD5_TG_TUNNEL_WALL_T;
    const double height = TD5_TG_TUNNEL_HEIGHT;
    /* [TUNNEL ZIGZAG DIAG 2026-08-30] TD5RE_TUNNEL_SEGCOLOR=1 tints consecutive
     * [si, si+1] tunnel segments alternately (red / green / blue by si % 3) so a
     * frame shows exactly where one swept segment ends and the next begins.
     * The reported "zigzag/staircase for a few spans" is either (a) chord
     * faceting -- the bore is a chain of straight segments, so its silhouette
     * steps on a curve -- in which case every staircase step lands ON a segment
     * boundary, or (b) something else entirely, in which case the steps ignore
     * the colour boundaries. Attribution by measurement, after switching the
     * mountain mass and the lamp strip off both failed to change the artifact.
     * Diagnostic only; default OFF. */
    const int segcolor = (getenv("TD5RE_TUNNEL_SEGCOLOR") &&
                          getenv("TD5RE_TUNNEL_SEGCOLOR")[0] == '1');
    const unsigned int dim = segcolor
        ? (((si % 3) == 0) ? 0xFFFF4040u : ((si % 3) == 1) ? 0xFF40FF40u
                                                           : 0xFF4040FFu)
        : 0xFF585868u;                      /* shadowed interior */
    const unsigned int lampc = 0xFFFFE8B4u; /* warm-white lamp glow (ARGB) */
    const int lining  = tg_tunnel_lining_page(si);
    const int lamp_pg = TD5_TG_PAGE_R6_TUNNEL + 5;      /* [item 8d] */
    /* [R7 item 9] portal facade page (was the R6 thin-lintel page +4).
     * [R8 item 4] the R7 page IS the reported "grey-ish texture with lighter
     * gray stripes" (dump: log/tgpage_301.ppm). Choose the R8 unbanded headwall
     * instead; TD5RE_R8_PORTAL=0 puts the R7 page back for the A/B. */
    /* [R9 item 5c] "the gray-ish texture looks wrong here" -- said again, about
     * the R8 page. R8's page is a legitimately good CAST CONCRETE page and that
     * is exactly its problem: a flat, even, pale grey wall is what the user keeps
     * calling grey-ish. Do not build a fourth grey concrete page. R9's face is
     * BOARD-FORMED and warm, with vertical shutter boards -- a portal reads as
     * built rather than poured, and the boards give the eye vertical structure
     * where three rounds of horizontal banding was the exact thing complained
     * about. Vertical, not horizontal, so the R8 lesson still holds.
     * TD5RE_R9_PORTAL_FACE=0 falls back to the R8 page for the A/B. */
    const int portal_pg =
        !td5_env_flag_on("TD5RE_AUTOTRACK_R7_PORTAL") ? TD5_TG_PAGE_R6_TUNNEL + 4
      : td5_env_flag_on("TD5RE_R9_PORTAL_FACE")       ? TD5_TG_PAGE_R9_PORTAL_FACE
      : td5_env_flag_on("TD5RE_R8_PORTAL")            ? TD5_TG_PAGE_R8_BRIDGE + 1
      :                                                 TD5_TG_PAGE_R7_BRIDGE + 0;
    const int lamps_on = td5_env_flag_on("TD5RE_AUTOTRACK_TUNNEL_LAMPS");
    /* [R9 item 5e] "walls and roofing should have different texture". They were
     * one page because they are one mesh, not because anyone decided they should
     * match -- tg_write_quad_mesh_col has carried per-segment pages since R5 and
     * the roof simply rode along in the wall segment. Give the ceiling its own
     * page and its own segment. TD5RE_R9_BORE_CEIL=0 puts the roof back on the
     * lining page for the A/B. */
    const int ceil_on = td5_env_flag_on("TD5RE_R9_BORE_CEIL");
    const int ceil_pg = ceil_on ? TD5_TG_PAGE_R9_BORE_CEIL : 0;
    /* [R9 item 5b/5d] How far the portal frame stands out from the mouth node.
     * Hoisted out of the portal block because the LINING now has to reach it --
     * see the mouth-extension note below. */
    const double proud  = TD5_TG_PORTAL_PROUD;
    const double tile = 3000.0;
    const TG_Node *nd = &nl->v[si];
    /* [TUNNEL SUBDIV port 2026-08-31] Was 96, which fit the two mouth segments
     * plus one interior segment. A subdivided curve turns that ONE interior
     * segment into up to TD5_TG_TUNNEL_SUBDIV_MAX of them, and each segment
     * costs 8 wall + 4 roof + 8 lamp vertices, so this has to grow with it or a
     * curved bore silently overruns the arrays. */
    double px[320], py[320], pz[320], uu[320], vv[320];
    /* [SPLIT 2026-09-06] col[] must match the point arrays: the push macro
     * below writes col[n] for EVERY vertex, and a lit bore span emits 160+
     * vertices, so the old col[96] overflowed onto whatever the compiler laid
     * out next. In the 31k-line monolith that happened to be harmless stack;
     * the moment the emitter moved into its own module the overflow landed on
     * px/py/pz and corrupted vertex 100/103 of every long tunnel span (found by
     * the byte-identity check of the module split against the unsplit build). */
    unsigned int col[320];
    int seg_page[4], seg_nq[4], nseg = 0;
    int n = 0, n_wall = 0, n_roof = 0, n_lamp = 0, n_portal = 0;

    /* +lateral = LEFT of travel: point at lateral t off node = (x + tz*t, y,
     * z - tx*t), the same frame the road/gore/sidewalk use. */
    #define TG_TUN_PUSH(ND, T, YY, U, V, C) do {                 \
        px[n] = (ND)->x + (ND)->tz * (T); py[n] = (YY);          \
        pz[n] = (ND)->z - (ND)->tx * (T);                        \
        uu[n] = (U); vv[n] = (V); col[n] = (C); n++;             \
    } while (0)

    /* Wall/roof segments. Vertices are grouped by texture page -- all WALL
     * quads, then all CEILING quads, then all LAMP quads, then the PORTAL --
     * because tg_write_quad_mesh_col draws each command's quads from a
     * contiguous vertex run, so a page change means a group boundary. */
    {
        TG_Node fn_in, fn_out;      /* synthesised mouth-extension end nodes */
        /* [TUNNEL SUBDIV port] +MAX so the single interior segment can become a
         * chain of sub-segments on a curve; the two mouth extensions are extra. */
        TG_Node sub[TD5_TG_TUNNEL_SUBDIV_MAX + 1];   /* interpolated sub-nodes */
        struct { const TG_Node *a, *c; double al, ar, cl, cr, u1, hh; }
            sg[TD5_TG_TUNNEL_SUBDIV_MAX + 2];
        int nsg = 0, k, pass;
        const int at_entry = !tg_span_in_tunnel(si - 1);
        const int at_exit  = !tg_span_in_tunnel(si + 1);
        /* [R9 item 5b/5d] MOUTH EXTENSION. This is the root cause of BOTH
         * remaining geometry complaints in item 5, and it is a hole R7 opened
         * and nobody looked into afterwards.
         *
         * R7 stood the portal frame 1600 units PROUD of the mouth node, so the
         * Group-C hill mass sitting AT that node could not occlude it. Correct,
         * and nothing was ever built in those 1600 units. So the lining begins
         * at the mouth NODE while the frame floats 1600 in front of it, and
         * between the two there is a strip of road with a concrete frame round
         * it and OPEN SKY overhead -- "the span that represents the entrance and
         * exit of the tunnel looks wrong" -- and the jamb, having nothing behind
         * it, is a single plane seen edge-on -- "something wrong with the depth
         * of the entrance pillar", a thin slab with no reveal.
         *
         * Carry the lining OUT to the frame plane. The bore then reaches the
         * portal that frames it, the sky strip closes, and the jamb gains 1600
         * units of visible depth for free because the wall behind it IS the
         * reveal. One extension fixes both, which is the tell that they were one
         * defect reported twice. TD5RE_R9_MOUTH=0 restores the R7 gap. */
        const int mouth_on = td5_env_flag_on("TD5RE_R9_MOUTH");
        double h0, s0;
        tg_tunnel_bore(nl, si, &h0, &s0);

        if (mouth_on && at_entry) {          /* step BACK against the tangent */
            fn_in = *nd;
            fn_in.x = nd->x - nd->tx * proud;
            fn_in.z = nd->z - nd->tz * proud;
            sg[nsg].a = &fn_in; sg[nsg].c = nd;
            sg[nsg].al = sg[nsg].cl = s0 + h0;
            sg[nsg].ar = sg[nsg].cr = s0 - h0;
            sg[nsg].u1 = proud / tile; sg[nsg].hh = h0; nsg++;
        }
        if (tg_span_in_tunnel(si + 1)) {     /* ordinary interior segment */
            /* [TUNNEL SUBDIV + BORE SMOOTHING, ported onto the R9 emitter
             * 2026-08-31] The interior span was ONE flat piece (~1560 units
             * deep). Vertices are shared so there are no gaps, but on a curve
             * each piece is a CHORD, so the surface kinks at every span
             * boundary -- a C1 break that reads as a staircase along the
             * wall/roof junction when seen down the bore at a grazing angle.
             * Confirmed with TD5RE_TUNNEL_SEGCOLOR=1: every staircase step
             * landed exactly on a segment colour boundary.
             *
             * Emit a CHAIN of sub-segments instead, so no piece turns more than
             * TD5RE_TUNNEL_SUBDIV_DEG degrees. The centreline is a cubic Hermite
             * through both nodes using their existing unit tangents -- it is
             * interpolating, so t=0/t=1 reproduce the nodes exactly and
             * consecutive spans stay welded; only the span INTERIOR bows out to
             * the arc the chord was missing. The bore is Catmull-Rom'd across
             * neighbouring spans (tg_tunnel_bore_smooth) because a linear ramp
             * left a corner at every node wherever the bore TAPERS, which
             * subdivision alone cannot remove.
             *
             * A straight, constant-width bore returns nsub == 1 and reproduces
             * master's single segment exactly. */
            /* The si+1 bore is no longer read directly: tg_tunnel_bore_smooth
             * supplies BOTH endpoints of every sub-piece, and at t=1 it returns
             * the si+1 sample, so consecutive spans still meet exactly. */
            double dx, dz, seglen;
            int nsub, q;
            dx = nl->v[si + 1].x - nd->x; dz = nl->v[si + 1].z - nd->z;
            seglen = sqrt(dx * dx + dz * dz);
            nsub = tg_tunnel_subdiv_count(nd, &nl->v[si + 1]);
            if (nsub > TD5_TG_TUNNEL_SUBDIV_MAX) nsub = TD5_TG_TUNNEL_SUBDIV_MAX;

            for (q = 0; q <= nsub; q++)
                tg_tunnel_hermite(nd, &nl->v[si + 1], seglen,
                                  (double)q / (double)nsub, &sub[q]);

            for (q = 0; q < nsub; q++) {
                double h_a, s_a, h_c, s_c;
                tg_tunnel_bore_smooth(nl, si, (double)q       / (double)nsub, &h_a, &s_a);
                tg_tunnel_bore_smooth(nl, si, (double)(q + 1) / (double)nsub, &h_c, &s_c);
                sg[nsg].a = &sub[q]; sg[nsg].c = &sub[q + 1];
                sg[nsg].al = s_a + h_a; sg[nsg].ar = s_a - h_a;
                sg[nsg].cl = s_c + h_c; sg[nsg].cr = s_c - h_c;
                /* U keeps the whole-span tiling, split across the sub-pieces, so
                 * the lining tiles continuously instead of restarting on each. */
                sg[nsg].u1 = ((seglen > 1.0) ? seglen / tile : 0.5) / (double)nsub;
                sg[nsg].hh = h_a; nsg++;
            }
        }
        if (mouth_on && at_exit) {           /* step FORWARD along the tangent */
            fn_out = *nd;
            fn_out.x = nd->x + nd->tx * proud;
            fn_out.z = nd->z + nd->tz * proud;
            sg[nsg].a = nd; sg[nsg].c = &fn_out;
            sg[nsg].al = sg[nsg].cl = s0 + h0;
            sg[nsg].ar = sg[nsg].cr = s0 - h0;
            sg[nsg].u1 = proud / tile; sg[nsg].hh = h0; nsg++;
        }

#ifndef TD5RE_RELEASE
        /* [TUNNEL SEG DIAG 2026-08-31] TD5RE_TUNNEL_SEG_DIAG=1 dumps every
         * assembled segment's endpoints and bore edges in world space. This is
         * the verification for the subdivision port: it re-derives, on MASTER's
         * emitter, the two properties the original measurements claimed --
         * (a) that consecutive segments still share an edge EXACTLY, so
         *     subdividing did not open a seam, and
         * (b) the crease angle along the wall edge, which is what the staircase
         *     actually is.
         * The earlier numbers were taken against the pre-R9 emitter and do NOT
         * automatically carry over, so they are measured again here. */
        if (getenv("TD5RE_TUNNEL_SEG_DIAG")) {
            for (k = 0; k < nsg; k++)
                TD5_LOG_I(LOG_TAG,
                    "tunseg si=%d k=%d/%d aL=%.2f,%.2f,%.2f aR=%.2f,%.2f,%.2f "
                    "cL=%.2f,%.2f,%.2f cR=%.2f,%.2f,%.2f",
                    si, k, nsg,
                    sg[k].a->x + sg[k].a->tz * sg[k].al, sg[k].a->y,
                    sg[k].a->z - sg[k].a->tx * sg[k].al,
                    sg[k].a->x + sg[k].a->tz * sg[k].ar, sg[k].a->y,
                    sg[k].a->z - sg[k].a->tx * sg[k].ar,
                    sg[k].c->x + sg[k].c->tz * sg[k].cl, sg[k].c->y,
                    sg[k].c->z - sg[k].c->tx * sg[k].cl,
                    sg[k].c->x + sg[k].c->tz * sg[k].cr, sg[k].c->y,
                    sg[k].c->z - sg[k].c->tx * sg[k].cr);
        }
#endif

        /* pass 0 = the two side WALLS, pass 1 = the ROOF, so the two pages end
         * up in two contiguous vertex runs without a second vertex array. */
        for (pass = 0; pass < 2; pass++) {
            for (k = 0; k < nsg; k++) {
                const TG_Node *a = sg[k].a, *c = sg[k].c;
                const double al = sg[k].al, ar = sg[k].ar;
                const double cl = sg[k].cl, cr = sg[k].cr;
                const double u1 = sg[k].u1;
                if (pass == 0) {
                    const double vtop = height / tile;
                    /* Left wall inner face */
                    TG_TUN_PUSH(a, al, a->y,          0.0, 0.0,  dim);
                    TG_TUN_PUSH(c, cl, c->y,          u1,  0.0,  dim);
                    TG_TUN_PUSH(c, cl, c->y + height, u1,  vtop, dim);
                    TG_TUN_PUSH(a, al, a->y + height, 0.0, vtop, dim);
                    /* Right wall inner face */
                    TG_TUN_PUSH(a, ar, a->y,          0.0, 0.0,  dim);
                    TG_TUN_PUSH(c, cr, c->y,          u1,  0.0,  dim);
                    TG_TUN_PUSH(c, cr, c->y + height, u1,  vtop, dim);
                    TG_TUN_PUSH(a, ar, a->y + height, 0.0, vtop, dim);
                } else {
                    /* Roof: near-left, far-left, far-right, near-right.
                     * V spans the bore WIDTH so the ceiling page's transverse
                     * ribs run across the road rather than along it. */
                    const double wv = (2.0 * sg[k].hh) / tile;
                    TG_TUN_PUSH(a, al, a->y + height, 0.0, 0.0, dim);
                    TG_TUN_PUSH(c, cl, c->y + height, u1,  0.0, dim);
                    TG_TUN_PUSH(c, cr, c->y + height, u1,  wv,  dim);
                    TG_TUN_PUSH(a, ar, a->y + height, 0.0, wv,  dim);
                }
            }
            if (pass == 0) n_wall = n; else n_roof = n - n_wall;
        }

        /* [item 8d] Emissive lamp strip near the top of each wall, inset a little
         * toward the bore so it does not z-fight the lining. One quad per side per
         * sub-piece = a continuous run of side lights down the bore. */
        if (lamps_on) {
            const double ins = 40.0;                 /* lateral inset from wall */
            const double ly0 = 0.70 * height, ly1 = 0.80 * height;
            for (k = 0; k < nsg; k++) {
                const TG_Node *a = sg[k].a, *c = sg[k].c;
                const double al = sg[k].al, ar = sg[k].ar;
                const double cl = sg[k].cl, cr = sg[k].cr, u1 = sg[k].u1;
                /* Left wall lamp */
                TG_TUN_PUSH(a, al - ins, a->y + ly0, 0.0, 1.0, lampc);
                TG_TUN_PUSH(c, cl - ins, c->y + ly0, u1,  1.0, lampc);
                TG_TUN_PUSH(c, cl - ins, c->y + ly1, u1,  0.0, lampc);
                TG_TUN_PUSH(a, al - ins, a->y + ly1, 0.0, 0.0, lampc);
                /* Right wall lamp */
                TG_TUN_PUSH(a, ar + ins, a->y + ly0, 0.0, 1.0, lampc);
                TG_TUN_PUSH(c, cr + ins, c->y + ly0, u1,  1.0, lampc);
                TG_TUN_PUSH(c, cr + ins, c->y + ly1, u1,  0.0, lampc);
                TG_TUN_PUSH(a, ar + ins, a->y + ly1, 0.0, 0.0, lampc);
            }
            n_lamp = n - n_wall - n_roof;
        }
    }

    /* [R7 item 9] Portal FACADE across a run mouth (near end at the first bored
     * node, far end at the last). The R6 portal was a single thin lintel beam, so
     * the entrance read as "a weird grey texture, not a tunnel entrance". Build a
     * proper concrete FRAME instead: a HEADER above the opening plus a JAMB down
     * each side, on the R7 concrete-portal page, framing the dark bore on three
     * sides. Each face is double-sided (front quad + reversed quad) so it reads
     * from both directions -- one emit serves both the entrance (faces the
     * approach) and the exit (faces the departure). UVs map the face's TOP edge
     * to page V=0 so the coping band sits along the top; U tiles by world lateral.
     * TD5RE_AUTOTRACK_R7_PORTAL=0 falls back to the R6 lintel (page + winding). */
    if (!tg_span_in_tunnel(si - 1) || !tg_span_in_tunnel(si + 1)) {
        double ph, psh, lo, ro;
        tg_tunnel_bore(nl, si, &ph, &psh);
        lo = psh + ph + wall_t;              /* left opening edge  */
        ro = psh - ph - wall_t;              /* right opening edge */
        if (td5_env_flag_on("TD5RE_AUTOTRACK_R7_PORTAL")) {
            /* [R12 item 6] Was a bare 1400 while the pillar's outer edge sat at
             * 1600, so the beam stopped 200 short of it on each side. Derived
             * from the pillar now -- see the PORTAL FRAME vs BUTTRESS block. */
            const double JW  = tg_portal_wing();
            const double HDR = 1500.0;       /* header height above the opening */
            const double fl  = lo + JW;      /* frame outer left  */
            const double fr  = ro - JW;      /* frame outer right */
            const double yb  = nd->y;                    /* facade foot (road)   */
            const double hb  = nd->y + height;           /* opening top          */
            const double yt  = nd->y + height + HDR;     /* header top (coping)  */
            /* Stand the headwall PROUD of the cutting: the Group-C mountain mass
             * (crown slab + buttresses, TD5_TG_PAGE_HILL) sits AT the mouth node
             * and would occlude a facade drawn in the node plane -- the R7 frame
             * has to project OUTWARD along the road, out past the buttress depth
             * (hz 1400), so it reads as a projecting concrete portal with the bore
             * recessed behind it. Outward = away from the tunnel interior: at the
             * entrance the interior is ahead (+tangent) so we step back, at the
             * exit it is behind so we step forward. */
            const double outsign = tg_span_in_tunnel(si + 1) ? -1.0 : 1.0;
            TG_Node fn = *nd;
            fn.x = nd->x + nd->tx * proud * outsign;
            fn.z = nd->z + nd->tz * proud * outsign;
            /* [R8 item 4] V used to be the face's world height divided by the
             * 3000-unit tile, so a jamb (2600 bore + 1500 header = 4100 tall)
             * sampled V 0..1.37 and the page REPEATED a third of the way up --
             * which is what put the R7 page's bright coping band across the
             * middle of both jambs, twice per mouth. That is the "lighter gray
             * stripes" the report names, and it is a mapping fault as much as an
             * art one: any page with a distinctive row would show it.
             *
             * Map V over the FACE, 0 at its top edge to 1 at its bottom, so no
             * face can ever sample the page more than once vertically. U keeps
             * the world-lateral tiling (a portal is wider than it is tall and
             * horizontal repetition across it is correct).
             * TD5RE_R8_PORTAL=0 restores the R7 world-height V for the A/B. */
            const int pv = td5_env_flag_on("TD5RE_R8_PORTAL");
            #define TG_PORTAL_V(ylo, yhi, yy) \
                (pv ? ((yhi) - (yy)) / ((yhi) - (ylo) > 1.0 ? (yhi) - (ylo) : 1.0) \
                    : ((yt) - (yy)) / tile)
            /* One frame face: laterals [la..lb], y [ylo..yhi]; front + reverse. */
            #define TG_PORTAL_FACE(la, lb, ylo, yhi) do {                        \
                const double v0 = TG_PORTAL_V((ylo),(yhi),(yhi));                 \
                const double v1 = TG_PORTAL_V((ylo),(yhi),(ylo));                 \
                TG_TUN_PUSH(&fn,(la),(yhi), (la)/tile,v0, 0xFFFFFFFFu);           \
                TG_TUN_PUSH(&fn,(lb),(yhi), (lb)/tile,v0, 0xFFFFFFFFu);           \
                TG_TUN_PUSH(&fn,(lb),(ylo), (lb)/tile,v1, 0xFFFFFFFFu);           \
                TG_TUN_PUSH(&fn,(la),(ylo), (la)/tile,v1, 0xFFFFFFFFu);           \
                TG_TUN_PUSH(&fn,(la),(yhi), (la)/tile,v0, 0xFFFFFFFFu);           \
                TG_TUN_PUSH(&fn,(la),(ylo), (la)/tile,v1, 0xFFFFFFFFu);           \
                TG_TUN_PUSH(&fn,(lb),(ylo), (lb)/tile,v1, 0xFFFFFFFFu);           \
                TG_TUN_PUSH(&fn,(lb),(yhi), (lb)/tile,v0, 0xFFFFFFFFu);           \
            } while (0)
            TG_PORTAL_FACE(fl, fr, hb, yt);      /* header over the opening */
            TG_PORTAL_FACE(fl, lo, yb, yt);      /* left jamb  */
            TG_PORTAL_FACE(ro, fr, yb, yt);      /* right jamb */
            #undef TG_PORTAL_FACE
            #undef TG_PORTAL_V
#ifndef TD5RE_RELEASE
            /* [R12 item 6] THE MEASUREMENT. Both objects the report compares,
             * in the same units, on the same line: the BEAM's outer lateral and
             * depth-out, and the PILLAR's. The two "gap" columns are the numbers
             * the item is judged on, and they are logged for whichever knob
             * state is live, so before/after is a log diff of one build. */
            if (getenv("TD5RE_R12_TUNNEL_DIAG")) {
                /* All laterals RELATIVE TO THE BORE CENTRE, which is the frame
                 * tg_emit_fb_tunnel measures its buttress offsets in. */
                const double side_x = ph + wall_t;            /* opening edge   */
                const double butt_out = side_x + TD5_TG_BUTT_INSET
                                                + TD5_TG_BUTT_HALFW;
                const double beam_out = fl;                   /* == side_x + JW */
                const double butt_deep = tg_portal_butt_deep();
                TD5_LOG_I(LOG_TAG,
                    "R12TDIAG mouth si=%d join=%d bore_half=%.0f shift=%.0f "
                    "opening_edge=%.0f beam_outer=%.0f pillar_outer=%.0f "
                    "LATERAL_GAP=%.0f beam_proud=%.0f pillar_front=%.0f "
                    "DEPTH_GAP=%.0f beam_y=%.0f..%.0f pillar_y=%.0f..%.0f "
                    "outsign=%.0f",
                    si, tg_r12_portal_join(), ph, psh,
                    side_x, beam_out - psh, butt_out,
                    butt_out - (beam_out - psh),
                    proud, butt_deep, butt_deep - proud,
                    hb - nd->y, yt - nd->y, -1500.0, 3300.0, outsign);
            }
#endif
            tg_acct(TG_ACCT_R7_BRIDGE, si);
        } else {
            const double y0 = nd->y + height, y1 = y0 + 500.0;
            const double pw = (lo - ro) / tile;
            TG_TUN_PUSH(nd, lo, y0, 0.0, 0.0,  0xFFFFFFFFu);
            TG_TUN_PUSH(nd, ro, y0, pw,  0.0,  0xFFFFFFFFu);
            TG_TUN_PUSH(nd, ro, y1, pw,  0.17, 0xFFFFFFFFu);
            TG_TUN_PUSH(nd, lo, y1, 0.0, 0.17, 0xFFFFFFFFu);
        }
        n_portal = n - n_wall - n_roof - n_lamp;
    }
    #undef TG_TUN_PUSH

    if (n <= 0) return 1;
    if (n_wall   > 0) { seg_page[nseg] = lining;    seg_nq[nseg] = n_wall / 4;   nseg++; }
    /* [R9 item 5e] the roof on its own page. With the knob off it falls back to
     * the lining page, which is byte-identical to merging the two runs. */
    if (n_roof   > 0) { seg_page[nseg] = ceil_on ? ceil_pg : lining;
                        seg_nq[nseg] = n_roof / 4;   nseg++; }
    if (n_lamp   > 0) { seg_page[nseg] = lamp_pg;   seg_nq[nseg] = n_lamp / 4;   nseg++; }
    if (n_portal > 0) { seg_page[nseg] = portal_pg; seg_nq[nseg] = n_portal / 4; nseg++; }
    tg_acct(TG_ACCT_TUNNEL, si);
    tg_acct(TG_ACCT_R6_TUNNEL, si);
    (*added)++;
    return tg_write_quad_mesh_col(blk, px, py, pz, uu, vv, col, n,
                                  seg_page, seg_nq, nseg);
}

/* Tunnel cross-section at span si: two side walls plus a roof, each its own
 * mesh so per-mesh frustum culling cannot pop the whole tunnel at once. The
 * enclosure is drawn DIM (vertex colour) so the interior reads as shadowed --
 * the engine has no interior lighting, so this fakes it. At the run ends a
 * PORTAL frame (a lintel above the road spanning the full mouth) marks the
 * entrance/exit so the near end does not read as an open box.
 *
 * All pieces sample TD5_TG_PAGE_TUNNEL. They used to sample TD5_TG_PAGE_WALL,
 * which is the city FACADE page -- with TD5RE_AUTOTRACK_REAL_TEX on that is a
 * photographic office frontage, so the inside of every tunnel read as building
 * windows. The bore has its own concrete lining page for exactly this reason.
 *
 * Port-original: shipped TD5 has no true tunnels, so there is no reference to
 * match -- this is an invented enclosed section. */
int tg_emit_tunnel(const TG_NodeList *nl, int si, TG_Buf *blk,
                          int *added)
{
    const TG_Node *n = &nl->v[si];
    const double wall_t = TD5_TG_TUNNEL_WALL_T;
    const double height = TD5_TG_TUNNEL_HEIGHT;
    const double lx = n->tz, lz = -n->tx;
    const unsigned int dim = 0xFF585868u;   /* shadowed blue-grey interior */
    const int lining = tg_tunnel_lining_page(si);   /* item 16a: per-run variety */
    double bore_half, bore_shift, side_x, cx, cz;
    int i;

    if (tg_r5_tunnel_sweep())               /* [R5 item 9] swept, road-following */
        return tg_emit_tunnel_swept(nl, si, blk, added);

    tg_tunnel_bore(nl, si, &bore_half, &bore_shift);
    side_x = bore_half + wall_t;
    /* Bore centreline: the road node displaced laterally onto the bore centre. */
    cx = n->x + lx * bore_shift;
    cz = n->z + lz * bore_shift;

    for (i = 0; i < 3; i++) {
        double ox = 0.0, oz = 0.0, hx, hy, cy;
        if (i < 2) {                                /* side walls */
            double sgn = i ? 1.0 : -1.0;
            ox = lx * side_x * sgn;
            oz = lz * side_x * sgn;
            hx = wall_t; hy = height * 0.5; cy = n->y + height * 0.5;
        } else {                                    /* roof */
            hx = side_x + wall_t; hy = 200.0; cy = n->y + height + 200.0;
        }
        if (!tg_emit_box_mesh(blk, cx + ox, cy, cz + oz,
                              hx, hy, 780.0, n->tx, n->tz,
                              lining, 3000.0, dim))
            return 0;
        (*added)++;
        tg_acct(TG_ACCT_TUNNEL, si);
    }

    /* Portal frame at a run END: a lit lintel across the mouth, above the road,
     * so the entrance/exit reads as a tunnel opening rather than an open box. */
    if (!tg_span_in_tunnel(si - 1) || !tg_span_in_tunnel(si + 1)) {
        if (!tg_emit_box_mesh(blk, cx, n->y + height + 150.0, cz,
                              side_x + wall_t, 500.0, wall_t,
                              n->tx, n->tz, lining, 2000.0,
                              0xFFFFFFFFu))
            return 0;
        (*added)++;
        tg_acct(TG_ACCT_TUNNEL, si);
    }
    return 1;
}

double tg_up_reach(const TG_NodeList *nl, int si, int is_left,
                          double half, double dp)
{
    const int ring = (s_ring_len > 1 && s_ring_len <= nl->count)
                   ? s_ring_len : nl->count;
    const TG_Node *n = &nl->v[si];
    const double ux = n->tz * (is_left ? 1.0 : -1.0);
    const double uz = -n->tx * (is_left ? 1.0 : -1.0);
    double want, selfr;
    int k;

    if (!td5_env_flag_on("TD5RE_R11_UP_REACH")) return TD5_TG_UP_REACH;
    want = half + tg_far_reach();
    /* [R12 item 14a] BELT AND BRACES on "no highway into the water". Placement
     * (tg_up_span_crossable) already refuses a water span outright, so this
     * cannot fire on a stock build -- it exists because the two rules answer
     * different questions and only this one is asked per ARM. A crossing whose
     * own span is dry can still have the sea on one side if a future placement
     * rule relaxes, and an arm is the thing that reaches, so the arm carries its
     * own limit: stop at the shore ramp's end, where the ground has fallen to
     * sea level, instead of sailing past it. */
    if (tg_r12_up_shore() && tg_biome_span_has_water(si) &&
        tg_water_side(si) == (is_left ? 1.0 : -1.0)) {
        const double shore = tg_road_half_width(nl, si) + tg_shore_end();
        if (want > shore) want = shore;
    }
    {   /* Span length from the node list itself: spec->span_length is not in
         * scope here and this emitter must not hold a second opinion. */
        const int a = (si + 1 < nl->count) ? si + 1 : si - 1;
        const double dx = nl->v[a].x - n->x, dz = nl->v[a].z - n->z;
        double sl = sqrt(dx * dx + dz * dz);
        if (!(sl > 1.0)) sl = 1500.0;
        selfr = (double)TD5_TG_UP_SELF_SPANS * sl;
    }
    if (want <= half) return want;

    for (k = 1; k <= TD5_TG_UP_ARM_STEPS; k++) {
        const double d = half + (want - half) * (double)k
                       / (double)TD5_TG_UP_ARM_STEPS;
        const double px = n->x + ux * d, pz = n->z + uz * d;
        int j;
        for (j = 0; j < nl->count; j++) {
            double dx, dz, rr;
            int sep = j - si;
            if (sep < 0) sep = -sep;
            if (ring - sep < sep) sep = ring - sep;      /* around the ring */
            if (j < ring) {
                if (sep <= TD5_TG_UP_SELF_SPANS) continue;   /* my own road */
            } else {
                const double ox = nl->v[j].x - n->x;
                const double oz = nl->v[j].z - n->z;
                if (ox * ox + oz * oz < selfr * selfr) continue;  /* my fork */
            }
            dx = nl->v[j].x - px; dz = nl->v[j].z - pz;
            rr = tg_carriageway_reach(nl, j, 1.0);
            {
                const double r2 = tg_carriageway_reach(nl, j, -1.0);
                if (r2 > rr) rr = r2;
            }
            rr += dp + TD5_TG_UP_ROAD_MARGIN;
            if (dx * dx + dz * dz < rr * rr) {
                const double got = half + (want - half) * (double)(k - 1)
                                 / (double)TD5_TG_UP_ARM_STEPS;
                TD5_LOG_I(LOG_TAG, "trackgen: [R11 item 7a] overpass @%d arm "
                          "%s capped %.0f -> %.0f (span %d's carriageway is "
                          "%.0f out)", si, is_left ? "L" : "R", want, got, j, d);
                return got;
            }
        }
    }
    return want;
}

/* Half-depth of an abutment along OUR road. Under item 7b this IS the deck's
 * half-depth, so the pillar is exactly as wide as the highway. */
static double tg_up_pier_halfdeep(void)
{
    return td5_env_flag_on("TD5RE_R11_UP_PIER")
         ? TD5_TG_UP_HALFDEEP : TD5_TG_UP_HALFDEEP + 1900.0;
}

/* ================= [R12 OVERPASS item 11a] THE PIER IS OFF THE ROAD =========
 * "This overpass's pillar stands ON the road."
 *
 * MEASURED ROOT, and it is the wrong AUTHORITY rather than a wrong number. The
 * abutments are placed at `half + 380` from the bore centre, where `half` comes
 * from tg_tunnel_bore -- and tg_tunnel_bore answers a DIFFERENT question. It
 * reports the clear width a BORE has to enclose at ONE span, from
 * `nl->v[si].width * 0.5`, and it CAPS itself at `w * 1.5` on a fork with the
 * stated consequence that "beyond the cap the branch simply runs outside the
 * tunnel". For a tunnel wall that trade is defensible; for a PIER it is the
 * bug, because "outside the enclosure" and "on the carriageway" are the same
 * place. Three separate ways the pier lands on tarmac fall out of that:
 *
 *   1. ONE SPAN. The pier is 2*pier_dp = 3000 deep along our road (R11 item 7b
 *      made it exactly as wide as the deck), so its footprint covers si-1..si+1,
 *      but its lateral is taken from span si's width alone. Where the road
 *      WIDENS -- and widening roads are a live feature as of the August 31
 *      change -- the neighbouring span is wider than the span the pier was
 *      sized on, and the pier foot is inside it.
 *   2. THE FORK CAP. On a fork span the branch corridor bows out past `w * 1.5`
 *      and tg_tunnel_bore deliberately stops following it. The pier then stands
 *      in the branch carriageway.
 *   3. NO MARGIN AT ALL. The pier's inner face sits at exactly `half`, i.e.
 *      flush with the road edge, where every other roadside object in this
 *      generator stands off by TD5_TG_CARRIAGEWAY_MARGIN.
 *
 * THE FIX IS TO ASK THE RIGHT AUTHORITY. tg_carriageway_reach is the single
 * definition of "outermost drivable lateral on this side" -- the on-road guard,
 * the roadside guardrail and the bridge parapet all already judge by it -- and
 * it covers the main road, the bowed branch at its current width and the gore
 * between them, and it already takes the wider of a span's two ends so a width
 * ramp cannot under-report. Ask it over the PIER'S OWN FOOTPRINT and add the
 * house verge. The pier can then only ever move OUTWARD, so the crossing's
 * clear span never shrinks and the deck (which reaches tens of thousands of
 * units further) is unaffected.
 *
 * TD5RE_R12_UP_PIERCLEAR=0 restores the bore-derived offset for an A/B. */
static double tg_up_pier_clear(const TG_NodeList *nl, int si, double side)
{
    double r = 0.0;
    int k;
    if (!nl) return 0.0;
    for (k = -TD5_TG_UP_CLEAR_SPANS; k <= TD5_TG_UP_CLEAR_SPANS; k++) {
        const int s = si + k;
        double rr;
        if (s < 0 || s >= nl->count) continue;
        rr = tg_carriageway_reach(nl, s, side);
        if (rr > r) r = rr;
    }
    return r + TD5_TG_CARRIAGEWAY_MARGIN;
}

/* Does the overpass deck's own footprint cover span si? Every massing gate
 * reads this, so "no buildings under the highway" is one statement. Stateless
 * and derived only from si, like tg_span_in_bridge_run. */
int tg_up_clear_span(int si)
{
    int k;
    if (!td5_env_flag_on("TD5RE_R11_UP_CLEAR")) return 0;
    for (k = -TD5_TG_UP_CLEAR_SPANS; k <= TD5_TG_UP_CLEAR_SPANS; k++) {
        const int s = si + k;
        if (s >= 0 && tg_underpass_span(s) == s) return 1;
    }
    return 0;
}

/* [R12 item 11c] The band that carries NO STREET INTERSECTION, widened from the
 * deck's own footprint (tg_up_clear_span) by TD5_TG_UP_XCLEAR spans each side.
 *
 * A junction is not just its zebra. MEASURED: gating only the crossing
 * predicate removed 2 zebras on seed 20260901 and left both intersections
 * standing -- the side-street CARRIAGEWAY (tg_city_emit_crossstreet) runs on
 * every span of a frontage gap, not only on the span the crossing is painted
 * on, so the junction the item is about survived its own crosswalk being
 * deleted. Both readers gate on this one predicate now, which is what makes
 * "no intersection under the deck" a single statement rather than a rule about
 * paint. */
int tg_up_xclear_span(int si)
{
    int k;
    if (!td5_env_flag_on("TD5RE_R12_UP_XGATE")) return 0;
    for (k = -TD5_TG_UP_XCLEAR; k <= TD5_TG_UP_XCLEAR; k++)
        if (si + k >= 0 && tg_up_clear_span(si + k)) return 1;
    return 0;
}

/* Distance in spans from si to the nearest overpass crossing inside `win`, or
 * -1. Pure function of si, like every other predicate this element owns, and
 * deliberately NOT routed through tg_up_clear_span: R14's two consumers must
 * answer the same way whether or not the R11/R12 corridor knobs are on. */
int tg_r14_up_dist(int si, int win)
{
    int k, best = -1;
    for (k = -win; k <= win; k++) {
        const int s = si + k;
        const int d = (k < 0) ? -k : k;
        if (s < 0) continue;
        if (tg_underpass_span(s) != s) continue;
        if (best < 0 || d < best) best = d;
    }
    return best;
}

/* MEASURE, DON'T GUESS. One line per emitted overpass with the three numbers
 * item 7 is about, so the fix is verified by diffing two logs rather than by
 * looking at a frame: HIGHWAY width (along our road), PILLAR width (the same
 * axis -- these two must be equal), and ARM length each side against where the
 * drawn ground actually ends. Logged unconditionally: one line per underpass
 * span is a handful of lines on a 1987-span track, and a silent measurement is
 * a measurement nobody checks. */
static void tg_r11_up_diag(int si, double rl, double rr, double pier_dp,
                           double dp)
{
    TD5_LOG_I(LOG_TAG,
              "trackgen: [R11 item 7] overpass @%d -- highway width %.0f, "
              "pillar width %.0f (ratio %.2f), arm L %.0f R %.0f "
              "(drawn edge %.0f), buildings cleared on spans %d..%d",
              si, 2.0 * dp, 2.0 * pier_dp, (2.0 * pier_dp) / (2.0 * dp),
              rl, rr, tg_far_reach(),
              si - TD5_TG_UP_CLEAR_SPANS, si + TD5_TG_UP_CLEAR_SPANS);
}

/* [R12 item 11a] MEASURE THE THING THE ITEM IS ABOUT, per pier, per side: where
 * the pier's road-facing face sits against the widest carriageway under its own
 * footprint. `intr` is the number the report is: positive means the pier foot
 * is INSIDE the tarmac, which is the defect verbatim. Logged unconditionally --
 * two lines per crossing on a 1987-span track -- because R11 shipped this
 * emitter's last round of defects verified by numbers that were never this
 * number, and a measurement nobody prints is a measurement nobody checks. */
static void tg_r12_pier_diag(const TG_NodeList *nl, int si, double sgn,
                             double shift, double was, double now)
{
    const double clear = tg_up_pier_clear(nl, si, sgn);
    const double face_was = sgn * (shift + sgn * (was - 380.0));
    const double face_now = sgn * (shift + sgn * (now - 380.0));
    TD5_LOG_I(LOG_TAG,
              "trackgen: [R12 item 11a] pier @%d %s -- carriageway reach+verge "
              "%.0f, foot was %.0f (intr %+.0f) now %.0f (intr %+.0f)",
              si, sgn > 0.0 ? "L" : "R", clear,
              face_was, clear - face_was, face_now, clear - face_now);
}

/* Takes moff/nmesh directly rather than reporting a count the caller divides
 * up. The caller's "n equal-sized boxes" recovery is only valid when every mesh
 * IS the same size, and this element deliberately mixes boxes with a quad mesh,
 * so each offset is recorded as it is written. */
int tg_emit_underpass(const TG_NodeList *nl, int si, TG_Buf *blk,
                             size_t *moff, int *nmesh, int maxmesh)
{
    const TG_Node *n = &nl->v[si];
    const double lx = n->tz, lz = -n->tx;
    const double tile = 3000.0;
    const double dp = TD5_TG_UP_HALFDEEP;
    const double pier_dp = tg_up_pier_halfdeep();      /* [R11 item 7b] */
    double reach_l, reach_r;                           /* [R11 item 7a] */
    const double y_soffit = n->y + TD5_TG_UP_CLEAR;
    const double y_top    = y_soffit + TD5_TG_UP_THICK;
    const unsigned int lit  = 0xFFFFFFFFu;
    const unsigned int dark = 0xFF6A6E76u;    /* under a deck it is shaded */
    double half, shift, ax, az, wall_off;
    double px[64], py[64], pz[64], uu[64], vv[64];
    unsigned int col[64];
    int seg_page[3], seg_nq[3], nseg = 0, nv = 0;
    int s;

    /* Same union rule the bore uses: where a fork's branch corridor runs
     * alongside, the crossing has to clear BOTH carriageways or an abutment
     * lands in the branch. Reusing it means the two elements cannot disagree
     * about how wide the road is here. */
    tg_tunnel_bore(nl, si, &half, &shift);
    ax = n->x + lx * shift;
    az = n->z + lz * shift;
    wall_off = half + 380.0;
    /* +lateral is LEFT here (lx = tz, lz = -tx), matching tg_up_reach's
     * is_left, so each arm takes its own cap. */
    reach_l = tg_up_reach(nl, si, 1, half, dp);
    reach_r = tg_up_reach(nl, si, 0, half, dp);

    /* [R11 item 7] The numbers the item is judged on, logged BEFORE the
     * geometry is written so the A/B is a log diff. */
    tg_r11_up_diag(si, reach_l, reach_r, pier_dp, dp);

    /* --- ABUTMENTS: one pier each side, carrying the deck.
     *
     * [R11 item 7b] Exactly as wide along our road as the deck above it
     * (2*pier_dp == 2*dp). It used to run dp + 1900 -- 6800 against the deck's
     * 3000 -- to read as a retaining embankment, and that mismatch is the
     * "support pillars are the wrong width" report. --- */
    for (s = 0; s < 2; s++) {
        const double sgn = s ? 1.0 : -1.0;      /* +1 = LEFT (lx = tz) */
        double woff = wall_off;
        /* [R12 item 11a] The pier's inner face must clear the widest carriageway
         * anywhere under its own footprint, with the house verge. `woff` is
         * measured from the BORE CENTRE (shift) and the inner face is 380 nearer
         * the road than the pier centre, so the requirement
         *     sgn * (shift + sgn * (woff - 380)) >= clear
         * rearranges to the offset below. max(), never min(): a pier may only
         * ever move further out. */
        if (td5_env_flag_on("TD5RE_R12_UP_PIERCLEAR")) {
            const double need = tg_up_pier_clear(nl, si, sgn) - sgn * shift + 380.0;
            if (need > woff) woff = need;
        }
        if (*nmesh + 1 > maxmesh) return 1;      /* budget, not an error */
        moff[(*nmesh)++] = blk->len;
        if (!tg_emit_box_mesh(blk, ax + lx * woff * sgn,
                              n->y + TD5_TG_UP_CLEAR * 0.5,
                              az + lz * woff * sgn,
                              380.0, TD5_TG_UP_CLEAR * 0.5, pier_dp,
                              n->tx, n->tz, TD5_TG_PAGE_R9_UP_ABUT, 2400.0,
                              0xFFE0DCD4u))
            return 0;
        tg_acct(TG_ACCT_R9_UNDERPASS, si);
        tg_r12_pier_diag(nl, si, sgn, shift, wall_off, woff);
    }

    /* --- DECK: soffit + top + two fasciae in ONE mesh, three pages.
     * Laterals run from -REACH to +REACH across our road; `dp` is the half
     * depth along it. --- */
    #define TG_UP_PUSH(T, D, YY, U, V, C) do {                       \
        px[nv] = n->x + lx * (T) + n->tx * (D);                      \
        py[nv] = (YY);                                               \
        pz[nv] = n->z + lz * (T) + n->tz * (D);                      \
        uu[nv] = (U); vv[nv] = (V); col[nv] = (C); nv++;             \
    } while (0)
    {
        const double L = shift + reach_l;
        const double R = shift - reach_r;
        const double ul = L / tile, ur = R / tile;
        const double vd = (2.0 * dp) / tile;
        /* SOFFIT -- the face the driver actually sees, looking up from the
         * road. Wound so it faces DOWN; scenery draws CULL_NONE anyway, which
         * is what lets one quad serve as the visible underside. */
        TG_UP_PUSH(L, -dp, y_soffit, ul, 0.0, dark);
        TG_UP_PUSH(R, -dp, y_soffit, ur, 0.0, dark);
        TG_UP_PUSH(R,  dp, y_soffit, ur, vd,  dark);
        TG_UP_PUSH(L,  dp, y_soffit, ul, vd,  dark);
        seg_page[nseg] = TD5_TG_PAGE_R9_UP_SOFFIT; seg_nq[nseg] = 1; nseg++;
        /* TOP -- the highway carriageway. V runs across the deck's own width so
         * the page's lane marking runs ALONG the crossing road, not across it. */
        TG_UP_PUSH(L, -dp, y_top, ul, 0.0, lit);
        TG_UP_PUSH(R, -dp, y_top, ur, 0.0, lit);
        TG_UP_PUSH(R,  dp, y_top, ur, 1.0, lit);
        TG_UP_PUSH(L,  dp, y_top, ul, 1.0, lit);
        seg_page[nseg] = TD5_TG_PAGE_R9_UP_DECK; seg_nq[nseg] = 1; nseg++;
        /* FASCIAE -- the deck edge beam, near side and far side. This is the
         * band you read as "a road goes over here" from the approach. */
        for (s = 0; s < 2; s++) {
            const double d = s ? dp : -dp;
            TG_UP_PUSH(L, d, y_top,    ul, 0.0, lit);
            TG_UP_PUSH(R, d, y_top,    ur, 0.0, lit);
            TG_UP_PUSH(R, d, y_soffit, ur, 1.0, lit);
            TG_UP_PUSH(L, d, y_soffit, ul, 1.0, lit);
        }
        seg_page[nseg] = TD5_TG_PAGE_R9_UP_PARAPET; seg_nq[nseg] = 2; nseg++;
    }
    #undef TG_UP_PUSH
    if (*nmesh + 1 > maxmesh) return 1;
    moff[(*nmesh)++] = blk->len;
    if (!tg_write_quad_mesh_col(blk, px, py, pz, uu, vv, col, nv,
                                seg_page, seg_nq, nseg))
        return 0;
    tg_acct(TG_ACCT_R9_UNDERPASS, si);

    /* --- PARAPETS: a low wall along each deck edge. Boxes, because a parapet
     * is seen from the side far more often than from above. --- */
    for (s = 0; s < 2; s++) {
        const double d = s ? (dp - 130.0) : -(dp - 130.0);
        /* The two arms cap independently, so the parapet centres on the
         * MIDPOINT of the deck rather than on the bore -- otherwise a short
         * arm's rail would overhang its own deck. */
        const double mid  = shift + (reach_l - reach_r) * 0.5;
        const double hrch = (reach_l + reach_r) * 0.5;
        if (*nmesh + 1 > maxmesh) return 1;
        moff[(*nmesh)++] = blk->len;
        if (!tg_emit_box_mesh(blk,
                              n->x + lx * mid + n->tx * d,
                              y_top + TD5_TG_UP_PARAPET * 0.5,
                              n->z + lz * mid + n->tz * d,
                              hrch, TD5_TG_UP_PARAPET * 0.5, 130.0,
                              n->tx, n->tz, TD5_TG_PAGE_R9_UP_PARAPET, 2000.0,
                              0xFFFFFFFFu))
            return 0;
        tg_acct(TG_ACCT_R9_UNDERPASS, si);
    }
    return 1;
}

/* The terrain line this span's elevation is judged against.
 *
 * This was previously the GLOBAL minimum elevation of the whole track, which
 * made "elevated" mean "higher than the single lowest point in 1800 spans" --
 * true for 87% of a rolling track, so bridge decks were emitted almost
 * everywhere and the guardrail gate inherited the same fault.
 *
 * Instead take the CHORD between the two window endpoints. That is
 * grade-invariant by construction: on any constant grade the midpoint of the
 * endpoints equals this node's own height, so the lift is exactly zero no
 * matter how steep the hill. Only local CONVEXITY -- the road humping up over
 * ground that falls away on both sides, which is what a bridge actually is --
 * produces a positive lift. */
double tg_local_ground_y(const TG_NodeList *nl, int si)
{
    const int w = td5_env_int("TD5RE_AUTOTRACK_LIFT_WINDOW",
                              TD5_TG_LIFT_WINDOW, 1, 200);
    int a = si - w;
    int b = si + w;
    if (a < 0) a = 0;
    if (b > nl->count - 1) b = nl->count - 1;
    return (nl->v[a].y + nl->v[b].y) * 0.5;
}

/* ===================== [R11 BRIDGE item 12] RUN COALESCE =====================
 * "on span 837 a bridge finishes and another one starts immediately -- emit ONE
 *  longer bridge instead of two abutting ones."
 *
 * ROOT CAUSE, and it is partitioning, not emission. Runs are fixed RUN-sized
 * blocks from span 0 and each block is selected independently by hashing its own
 * index (tg_span_in_bridge_run). Nothing ever looked at the NEIGHBOUR, so two
 * consecutive blocks drawing "yes" produced two complete crossings sharing a
 * span boundary: the elevation pass laid a raised cosine over each, and a raised
 * cosine is zero-valued AND zero-sloped at both ends, so the road came back DOWN
 * to the terrain line at the join and climbed again -- a visible dip with a deck
 * end, a deck start, two sets of end piers and two river rectangles butted
 * together. Seed 20260901 span 837 is exactly that boundary.
 *
 * THE FIX PRESERVES SPAN NUMBERING, which was the constraint on this item.
 * tg_span_in_bridge_run is UNTOUCHED -- the set of bridge spans on the track is
 * bit-identical before and after. What changes is only what a bridge span
 * considers to be ITS OWN crossing: the maximal CHAIN of adjacent selected runs
 * instead of the single block. Two abutting 40-span runs become one 80-span
 * bridge over the same 80 spans. Nothing moves, nothing is added or removed, so
 * every other span reference in this feedback round stays valid.
 *
 * Because tg_bridge_run_bounds is the single authority the deck-Y, the river
 * level, the gorge phase, the pier grid, the gantry grid, the emitter and the
 * coastline all read, widening it here is the whole geometric change; the only
 * two things keyed on the run index directly are the STYLE (below, moved onto
 * the chain so one crossing is one style end to end) and the elevation hump.
 *
 * GRADE. A raised cosine of height H over L spans peaks at H*PI/L, so a longer
 * chain is GENTLER, not steeper: 2000*PI/80 = 78.5/span (grade 0.052) against
 * 2000*PI/40 = 157 (grade 0.105) and the 0.120 cap. Coalescing can therefore
 * never trip tg_apply_elevation's global rescale.
 *
 * TD5RE_R11_BRIDGE_COALESCE=0 restores the per-block crossings for an A/B.
 * ========================================================================= */
static int tg_r11_coalesce(void)
{
    return td5_env_flag_on("TD5RE_R11_BRIDGE_COALESCE");
}

/* First span of the CHAIN of adjacent selected runs containing si.
 *
 * Needs no node list (the backward walk is bounded by span 0 and by the grid
 * clearance inside tg_span_in_bridge_run), which is what lets tg_bridge_style --
 * which only ever gets an si -- key on the chain too. */
static int tg_bridge_chain_first(int si)
{
    const int run = TD5_TG_BRIDGE_RUN;
    int a = (si / run) * run;
    if (!tg_r11_coalesce() || !tg_span_in_bridge_run(si)) return a;
    while (a - run >= 0 && tg_span_in_bridge_run(a - run)) a -= run;
    return a;
}

/* The span range of the bridge CROSSING containing si -- one run, or the whole
 * chain of abutting runs under item 12. Partitioned exactly the way
 * tg_apply_elevation partitions it, so the deck hump, the river level and the
 * gorge all describe the same crossing. */
void tg_bridge_run_bounds(const TG_NodeList *nl, int si, int *s0, int *s1)
{
    const int run = TD5_TG_BRIDGE_RUN;
    int a = tg_bridge_chain_first(si);
    int b = a + run - 1;
    if (tg_r11_coalesce() && tg_span_in_bridge_run(si))
        while (b + 1 <= nl->count - 1 && tg_span_in_bridge_run(b + 1)) b += run;
    if (b > nl->count - 1) b = nl->count - 1;
    if (a > b) a = b;
    *s0 = a; *s1 = b;
}

/* The LOWEST road node in the bridge run containing si.
 *
 * The one determined height for the crossing. Everything that has to sit UNDER
 * the deck -- the river, the pier tops -- measures from this instead of from its
 * own span's y, and because it is the run MINIMUM the result is at or below the
 * road at every span in the run by construction. Nothing derived from it can
 * surface through the deck no matter what the profile does in between.
 *
 * Off a deliberate run there is no crossing to be level with (the lift-triggered
 * path is a single humped span), so fall back to the span's own height rather
 * than to the minimum of an arbitrary 24-span block, which could be far below. */
static double tg_bridge_deck_y(const TG_NodeList *nl, int si)
{
    int s0, s1, k;
    double lo;

    if (!tg_span_in_bridge_run(si)) return nl->v[si].y;
    tg_bridge_run_bounds(nl, si, &s0, &s1);
    lo = nl->v[s0].y;
    for (k = s0 + 1; k <= s1; k++)
        if (nl->v[k].y < lo) lo = nl->v[k].y;
    return lo;
}

/* River level under a bridge deck at span si (the banks drop to here).
 *
 * Flat over the whole run: this used to be nl->v[si].y - CHASM, i.e. the river
 * surface followed the deck's raised-cosine hump, so the water arched up under
 * the middle of the bridge.
 *
 * The chord MIDPOINT that replaced it was still not safe, and is half of the
 * "water on top of the tunnel" report. A midpoint is only guaranteed to be below
 * the road at the two endpoints it is taken from; anywhere the run's interior
 * dips under its own chord -- or where the 24-span window spills past the main
 * ring onto appended branch-corridor nodes, whose y has nothing to do with this
 * crossing -- the river surfaces above the local road, and a bore anchored to
 * that same local road is then under water. Same class of error as the
 * 2026-08-24 elevated-bridge gate, one level up: a value taken from two samples
 * where it had to hold over the whole run.
 *
 * The run MINIMUM (tg_bridge_deck_y) is that guarantee, and it is the rule
 * tg_sea_level_y already uses for the sea for exactly the same reason. */
/* [R8 item 11] Does the crossing containing si cross WATER? A property of the
 * RUN, decided once, not of the span.
 *
 * MEASURED FAULT this replaces (seed 777, run 160-199): the old test was
 * `k_biomes[tg_biome_for_span(si)].water` evaluated per span, and
 * tg_biome_for_span DITHERS per span inside a biome blend band -- it hashes on
 * si and returns the neighbouring biome for a random subset of spans near the
 * boundary. Span 165 therefore drew the DRY branch while 164 and 166 drew the
 * SEA branch, and the river surface stepped 1239 units down for exactly one
 * span (R8BDIAG: si=165 surf=-9211 against -7972 either side). Everything
 * downstream stepped with it -- the pier foot, the gorge bed, and the submerge
 * depth of the bank -- which is the reported "alternation between water spans
 * and tile spans at different height", one span wide, in the middle of a run.
 *
 * A body of water is one body. Ask the question once for the whole run and let
 * every span in it agree, the same reasoning tg_bridge_deck_y already applies to
 * the deck height and tg_sea_level_y to the sea. ANY water span carries the run:
 * a crossing that touches the sea at either end is the sea's, so the two
 * surfaces join instead of stepping. */
/* Single A/B switch for the whole R8 item-11 rewrite (run-level water level,
 * shore-shaped bank, longitudinally continuous ground slabs). Default ON;
 * TD5RE_R8_BRIDGE_WATER=0 restores the R7 behaviour so a frame pair can be
 * taken at one span with one variable changed. */
int tg_r8_bridge_water(void)
{
    return td5_env_flag_on("TD5RE_R8_BRIDGE_WATER");
}

int tg_bridge_run_is_water(const TG_NodeList *nl, int si)
{
    int s0, s1, k;
    if (!tg_r8_bridge_water() || !tg_span_in_bridge_run(si))
        return k_biomes[tg_biome_for_span(si)].water;
    tg_bridge_run_bounds(nl, si, &s0, &s1);
    for (k = s0; k <= s1; k++)
        if (k_biomes[tg_biome_for_span(k)].water) return 1;
    return 0;
}

double tg_bridge_water_y(const TG_NodeList *nl, int si)
{
    /* [R6 item 14] A bridge over a WATER biome crosses the SEA, so its water has
     * to sit at the SAME level as the sea beyond the run. The deep dedicated
     * "river" plane sat ~1000 units BELOW that sea (deck - CHASM vs biome-min -
     * WATER_DROP), so where the two met at the run ends the surface stepped down
     * -- half of "the water is not continuous". Matching sea level makes the
     * water one continuous body and drags the gorge bed and the pier feet with it
     * (both derive from here). Over a DRY biome there is no sea to match, so keep
     * the deep gorge river.
     *
     * [R8 item 11] Both halves are now RUN quantities. The water test is the
     * run vote above. The sea level itself is per BIOME run, and a 40-span
     * bridge run can straddle two of those, so take the MINIMUM across the
     * crossing: at or below every span's own sea, hence never surfacing through
     * a deck or a bank that was built against a lower neighbour. */
    if (tg_bridge_run_is_water(nl, si)) {
        int s0, s1, k;
        double lo;
        if (!tg_r8_bridge_water() || !tg_span_in_bridge_run(si))
            return tg_sea_level_y(nl, si);
        tg_bridge_run_bounds(nl, si, &s0, &s1);
        lo = tg_sea_level_y(nl, s0);
        for (k = s0 + 1; k <= s1; k++) {
            const double v = tg_sea_level_y(nl, k);
            if (v < lo) lo = v;
        }
        return lo;
    }
    return tg_bridge_deck_y(nl, si) - TD5_TG_BRIDGE_CHASM - 300.0;
}

/* The VISIBLE river surface under a bridge run -- the y the water quad is drawn
 * at, as opposed to tg_bridge_water_y's reference level.
 *
 * [R8 item 11] Single definition. Three call sites computed this offset
 * independently (the water emitter, the coastline band, the gorge submerge
 * ramp), each re-deriving the biome water test, so a change to that test had to
 * be made in three places or the bank and the water it hides under would
 * disagree. Over the sea the surface IS the reference level (they must be
 * coplanar); over a dry gorge the +100 keeps the river just under its banks. */
double tg_bridge_water_surf_y(const TG_NodeList *nl, int si)
{
    return tg_bridge_water_y(nl, si)
         + (tg_bridge_run_is_water(nl, si) ? 0.0 : 100.0);
}

/* How fully span si is "over the gorge", 0 at both ends of the bridge run and 1
 * at the crown, as a raised cosine -- the same shape as the deck hump.
 *
 * Why a ramp and not a flag: the banks beside a bridge span sit far lower than
 * ordinary terrain, so switching that on at a run boundary would leave exactly
 * the kind of unpainted vertical riser this batch is fixing. Ramping the gorge
 * in and out means the terrain is continuous at the run ends by construction. */
double tg_bridge_gorge_phase(const TG_NodeList *nl, int si)
{
    int s0, s1;
    double u;
    if (!tg_span_in_bridge_run(si)) return 0.0;
    tg_bridge_run_bounds(nl, si, &s0, &s1);
    if (s1 <= s0) return 0.0;
    u = (double)(si - s0) / (double)(s1 - s0);
    return 0.5 - 0.5 * cos(2.0 * TD5_TG_PI * u);
}

/* Visible SUPPORT STRUCTURE under and above the deck (towers, twin piers,
 * cross-braces, under-deck girder). Default ON; TD5RE_AUTOTRACK_BRIDGE_STRUCT=0
 * leaves only the parapets and the single centre pier the deck had before. */
int tg_bridge_struct_enabled(void)
{
    return td5_env_flag_on("TD5RE_AUTOTRACK_BRIDGE_STRUCT");
}

/* Page for everything STRUCTURAL on a bridge -- girder, piers, braces, towers.
 *
 * Reported: "bridge pillars should have a different texture than bridge
 * fences". They shared TD5_TG_PAGE_WALL, which is the city FACADE page: with
 * TD5RE_AUTOTRACK_REAL_TEX on the piers were a photographic office frontage,
 * and the parapet immediately above them was the same frontage, so deck and
 * supports read as one undifferentiated slab.
 *
 * [R4 item 18] It shared TD5_TG_PAGE_TUNNEL, the bore LINING page whose damp/
 * soot blotches are meant to read in a dim enclosed section. On a bright
 * EXTERIOR tower those blotches read as a checkerboard -- the "pillar looks
 * wrong and has wrong texture" report. TD5_TG_PAGE_R4_PIER is smooth cast
 * concrete authored for a lit surface. TD5RE_AUTOTRACK_BRIDGE_PIER_TEX=0 puts
 * the structure back on the city FACADE page for comparison. */
static int tg_bridge_pier_page(void)
{
    return td5_env_flag_on("TD5RE_AUTOTRACK_BRIDGE_PIER_TEX")
         ? TD5_TG_PAGE_R4_PIER : TD5_TG_PAGE_WALL;
}

int tg_bridge_style(int si)
{
    unsigned int h;
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_BRIDGE_VARIETY")) return 0;
    /* [R11 item 12] Keyed on the CHAIN, not the block. Two coalesced runs are
     * one crossing, and one crossing has to be one style end to end or the
     * "single longer bridge" changes material halfway across. */
    h = (unsigned)(tg_bridge_chain_first(si) / TD5_TG_BRIDGE_RUN) * 2654435761u;
    return (int)((h >> 12) % (unsigned)TD5_TG_BRIDGE_STYLES);
}

/* Structural page for a given style. Concrete keeps the R4 cast-concrete page;
 * steel and masonry get their own R6 pages so the piers read differently, not
 * just at a different size. Honours the same PIER_TEX A/B switch. */
int tg_bridge_pier_page_for(int style)
{
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_BRIDGE_PIER_TEX"))
        return TD5_TG_PAGE_WALL;
    if (style == 1) return TD5_TG_PAGE_R6_BRIDGE + 1;   /* steel   */
    if (style == 2) return TD5_TG_PAGE_R6_BRIDGE + 2;   /* masonry */
    return TD5_TG_PAGE_R4_PIER;                          /* concrete*/
}

/* Pier PITCH (spans between piers) for a style: steel viaducts stand on a
 * tighter grid, masonry arches on a wider one. */
int tg_bridge_pier_pitch(int style)
{
    if (style == 1) return 3;
    if (style == 2) return 6;
    return 4;
}

/* ===================== [R9 BRIDGE item 9] STRUCTURAL TIE =====================
 * "the structure that goes over the bridge should be connected with the
 * structure below."
 *
 * MEASURED CAUSE, not inherited. The over-deck gantry and the under-deck piers
 * were authored by two emitters that never shared a number, and they disagreed
 * on BOTH axes:
 *
 *   SPAN.    Piers stand where `si % pitch == 0` (pitch 4 concrete / 3 steel /
 *            6 masonry). The gantry stood where `si % 6 == 0`. On seed 99991's
 *            run 1000-1039 that is pitch 4 against 6, so only every third
 *            gantry (si % 12) had anything under it at all -- and the span the
 *            user photographed, 1002, is a gantry span with NO pier: 1002 % 6
 *            == 0 but 1002 % 4 == 2. The TOWER pair is worse: it stands at the
 *            crown span s0 + (s1-s0)/2 = 1019, and 1019 % 4 == 3, so the
 *            gateway that is meant to read as the crossing's silhouette had no
 *            pier under it on ANY run whose crown missed the pitch grid.
 *   LATERAL. Pier legs sit at `half * legpos` (0.70 / 0.85 of the MAIN half
 *            width, i.e. under the carriageway). Gantry legs and towers sit at
 *            the deck EDGE. Even where a pier and a gantry shared a span the
 *            two columns were 300-900 units apart laterally, which is exactly
 *            the "two independent structures occupying the same crossing" the
 *            report describes.
 *
 * THE FIX IS ONE SHARED AUTHORITY, in the same shape as tg_carriageway_reach:
 * a single function says where a vertical structural line stands on a given
 * side of a given span, and the pier legs, the gantry legs and the tower legs
 * all ask it. Nothing can drift because there is nothing to keep in sync.
 * The line chosen is the PARAPET line (drivable reach + RAIL_OUT): it is
 * outboard of anything drivable (so the on-road guard has no quarrel with it),
 * it is where the deck edge actually is, and it is where the tower already
 * stood -- so the column reads bed -> pier -> deck edge -> gantry leg -> beam
 * as one continuous member.
 *
 * Span alignment follows from the same idea: pier spans become RUN-RELATIVE
 * (s0 + k*pitch) and the CROWN is forced to carry a pier, so the tower always
 * lands; the gantry is then placed only on pier spans, every second pier.
 *
 * TD5RE_R9_BRIDGE_TIE=0 restores the round-8 independent placement for an A/B.
 * ========================================================================= */
int tg_r9_bridge_tie(void)
{
    return td5_env_flag_on("TD5RE_R9_BRIDGE_TIE");
}

/* Defined with the parapet/gantry emitters further down; the gantry PLACEMENT
 * gate now lives up here with the pier gate so the two cannot drift. */
static int tg_bridge_overhead_enabled(void);

/* THE shared lateral. `side` is +1 = left of travel, -1 = right, matching
 * tg_carriageway_reach and tg_emit_bridge_rails. */
double tg_bridge_column_lateral(const TG_NodeList *nl, int si,
                                       double side)
{
    if (!tg_r9_bridge_tie())
        return nl->v[si].width * 0.5;      /* R8: caller applied its own factor */
    return tg_carriageway_reach(nl, si, side) + TD5_TG_BRIDGE_RAIL_OUT;
}

/* Does span si carry a PIER? Run-relative under the tie so the crown (where the
 * towers stand) is always a pier span; the pre-R9 global grid otherwise. */
int tg_bridge_pier_here(const TG_NodeList *nl, int si)
{
    const int pitch = tg_bridge_pier_pitch(tg_bridge_style(si));
    int s0, s1;
    if (!tg_r9_bridge_tie() || !tg_span_in_bridge_run(si))
        return (si % pitch) == 0;
    tg_bridge_run_bounds(nl, si, &s0, &s1);
    if (si == s0 + (s1 - s0) / 2) return 1;          /* crown -> towers land */
    return ((si - s0) % pitch) == 0;
}

/* Does span si carry an over-deck GANTRY? Only on a pier span, and only every
 * SECOND pier so the spacing stays about what `si % 6` gave. MASONRY (style 2)
 * gets none, for the same reason it gets no towers: a stone arch viaduct is
 * arches all the way, and its single fat centre pier cannot carry a gantry leg
 * at the deck edge without inventing a member that is not there. */
int tg_bridge_gantry_here(const TG_NodeList *nl, int si)
{
    const int style = tg_bridge_style(si);
    const int pitch = tg_bridge_pier_pitch(style);
    int s0, s1;
    if (!tg_bridge_struct_enabled() || !tg_bridge_overhead_enabled()) return 0;
    if (!tg_r9_bridge_tie()) return (si % 6) == 0;
    if (style == 2) return 0;
    if (!tg_span_in_bridge_run(si)) return 0;
    if (pitch <= 0) return 0;
    tg_bridge_run_bounds(nl, si, &s0, &s1);
    return ((si - s0) % (pitch * 2)) == 0;
}

int tg_r9_bridge_tex(void)
{
    return td5_env_flag_on("TD5RE_R9_BRIDGE_TEX");
}

/* Bridge: a deck over a river. The road STRIP is the deck; here we add the
 * parapets (side walls) that read as a bridge from the car, plus the structure
 * that carries it. The ground beside a bridge run drops away into the gorge
 * (tg_emit_ground) and the river plane is emitted separately.
 *
 * WHY the extra boxes: with only parapets and one slim centre pier, a deck seen
 * from the side or from underneath read as a floating slab -- the feedback item.
 * Real crossings of this kind (Golden Gate, Sydney Harbour) are legible because
 * of three things, and each maps to one piece here:
 *   - a continuous girder under the deck, so the underside has depth;
 *   - PAIRED piers at the deck edges with a cross-brace between them, not a
 *     single post on the centreline;
 *   - a tower pair rising ABOVE the deck at the crossing's middle, braced
 *     across the top, which is the silhouette that says "bridge" at distance.
 * All pieces are boxes, which matters: tg_emit_models recovers each piece's
 * offset by dividing the appended bytes by the piece count, and that only holds
 * while every piece is the same size. Do not mix a non-box mesh in here. */
int tg_emit_bridge(const TG_NodeList *nl, int si,
                          TG_Buf *blk, int *added)
{
    const TG_Node *n = &nl->v[si];
    const double ref  = tg_local_ground_y(nl, si);
    const double lift = n->y - ref;
    const int deliberate = tg_span_in_bridge_run(si);
    const double half = n->width * 0.5;
    const int style = tg_bridge_style(si);            /* [R6 item 17] */
    const int pier_page = tg_bridge_pier_page_for(style);
    /* [R7 item 16] "The texture of the pillar doesn't seem fine." The pier boxes
     * tiled every 3000 world units, but a pier LEG is only ~400 wide, so a side
     * face sampled just U 0..0.13 of the page -- a thin vertical sliver that
     * flattened the I-beam / ashlar / form-board pattern into featureless grey.
     * A ~600-unit repeat maps close to one full page WIDTH across a leg (so the
     * flanges land at the leg edges and the web fills the middle) and tiles the
     * detail up the column. Only the vertical members use it; the girder stays at
     * 3000 (it is a wide flat beam along the road). TD5RE_AUTOTRACK_R7_PIERTEX=0
     * restores the 3000 sliver for an A/B. */
    const double pier_tile = td5_env_flag_on("TD5RE_AUTOTRACK_R7_PIERTEX")
                           ? 600.0 : 3000.0;
    /* Deck level for the boxes: the strip surface, with the girder just under. */
    const double wy = deliberate ? tg_bridge_water_y(nl, si) : ref;
    int s0, s1, s;

    if (!deliberate && lift < TD5_TG_BRIDGE_MIN_LIFT) return 1;
    tg_bridge_run_bounds(nl, si, &s0, &s1);

    /* NOTE the parapets are NOT emitted here. [R3 item 12] the old per-span
     * parapet BOX sat level at its own node height, so on the humped deck and
     * its approach ramps the boxes stepped up in a staircase -- the "guardrails
     * look like steps" report. A level box cannot follow a slope, so the rail
     * moved to tg_emit_bridge_rails, which draws a SLOPED ribbon whose top edge
     * runs node-to-node and therefore inclines with the deck. It has to be a
     * separate quad mesh with its own recorded offset because everything emitted
     * in THIS function is a same-byte-size box, recovered by the caller dividing
     * the appended bytes by the box count (see the header comment). */

    if (tg_bridge_struct_enabled()) {
        /* Girder: full-width beam immediately under the deck, one per span, so
         * the underside is a structural depth and not a paper-thin surface.
         * 780 along the road = the same half-length the tunnel sections use, so
         * consecutive spans butt together into a continuous beam. */
        if (!tg_emit_box_mesh(blk, n->x, n->y - 260.0, n->z,
                              half + 60.0, 170.0, 780.0,
                              n->tx, n->tz, pier_page, 3000.0, 0xFFD8D8D8u))
            return 0;
        (*added)++;
        tg_acct(TG_ACCT_BRIDGE, si);
    }

    /* Piers on the style's PITCH (item 17), dropping from the deck underside to
     * the river bed (item 12).
     *
     * Reported: "bridge road should have a predetermined height, this will make
     * pillars not visible on the road surface during bridges". Root cause was
     * arithmetic, and it was exact rather than marginal: the pier was a box of
     * half-height h centred at n->y - h, so its TOP FACE landed at n->y -- the
     * road surface itself, coplanar with the deck. Every pier was drawn into the
     * carriageway, z-fighting the tarmac it was supposed to be holding up. See
     * the item-12 note below for the height fix. */
    if (tg_bridge_pier_here(nl, si)) {
        /* [R6 item 12] Piers span the LOCAL deck underside down INTO the bed.
         *
         * Was: top = run-minimum deck (tg_bridge_deck_y) - UNDER, bottom = water
         * level. Two faults, both visible from the side (frame-verified): at the
         * crown the deck sits up to BRIDGE_HEIGHT above the run minimum, so a pier
         * topped out at the run min hung ~3000 units BELOW the deck underside --
         * disconnected posts floating in the gorge, not supports. And the bottom
         * stopped at the water SURFACE, 150 above the gorge bed, so it never
         * reached the floor ("pillars are not reaching the floor").
         *
         * Now the top follows n->y (this span's carriageway) less UNDER, so it is
         * always just below the local deck -- meets the girder at every span,
         * crown included, and still can't break the road. The bottom drops to
         * wy - 400, past the water surface and below the gorge bed (bed sits at
         * wy - 150), so the pier visibly plants on the floor. */
        const double top = n->y - TD5_TG_BRIDGE_UNDER;
        const double bot = wy - 400.0;
        double h = (top - bot) * 0.5;
        double cy;
        if (h < 150.0) h = 150.0;
        cy = top - h;
        if (td5_env_flag_off("TD5RE_BRIDGE_DIAG"))
            TD5_LOG_I(LOG_TAG,
                "BRIDGEDIAG si=%d ny=%.0f deckmin=%.0f wy=%.0f "
                "briverSurf=%.0f sea=%.0f pier_top=%.0f pier_bot=%.0f "
                "gorge_bed_crown=%.0f",
                si, n->y, tg_bridge_deck_y(nl, si), wy,
                tg_bridge_water_y(nl, si) + 100.0, tg_sea_level_y(nl, si),
                top, cy - h, tg_bridge_water_y(nl, si) - 150.0);
        if (tg_bridge_struct_enabled() && style == 2) {
            /* [R6 item 17] MASONRY: one FAT pier on a wide pitch, with a shallow
             * arch cap under the deck that reads as the springing of a stone
             * arch -- no leg pair, no towers (added below only for 0/1). */
            if (!tg_emit_box_mesh(blk, n->x, cy, n->z,
                                  620.0, h, 620.0, n->tx, n->tz,
                                  pier_page, pier_tile, 0xFFFFFFFFu))
                return 0;
            (*added)++;
            tg_acct(TG_ACCT_BRIDGE, si);
            if (!tg_emit_box_mesh(blk, n->x, top - 140.0, n->z,
                                  half * 0.9, 170.0, 360.0, n->tx, n->tz,
                                  pier_page, pier_tile, 0xFFFFFFFFu))
                return 0;
            (*added)++;
            tg_acct(TG_ACCT_BRIDGE, si);
        } else if (tg_bridge_struct_enabled()) {
            /* [R6 item 17] Paired legs under the deck EDGES. STEEL (style 1) legs
             * are slimmer, set wider and get a SECOND (upper) cross-brace so the
             * pier reads as a braced steel bent; CONCRETE (style 0) legs are
             * stockier with a single brace. */
            const double legw   = (style == 1) ? 200.0 : 300.0;
            const double legpos = (style == 1) ? 0.85 : 0.70;
            /* [R9 item 9] Lateral comes from the SHARED column authority, so the
             * pier leg, the gantry leg above it and the tower leg are all one
             * vertical line. Pre-R9 this was `half * legpos`, which put the leg
             * under the carriageway while everything above the deck stood at the
             * deck edge. `legpos` survives only as the A/B path. */
            const double latl = tg_r9_bridge_tie()
                              ? tg_bridge_column_lateral(nl, si, 1.0)
                              : half * legpos;
            const double latr = tg_r9_bridge_tie()
                              ? tg_bridge_column_lateral(nl, si, -1.0)
                              : half * legpos;
            const double bracehalf = (latl > latr ? latl : latr) + legw;
            for (s = 0; s < 2; s++) {
                double side = s ? 1.0 : -1.0;
                double lx = n->tz * side, lz = -n->tx * side;
                double lat = s ? latl : latr;
                if (!tg_emit_box_mesh(blk, n->x + lx * lat,
                                      cy, n->z + lz * lat,
                                      legw, h, legw, n->tx, n->tz,
                                      pier_page, pier_tile, 0xFFFFFFFFu))
                    return 0;
                (*added)++;
                tg_acct(TG_ACCT_BRIDGE, si);
                if (tg_r9_bridge_tie()) tg_acct(TG_ACCT_R9_BRIDGE, si);
            }
            /* Brace across the legs, a third of the way down the pier. */
            if (!tg_emit_box_mesh(blk, n->x, top - h * 0.55, n->z,
                                  bracehalf, 130.0, 200.0,
                                  n->tx, n->tz, pier_page, pier_tile,
                                  0xFFFFFFFFu))
                return 0;
            (*added)++;
            tg_acct(TG_ACCT_BRIDGE, si);
            if (style == 1) {                 /* second, upper brace for steel */
                if (!tg_emit_box_mesh(blk, n->x, top - h * 0.22, n->z,
                                      bracehalf, 110.0, 180.0,
                                      n->tx, n->tz, pier_page, pier_tile,
                                      0xFFFFFFFFu))
                    return 0;
                (*added)++;
                tg_acct(TG_ACCT_BRIDGE, si);
            }
        } else {
            if (!tg_emit_box_mesh(blk, n->x, cy, n->z,
                                  450.0, h, 450.0,
                                  n->tx, n->tz, pier_page, pier_tile,
                                  0xFFFFFFFFu))
                return 0;
            (*added)++;
            tg_acct(TG_ACCT_BRIDGE, si);
        }
    }

    /* Towers: once per crossing, at the crown span, so a run has ONE gateway
     * rather than a forest of posts. [R6 item 17] CONCRETE (0) gets a stocky
     * gateway; STEEL (1) a slimmer, taller pylon pair; MASONRY (2) has NO towers
     * (a stone viaduct is arches all the way, not a gateway) -- so the three
     * crossings on a track read differently at distance. */
    if (deliberate && tg_bridge_struct_enabled() && style != 2 &&
        si == s0 + (s1 - s0) / 2) {
        const double th = (style == 1) ? 2200.0 : 1700.0;  /* tower half-height */
        const double tw = (style == 1) ? 170.0 : 240.0;    /* tower half-width  */
        /* [R9 item 9] Same shared lateral as the pier legs directly below (the
         * crown is forced to be a pier span by tg_bridge_pier_here), and the
         * tower now starts at the PIER TOP rather than at deck level, so the
         * gateway is the visible continuation of the column rather than a
         * separate object planted on the deck. */
        const int  tie   = tg_r9_bridge_tie();
        const double tlatl = tie ? tg_bridge_column_lateral(nl, si, 1.0)
                                 : half + 300.0;
        const double tlatr = tie ? tg_bridge_column_lateral(nl, si, -1.0)
                                 : half + 300.0;
        const double tfoot = tie ? (n->y - TD5_TG_BRIDGE_UNDER) : n->y;
        const double thh   = tie ? (n->y + 2.0 * th - tfoot) * 0.5 : th;
        const double tcy   = tie ? (tfoot + thh) : (n->y + th);
        const double xhalf = (tlatl > tlatr ? tlatl : tlatr) + 240.0;
        const int    pyl   = tg_r9_bridge_tex() ? TD5_TG_PAGE_R9_PYLON : pier_page;
        const int    bmp   = tg_r9_bridge_tex() ? TD5_TG_PAGE_R9_BEAM  : pier_page;
        for (s = 0; s < 2; s++) {
            double side = s ? 1.0 : -1.0;
            double lx = n->tz * side, lz = -n->tx * side;
            double lat = s ? tlatl : tlatr;
            if (!tg_emit_box_mesh(blk, n->x + lx * lat,
                                  tcy, n->z + lz * lat,
                                  tw, thh, tw, n->tx, n->tz,
                                  pyl, pier_tile, 0xFFFFFFFFu))
                return 0;
            (*added)++;
            tg_acct(TG_ACCT_BRIDGE, si);
            if (tie) tg_acct(TG_ACCT_R9_BRIDGE, si);
        }
        /* Cross-member near the top of the towers, closing the gateway. */
        if (!tg_emit_box_mesh(blk, n->x, n->y + th * 1.7, n->z,
                              tie ? xhalf : half + 540.0, 160.0, 200.0,
                              n->tx, n->tz, bmp, 3000.0,
                              0xFFFFFFFFu))
            return 0;
        (*added)++;
        tg_acct(TG_ACCT_BRIDGE, si);
    }
    return 1;
}

/* Does span si carry a bridge DECK? The same two-way test tg_emit_bridge makes
 * internally (a deliberate run, OR a lone span lifted above the local terrain),
 * factored out so the parapet emitter gates on EXACTLY the spans that got a
 * deck -- a rail on a span with no deck under it would float. */
static int tg_span_is_bridge_deck(const TG_NodeList *nl, int si)
{
    double lift;
    if (tg_span_in_bridge_run(si)) return 1;
    lift = nl->v[si].y - tg_local_ground_y(nl, si);
    return lift >= TD5_TG_BRIDGE_MIN_LIFT;
}

/* [R9 RAILFIX] Does the DECK PARAPET actually stand on span si?
 *
 * tg_span_is_bridge_deck answers "is there a deck here", which is not the same
 * question: the parapet pass runs in the scenery loop, which skips everything
 * past the main ring (fork pads and appended branch corridors carry road only),
 * and it needs a far node to build a panel between. So a corridor span can be
 * "in a bridge run" by hash and carry no parapet at all.
 *
 * That distinction is the whole reason this exists. The roadside guardrail has
 * to yield the deck edges to the parapet (otherwise both rail the same edge --
 * items 8/12), and it must yield ONLY where the parapet is really there, or the
 * hand-off leaves a corridor span with no barrier of either kind. So the gate is
 * defined ONCE here and asked by both emitters, instead of each keeping its own
 * copy of "on a bridge" that can drift apart. */
/* [R9 RAILFIX] Master A/B for the whole edge-ownership hand-off. Default ON.
 * TD5RE_R9_RAILFIX=0 restores the round-8 behaviour (both treatments fire on a
 * shared edge) from the SAME build, which is what makes the before/after
 * uniqueness numbers a measurement rather than two different binaries. */
int tg_railfix_on(void) { return td5_env_flag_on("TD5RE_R9_RAILFIX"); }

int tg_rail_deck_here(const TG_NodeList *nl, int si)
{
    const int ring = (s_ring_len > 0) ? s_ring_len : nl->count;
    if (si < 0 || si >= ring) return 0;      /* pad / corridor: no parapet pass */
    if (si + 1 >= nl->count) return 0;       /* no far node to panel towards   */
    return tg_span_is_bridge_deck(nl, si);
}

static unsigned char s_r13_approach[TD5_TG_MAX_SPANS];

int tg_r13_approach_span(int si)
{
    if (si < 0 || si >= TD5_TG_MAX_SPANS) return 0;
    return s_r13_approach[si] != 0;
}

/* Filled once, as soon as the node list and s_ring_len are final. */
void tg_r13_approach_build(const TG_NodeList *nl, int nspans)
{
    int si;
    memset(s_r13_approach, 0, sizeof(s_r13_approach));
    if (!nl || !td5_env_flag_on("TD5RE_R13_APPROACH")) return;
    if (nspans > TD5_TG_MAX_SPANS) nspans = TD5_TG_MAX_SPANS;
    for (si = 0; si < nspans && si < nl->count; si++) {
        double lift;
        if (tg_span_in_bridge_run(si)) continue;   /* the deck, not its ramp */
        if (tg_span_in_tunnel(si)) continue;       /* a bore owns its own edges */
        if (!tg_span_near_bridge(si, TD5_TG_R13_APPROACH)) continue;
        lift = nl->v[si].y - tg_local_ground_y(nl, si);
        if (lift < 0.0) lift = -lift;
        if (lift >= TD5_TG_R13_APPROACH_LIFT) s_r13_approach[si] = 1;
    }
}

/* FROZEN round-8 coverage rule, used ONLY as the report's baseline.
 *
 * Deliberately a SECOND COPY of the live rule rather than a call into it, which
 * is the opposite of this file's usual policy and is the right call here: a
 * baseline that tracks the code under test cannot detect a regression in that
 * code. That is not hypothetical -- the first version of this instrument
 * recorded its baseline as a side effect of the guardrail emitter running, so
 * when a draft added a SPAN-LEVEL gate that suppressed the emitter, the baseline
 * fell with it (1178 -> 830) and the zero-rail counter read a false 0 while 30
 * edges really had lost their only rail. A baseline must be independent of the
 * path it is judging.
 *
 * Span-level, not per-side: the question it answers is "did this span end up
 * with NO barrier at all", which is the safety property. Mirrors the round-8
 * tg_span_needs_guardrail: tunnel exclusion, then dilation over the bridge-run /
 * elevation / bend rules. */
static int tg_rail_r8_baseline_raw(const TG_NodeList *nl, int si)
{
    double cross, dot, ang_deg;
    if (si < 1 || si + 2 >= nl->count) return 0;
    if (tg_span_in_tunnel(si)) return 0;
    if (tg_span_in_bridge_run(si)) return 1;
    if (nl->v[si].y - tg_local_ground_y(nl, si) >= TD5_TG_BRIDGE_MIN_LIFT)
        return 1;
    cross = nl->v[si].tx * nl->v[si + 1].tz - nl->v[si].tz * nl->v[si + 1].tx;
    dot   = nl->v[si].tx * nl->v[si + 1].tx + nl->v[si].tz * nl->v[si + 1].tz;
    ang_deg = atan2(fabs(cross), dot) * 180.0 / TD5_TG_PI;
    return (ang_deg * 10.0) >=
           (double)td5_env_int("TD5RE_AUTOTRACK_RAIL_DEG10", 50, 0, 900);
}

static int tg_rail_r8_baseline(const TG_NodeList *nl, int si)
{
    const int pad = td5_env_int("TD5RE_AUTOTRACK_RAIL_PAD", 3, 0, 16);
    int k;
    if (tg_span_in_tunnel(si)) return 0;
    for (k = -pad; k <= pad; k++)
        if (tg_rail_r8_baseline_raw(nl, si + k)) return 1;
    return 0;
}

/* ===================== [R11 CROSS] shared knobs / counters =================
 * Items 8, 9 and 15 are read by the rail-edge report, the median block and
 * the guardrail block, in that file order, so the knobs and the counters
 * they share sit above all three. */
/* Diagnostic: name every roadside barrier that lands on a span carrying a
 * pedestrian crossing or a side-street mouth, and every median span with the
 * height it actually stands at. TD5RE_R11_CROSS_DIAG=1 (default off). */
long s_r11_rail_on_cross;      /* rails standing on a zebra span       */

long s_r11_rail_on_xstreet;    /* rails standing across a street mouth */

int tg_r11_cross_diag(void)
{
    return td5_env_flag_off("TD5RE_R11_CROSS_DIAG");
}

/* Items 9 + 15 fix knob: the roadside barrier exempts crossing / intersection
 * spans. Default ON; TD5RE_R11_XGUARD=0 pins the pre-R11 behaviour for a
 * single-variable A/B. */
int tg_r11_xguard(void) { return td5_env_flag_on("TD5RE_R11_XGUARD"); }

/* Item 8 fix knob: a median must stand proud of the carriageway it divides.
 * Default ON; TD5RE_R11_MEDIAN_RISE=0 pins the pre-R11 heights. */
int tg_r11_median_rise(void)
{
    return td5_env_flag_on("TD5RE_R11_MEDIAN_RISE");
}

/* ============== [R12 GEOM item 7] THE MEDIAN'S END FACES ====================
 * "The BEGINNING of the median has no visible end face -- it reads as an open
 * box, you can see into the hollow interior of the raised island."
 *
 * DIRECT consequence of R11 item 8 above, and the source says so before any
 * frame does: tg_emit_avenue_divider builds the island as a THREE-quad prism --
 * one up-facing TOP and the two road-facing SIDE WALLS -- and there is no
 * fourth or fifth quad anywhere in it. A prism with walls and a lid and no ends
 * is an open box by construction. Pre-R11 the island only ran on avenue forks
 * and was capped at 520 half-width with a 150..360 rise, so the open end was a
 * small dark notch; R11 widened it to fill the gore (up to ~1000 half-width) and
 * stood it 296 above the tarmac, which turned that notch into a mouth you drive
 * straight at.
 *
 * The ends are the ONLY open faces, and that is a property of the arithmetic
 * rather than an observation: span si builds its near cross-section from
 * corridor step j and its far cross-section from j+1, and span si-1 builds its
 * far cross-section from that same j with the same expressions on the same node.
 * Two consecutive island spans therefore share an exactly coincident
 * cross-section and need nothing between them. Only the FIRST span of a run
 * (nothing at j-1) and the LAST (nothing at j+2) have an unclosed section.
 *
 * So: cap the leading and the trailing end of every run, and cap NOTHING in
 * between -- an interior cap would be a vertical quad coincident with its
 * neighbour's, i.e. z-fighting plus two wasted quads per span on 100+ spans.
 *
 * Deciding "does the neighbour have an island" is the only new question, and it
 * is answered by ONE predicate (tg_r12_median_at) that the emitter also uses on
 * ITSELF, so placement and capping cannot drift -- the same single-authority
 * shape as tg_bridge_column_lateral and tg_carriageway_reach.
 *
 * The gore FLOOR is untouched, as R11 requires: it underlaps both carriageways
 * by TD5_TG_GORE_OVERLAP to close the fork-mouth slit, and the caps live inside
 * the island's own inset footprint, so nothing new enters a live lane.
 *
 * TD5RE_R12_MEDIAN_CAP=0 restores the open ends for a single-variable A/B.
 * ========================================================================= */
int tg_r12_median_cap(void)
{
    return td5_env_flag_on("TD5RE_R12_MEDIAN_CAP");
}

/* Opt-in vertex-level dump of the island prism (per-quad corners + the face
 * normal each winding produces), which is how "the end face is absent" was
 * confirmed as a fact about emitted geometry rather than inferred from a
 * screenshot. TD5RE_R12_GEOM_DIAG=1. */
int tg_r12_geom_diag(void)
{
    return td5_env_flag_off("TD5RE_R12_GEOM_DIAG");
}

long s_r12_median_caps;      /* end faces emitted (leading + trailing) */

long s_r12_median_runs;      /* island runs seen (leading caps)        */

/* THE fill test, lifted out of tg_emit_avenue_divider verbatim so the neighbour
 * query below and the emitter itself cannot answer it differently. */
int tg_r12_median_fill(double gw0, double gw1)
{
    return tg_r11_median_rise()
        && gw0 <= TD5_TG_R11_MEDIAN_MAX
        && gw1 <= TD5_TG_R11_MEDIAN_MAX
        && (gw0 > TD5_TG_R11_MEDIAN_SLIM || gw1 > TD5_TG_R11_MEDIAN_SLIM);
}

/* Gore widths at both ends of MAIN-road span si of fork fi, by the same
 * expressions tg_emit_models uses at the divider call site (branch shift +
 * branch half width, both from the fork's own separation). `br_lanes` is passed
 * from that call site rather than re-derived, so this is the caller's number and
 * not a second opinion about how many lanes the branch carries. */
static void tg_r12_median_gore_w(const TG_NodeList *nl, int si, int fi,
                                 int br_lanes, double *gw0, double *gw1)
{
    (void)br_lanes;   /* [FORK KINDS] the fork's own split is read via fi */
    const int    j   = si - s_forks[fi].F - 1;
    const double sh0 = tg_fork_br_shift(fi, j,     nl->v[si].width);
    const double sh1 = tg_fork_br_shift(fi, j + 1, nl->v[si + 1].width);
    const double h0  = nl->v[si].width
                     * tg_fork_br_wscale(fi, j) * 0.5;
    const double h1  = nl->v[si + 1].width
                     * tg_fork_br_wscale(fi, j + 1) * 0.5;
    *gw0 = -(sh0 + h0);
    *gw1 = -(sh1 + h1);
}

/* Does a raised median island stand on MAIN-road span si? Mirrors every gate on
 * the path to tg_write_quad_mesh inside the divider: the call site's fork /
 * avenue / knob gates, then the emitter's sliver and fill gates. Asked by the
 * emitter about si-1 and si+1 to find a run's ends, and about si itself as a
 * self-check (logged under the diag knob), which is what keeps this from
 * becoming a stale copy of the placement rule. */
static int tg_median_at_raw(const TG_NodeList *nl, int si, int br_lanes)
{
    int fi;
    double gw0, gw1;
    if (!nl || si < 0 || si + 1 >= nl->count) return 0;
    fi = tg_fork_of_main(si);
    if (fi < 0) return 0;
    if (!tg_fork_is_avenue(fi) && !tg_r11_median_rise()) return 0;
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_AVENUE_DIVIDER")) return 0;
    tg_r12_median_gore_w(nl, si, fi, br_lanes, &gw0, &gw1);
    if (gw0 < 200.0 && gw1 < 200.0) return 0;              /* sliver */
    if (!tg_r12_median_fill(gw0, gw1) && !tg_fork_is_avenue(fi)) return 0;
    return 1;
}

/* [MEDIAN/BRIDGE 2026-09-07] A median may only appear on a bridge run if it
 * covers EVERY span of that run.
 *
 * Reported: "I see this median during a bridge and just the beginning -- if you
 * want to add a median to a bridge keep it in all its duration."
 *
 * The median is not a bridge feature. It is the raised island standing on the
 * GORE between a fork's two carriageways, so it exists exactly where a fork
 * exists (tg_fork_of_main) and its geometry is the fork's gore widths. Where a
 * fork region overlaps only the head of a bridge run, the median starts on the
 * deck and stops mid-span, which is what was seen.
 *
 * Literally extending it to the rest of the run is not possible: past the fork
 * there is only one carriageway, so there is no gore for an island to stand on
 * and no gw0/gw1 to build it from. Holding the fork's end widths would be
 * fabricated geometry -- a divider splitting a road that is not split.
 *
 * So uniformity is enforced the only way the geometry allows: all spans of the
 * run, or none. A fork long enough to span the whole bridge still draws its
 * median across the whole bridge; a fork that only clips the end draws none
 * there and stays a normal divided avenue on solid ground either side.
 *
 * Off a bridge there is no constraint -- returns 1. */
int tg_median_bridge_uniform(const TG_NodeList *nl, int si, int br_lanes)
{
    int s0 = 0, s1 = 0, s;
    if (!nl) return 1;
    if (!td5_env_flag_on("TD5RE_MEDIAN_BRIDGE_UNIFORM")) return 1;  /* =0 for A/B */
    if (!tg_span_in_bridge_run(si)) return 1;
    tg_bridge_run_bounds(nl, si, &s0, &s1);
    if (s1 < s0) return 1;                                 /* degenerate bounds */
    for (s = s0; s <= s1; s++)
        if (!tg_median_at_raw(nl, s, br_lanes)) return 0;
    return 1;
}

/* Public predicate = the placement rule AND the bridge-uniformity rule, so the
 * emitter's end-cap probes of si-1 / si+1 keep agreeing with what is actually
 * emitted. The uniformity loop calls the _raw form, so this does not recurse. */
int tg_r12_median_at(const TG_NodeList *nl, int si, int br_lanes)
{
    return tg_median_at_raw(nl, si, br_lanes) &&
           tg_median_bridge_uniform(nl, si, br_lanes);
}

/* Number of edges carrying more than one rail class, with the offenders named.
 * Returns doubled + zero-rail regressions, i.e. every violation of "exactly one
 * rail per edge", so a caller can assert on a single number. */
int tg_rail_edge_report(const TG_NodeList *nl, int nspans)
{
    long own[TG_RAIL_CLASS_COUNT];
    long pair[TG_RAIL_CLASS_COUNT][TG_RAIL_CLASS_COUNT];
    char first[240], zfirst[240];
    int si, s, a, b, dup = 0, pos = 0, nlisted = 0;
    int zero = 0, zpos = 0, zlisted = 0;
    long would = 0, railed = 0;

    memset(own, 0, sizeof(own));
    memset(pair, 0, sizeof(pair));
    first[0] = '\0';
    zfirst[0] = '\0';
    if (nspans > TD5_TG_MAX_SPANS) nspans = TD5_TG_MAX_SPANS;

    for (si = 0; si < nspans; si++)
        for (s = 0; s < 2; s++) {
            const unsigned m = s_rail_edge[si][s];
            int n = 0;
            for (a = 0; a < TG_RAIL_CLASS_COUNT; a++)
                if (m & (1u << a)) { own[a]++; n++; }
            if (n > 0) railed++;
            if (s_rail_would[si][s]) would++;
            /* SAFETY HALF OF THE INVARIANT, judged against the FROZEN round-8
             * baseline rather than against anything this build recorded. The
             * round-8 rules said this span must carry a barrier, and it now
             * carries none on this edge. */
            /* [R11 CROSS items 9+15] An edge where a street crosses is meant
             * to carry nothing, so it is NOT a zero-rail regression -- it is
             * the fix. Excluded here and reported on its own line below, so the
             * invariant keeps meaning "an edge lost its only rail by accident"
             * instead of quietly absorbing an intentional gap. */
            if (n == 0 && tg_rail_r8_baseline(nl, si)
                && !tg_r11_street_crosses_here(nl, si, s ? -1.0 : 1.0)) {
                zero++;
                if (zlisted < 12 && zpos < (int)sizeof(zfirst) - 24) {
                    zpos += snprintf(zfirst + zpos,
                                     sizeof(zfirst) - (size_t)zpos, "%s%d%s",
                                     zlisted ? " " : "", si, s ? "R" : "L");
                    zlisted++;
                }
                if (td5_env_flag_off("TD5RE_R9_RAILFIX_REPORT"))
                    TD5_LOG_I(LOG_TAG,
                              "trackgen:   ZERO-RAIL si=%d side=%s (r8 baseline "
                              "wanted a barrier, nothing stands)",
                              si, s ? "R" : "L");
            }
            if (n < 2) continue;
            dup++;
            /* Full dump, opt-in. The capped "first:" list below is enough to see
             * that a doubling EXISTS; naming the span the USER named needs every
             * offender printed, so TD5RE_R9_RAILFIX_REPORT=1 prints them all. */
            if (td5_env_flag_off("TD5RE_R9_RAILFIX_REPORT"))
                TD5_LOG_I(LOG_TAG, "trackgen:   dup-edge si=%d side=%s%s%s%s",
                          si, s ? "R" : "L",
                          (m & (1u << TG_RAIL_ROADSIDE))   ? " roadside"   : "",
                          (m & (1u << TG_RAIL_DECK))       ? " deck"       : "",
                          (m & (1u << TG_RAIL_KERBFENCE))  ? " kerb-fence" : "");
            for (a = 0; a < TG_RAIL_CLASS_COUNT; a++)
                for (b = a + 1; b < TG_RAIL_CLASS_COUNT; b++)
                    if ((m & (1u << a)) && (m & (1u << b))) pair[a][b]++;
            if (nlisted < 12 && pos < (int)sizeof(first) - 24) {
                pos += snprintf(first + pos, sizeof(first) - (size_t)pos,
                                "%s%d%s", nlisted ? " " : "", si, s ? "R" : "L");
                nlisted++;
            }
        }

    TD5_LOG_I(LOG_TAG,
              "trackgen: ---- rail-edge uniqueness (%d spans, %d edges) ----",
              nspans, nspans * 2);
    for (a = 0; a < TG_RAIL_CLASS_COUNT; a++)
        TD5_LOG_I(LOG_TAG, "trackgen:   %-11s edges=%ld",
                  k_rail_class[a], own[a]);
    TD5_LOG_I(LOG_TAG, "trackgen:   railed edges=%ld", railed);
    /* THE INVARIANT, both halves, side by side. Exactly one rail per edge means
     * never two AND never zero; either number alone proves nothing. */
    TD5_LOG_I(LOG_TAG, "trackgen:   DOUBLED edges=%d%s%s", dup,
              nlisted ? "  first: " : "", nlisted ? first : "");
    for (a = 0; a < TG_RAIL_CLASS_COUNT; a++)
        for (b = a + 1; b < TG_RAIL_CLASS_COUNT; b++)
            if (pair[a][b])
                TD5_LOG_I(LOG_TAG, "trackgen:     %s + %s = %ld edges",
                          k_rail_class[a], k_rail_class[b], pair[a][b]);
    TD5_LOG_I(LOG_TAG, "trackgen:   ZERO-RAIL regressions=%d%s%s", zero,
              zlisted ? "  first: " : "", zlisted ? zfirst : "");
    /* RECONCILIATION. The yield counter and the roadside delta are recorded at
     * the SAME point in the same loop, so these must balance exactly:
     *   would = roadside-now + yielded.
     * A non-zero residual means the yield is not the set of edges the roadside
     * actually gave up, which is what an earlier version of this report got
     * wrong (690 counted against 726 real losses). */
    TD5_LOG_I(LOG_TAG,
              "trackgen:   roadside would=%ld now=%ld yielded=%ld residual=%ld",
              would, own[TG_RAIL_ROADSIDE],
              tg_acct_total(TG_ACCT_R9_RAILYIELD),
              would - own[TG_RAIL_ROADSIDE]
                    - tg_acct_total(TG_ACCT_R9_RAILYIELD)
                    - s_r11_rail_on_cross - s_r11_rail_on_xstreet);
    /* [R11 CROSS items 9+15] The third bucket of the same reconciliation: edges
     * the round-8 rules would have railed and that are now deliberately bare
     * because a street crosses them. Both numbers must be 0 with
     * TD5RE_R11_XGUARD=0 and non-zero with it on -- that pair is the A/B. */
    TD5_LOG_I(LOG_TAG,
              "trackgen:   [R11 CROSS] rails dropped at a zebra=%ld, at a "
              "side-street mouth=%ld (knob TD5RE_R11_XGUARD=%s)",
              s_r11_rail_on_cross, s_r11_rail_on_xstreet,
              tg_r11_xguard() ? "on" : "off");
    return dup + zero;
}

/* [R3 item 13] Overhead ribs across the deck are OPTIONAL structure above the
 * road. Only drawn when the support structure is on; default ON so the fuller
 * "there should be structure above" silhouette is what ships. Set
 * TD5RE_AUTOTRACK_BRIDGE_OVERHEAD=0 to leave just the deck, rails and towers. */
static int tg_bridge_overhead_enabled(void)
{
    return td5_env_flag_on("TD5RE_AUTOTRACK_BRIDGE_OVERHEAD");
}

/* One sloped parapet panel spanning node si -> si+1 on one deck edge. This is
 * the item-12 fix: the top edge runs from (x0,y0) to (x1,y1), so it INCLINES
 * with the deck instead of the old level box that stepped. A single quad, its
 * own recorded mesh offset (it cannot join tg_emit_bridge's equal-size box
 * group). Sampled from the barrier page like the old parapet. */
static int tg_emit_bridge_rail_panel(TG_Buf *m,
                                     double x0, double y0, double z0,
                                     double x1, double y1, double z1,
                                     int si, size_t *moff, int *pn)
{
    double px[4], py[4], pz[4], uu[4], vv[4];
    const double base = TD5_TG_BRIDGE_RAIL_BASE, h = TD5_TG_BRIDGE_RAIL_H;
    /* [R4 item 16b] Was TD5_TG_PAGE_RAIL, an armco authored for a HORIZONTAL
     * strip. This panel maps u across the panel HEIGHT and v along the road, so
     * that page sampled rotated and "is not a guardrail texture". The R4 page is
     * drawn in these axes -- rails run horizontally, posts periodically along.
     * [R5 item 17a] The R4 page still read as a CONCRETE barrier, not a
     * guardrail; the R5 armco was steel but still an OPAQUE slab.
     * [R6 item 13] A guardrail with no extrusion has to be ALPHA-KEYED, not a
     * solid panel -- real armco is mostly air. Default to the R6 W-beam page
     * (type 1, transparent between beam and posts). TD5RE_AUTOTRACK_ARMCO=0
     * restores the R5 opaque steel face for an A/B. */
    int seg_page = td5_env_flag_on("TD5RE_AUTOTRACK_ARMCO")
                 ? TD5_TG_PAGE_R6_BRIDGE + 0 : TD5_TG_PAGE_R5_BRIDGE + 0;
    int seg_nq = 1;

    /* Quad: near-bottom, far-bottom, far-top, near-top. u across the page over
     * the panel height (0..1 = the whole barrier face); v advances one page per
     * span down the run. */
    px[0] = x0; py[0] = y0 + base;     pz[0] = z0; uu[0] = 0.0; vv[0] = (double)si;
    px[1] = x1; py[1] = y1 + base;     pz[1] = z1; uu[1] = 0.0; vv[1] = (double)(si + 1);
    px[2] = x1; py[2] = y1 + base + h; pz[2] = z1; uu[2] = 1.0; vv[2] = (double)(si + 1);
    px[3] = x0; py[3] = y0 + base + h; pz[3] = z0; uu[3] = 1.0; vv[3] = (double)si;

    /* [R13 RAIL item 4b] The parapet's actual emitted vertices and UVs, so
     * "the mirroring is painted, not geometric" is a measurement rather than a
     * reading of this function. What it has to show is ONE quad per edge with
     * MONOTONIC u and v -- no triangle-wave fold (the R12 tree-line idiom), no
     * transposition, and no second panel on the same edge. Windowed, opt-in via
     * TD5RE_R13_RAIL_VERTS=<span>. */
    {
        const int want = td5_env_int("TD5RE_R13_RAIL_VERTS", -1, -1, 100000);
        if (want >= 0 && si >= want - 1 && si <= want + 1) {
            int k;
            TD5_LOG_I(LOG_TAG,
                      "trackgen: [R13 4b] parapet si=%d page=%d nq=%d", si,
                      seg_page, seg_nq);
            for (k = 0; k < 4; k++)
                TD5_LOG_I(LOG_TAG,
                          "trackgen: [R13 4b]   v%d pos=(%.0f,%.0f,%.0f) uv=(%.3f,%.3f)",
                          k, px[k], py[k], pz[k], uu[k], vv[k]);
        }
    }
    moff[*pn] = m->len;
    if (!tg_write_quad_mesh(m, px, py, pz, uu, vv, 4, &seg_page, &seg_nq, 1))
        return 0;
    (*pn)++;
    tg_acct(TG_ACCT_BRIDGE, si);
    return 1;
}

/* Parapets on both deck edges (item 12) plus optional overhead ribs (item 13),
 * for span si. Recorded via moff/pn like tg_emit_bridge_water, so each mesh
 * carries its own offset -- these are quads, not the boxes tg_emit_bridge's
 * caller recovers by even division. */
/* [R5 item 17b] One DECK KERB quad along one edge, span si -> si+1, filling the
 * horizontal gap between the carriageway edge and the parapet line.
 *
 * The parapet sits at width/2 + RAIL_OUT (120) but the road mesh only reaches
 * width/2, so between the tarmac and the barrier there was a 120-unit strip of
 * nothing at deck level -- you looked straight down through it to the river.
 * That open strip is the "gap between the guardrails and the road" report. This
 * lays a flat concrete kerb across it at deck level, so the deck reads as solid
 * out to the barrier. Concrete (pier) page, its own recorded offset.
 * (TD5_TG_BRIDGE_RAIL_OUT itself moved up to the R9 structural-tie block, where
 * the substructure now reads it too.) */
static int tg_emit_bridge_kerb_panel(TG_Buf *m,
                                     double ix0, double iz0, double ox0, double oz0,
                                     double ix1, double iz1, double ox1, double oz1,
                                     double y0, double y1,
                                     int si, size_t *moff, int *pn)
{
    double px[4], py[4], pz[4], uu[4], vv[4];
    int seg_page = tg_bridge_pier_page(), seg_nq = 1;

    /* inner-near, inner-far, outer-far, outer-near -- a proper ring. U across
     * the kerb width, V one page per span down the run. */
    px[0] = ix0; py[0] = y0; pz[0] = iz0; uu[0] = 0.0; vv[0] = (double)si;
    px[1] = ix1; py[1] = y1; pz[1] = iz1; uu[1] = 0.0; vv[1] = (double)(si + 1);
    px[2] = ox1; py[2] = y1; pz[2] = oz1; uu[2] = 1.0; vv[2] = (double)(si + 1);
    px[3] = ox0; py[3] = y0; pz[3] = oz0; uu[3] = 1.0; vv[3] = (double)si;

    moff[*pn] = m->len;
    if (!tg_write_quad_mesh(m, px, py, pz, uu, vv, 4, &seg_page, &seg_nq, 1))
        return 0;
    (*pn)++;
    tg_acct(TG_ACCT_R5_BRIDGE, si);
    return 1;
}

int tg_emit_bridge_rails(const TG_NodeList *nl, int si,
                                TG_Buf *m, size_t *moff, int *pn)
{
    const TG_Node *n0 = &nl->v[si];
    const TG_Node *n1;
    double half0, half1, road0, road1;
    const int kerb = td5_env_flag_on("TD5RE_AUTOTRACK_BRIDGE_KERB");
    int s;

    if (!tg_rail_deck_here(nl, si)) return 1;   /* [R9 RAILFIX] shared gate */
    n1 = &nl->v[si + 1];

    for (s = 0; s < 2; s++) {
        /* +1 = LEFT of travel (the lateral sign tg_append_row uses), -1 = right,
         * which is the side a branch corridor bows out to. */
        const double side = s ? 1.0 : -1.0;
        double x0, z0, x1, z1;
        /* [R8 item 13] "At the beginning of bridge on span 161, if the bridge
         * has sidewalk the guardrail should be on both sides."
         *
         * Both parapets were derived from `n->width * 0.5` -- the MAIN
         * carriageway's own half width. On an ordinary span that is the deck
         * edge and the rail lands right. On a FORK span it is not: the deck
         * there carries the main half carriageway, the gore (with its raised
         * avenue median -- the "sidewalk" in the report) and the branch
         * corridor, and the corridor bows out to well past the main half width.
         * So the right-hand parapet was planted in the MIDDLE of the deck, on
         * the median, and the branch carriageway's outboard edge -- the one you
         * are actually driving next to -- had no barrier at all.
         *
         * MEASURED (seed 777): bridge run 160-199 and fork 144-169 overlap at
         * spans 160-169, so the reported span 161 is a forked deck. Same overlap
         * exists at fork 510-631 / bridge 480-519.
         *
         * tg_carriageway_reach is the single definition of "outermost drivable
         * lateral on this side" -- the R7 on-road guard and the ordinary
         * guardrail emitter already ask it the same question. Asking it here
         * puts each parapet on the true outboard edge of whatever the deck
         * carries, so a forked deck is railed on both of ITS sides rather than
         * on both sides of one of its carriageways. Off a fork it returns the
         * main half width, so unforked decks are unchanged.
         * TD5RE_R8_BRIDGE_RAIL_REACH=0 restores the main-width derivation. */
        const int reach_on = td5_env_flag_on("TD5RE_R8_BRIDGE_RAIL_REACH");
        road0 = reach_on ? tg_carriageway_reach(nl, si,     side)
                         : n0->width * 0.5;
        road1 = reach_on ? tg_carriageway_reach(nl, si + 1, side)
                         : n1->width * 0.5;
        half0 = road0 + TD5_TG_BRIDGE_RAIL_OUT;
        half1 = road1 + TD5_TG_BRIDGE_RAIL_OUT;
        x0 = n0->x + n0->tz * side * half0;
        z0 = n0->z - n0->tx * side * half0;
        x1 = n1->x + n1->tz * side * half1;
        z1 = n1->z - n1->tx * side * half1;
        /* [R5 item 17b] Kerb first, so the deck is solid out to the barrier. */
        if (kerb) {
            const double ix0 = n0->x + n0->tz * side * road0;
            const double iz0 = n0->z - n0->tx * side * road0;
            const double ix1 = n1->x + n1->tz * side * road1;
            const double iz1 = n1->z - n1->tx * side * road1;
            if (!tg_emit_bridge_kerb_panel(m, ix0, iz0, x0, z0, ix1, iz1, x1, z1,
                                           n0->y, n1->y, si, moff, pn))
                return 0;
        }
        if (!tg_emit_bridge_rail_panel(m, x0, n0->y, z0, x1, n1->y, z1,
                                       si, moff, pn))
            return 0;
        tg_rail_edge_note(TG_RAIL_DECK, si, side);      /* [R9 RAILFIX] */
        tg_acct(TG_ACCT_R8_BRIDGE, si);
    }

    /* Overhead rib, every 6th span. [R4 item 17] The cross-beam alone floated:
     * a bar in the sky with nothing joining it to the deck ("floating element").
     * Give it two VERTICAL LEGS down to the parapet tops so it reads as a portal
     * gantry, not a hovering slab.
     *
     * [R5 item 13] "geometry going over the bridge that doesn't have any depth
     * to it, looks 2D flat": the round-4 beam and legs were single QUADS -- a
     * horizontal slab with no thickness and vertical sheets with no lateral
     * width, so edge-on they vanished and read as flat cut-outs spanning the
     * deck. Build them as solid BOXES instead (tg_emit_box_mesh, six faces each),
     * so the gantry has real depth from every angle. Concrete (pier) page, so it
     * matches the towers. TD5RE_AUTOTRACK_BRIDGE_OVERHEAD=0 removes the gantry. */
    if (tg_bridge_gantry_here(nl, si)) {
        const int pier_page = tg_bridge_pier_page_for(tg_bridge_style(si));
        const int tie = tg_r9_bridge_tie();
        /* [R9 item 9] The gantry legs stand on the same lateral as the pier legs
         * below (tg_bridge_column_lateral), on a span that tg_bridge_gantry_here
         * guarantees carries a pier, and they now reach DOWN past the deck edge
         * to the pier top instead of starting at the parapet. Leg and pier are
         * one continuous column; there is no longer a joint to misalign. */
        const int    pyl = tg_r9_bridge_tex() ? TD5_TG_PAGE_R9_PYLON : pier_page;
        const int    bmp = tg_r9_bridge_tex() ? TD5_TG_PAGE_R9_BEAM  : pier_page;
        const double latl = tie ? tg_bridge_column_lateral(nl, si,  1.0) : half0;
        const double latr = tie ? tg_bridge_column_lateral(nl, si, -1.0) : half0;
        const double beamhalf = tie ? (latl > latr ? latl : latr) : half0;
        const double ry = n0->y + TD5_TG_BRIDGE_RAIL_BASE
                        + TD5_TG_BRIDGE_RAIL_H + 1800.0;   /* clears traffic */
        /* Leg foot: the pier top under the tie, the parapet top without it. */
        const double ly = tie
                        ? (n0->y - TD5_TG_BRIDGE_UNDER)
                        : (n0->y + TD5_TG_BRIDGE_RAIL_BASE + TD5_TG_BRIDGE_RAIL_H);
        const double lx = n0->tz, lz = -n0->tx;            /* left unit  */
        const double fwx = n0->tx, fwz = n0->tz;           /* along-road unit */
        const double lex = n0->x + lx * latl, lez = n0->z + lz * latl;
        const double rex = n0->x - lx * latr, rez = n0->z - lz * latr;
        int leg;
        /* Beam: a box spanning both deck edges, 260 tall x 320 deep along road. */
        moff[*pn] = m->len;
        if (!tg_emit_box_mesh(m, n0->x, ry, n0->z, beamhalf, 130.0, 160.0,
                              fwx, fwz, bmp, 3000.0, 0xFFFFFFFFu))
            return 0;
        (*pn)++;
        tg_acct(TG_ACCT_BRIDGE, si);
        /* Two legs: a solid post at each deck edge, from pier top to beam. */
        for (leg = 0; leg < 2; leg++) {
            const double ex = leg ? rex : lex, ez = leg ? rez : lez;
            const double cy = (ly + ry) * 0.5, hy = (ry - ly) * 0.5;
            moff[*pn] = m->len;
            if (!tg_emit_box_mesh(m, ex, cy, ez, 150.0, hy, 150.0,
                                  fwx, fwz, pyl, 600.0, 0xFFFFFFFFu))
                return 0;
            (*pn)++;
            tg_acct(TG_ACCT_BRIDGE, si);
            if (tie) tg_acct(TG_ACCT_R9_BRIDGE, si);
        }
    }
    return 1;
}

int tg_emit_bridge_water(const TG_NodeList *nl, int si,
                                TG_Buf *m, size_t *moff, int *pn)
{
    double px[4], py[4], pz[4], uu[4], vv[4];
    const TG_Node *n0 = &nl->v[si];
    const TG_Node *n1;
    double lx, lz, wy;
    const double BW = TD5_TG_BRIDGE_WATER_HALF;
    int i, seg_page = TD5_TG_PAGE_WATER, seg_nq = 1;

    if (si + 1 >= nl->count) return 1;
    if (!tg_water_span_clear(si)) return 1;
    n1 = &nl->v[si + 1];
    lx = n0->tz; lz = -n0->tx;              /* left unit */
    /* [R6 item 14] Over a water biome sit EXACTLY at sea level so the river and
     * the sea beyond the run are coplanar (no step). Over a dry biome the +100
     * keeps the surface just under the banks as before.
     * [R8 item 11] via the shared surface accessor, so this and the bank that
     * hides under it cannot be computed from two different water tests. */
    wy = tg_bridge_water_surf_y(nl, si);

    /* ============ [R11 WATER item 13] "GAPS IN THE WATER ON SPAN 848"
     * MEASURED. All four corners used to be swept along n0's normal alone, so
     * span si's FAR edge left node si+1 along n0's normal while span si+1's
     * NEAR edge left that same node along n1's. Wherever the road turns the two
     * edges diverge and open a wedge -- zero at the centreline, BW*dheading at
     * the rim, so 1.72 degrees of turn is ~960 units of open water 32000 out.
     * Over a bridge run tg_emit_fb_terrain returns before BOTH the far band and
     * the far shore, so the river IS the drawn world out there and that wedge is
     * not a seam in a puddle: it is a hole straight through to the sky.
     * Measured on seed 20260901: 238 of 320 river span-seams open more than 200
     * units, mean 720, worst 3925 at span 1359; 959 at the reported span 848.
     *
     * tg_emit_water -- the SEA -- never had this, and the difference is the
     * whole proof that it is a defect rather than a choice: it sweeps each
     * corner along its OWN node's normal, so neighbouring spans share their two
     * boundary points exactly and the surface is watertight by construction.
     * Give the river the same rule. The second half of the same line answers
     * item 14: take the reach from tg_r11_wet_reach (the far band's own reach)
     * instead of a fixed gorge half-width, so the water ends where the drawn
     * world ends rather than 1000 units inside it. Purely lateral -- every
     * corner keeps the same `wy`, so piers, the submerged gorge skirt and the
     * relief system are untouched.
     * TD5RE_R11_WATER=0 restores the single-normal parallelogram.
     * ==================================================================== */
    if (td5_env_flag_on("TD5RE_R11_WATER")) {
        const double lx0 = n0->tz, lz0 = -n0->tx;
        const double lx1 = n1->tz, lz1 = -n1->tx;
        const double b0 = tg_r11_wet_reach(nl, si);
        const double b1 = tg_r11_wet_reach(nl, si + 1);
        px[0] = n0->x - lx0 * b0; pz[0] = n0->z - lz0 * b0;
        px[1] = n1->x - lx1 * b1; pz[1] = n1->z - lz1 * b1;
        px[2] = n1->x + lx1 * b1; pz[2] = n1->z + lz1 * b1;
        px[3] = n0->x + lx0 * b0; pz[3] = n0->z + lz0 * b0;
    } else {
        px[0] = n0->x - lx * BW; pz[0] = n0->z - lz * BW;
        px[1] = n1->x - lx * BW; pz[1] = n1->z - lz * BW;
        px[2] = n1->x + lx * BW; pz[2] = n1->z + lz * BW;
        px[3] = n0->x + lx * BW; pz[3] = n0->z + lz * BW;
    }
    for (i = 0; i < 4; i++) {
        py[i] = wy;
        uu[i] = px[i] / TD5_TG_WATER_TILE;
        vv[i] = pz[i] / TD5_TG_WATER_TILE;
    }

    moff[*pn] = m->len;
    if (!tg_write_quad_mesh(m, px, py, pz, uu, vv, 4, &seg_page, &seg_nq, 1))
        return 0;
    (*pn)++;
    tg_acct_range(TG_ACCT_WATER, si, si + 1);   /* river under a bridge run */
    return 1;
}

/* [R4 item 20] COASTLINE where the bridge water meets the land, at the two
 * LONGITUDINAL ends of the river rectangle (perpendicular to the bridge). The
 * river is a flat plane under the run; before the run starts and after it ends
 * the terrain is back at road level, so the plane's transverse edge met the bank
 * as an open seam ("grass slopes not connecting properly with water"). This lays
 * one shore band per open end, from the water-edge node at the river surface out
 * to the neighbouring land node at ground level, so water grades into shore into
 * bank. Gated (default ON); TD5RE_AUTOTRACK_COASTLINE=0 removes it.
 *
 * VISUAL CAVEAT: the chase camera never gives a side view of the gorge, so this
 * band's appearance at the junction is NOT frame-verified -- only that it FIRES
 * at the run-boundary spans (element inventory "coastline"). */
int tg_emit_bridge_coast(const TG_NodeList *nl, int si,
                                TG_Buf *m, size_t *moff, int *pn)
{
    const double CH = TD5_TG_COAST_HALF;
    int s0, s1, ends = 0, k;
    int wnode[2], lnode[2];

    if (!tg_span_in_bridge_run(si)) return 1;
    if (!tg_water_span_clear(si)) return 1;         /* no river here */
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_COASTLINE")) return 1;
    tg_bridge_run_bounds(nl, si, &s0, &s1);

    /* River nodes run s0 .. s1+1; its open ends are node s0 (back onto s0-1) and
     * node s1+1 (forward onto s1+2). */
    if (si == s0 && s0 - 1 >= 0) {
        wnode[ends] = s0;     lnode[ends] = s0 - 1;     ends++;
    }
    if (si == s1 && s1 + 2 < nl->count) {
        wnode[ends] = s1 + 1; lnode[ends] = s1 + 2;     ends++;
    }

    for (k = 0; k < ends; k++) {
        const TG_Node *nw = &nl->v[wnode[k]];
        const TG_Node *nn = &nl->v[lnode[k]];
        const double wy = tg_bridge_water_surf_y(nl, si);      /* river surface */
        const double gy = nn->y - 70.0;   /* land at road (== TD5_TG_GROUND_DROP,
                                            * whose #define sits below the GROUND
                                            * block; the 70u z-fight bias). */
        const double lx = nw->tz, lz = -nw->tx;                /* left unit     */
        double px[4], py[4], pz[4], uu[4], vv[4];
        int seg_page = TD5_TG_PAGE_R4_COAST, seg_nq = 1, i;

        /* ============ [R9 BRIDGE item 9] "the coastline geometry looks wrong"
         * MEASURED, three faults, all in the four lines above:
         *
         * 1. IT IS TOO NARROW BY MORE THAN HALF. The river rectangle is
         *    TD5_TG_BRIDGE_WATER_HALF = 32000 either side of the centreline;
         *    this band is TD5_TG_COAST_HALF = 12000. So 20000 units of the
         *    water's transverse end on EACH side meet the bank with no shore at
         *    all -- the band ends in mid-air and the water simply stops. That is
         *    the blocky edge in the report's screenshot: not a bad texture, a
         *    band that covers 24000 of a 64000-wide seam.
         * 2. ITS LAND EDGE IS FLAT AT ROAD LEVEL. `gy` is one number applied to
         *    both land corners, so the whole 24000-wide edge is a level line at
         *    the road's height. The ground it is supposed to meet is NOT level
         *    out there -- tg_ground_side drops it by the verge profile and, on a
         *    seaward run, ramps it through sea level. The band therefore floats
         *    above the terrain at its outer corners and cuts into it further in.
         *    This is item 7's principle one element over: the two surfaces never
         *    asked each other what height they were.
         * 3. IT IS ON THE WATER PAGE'S AXES. TD5_TG_PAGE_R4_COAST is UV'd by
         *    world x/z at TD5_TG_WATER_TILE like the river itself, so a tilted
         *    shore is textured as though it were more river.
         *
         * FIX: lay the band as a lateral STRIP of columns out to the river's own
         * half-width, take each column's land height from tg_ground_side (the
         * same authority the skirt and the far band read), and texture it on the
         * R9 SHORE page in the band's own axes -- u across the strip, v from the
         * waterline to the bank. TD5RE_R9_BRIDGE_COAST=0 restores the R4 band.
         * ==================================================================== */
        if (td5_env_flag_on("TD5RE_R9_BRIDGE_COAST")) {
            /* ======== [R11 WATER item 14] "AT THE END OF THE BRIDGE THE
             * COASTLINE SHOULD REACH THE END OF THE DRAWN AREA -- IT STOPS
             * SHORT, LEAVING A VISIBLE EDGE." Two measured faults, and NEITHER
             * is item 13's seam (the road is straight at the reported run end,
             * so the divergence there is 0.000 degrees -- these are two
             * different bugs that happen to share an emitter pair).
             *
             * 1. THE BAND IS NARROWER THAN THE WORLD BEHIND IT. R9 sized it to
             *    TD5_TG_BRIDGE_WATER_HALF (32000), but the group just past the
             *    run is not over a bridge, so its far band IS drawn, out to
             *    width/2 + FAR_REACH = 33000. The shore therefore ends 1000
             *    units inside the ground it is meant to close onto and the rim
             *    shows as an open edge. tg_r11_wet_reach sizes both the band
             *    and the river it grows out of from that same far-band reach,
             *    which is what "the end of the drawn area" means here.
             * 2. ITS LAND EDGE IS ON THE WRONG NORMAL. Both the water corners
             *    AND the land corners were swept along the WATER node's normal
             *    `lx`, so at the rim the land edge misses the land node's own
             *    cross-section by CW*dheading -- measured 959 units at the run
             *    end at span 839 on seed 20260901. That is R9's own fault #2
             *    ("the two surfaces never asked each other what height they
             *    were") one axis over: they never asked what DIRECTION either.
             *    Sweep each end along its own node's normal and the band lands
             *    on the terrain the profile was sampled from.
             *
             * Lateral only -- every corner keeps the height it already had (wy
             * at the waterline, nn->y - drop on the bank), so piers, the gorge
             * skirt and the relief system are untouched.
             * TD5RE_R11_WATER=0 restores the R9 band. ================ */
            const int r11 = td5_env_flag_on("TD5RE_R11_WATER");
            const double CW = r11 ? tg_r11_wet_reach(nl, wnode[k])
                                  : TD5_TG_BRIDGE_WATER_HALF;
            const double CL = r11 ? tg_r11_wet_reach(nl, lnode[k])
                                  : TD5_TG_BRIDGE_WATER_HALF;
            const double nlx = r11 ? nn->tz : lx;   /* land node's left unit */
            const double nlz = r11 ? -nn->tx : lz;
            const int NC = 8;                      /* columns each side of centre */
            const TG_Biome *lb = &k_biomes[tg_biome_for_span(lnode[k])];
            const double wsd = lb->water ? tg_water_side(lnode[k]) : 0.0;
            /* ======== [R14 COAST item 5a] "on the LEFT side of the bridge,
             * polygons collide with the water below and with the coastline."
             *
             * MEASURED at this emit site on seed 20260901, all 7 runs x 2 ends x
             * 2 sides: the near skirt reaches 12000 and the band reaches 33000,
             * and over the shared footprint the flat skirt sits 1270..3944 units
             * ABOVE the band (1603 at the reported bridge's entry span 1319,
             * 1303 at its exit span 1360). Two surfaces own that ground and they
             * never asked each other what height they were -- R9's own fault #2,
             * recurring between a DIFFERENT pair of emitters. It is NOT the
             * own-normal sweep class R11 WATER fixed here: R11's defect was
             * lateral (which DIRECTION a corner leaves its node), this one is
             * vertical, and R11's fix is what exposed it by taking the band's
             * reach out from 32000 to the far band's 33000.
             *
             * ROOT CAUSE: the band asks tg_ground_side, which is the NEAR
             * cross-section and only defined out to the verge. Past that the
             * loop below clamps to the profile's last point, so 21000 of the
             * band's 33000 width is pinned flat at road level while the world
             * actually out there -- the far band -- has descended. At the verge
             * edge that leaves a 1270..3944 unit cliff between the two.
             *
             * FIX: ask the authority that covers the WHOLE width. tg_topo_chain
             * is exactly that -- "one span-side's whole ground surface, near
             * skirt and far band concatenated" -- and it is what the far band
             * itself is built from, so the band now lands on the terrain rather
             * than on an extrapolation of the terrain's first 12000 units.
             * TD5RE_R14_COAST_GROUND=0 restores the clamped near profile. */
            const int r14g = td5_env_flag_on("TD5RE_R14_COAST_GROUND");
            TG_GroundProf pl, pr;
            TG_TopoChain tl, tr;
            int c;
            tg_ground_side(nl, lnode[k], 1, wsd, &pl);
            tg_ground_side(nl, lnode[k], 0, wsd, &pr);
            if (r14g) {
                tg_topo_chain(nl, lnode[k], 1, &tl);
                tg_topo_chain(nl, lnode[k], 0, &tr);
            }
            for (c = 0; c < 2 * NC; c++) {
                const double t0 = -1.0 + (double)c / (double)NC;
                const double t1 = -1.0 + (double)(c + 1) / (double)NC;
                double qx[4], qy[4], qz[4], qu[4], qv[4];
                int sp = TD5_TG_PAGE_R9_SHORE, nq = 1, e;
                double d[2]; d[0] = t0; d[1] = t1;
                for (e = 0; e < 2; e++) {
                    /* Land height at |lateral| from the road edge, read off the
                     * profile for whichever side this column is on. */
                    const TG_GroundProf *p = (d[e] >= 0.0) ? &pl : &pr;
                    const double off = fabs(d[e]) * CL;   /* LAND distance */
                    double drop = p->dy[p->n - 1];
                    int j;
                    if (r14g) {
                        /* [R14 COAST 5a] The whole-width authority. */
                        drop = tg_topo_drop_at((d[e] >= 0.0) ? &tl : &tr, off);
                    } else {
                        for (j = 1; j < p->n; j++) {
                            if (off <= p->d[j]) {
                                const double sw = p->d[j] - p->d[j - 1];
                                const double u = sw > 1e-6
                                               ? (off - p->d[j - 1]) / sw : 0.0;
                                drop = p->dy[j - 1]
                                     + (p->dy[j] - p->dy[j - 1]) * u;
                                break;
                            }
                        }
                    }
                    /* water corner (e-th), then land corner (e-th) */
                    qx[e ? 1 : 0] = nw->x + lx  * (d[e] * CW);
                    qz[e ? 1 : 0] = nw->z + lz  * (d[e] * CW);
                    qy[e ? 1 : 0] = wy;
                    qx[e ? 2 : 3] = nn->x + nlx * (d[e] * CL);
                    qz[e ? 2 : 3] = nn->z + nlz * (d[e] * CL);
                    qy[e ? 2 : 3] = nn->y - drop;
                }
                /* u across the strip, v from waterline (0) to bank (1). */
                qu[0] = t0 * 4.0; qv[0] = 0.0;
                qu[1] = t1 * 4.0; qv[1] = 0.0;
                qu[2] = t1 * 4.0; qv[2] = 1.0;
                qu[3] = t0 * 4.0; qv[3] = 1.0;
                moff[*pn] = m->len;
                if (!tg_write_quad_mesh(m, qx, qy, qz, qu, qv, 4, &sp, &nq, 1))
                    return 0;
                (*pn)++;
                tg_acct(TG_ACCT_COASTLINE, wnode[k]);
                tg_acct(TG_ACCT_R9_BRIDGE, wnode[k]);
            }
            /* [R14 COAST item 5a] TWO SURFACES OWN THIS GROUND. The band lies
             * over the span BETWEEN its two nodes, and that span also lays an
             * ordinary ground SKIRT -- it is outside the bridge run, so its
             * gorge phase is 0 and its skirt is the flat road-level verge. The
             * band ramps from the water surface up to the bank across the same
             * footprint, so over the skirt's whole reach the flat verge caps it
             * and past that reach the band is suddenly the only surface. That
             * step is what "polygons collide with the water and the coastline"
             * looks like from the deck. Measured here, at the emit site, so the
             * number is the geometry actually written. */
            if (td5_env_flag_off("TD5RE_R14_COAST_REPORT")) {
                const int fs = (wnode[k] < lnode[k]) ? wnode[k] : lnode[k];
                int sd;
                for (sd = 0; sd < 2; sd++) {
                    const TG_GroundProf *p = sd ? &pl : &pr;
                    TG_TopoChain tc;
                    /* The CLIFF: how far the band's land edge is from the ground
                     * that is actually there, sampled just OUTSIDE the near
                     * skirt (where the old clamp starts extrapolating) and at
                     * the band's own outer rim. Zero on both = the band lands on
                     * the terrain. */
                    const double so = p->d[p->n - 1];
                    const double o1 = so + 1000.0, o2 = CL;
                    double b1, b2, g1, g2;
                    int j;
                    tg_topo_chain(nl, lnode[k], sd, &tc);
                    g1 = tg_topo_drop_at(&tc, o1);
                    g2 = tg_topo_drop_at(&tc, o2);
                    if (r14g) { b1 = g1; b2 = g2; }
                    else {
                        b1 = b2 = p->dy[p->n - 1];
                        for (j = 1; j < p->n; j++)
                            if (o1 <= p->d[j]) { b1 = p->dy[j]; break; }
                    }
                    TD5_LOG_W(LOG_TAG, "R14COAST cap: run-end node %d (span %d) "
                              "%s skirt=%.0f band=%.0f | at %.0f band drop %.0f "
                              "ground %.0f cliff %.0f | at rim band drop %.0f "
                              "ground %.0f cliff %.0f",
                              wnode[k], fs, sd ? "LEFT" : "RIGHT", so, CL,
                              o1, b1, g1, g1 - b1, b2, g2, g2 - b2);
                }
            }
            continue;
        }

        px[0] = nw->x - lx * CH; py[0] = wy; pz[0] = nw->z - lz * CH;
        px[1] = nn->x - lx * CH; py[1] = gy; pz[1] = nn->z - lz * CH;
        px[2] = nn->x + lx * CH; py[2] = gy; pz[2] = nn->z + lz * CH;
        px[3] = nw->x + lx * CH; py[3] = wy; pz[3] = nw->z + lz * CH;
        for (i = 0; i < 4; i++) {
            uu[i] = px[i] / TD5_TG_WATER_TILE;
            vv[i] = pz[i] / TD5_TG_WATER_TILE;
        }
        moff[*pn] = m->len;
        if (!tg_write_quad_mesh(m, px, py, pz, uu, vv, 4, &seg_page, &seg_nq, 1))
            return 0;
        (*pn)++;
        tg_acct(TG_ACCT_COASTLINE, wnode[k]);
    }
    return 1;
}

/* [R12 item 14a] The overpass arm caps itself against the coastline through
 * this, so "where the water starts" is stated once. See the declaration in the
 * overpass block. */
static double tg_shore_end(void) { return TD5_TG_SHORE_END; }

/* Ground as two SKIRT STRIPS per span -- one off each verge -- built from the
 * same road-edge computation the road mesh uses. That makes the terrain follow
 * width changes, curvature and elevation exactly, and adjacent spans share edge
 * positions so there are no seams.
 *
 * This replaced a flat box slab per display-list entry. A box cannot follow a
 * curving or undulating road: its top face sat at one height and one heading
 * for four spans, so on bends its straight edge cut visibly across the verge
 * and slab-to-slab joins showed.
 */
/* Is this biome a SNOW biome? Derived from the drivable surface rather than
 * carried in TG_Biome: the biome table is shared with several parallel work
 * areas this cycle, and "the road is ice" is already exactly the fact we mean --
 * an icy road with green grass beside it was the feedback item. Kept as its own
 * accessor so a future snow biome with a grippy road only has to change here. */
int tg_biome_is_snow(const TG_Biome *b)
{
    return b->road_surf == RS_ICE;
}

/* The page the terrain around span si is textured from.
 *
 * Overrides the biome's own ground_page with SNOW on icy biomes -- but NEVER on
 * a tunnel span: a tunnel is an enclosed section (Group C's massing sits around
 * its mouths) and snow inside it would show through the lining as a bright
 * floor. Cross-group constraint, gate kept here so both the skirt and the far
 * terrain read the same page. TD5RE_AUTOTRACK_SNOW=0 restores the green skirt. */
int tg_ground_page_for_span(int si, const TG_Biome *b)
{
    /* [R3 item 14] "patches of grass on bridges". The terrain beside a bridge
     * run is the GORGE bank -- rock/earth dropping to the river -- not lawn, but
     * it inherited the biome's green ground page (FIELDS/COAST carry bridges), so
     * grass slabs showed under and beside the deck. Give the whole run the
     * neutral concrete/earth embankment page instead. Cosmetic only; the gorge
     * SHAPE (pull-back + drop to the river) is set in tg_ground_side. */
    if (tg_span_in_bridge_run(si)) return TD5_TG_PAGE_GROUND;
    if (!tg_biome_is_snow(b)) return b->ground_page;
    if (tg_span_in_tunnel(si)) return b->ground_page;
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_SNOW")) return b->ground_page;
    /* [R8 TERRAIN item 16] "use different snow ground textures too". One page
     * used to cover every snow span on the track. Pick a variant per biome CELL,
     * not per span: R7 item 12 is the recorded cost of a per-span page roll (a
     * fork's median DITHERED between two pages span by span), so the selector
     * keys on the hard cell index and one variant runs a whole 150-span cell.
     * TD5RE_R8_TERRAIN_SNOW=0 restores the single page. */
    if (td5_env_flag_on("TD5RE_R8_TERRAIN_SNOW")) {
        const unsigned h = (unsigned)((si < 0 ? 0 : si) / TD5_TG_BIOME_RUN)
                         * 2654435761u;
        return TD5_TG_PAGE_R8_SNOWGND
             + (int)((h >> 15) % (unsigned)TD5_TG_R8_SNOWGND_N);
    }
    return TD5_TG_PAGE_SNOW;
}

/* [R8 TERRAIN item 16] The MEDIAN surface in a snow biome. A gore floor and an
 * avenue island both inherited the biome's own ground page, which on an icy
 * biome is the green summer page (tg_fork_gore_page reads k_biomes[].ground_page
 * directly, bypassing the SNOW override in tg_ground_page_for_span) -- so a snow
 * run had a green strip down the middle of its avenues. Returns the ploughed-snow
 * page, which is deliberately NOT the snow GROUND page: the user asked for the
 * median to be snowy "but a different texture". Returns `fallback` off snow.
 *
 * `tunnel_exempt` says whether "si is inside a bore" may veto the snow page.
 * TRUE is right when si is the span BEING DRAWN (no snow falls indoors); it is
 * wrong when si is only an ANCHOR standing for a whole fork -- see
 * tg_fork_gore_page and the [R11 BIOME item 3] note there. */
int tg_r8_median_page_ex(int si, int fallback, int tunnel_exempt)
{
    const TG_Biome *b = &k_biomes[tg_biome_cell_index(si)];
    if (!td5_env_flag_on("TD5RE_R8_TERRAIN_SNOW")) return fallback;
    if (!tg_biome_is_snow(b)) return fallback;
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_SNOW")) return fallback;
    if (tunnel_exempt && tg_span_in_tunnel(si)) return fallback;
    return TD5_TG_PAGE_R8_SNOWMED;
}

/* Per-SPAN form: si is the span being drawn, so the bore exemption applies. */
int tg_r8_median_page(int si, int fallback)
{
    return tg_r8_median_page_ex(si, fallback, 1);
}

/* [R9 TOPO C4, item 11] THE GROUND IS A SURFACE, NOT A CATEGORY.
 *
 * "the transition around span 1050 is grass in between tiles, this is wrong."
 * Seed 99991 has INDUSTRIAL 900-1049 and COAST 1050-1199, so 1050 is exactly a
 * biome cell boundary -- and INDUSTRIAL's ground_page is TD5_TG_PAGE_GROUND
 * (concrete tiles) while COAST's is TD5_TG_PAGE_GREEN (grass).
 *
 * MECHANISM, and it is not the R8 BRIDGE one-end-sampled bug the triage
 * suspected: that bug was FIXED in R8 (tg_emit_ground now samples a profile at
 * BOTH ends of every slab) and it was about HEIGHT, not material. This is the
 * categorical dither. tg_emit_ground takes its biome from tg_biome_for_span,
 * the BLENDED index, which inside the +-20-span band around a cell boundary
 * assigns each span to the incoming or outgoing biome by a PER-SPAN HASH. For
 * trees that is exactly right and is why the blend exists -- a forest thins
 * into a city over 140 m instead of stopping on one span. For the GROUND
 * SURFACE it means forty consecutive slabs each roll their own material, so a
 * grass slab lands between two tile slabs. That is not a transition, it is a
 * chequerboard, and it is what the user photographed.
 *
 * The precedent is already in this file, one level down: R7 CITY item 7 took
 * kerb height and railing off the dither because "a raised kerb and a guard
 * rail are per-RUN structure, not a dithered categorical field", and R7 item 12
 * did the same for the fork median because it "DITHERED between two biomes'
 * ground pages span by span". This is the same statement about the ground
 * itself, made once, where every ground consumer reads it.
 *
 * So the SURFACE material keys on the HARD cell (tg_biome_cell_index): one
 * material per ground run, changing exactly once, at the boundary. Everything
 * that STANDS on the ground -- trees, props, facades -- keeps the dither.
 * TD5RE_R9_TOPO=0 restores the per-span roll for an A/B. */
int tg_topo_ground_index(int si)
{
    return tg_topo_enabled() ? tg_biome_cell_index(si) : tg_biome_for_span(si);
}

/* The one page the ground surface at span si is drawn on. Routed through
 * tg_ground_page_for_span so the bridge-run, snow and tunnel overrides that
 * already had to agree between skirt and far band still apply, unchanged. */
int tg_topo_surface_page(int si)
{
    return tg_ground_page_for_span(si, &k_biomes[tg_topo_ground_index(si)]);
}

/* Lateral clearance the RIGHT-hand skirt must leave at span si so it does not
 * cover the branch carriageway.
 *
 * Root cause of "grass on top of the road on a branch": the skirt is built from
 * the MAIN centreline's road edges (tg_road_edge with shift 0, full width), so
 * its inner edge sits at -width/2 -- but over a fork the branch half-carriageway
 * has bowed outward to as far as -(width/2 + BOW*width), i.e. underneath the
 * skirt. Pushing the skirt's inner edge out to the branch's OUTER edge leaves
 * the carriageway uncovered; the gore mesh already fills the wedge inboard of
 * it, so nothing shows through. Returns 0 where no fork is active.
 *
 * This was the second hand-rolled copy of the fork arithmetic (the note in the
 * CARRIAGEWAY QUERY section named it). It read s_forks directly and assumed a
 * FIXED half carriageway (w*0.25 out from the corridor centre), so once the
 * corridor could widen and taper it UNDER-REPORTED and the skirt went back over
 * the branch. Retired onto tg_carriageway_reach at the round-2 merge
 * (2026-08-27).
 *
 * The 0-off-a-fork return is DELIBERATE and is why this does not just call
 * tg_carriageway_clear_gap with a 200 margin: the skirt's inner point sits
 * flush with the asphalt on an ordinary span on purpose (see tg_ground_side --
 * setting it back left a thin void between road and grass). The margin is only
 * wanted where there is actually a carriageway to clear, so the excess over the
 * main road's own half width is what gets tested. */
double tg_ground_branch_clear(const TG_NodeList *nl, int si)
{
    double over;

    if (!tg_branches_enabled()) return 0.0;
    /* How far the outermost carriageway reaches PAST the main road edge. Zero
     * on any span no corridor bows across, since reach floors at the half width. */
    over = tg_carriageway_reach(nl, si, -1.0) - tg_road_half_width(nl, si);
    if (over <= 0.0) return 0.0;
    return over + 200.0;                 /* + margin, no shared edge */
}

void tg_r12_spanq(int nspans)
{
#ifndef TD5RE_RELEASE
    int q, s;
    if (!getenv("TD5RE_R12_SPANQ")) return;
    q = atoi(getenv("TD5RE_R12_SPANQ"));
    if (nspans > TD5_TG_MAX_SPANS) nspans = TD5_TG_MAX_SPANS;
    for (s = q - 3; s <= q + 3; s++) {
        char kinds[512];
        int pos = 0, k;
        if (s < 0 || s >= nspans) continue;
        kinds[0] = '\0';
        for (k = 0; k < TG_ACCT_KIND_COUNT; k++) {
            if (!TG_ACCT_MASK_TEST(s, k)) continue;
            if (pos < (int)sizeof(kinds) - 20)
                pos += snprintf(kinds + pos, sizeof(kinds) - (size_t)pos,
                                "%s%s", pos ? " " : "", k_acct_names[k]);
        }
        {
            const TG_Biome *b = &k_biomes[tg_biome_cell_index(s)];
            TD5_LOG_I(LOG_TAG,
                      "R12SPANQ span=%d biome=%s tunkind=%d "
                      "treeline_band=%.0f back=%.0f page=%d: %s",
                      s, b->name, tg_tunnel_kind(s),
                      tg_treeline_height(b), tg_treeline_back(b),
                      TD5_TG_PAGE_TREELINE,
                      kinds[0] ? kinds : "(nothing accounted)");
        }
    }
#else
    (void)nspans;
#endif
}

void tg_r8_bridge_diag(const TG_NodeList *nl)
{
    int si;

    if (!td5_env_flag_off("TD5RE_R8_BRIDGE_DIAG")) return;

    TD5_LOG_I(LOG_TAG, "R8BDIAG hdr "
              "si,phase,node_y,deckmin,water_y,surf,bed,"
              "skirt_in_y,skirt_out_y,skirt_in_d,skirt_out_d,"
              "pier,pier_top,pier_bot,style,waterq,seaward");
    for (si = 0; si + 1 < nl->count; si++) {
        double phase, wy, surf, bed, ptop, pbot, h;
        TG_GroundProf pl, pr;
        const TG_Node *n = &nl->v[si];
        const double wsd = tg_water_side(si);
        int style, pitch, has_pier, seaward;

        if (!tg_span_in_bridge_run(si)) continue;
        phase = tg_bridge_gorge_phase(nl, si);
        wy    = tg_bridge_water_y(nl, si);
        surf  = tg_bridge_water_surf_y(nl, si);
        bed   = wy - 150.0;
        style = tg_bridge_style(si);
        pitch = tg_bridge_pier_pitch(style);
        has_pier = ((si % pitch) == 0);
        ptop  = n->y - TD5_TG_BRIDGE_UNDER;
        h     = (ptop - (wy - 400.0)) * 0.5;
        if (h < 150.0) h = 150.0;
        pbot  = ptop - 2.0 * h;
        tg_ground_side(nl, si, 1, wsd, &pl);
        tg_ground_side(nl, si, 0, wsd, &pr);
        seaward = (wsd > 0.0);
        TD5_LOG_I(LOG_TAG, "R8BDIAG %d,%.3f,%.0f,%.0f,%.0f,%.0f,%.0f,"
                  "%.0f,%.0f,%.0f,%.0f,%d,%.0f,%.0f,%d,%d,%d",
                  si, phase, n->y, tg_bridge_deck_y(nl, si), wy, surf, bed,
                  n->y - pl.dy[0], n->y - pl.dy[pl.n - 1],
                  pl.d[0], pl.d[pl.n - 1],
                  has_pier, ptop, pbot, style,
                  tg_water_span_clear(si), seaward);
    }
    for (si = 1; si + 1 < nl->count; si++) {
        double bh, bs;
        if (!tg_span_in_tunnel(si)) continue;
        if (tg_span_in_tunnel(si - 1) && tg_span_in_tunnel(si + 1)) continue;
        tg_tunnel_bore(nl, si, &bh, &bs);
        TD5_LOG_I(LOG_TAG, "R8TDIAG mouth si=%d lining_page=%d portal_page=%d "
                  "bore_half=%.0f shift=%.0f node_y=%.0f",
                  si, tg_tunnel_lining_page(si),
                  !td5_env_flag_on("TD5RE_AUTOTRACK_R7_PORTAL")
                      ? TD5_TG_PAGE_R6_TUNNEL + 4
                  : td5_env_flag_on("TD5RE_R8_PORTAL")
                      ? TD5_TG_PAGE_R8_BRIDGE + 1 : TD5_TG_PAGE_R7_BRIDGE + 0,
                  bh, bs, nl->v[si].y);
    }
}

/* [R7 item 18] "Water near the road but trees floating in the air ... add a
 * check to see if trees are touching ground, and if it's water or a coastline
 * trees should not be placed." Two rules, one gate:
 *
 * 1. GROUND. The tree base used n->y -- the ROAD height -- so a tree set back
 *    onto terrain that falls away from the road hung in the air above it. Sample
 *    the drop-below-road at the trunk's lateral distance from the SAME
 *    cross-section the ground mesh is built from (tg_ground_side), and stand the
 *    tree on that. Away from a coast the skirt is near-flat (70 raw), so this is
 *    a no-op there; on a seaward coast the terrain ramps down and this makes the
 *    trunk follow it instead of floating.
 *
 * 2. WATER / COASTLINE. On a coastal run's SEAWARD side the terrain leaves a flat
 *    verge (TD5_TG_SHORE_VERGE) and then ramps THROUGH sea level to the shore.
 *    Anything planted past that flat verge is on the beach ramp or the sea, so
 *    refuse it -- exactly the span-1120 "trees floating over water" report. The
 *    landward side and non-coastal biomes are unaffected.
 *
 * Bridge decks are already skipped by both callers, so the gorge branch of
 * tg_ground_side never applies here. Gated by TD5RE_R7_FLORA (default ON; =0
 * restores the R6 behaviour of planting at road height with no water check). */
int tg_flora_plant(const TG_NodeList *nl, int si, const TG_Biome *b,
                          double side, double gap, double tw,
                          double *cx, double *cz, double *base_y)
{
    const TG_Node *n = &nl->v[si];
    const double d  = gap + tw * 0.5;             /* trunk distance from road edge */
    const double lx = n->tz * side, lz = -n->tx * side;
    const double water_side = b->water ? tg_water_side(si) : 0.0;
    TG_GroundProf p;
    double dy;
    int k;

    *cx = n->x + lx * (n->width * 0.5 + gap + tw * 0.5);
    *cz = n->z + lz * (n->width * 0.5 + gap + tw * 0.5);
    *base_y = n->y;

    if (!td5_env_flag_on("TD5RE_R7_FLORA")) return 1;   /* A/B: R6 behaviour */

    /* Rule 2: over the seaward beach/sea of a coastal run -> no tree. */
    if (b->water && side == tg_water_side(si) && d > (double)TD5_TG_SHORE_VERGE)
        return 0;

    /* [R9 TOPO item 6] Rule 1 used to sample the SKIRT only and clamp to its
     * outer point, so anything planted past the skirt stood at the LIP height
     * while the ground under it had already descended -- "the trees are not
     * following". Sample the whole chain instead: skirt AND the far band's
     * descent, one surface, the same one the ground slab is built from. */
    if (tg_topo_enabled()) {
        TG_TopoChain c;
        tg_topo_chain(nl, si, side > 0.0, &c);
        *base_y = n->y - tg_topo_drop_at(&c, d);
        return 1;
    }

    /* Rule 1: rest on the sampled terrain, not at road height. */
    tg_ground_side(nl, si, side > 0.0, water_side, &p);
    if (d <= p.d[0]) {
        dy = p.dy[0];
    } else if (d >= p.d[p.n - 1]) {
        dy = p.dy[p.n - 1];
    } else {
        dy = p.dy[p.n - 1];
        for (k = 0; k + 1 < p.n; k++)
            if (d <= p.d[k + 1]) {
                const double t = (d - p.d[k]) / (p.d[k + 1] - p.d[k]);
                dy = p.dy[k] + t * (p.dy[k + 1] - p.dy[k]);
                break;
            }
    }
    *base_y = n->y - dy;
    return 1;
}

/* [R7 item 12] The median (gore floor) surface page, held STABLE for the whole
 * length of a fork. tg_emit_gore used to key its page on tg_biome_for_span(si) --
 * the BLENDED biome index -- so where a biome boundary crosses a fork (seed 99991
 * fork 3, 510-631, enters CITY at 600) the median floor DITHERED between the two
 * biomes' ground pages span by span, the "median switches between grass and
 * tiles" report (item 12, span 611). Keyed to the fork's HARD start cell, exactly
 * as the divider TREATMENT keys on the fork ordinal (tg_emit_avenue_divider,
 * fork_index % 3), so ONE material runs the length of the median instead of a
 * per-span roll. */
int tg_fork_gore_page(int fork_index)
{
    int si, page;
    if (fork_index < 0 || fork_index >= s_fork_count)
        return k_biomes[0].ground_page;
    si   = s_forks[fork_index].F;
    page = k_biomes[tg_biome_cell_index(si)].ground_page;
    /* [R8 TERRAIN item 16] On an icy biome that ground_page is still the green
     * summer page -- this reads k_biomes directly and so never saw the SNOW
     * override. Route it to the ploughed-snow median page instead. Keyed on the
     * fork's HARD start cell exactly as the rest of this function is, so the
     * median stays ONE material for the whole fork (the R7 item 12 contract).
     *
     * [R11 BIOME item 3] "On span 331 everything is snowy but the median is
     * green." NOT a missing lookup -- the lookup above is exactly right and
     * fires. The defect is WHICH SPAN IT IS ASKED ABOUT.
     *
     * `si` here is an ANCHOR, not a location. It stands for the whole fork so
     * that ONE material runs its length, and every property this function reads
     * off it is genuinely a whole-fork property: the biome cell is 150 spans,
     * the snow decision is the biome's. But tg_r8_median_page ALSO tests "is si
     * inside a bore", and that is not a whole-fork property -- a bore is a
     * 20-span enclosure and a fork is up to 120 spans. Seed 20260901 puts bore
     * run 300-319 nose to tail with fork 1 at F=319, so the anchor lands on the
     * single enclosed span of a 40-span fork, the exemption fires, and all 40
     * spans of median -- every one of them outdoors, in ALPINE -- fall back to
     * ALPINE's summer ground_page, which is green grass. One indoor span
     * de-snowed the whole median.
     *
     * The bore is handled where a bore can be handled correctly: PER SPAN, at
     * the gore emit site, which already swaps in the bore-floor concrete for
     * the spans actually enclosed ([R8 item 17], TD5RE_R8_BORE_MEDIAN). So the
     * anchor lookup must not carry the tunnel test at all -- it would only ever
     * be answering it about the wrong span. Same shape as the R8 TERRAIN slab
     * bug (one cross-section sampled for both edges of a slab) and the R7 item
     * 12 bug this function was written for: a per-span fact used as a per-run
     * one. TD5RE_R11_BIOME_MEDIAN=0 restores the anchor-tests-the-bore
     * behaviour for an A/B. */
    return tg_r8_median_page_ex(
        si, page, td5_env_flag_on("TD5RE_R11_BIOME_MEDIAN") ? 0 : 1);
}

/* `half_n` / `half_f` are the branch carriageway's OWN half width at each end of
 * the span, not a fixed quarter of the road. Until 2026-08-27 this computed
 * width*0.25 internally, which was the half width of a FIXED half carriageway
 * -- once the corridor could widen and taper (tg_branch_wscale) the gore stopped
 * reaching the branch's left edge and re-opened the see-through slit at the
 * mouth that TD5_TG_GORE_OVERLAP exists to close. */
int tg_emit_gore(const TG_NodeList *nl, int si,
                        double shift_n, double shift_f,
                        double half_n, double half_f, int ground_page,
                        TG_Buf *blk)
{
    const TG_Node *a = &nl->v[si], *c = &nl->v[si + 1];
    const double drop = TD5_TG_GORE_DROP;
    const double ov   = TD5_TG_GORE_OVERLAP;
    /* Branch left edge, pushed a further `ov` to the RIGHT (lateral is +ve to
     * the left of travel, and the branch sits at negative lateral). */
    double tnr = shift_n + half_n - ov;            /* near */
    double tfr = shift_f + half_f - ov;            /* far  */
    double px[4], py[4], pz[4], uu[4], vv[4];
    double cx = 0, cy = 0, cz = 0, radius = 0;
    /* [R8 SHAPE G5] Lateral texture repeat. V already advances one tile per
     * span, so U spanning 0..1 across the WHOLE gore only matched the V density
     * while the gore was about a span wide -- which it was, at bow 1.20. A long
     * fork's gore is 18000+ units across, i.e. one tile smeared over twelve
     * span lengths. Tile U at the same world period as V so the median reads at
     * the same texel density however wide it gets. Floored at 1.0 so a narrow
     * avenue gore is byte-identical to the shipped one. */
    double un = 1.0, uf = 1.0;
    int i;

    /* [R11 CROSS item 8] INSTRUMENT: the gore floor is THE median surface, and
     * it is laid at road level minus TD5_TG_GORE_DROP on every fork. Log its
     * width and its rise so "flush with the road" is a number, not a guess. */
    if (tg_r11_cross_diag())
        TD5_LOG_I(LOG_TAG, "trackgen: [R11 GORE] span %4d width=%.0f "
                  "rise=%.0f (road level minus drop)", si,
                  fabs(shift_n + half_n), -drop);

    if (tg_r8_longbranch_enabled()) {
        un = fabs(tnr - ov) / (double)TD5_TG_SPAN_LENGTH;
        uf = fabs(tfr - ov) / (double)TD5_TG_SPAN_LENGTH;
        if (un < 1.0) un = 1.0;
        if (uf < 1.0) uf = 1.0;
    }

    /* near-left = road centre pushed `ov` INTO the main carriageway,
     * near-right = branch left edge pushed `ov` into the branch, then far. */
    px[0]=a->x+a->tz*ov;    py[0]=a->y-drop; pz[0]=a->z-a->tx*ov;    uu[0]=0.0; vv[0]=(double)si;
    px[1]=a->x+a->tz*tnr;   py[1]=a->y-drop; pz[1]=a->z-a->tx*tnr;   uu[1]=un;  vv[1]=(double)si;
    px[2]=c->x+c->tz*tfr;   py[2]=c->y-drop; pz[2]=c->z-c->tx*tfr;   uu[2]=uf;  vv[2]=(double)si+1.0;
    px[3]=c->x+c->tz*ov;    py[3]=c->y-drop; pz[3]=c->z-c->tx*ov;    uu[3]=0.0; vv[3]=(double)si+1.0;

    for (i = 0; i < 4; i++) { cx += px[i]; cy += py[i]; cz += pz[i]; }
    cx /= 4; cy /= 4; cz /= 4;
    for (i = 0; i < 4; i++) {
        double dx=px[i]-cx, dy=py[i]-cy, dz=pz[i]-cz;
        double d = sqrt(dx*dx+dy*dy+dz*dz);
        if (d > radius) radius = d;
    }
    if (!(radius > 0.0)) radius = 1.0;

    tg_put_u16(blk, 259);
    tg_put_u16(blk, 0);
    tg_put_u32(blk, 1);
    tg_put_u32(blk, 4);
    tg_put_f32(blk, radius);
    tg_put_f32(blk, cx); tg_put_f32(blk, cy); tg_put_f32(blk, cz);
    tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
    tg_put_u32(blk, 0);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE + TD5_TG_CMD_SIZE);
    tg_put_u32(blk, 0);
    tg_put_u16(blk, 0);
    tg_put_u16(blk, (unsigned)ground_page);
    tg_put_u32(blk, 0);
    tg_put_u16(blk, 0);
    tg_put_u16(blk, 1);                    /* one quad */
    tg_put_u32(blk, 0);
    for (i = 0; i < 4; i++) {
        tg_put_f32(blk, px[i]); tg_put_f32(blk, py[i]); tg_put_f32(blk, pz[i]);
        tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
        tg_put_u32(blk, 0xFFFFFFFFu);
        tg_put_f32(blk, uu[i]); tg_put_f32(blk, vv[i]);
        tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
    }
    return !blk->oom;
}
